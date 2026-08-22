//------------------------------------------------------------------------------
// This file is part of XrdHttpTpcR: the resumable HTTP-TPC handler.
//
// XRootD is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// XRootD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with XRootD.  If not, see <http://www.gnu.org/licenses/>.
//------------------------------------------------------------------------------

#ifndef __XRD_TPCR_CONFIG_HH__
#define __XRD_TPCR_CONFIG_HH__

#include <cstddef>
#include <cstdint>
#include <string>

namespace TPCR {

// Parsed `tpcr.*` configuration (FR-30; surface defined in 02-ARCHITECTURE
// §12).  Kept separate from the TPCRHandler so the parsing logic is unit
// testable without a server environment (T-U3): TPCRHandler::Configure feeds
// each `tpcr.` directive it encounters through Set().
//
// Directives are added here in the same work package that consumes them, so a
// parsed directive is always an implemented one.  An unknown `tpcr.` directive
// is a configuration error and fails server startup (fail fast, FR-30).
struct Config {
    // --- WP-2 ---
    // Range/block size for all pull transfers (FR-10) and the slab size of
    // the global buffer pool.  Default 16 MiB.
    size_t   block_size  = 16 * 1024 * 1024;      // tpcr.blocksize
    // Server-global byte budget of the slab pool (NFR-1).  Default 4 GiB.
    uint64_t mempool_max = 4ULL * 1024 * 1024 * 1024;  // tpcr.mempool.max
    // Cap on X-Number-Of-Streams.  Values above it are clamped, not rejected
    // (NFR-1, BUG-10).  Default 16.
    size_t   streams_max = 16;                    // tpcr.streams.max

    // Applies the streams cap to a client-requested value (NFR-1/BUG-10):
    // 0 means "default" (1), values above the cap clamp to it.  The caller
    // has already rejected negative/unparseable input.  Sets clamped so the
    // caller can log the adjustment.
    size_t ClampStreams(long requested, bool &clamped) const;

    // Applies one `tpcr.` directive.  `directive` is the full word as it
    // appears in the config file (e.g. "tpcr.blocksize"); `value` the single
    // value token following it.  Returns false and fills `err` on unknown
    // directives or invalid values -- the caller must treat that as a fatal
    // configuration error (FR-30).
    bool Set(const std::string &directive, const std::string &value,
             std::string &err);

    // Value parsers, exposed for reuse and tests.  All reject empty input,
    // trailing junk, and out-of-range values.
    //
    // Sizes accept an optional binary suffix: k/K, m/M, g/G (powers of 1024).
    static bool ParseSize(const std::string &value, uint64_t &out);
    // Durations accept an optional suffix: s (default), m, h.
    static bool ParseDuration(const std::string &value, uint64_t &out_seconds);
    static bool ParseUnsigned(const std::string &value, uint64_t &out);
    static bool ParseBool(const std::string &value, bool &out);
};

} // namespace TPCR

#endif // __XRD_TPCR_CONFIG_HH__
