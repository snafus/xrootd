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
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <sys/types.h>

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
    static int InUse() {return s_in_use.load();}

private:
    static std::atomic<int> s_in_use;
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
