/**
 * A multistream pull in blocks: what the threads of one pull share, and the
 * count of stream threads in the server.
 *
 * How a pull in blocks works
 * --------------------------
 * The file is cut into blocks (see PlanBlocks()).  Each stream of the pull is
 * a thread, a "worker", that owns one libcurl handle and its State.  A worker
 * takes the next block, requests it with a range request, writes it at its
 * own offset in pieces of one write size, reports it done, and takes the next.
 * The thread of the COPY request is the "coordinator": it starts the workers,
 * sends the performance markers to the client, watches that the pull moves,
 * and gives the verdict once every worker has ended.
 *
 * Who may touch what
 * ------------------
 * - Nothing of libcurl is shared: a handle is used by its worker only, from
 *   the start of the thread until it has been joined.
 * - BlockPull is the one object the workers share.  Everything in it is
 *   behind its mutex, except the three atomics, which are read without it.
 * - The response to the client, the log and the transfer record belong to the
 *   coordinator.  Workers report through BlockPull and never write to them.
 *   (The socket callbacks of libcurl, which run in a worker, do touch the
 *   record; the parts they touch have a mutex or are atomics.)
 * - The file is written by the workers through Stream::WriteAt(), one at a
 *   time, and is closed by the coordinator after the workers are joined.
 *
 * Stopping
 * --------
 * The first failure is recorded, once, and every worker is told to stop.  A
 * worker sees the stop in the progress callback of libcurl (called about once
 * a second), before each write to the file, or when it asks for a block.
 * BlockPull outlives the workers: the coordinator joins every thread on every
 * way out before it lets go of it.
 */

#ifndef __XRD_TPC_BLOCK_PULL_HH__
#define __XRD_TPC_BLOCK_PULL_HH__

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <sys/types.h>

#include "XrdSys/XrdSysRAtomic.hh"

namespace TPC {

/**
 * The stream threads in use in this process, over all the pulls in blocks.
 *
 * A pull takes a share before it starts and gives it back, through the
 * destructor of the share, when its threads have been joined.  Nothing waits
 * for a share: a pull that cannot have two threads is carried out with a
 * single request, which needs no thread of its own.
 *
 * The count is one per process.  Each handler compares it with its own limit.
 */
class StreamThreadBudget {
public:
    class Share {
    public:
        Share() = default;
        ~Share() {ShrinkTo(0);}
        Share(Share &&other) noexcept : m_count(other.m_count) {other.m_count = 0;}
        Share &operator=(Share &&other) noexcept;
        Share(const Share &) = delete;
        Share &operator=(const Share &) = delete;

        int Count() const {return m_count;}
        // Gives back what is above `count`.
        void ShrinkTo(int count);

    private:
        friend class StreamThreadBudget;
        explicit Share(int count) : m_count(count) {}
        int m_count{0};
    };

    // Takes as many threads as are free below `limit`, at most `wanted`.
    // The share is empty when none is free.
    static Share Take(int wanted, int limit);

    // Threads taken at this moment.
    static int InUse() {return s_in_use;}

    // The count itself, to be registered with the summary statistics of the
    // server: what is reported is then the number the budget works with, and
    // cannot lag behind it.
    static RAtomic_int32_t &Counter() {return s_in_use;}

private:
    static RAtomic_int32_t s_in_use;
};

/**
 * Why a pull in blocks failed: what the first worker to fail saw.
 */
struct BlockFailure {
    int error_code{0};               // a State::ErrorCode, or BlockPull::errInternal
    int status_code{0};              // the status of the source that failed the request; 0 if none did
    CURLcode curl_result{CURLE_OK};  // what libcurl returned for the request
    std::string message;             // from the source, the storage, or this code
    size_t stream{0};                // the worker, counted from 1; 0 for the coordinator
    off_t block_start{0};            // the block it was fetching: bytes [block_start, block_end);
    off_t block_end{0};              // both 0 when it was not fetching one
    off_t block_received{0};         // bytes of that block received when it failed
    // Not 0: the request was ended by the stall check of its stream.  The
    // seconds the stream was allowed to wait, and those it had waited.
    int stall_limit{0};
    int stall_idle{0};
};

/**
 * What a worker tells about itself, for the stall check of the coordinator.
 *
 * The check of the whole pull (do the bytes of all streams together grow?)
 * does not see one stream that has stopped among others that move.  So each
 * worker publishes how much it has received and what it is doing, and the
 * coordinator ends the request of a worker that is waiting for the source
 * and gets nothing.  libcurl does not call its progress callback while a
 * worker is inside a write, so it could not judge this itself: time spent
 * writing, or waiting for a block, must not count against the source.
 *
 * All members are atomics: the worker writes `received`, `request_start`,
 * `phase` and `request`; the coordinator writes `stall_limit`, `stall_idle`
 * and `abort`.
 * They are read one by one, not together: a look that mixes two requests is
 * off by one look, a second, and no more.
 */
struct WorkerStatus {
    enum Phase : int {
        Idle = 0,       // between two requests, or waiting for a block
        Receiving = 1,  // in a request: connecting, or waiting for data from the source
        Writing = 2     // in a request, inside a write to the file or waiting for its turn to write
    };

