/**
 * Settings of a multistream pull, and the plan of its blocks.
 *
 * A pull with several streams is carried out in one of two ways:
 *
 * - sequential (the default): ranges of 16 MiB are requested by all the
 *   streams and written to the file in file order, through reorder buffers.
 *   It works on any storage.
 * - blocks: each stream is a thread that requests large blocks of the file,
 *   one after another, and writes each at its own offset.  It needs storage
 *   that accepts writes at any offset.
 *
 * The settings come from the http_multi.* directives of the configuration
 * file (see XrdHttpTpcConfigure.cc).  Nothing here reads the configuration or
 * touches the server: the structure is filled with numbers, checked once with
 * Validate(), and then only read.  That keeps it testable on its own.
 */

#ifndef __XRD_TPC_PULL_OPTIONS_HH__
#define __XRD_TPC_PULL_OPTIONS_HH__

#include <cstddef>
#include <string>
#include <sys/types.h>

namespace TPC {

struct PullOptions {
    enum class Mode {
        Sequential,  // ranges written in file order (the default)
        Blocks       // one thread per stream, blocks written at their offsets
    };

    static constexpr off_t KiB = 1024;
    static constexpr off_t MiB = 1024 * KiB;
    static constexpr off_t GiB = 1024 * MiB;
    static constexpr off_t TiB = 1024 * GiB;

    // Limits of what a site may configure.
    static constexpr int   MaxStreamsPerRequest = 100;      // the cap the handler has always had
    static constexpr int   MaxStreamThreads     = 4096;
    static constexpr off_t MinWriteSize         = 1 * MiB;
    static constexpr off_t MaxWriteSize         = 64 * MiB;
    static constexpr off_t MaxBlockSize         = 1 * TiB;
    static constexpr int   MaxRetryAttempts     = 100;
    static constexpr int   MaxRetryWait         = 300;      // seconds

    // http_multi.mode
    Mode mode{Mode::Sequential};

    // http_multi.max_streams <request> [<server>]
    //
    // The most streams one request may have.  A request for more is given
    // this many and is not refused; only a request for more than the cap of
    // 100 is refused, as it always was.
    int max_streams_request{MaxStreamsPerRequest};
    // The most stream threads the server runs at any time, over all the pulls
    // in blocks mode.  Each thread holds one write buffer, so this also
    // bounds the memory of the mode: max_stream_threads * write_size.
    int max_stream_threads{128};

    // http_multi.block_size <min> [<max>]
    //
    // A block is as large as file size / streams, kept within these bounds.
    // The minimum stops a small file from being cut into many short requests.
    // The maximum bounds how far the file is written out of order, which is
    // what the real-time checksum of the storage has to keep track of and
    // what is discarded when a pull fails.  On a very fast path a stream
    // finishes a block in seconds and the pause between two blocks weighs
    // more: raise the maximum there (10 GiB and more).
    off_t block_min{256 * MiB};
    off_t block_max{4 * GiB};

    // http_multi.write_size
    //
    // The size of each write to the file.  Blocks are whole numbers of
    // writes, so every write but the last of the file starts at a multiple of
    // this size.
    off_t write_size{16 * MiB};

    // http_multi.retry <attempts> [<wait>]
    //
    // The tries a block gets without making progress, and the wait in
    // seconds before the second one.  One attempt means no retry.
    // Read and checked; not used yet: a block that fails still fails the pull.
    int retry_attempts{3};
    int retry_wait{2};

    /**
     * Checks the settings against the limits and against each other, and
     * rounds the block bounds up to a whole number of writes.  To be called
     * once, after the configuration file has been read.
     *
     * @param error  set to what is wrong, in words for the log, on failure
     * @return true if the settings can be used
     */
    bool Validate(std::string &error);
};

/**
 * How one pull is cut into blocks.
 */
struct BlockPlan {
    off_t  block_size{0};  // size of every block but the last
    size_t blocks{0};      // number of blocks in the file
    size_t workers{0};     // streams that have a block to fetch: min(streams, blocks)
};

/**
 * Plans the blocks of a pull.
 *
 * The aim is one block per stream, because a stream that fetches a single
 * block pays no pause between requests.  The block is then kept within the
 * configured bounds and rounded up to a whole number of writes.
 *
 * @param file_size  size of the source file in bytes
 * @param streams    streams granted to the pull
 * @param options    validated settings
 * @return the plan; all zero when the file is empty or no stream was granted.
 *         With fewer than two workers the caller pulls the file with a single
 *         request instead.
 */
BlockPlan PlanBlocks(off_t file_size, size_t streams, const PullOptions &options);

}

#endif // __XRD_TPC_PULL_OPTIONS_HH__
