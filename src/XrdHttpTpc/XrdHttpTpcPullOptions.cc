#include "XrdHttpTpcPullOptions.hh"

#include <algorithm>
#include <sstream>

using namespace TPC;

namespace {

// The value rounded up to a multiple of the unit; both are positive.
off_t RoundUp(off_t value, off_t unit) {
    return ((value + unit - 1) / unit) * unit;
}

// A size in the unit a person would write in the configuration file.
std::string Readable(off_t bytes) {
    std::stringstream ss;
    if (bytes >= PullOptions::GiB && bytes % PullOptions::GiB == 0) {
        ss << bytes / PullOptions::GiB << " GiB";
    } else if (bytes >= PullOptions::MiB && bytes % PullOptions::MiB == 0) {
        ss << bytes / PullOptions::MiB << " MiB";
    } else {
        ss << bytes << " bytes";
    }
    return ss.str();
}

}

bool PullOptions::Validate(std::string &error)
{
    std::stringstream ss;

    if (max_streams_request < 1 || max_streams_request > MaxStreamsPerRequest) {
        ss << "http_multi.max_streams: the streams of one request must be from 1 to "
           << MaxStreamsPerRequest << ", not " << max_streams_request;
    } else if (max_stream_threads < 2 || max_stream_threads > MaxStreamThreads) {
        // Fewer than two threads cannot carry a pull in blocks mode.
        ss << "http_multi.max_streams: the stream threads of the server must be from 2 to "
           << MaxStreamThreads << ", not " << max_stream_threads;
    } else if (write_size < MinWriteSize || write_size > MaxWriteSize || write_size % MiB != 0) {
        ss << "http_multi.write_size must be a whole number of MiB from " << Readable(MinWriteSize)
           << " to " << Readable(MaxWriteSize) << ", not " << Readable(write_size);
    } else if (block_min < write_size) {
        ss << "http_multi.block_size: the minimum (" << Readable(block_min)
           << ") must be at least the write size (" << Readable(write_size) << ")";
    } else if (block_max < block_min) {
        ss << "http_multi.block_size: the maximum (" << Readable(block_max)
           << ") must be at least the minimum (" << Readable(block_min) << ")";
    } else if (block_max > MaxBlockSize) {
        ss << "http_multi.block_size: the maximum must be at most " << Readable(MaxBlockSize)
           << ", not " << Readable(block_max);
    } else if (retry_attempts < 1 || retry_attempts > MaxRetryAttempts) {
        ss << "http_multi.retry: the attempts must be from 1 to " << MaxRetryAttempts
           << ", not " << retry_attempts;
    } else if (retry_wait < 0 || retry_wait > MaxRetryWait) {
        ss << "http_multi.retry: the wait must be from 0 to " << MaxRetryWait
           << " seconds, not " << retry_wait;
    }

    error = ss.str();
    if (!error.empty()) {return false;}

    // A block is a whole number of writes.  The maximum was at least the
    // minimum and stays so, both being rounded up to the same unit.  Rounding
    // can take it past the limit when the write size does not divide the
    // limit; it is then the largest whole number of writes within the limit.
    block_min = RoundUp(block_min, write_size);
    block_max = RoundUp(block_max, write_size);
    if (block_max > MaxBlockSize) {block_max = (MaxBlockSize / write_size) * write_size;}
    return true;
}

BlockPlan TPC::PlanBlocks(off_t file_size, size_t streams, const PullOptions &options)
{
    BlockPlan plan;
    if (file_size <= 0 || streams == 0) {return plan;}

    // One block per stream ...
    const off_t count = static_cast<off_t>(streams);
    off_t block = file_size / count + (file_size % count ? 1 : 0);
    // ... within the bounds, which Validate() made whole numbers of writes ...
    block = std::min(std::max(block, options.block_min), options.block_max);
    // ... and itself a whole number of writes.  A block between the bounds
    // that is rounded up stays at or below the maximum.
    block = RoundUp(block, options.write_size);

    plan.block_size = block;
    plan.blocks = static_cast<size_t>(file_size / block + (file_size % block ? 1 : 0));
    plan.workers = std::min(streams, plan.blocks);
    return plan;
}