    std::atomic<off_t>    received{0};         // bytes received by this worker, over all its requests
    std::atomic<off_t>    request_start{0};    // what `received` was when the request in progress began
    std::atomic<int>      phase{Idle};
    std::atomic<unsigned> request{0};          // number of the request in progress; counts from 1
    std::atomic<int>      stall_limit{0};      // when `abort` was set: the seconds allowed,
    std::atomic<int>      stall_idle{0};       // and the seconds for which nothing had come
    // The request the coordinator wants ended; 0 for none.  It names the
    // request, so that an order that comes a moment too late, when the worker
    // has gone on to its next request, ends nothing.
    std::atomic<unsigned> abort{0};
};

/**
 * What the coordinator remembers of one worker between two looks at its
 * WorkerStatus.  Used by the coordinator only.
 */
class StallWatch {
public:
    /**
     * Looks at a worker.
     *
     * @param now            seconds on a steady clock
     * @param received,request_start,phase,request  what the worker published
     * @param first_timeout  seconds a request may wait for its first byte
     * @param timeout        seconds a request may wait for more once it has some
     * @return 0, or the seconds for which the worker has been waiting for the
     *         source and got nothing, if that is longer than allowed
     */
    long long Stalled(long long now, off_t received, off_t request_start, int phase,
                      unsigned request, int first_timeout, int timeout);

    // The seconds a request in that state may wait: `first_timeout` until it
    // has had a byte, `timeout` afterwards.
    static int Allowed(off_t received, off_t request_start, int first_timeout, int timeout) {
        return received <= request_start ? first_timeout : timeout;
    }

private:
    bool m_seen{false};
    unsigned m_request{0};        // the request last looked at
    off_t m_received{0};          // bytes at the last look
    long long m_since{0};         // when it last received, wrote, or started the request
};

/**
 * The check that the source file does not change during a pull.
 *
 * A pull fetches a file in several requests, and their parts may only be
 * put together if they come from the same version of the file (RFC 9110,
 * Section 15.3.7.3).  Each response is compared with what earlier ones said:
 *
 * - the total length it gives in Content-Range, with the size of the file;
 * - a strong ETag, or failing that Last-Modified, with the first one from
 *   the same host.
 *
 * Validators are compared between responses of one server only.  Behind a
 * redirector the requests of a pull may be answered by different servers
 * that hold copies of the file, whose ETag and Last-Modified commonly differ
 * although the content is the same.  A header that is absent is not a
 * mismatch.
 *
 * Servers that share one address (behind a load balancer, or a proxy) cannot
 * be told apart here, and if their validators differ a healthy pull fails.
 * For such a source the comparison of validators can be turned off, which
 * leaves the comparison of sizes.
 *
 * Safe to call from every stream thread.
 */
class SourceCheck {
public:
    // What one response says about the version of the file.
    struct Seen {
        std::string host;           // the server that answered, as tcp:address:port; empty if unknown
        off_t total{-1};            // total length from Content-Range; -1 if not given
        std::string etag;           // the ETag header as sent; empty if none
        std::string last_modified;  // the Last-Modified header as sent; empty if none
    };

    /**
     * @param file_size   size of the source file, as the pull was told it
     * @param validators  whether ETag and Last-Modified are compared as well
     */
    explicit SourceCheck(off_t file_size, bool validators = true)
        : m_file_size(file_size), m_validators(validators) {}

