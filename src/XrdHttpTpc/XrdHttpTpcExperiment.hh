#ifndef __XRD_TPC_EXPERIMENT_HH__
#define __XRD_TPC_EXPERIMENT_HH__

/**
 * EXPERIMENT, NOT FOR MERGING.
 *
 * Switches for the test build of the block-write model of a multistream pull.
 * They are read from the environment of the server, so that one build can be
 * measured in both models:
 *
 *   XRD_TPC_EXP_BLOCK_MIB  unset or 0: the present model (16 MiB ranges, written
 *                          in sequence through the reorder buffers).
 *                          N: each stream requests blocks of N MiB and writes
 *                          them at their own offset.
 *                          auto: one block per stream (file size / streams).
 *   XRD_TPC_EXP_WRITE_MIB  size of each write to the file in the block-write
 *                          model (default 16).
 *   XRD_TPC_EXP_CURL_BUFFER_KIB
 *                          size of the buffer libcurl receives into, and so the
 *                          largest piece it hands over at a time, in KiB.  Unset
 *                          or 0: the default of libcurl (16).  Any pull.
 */

#include <cstdlib>
#include <cstring>
#include <sys/types.h>

namespace TPC {
namespace Experiment {

const off_t MiB = 1024 * 1024;

// Size of each write to the file, in bytes.
inline size_t WriteSize() {
    const char *val = getenv("XRD_TPC_EXP_WRITE_MIB");
    long long mib = (val && *val) ? atoll(val) : 16;
    return static_cast<size_t>((mib > 0 ? mib : 16) * MiB);
}

// Size of the receive buffer of libcurl in bytes; 0 to leave its default.
inline long CurlBufferSize() {
    const char *val = getenv("XRD_TPC_EXP_CURL_BUFFER_KIB");
    long kib = (val && *val) ? atol(val) : 0;
    return kib > 0 ? kib * 1024 : 0;
}

// Whether the block-write model is selected.
inline bool BlockWrites() {
    const char *val = getenv("XRD_TPC_EXP_BLOCK_MIB");
    return val && *val && (!strcmp(val, "auto") || atoll(val) > 0);
}

// Size of a block in bytes, for a file of the given size pulled with the given
// number of streams.  A block is a whole number of writes, so that every block
// starts on a write boundary.
inline off_t BlockSize(off_t file_size, size_t streams) {
    const char *val = getenv("XRD_TPC_EXP_BLOCK_MIB");
    off_t wsize = WriteSize();
    off_t block;
    if (!strcmp(val, "auto")) {
        block = (file_size + streams - 1) / streams;
    } else {
        block = atoll(val) * MiB;
    }
    if (block < wsize) {block = wsize;}
    return ((block + wsize - 1) / wsize) * wsize;
}

}
}

#endif
