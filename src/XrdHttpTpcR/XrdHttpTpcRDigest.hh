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

#ifndef __XRD_TPCR_DIGEST_HH__
#define __XRD_TPCR_DIGEST_HH__

// Streaming integrity digests over the committed prefix (WP-10; 02 ARCH §8).
//
// Two digests ride the Stream's in-order commit hook -- the data passes
// through exactly once, in order, by construction:
//
//  * adler32 (primary, FR-26): the WLCG-standard file checksum.  Streaming
//    and trivially resumable (the 32-bit value IS the state), so on
//    completion it yields the whole-file checksum the ecosystem compares --
//    without re-reading terabytes.  Injected into the checksum store at
//    WP-11 (FR-27) and compared against the source Repr-Digest (FR-29).
//
//  * CRC32C epochs (secondary, FR-28): one digest per checkpoint interval
//    [previous W, W), kept in the journal.  Used only internally, for the
//    resume-time tail verification that catches torn writes and lying
//    backends at bounded cost.
//
// SUB-4: the serialized digest state is as correctness-critical as W itself
// -- adler32 over [0, W) cannot be recomputed without re-reading [0, W), so
// it participates in the same checkpoint atomicity (the Checkpointer
// snapshots it under the same journal commit that records W).
//
// The adler32 implementation is the standard zlib-derived algorithm written
// locally (~20 lines) so its state is explicitly seedable; CRC32C uses the
// in-tree XrdOucCRC.  No zlib dependency in the shipped library (the T-U12
// tests compare against zlib as an independent reference).

#include "XrdHttpTpcRJournal.hh"   // JournalRecord::EpochDigest

#include <cstdint>
#include <string>

#include <sys/types.h>

namespace TPCR {

class TransferDigests {
public:
    // Fresh digests covering the empty prefix.
    TransferDigests() = default;

    // Restores state persisted by Serialize().  `expected_covered` is the
    // journal watermark W the state must correspond to (SUB-4); any
    // disagreement -- wrong version, wrong coverage, malformed blob --
    // rejects the resume (the caller maps this to RESUME_REJECTED).
    bool Restore(const std::string &blob, off_t expected_covered,
                 std::string &err);

    // Opaque blob for JournalRecord::digest_state.
    std::string Serialize() const;

    // The Stream commit-hook target: bytes arrive strictly in order and
    // exactly once.  `offset` must equal Covered() (NFR-7: any disagreement
    // poisons the digests, which fails the transfer at the completion gate
    // rather than ever attesting a wrong checksum).
    void Update(off_t offset, const char *data, size_t size);

    // Bytes hashed so far == the stream's committed offset.
    off_t Covered() const {return m_covered;}

    // True if Update ever observed an ordering violation (see above).
    bool Poisoned() const {return m_poisoned;}

    // adler32 of [0, Covered()).
    uint32_t Adler() const {return m_adler;}
    // Lowercase 8-hex-digit rendering (the Repr-Digest / xattr format).
    std::string AdlerHex() const;

    // Closes the current CRC32C epoch [EpochStart(), Covered()) -- called by
    // the checkpoint snapshot -- and starts the next one.
    JournalRecord::EpochDigest CloseEpoch();
    off_t EpochStart() const {return m_epoch_start;}

    // adler32 of the concatenation A||B from adler(A), adler(B), len(B)
    // (the standard combine; exposed for T-U12 and future parallel use).
    static uint32_t CombineAdler(uint32_t adler_a, uint32_t adler_b,
                                 uint64_t len_b);

private:
    off_t m_covered = 0;
    off_t m_epoch_start = 0;
    uint32_t m_adler = 1;        // adler32 of the empty string
    uint32_t m_epoch_crc = 0;    // CRC32C accumulator of the open epoch
    bool m_poisoned = false;
};

} // namespace TPCR

#endif // __XRD_TPCR_DIGEST_HH__