    // False, with what changed in `error`, if the response disagrees with the
    // size of the file or with an earlier response of the same host.
    bool Agrees(const Seen &seen, std::string &error);

private:
    struct Version {
        std::string etag;           // the first strong ETag of the host
        std::string last_modified;  // its first Last-Modified
    };

    const off_t m_file_size;
    const bool m_validators;
    std::mutex m_mutex;                      // guards m_hosts
    std::map<std::string, Version> m_hosts;
};

/**
 * A block that is complete, for the log.
 */
struct BlockReport {
    size_t stream;
    off_t start;     // bytes [start, end)
    off_t end;
    double seconds;  // from the request to the last write
};

class BlockPull {
public:
    // A failure that is none of the State error codes; its message is
    // reported as it is.
    static const int errInternal = 100;

    /**
     * @param file_size   size of the source file
     * @param block_size  size of every block but the last; a whole number of writes
     * @param workers     number of stream threads; sets how far the hand-out
     *                    may run ahead of the oldest block that is not done
     */
    BlockPull(off_t file_size, off_t block_size, size_t workers);

    // ---- For the workers --------------------------------------------------

    /**
     * Hands out the next block, in file order.
     *
     * Waits while the hand-out is a whole window (twice the workers, in
     * blocks) ahead of the oldest block that is not done.  Without that
     * limit one slow worker would let the others write ever further ahead of
     * it, and the bound on out-of-order data that the block size is meant to
     * give would not hold.
     *
     * @return false when no block is left or the pull is stopping.
     */
    bool NextBlock(off_t &offset, off_t &size);

    // The block starting at `offset` has been received in full and written.
    void BlockDone(size_t stream, off_t offset, off_t size, double seconds);

    // Records the first failure and stops the pull.  Later calls change
    // nothing but still stop it.  Called from catch blocks: does not throw.
    void Fail(const BlockFailure &failure) noexcept;

    // The last thing a worker does.
    void WorkerEnded() noexcept;

    // ---- For the coordinator ----------------------------------------------

    // To be called before each thread is started, and when it could not be.
    void WorkerStarting();
    void WorkerNotStarted();

    // Tells every worker to stop and wakes those that wait.  Does not throw.
    void Stop() noexcept;

    // Waits until no worker runs, at most for `patience`.  True if none runs.
    bool WaitForWorkers(std::chrono::seconds patience);

    // Workers started and not ended.
    int Running();

    // The blocks completed since the last call.
    std::vector<BlockReport> TakeReports();

    bool Failed();
    BlockFailure Failure();
    size_t BlocksDone();
    size_t Blocks() const {return m_blocks;}
    off_t FileSize() const {return m_file_size;}
    // True when every block has been handed out.
    bool AllHandedOut();

    /**
     * The offset below which the file is made of complete blocks only: the
     * start of the oldest block that is not done.  A pull that has failed is
     * cut back to it, so that what is left holds no hole and no block that
     * has not passed its check.
     */
    off_t CompleteUpTo();

    // ---- Read without the mutex -------------------------------------------

    std::atomic<bool>  stop{false};     // every worker is to stop; set by Stop() only
    std::atomic<off_t> received{0};     // bytes received from the source, all workers
    std::atomic<off_t> written{0};      // bytes the storage has taken, all workers

private:
    // The start of the oldest block that is not done.  Mutex held.
    off_t OldestUnfinished() const;

    const off_t  m_file_size;
    const off_t  m_block_size;
    const size_t m_blocks;
    const off_t  m_window;  // how far, in bytes, the hand-out may be ahead of the oldest unfinished block

    std::mutex m_mutex;                 // guards everything below
    std::condition_variable m_changed;  // a block is done, a worker has ended, or the pull stops
    off_t m_next_offset{0};             // start of the next block to hand out
    std::set<off_t> m_in_flight;        // starts of the blocks handed out and not done
    size_t m_blocks_done{0};
    int m_running{0};                   // workers started and not ended
    bool m_failed{false};
    BlockFailure m_failure;             // the first failure
    std::vector<BlockReport> m_reports;
};

}

#endif // __XRD_TPC_BLOCK_PULL_HH__
