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

#ifndef __XRD_TPCR_JOURNAL_HH__
#define __XRD_TPCR_JOURNAL_HH__

// The TPCR journal (WP-7; 02-ARCHITECTURE §3): a small, versioned,
// self-describing record persisted as a sidecar next to the partial
// destination file, through the same SFS/OSS stack as the data (FR-18).
//
// The journal is the ONLY trusted resume state: its fsync-backed committed
// watermark W describes the valid prefix of the partial; the partial's own
// length never does (FR-25 -- failed ranges leave holes, BUG-12).
//
// Update protocol (SUB-1 + XRD-5, verified by the WP-6 backend matrix):
//     sync() the DATA file  ->  write <journal>.tmp  ->  sync  ->  close
//     ->  atomic rename over the journal.
// That ordering is the entire durability argument: journal W <= durable
// data bytes at every instant.  A crash between the two steps merely
// resumes from the previous W.
//
// Parsing is defensive (SUB-9): the journal lives in user-writable
// namespace, so it is attacker-adjacent input.  Bounded field sizes, a
// version check, and a whole-record CRC32C; any anomaly rejects the record
// (RESUME_REJECTED at the caller), never crashes, and never lets a forged W
// make the server skip bytes it never wrote (tail verification, FR-28,
// plus the validator ladder are the further defenses).

#include "XrdHttpTpcRScheduler.hh"   // SourceValidators

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <sys/types.h>

class XrdSfsFileSystem;
class XrdSecEntity;
class XrdSysError;

namespace TPCR {

class Stream;

// One journal record: everything a future session needs to resume at W.
struct JournalRecord {
    static const uint32_t kFormatVersion = 1;

    // Bounds for the defensive parser (SUB-9).  A record violating any of
    // them is rejected outright.
    static const size_t kMaxUrl = 4096;
    static const size_t kMaxETag = 1024;
    static const size_t kMaxLastModified = 256;
    static const size_t kMaxDigestState = 64 * 1024;
    static const size_t kMaxDigestName = 64;
    static const size_t kMaxDigestValue = 1024;
    static const size_t kMaxReprDigests = 16;
    // ~2,500 epochs cover 10 TB at the default 4 GiB cadence (NFR-5), and
    // the Checkpointer prunes epochs below the verify-tail (WP-14/C4), so
    // real records stay far below these.  The caps must satisfy
    // kMaxEpochs * 20 (serialized epoch size) < kMaxRecordBytes, or a
    // record could serialize/commit and then be rejected on the next load
    // -- exactly the trap this pair once contained.
    static const size_t kMaxEpochs = 32768;          // 32768*20 = 640 KiB
    static const size_t kMaxRecordBytes = 1 * 1024 * 1024;

    // --- Core resume state ---
    off_t committed = 0;                 // W: bytes durably in order
    std::string source_url;              // normalized (query stripped)
    SourceValidators validators;         // captured at session-start HEAD

    // --- Lease (FR-22, SUB-8): advisory mutual exclusion between
    //     cooperating TPCR gateways on a shared filesystem. ---
    std::array<unsigned char, 16> lease_owner{};
    int64_t lease_expiry = 0;            // unix seconds

    // --- Integrity (WP-10 fills these; format carries them now so the
    //     record layout never changes underneath a deployed fleet). ---
    std::string digest_state;            // opaque serialized digest state
    struct EpochDigest {
        uint64_t offset;
        uint64_t length;
        uint32_t crc32c;
    };
    std::vector<EpochDigest> epochs;     // per-checkpoint CRC32C (FR-28)

    // --- Informational (resume does NOT require these to match; only W
    //     matters -- 02 §3). ---
    uint64_t block_size = 0;
    uint32_t streams = 0;
    uint32_t attempts = 0;
    int64_t created = 0;
    int64_t updated = 0;

    // Serializes to the on-disk format (fixed header, length-prefixed
    // fields, trailing CRC32C).  Fails only if a field exceeds its bound.
    bool Serialize(std::string &out, std::string &err) const;

    // Defensive parse (SUB-9).  Returns false with a reason on ANY anomaly:
    // short buffer, bad magic/version/CRC, oversized or truncated field,
    // internally inconsistent values (e.g. W < 0, W > content length).
    static bool Parse(const void *data, size_t size, JournalRecord &out,
                      std::string &err);

    // --- Lease helpers (FR-22) ---
    bool LeaseLive(time_t now) const {
        return lease_expiry > static_cast<int64_t>(now);
    }
    bool LeaseOwnedBy(const std::array<unsigned char, 16> &owner) const {
        return lease_owner == owner;
    }
    static std::array<unsigned char, 16> NewLeaseOwner();

    // Age (secs since last update) for the lazy GC decision (FR-24).
    int64_t Age(time_t now) const {return static_cast<int64_t>(now) - updated;}
};

// Strips the query/opaque part of a source URL: tokens in the query change
// between sessions and must not defeat the source-identity comparison.
std::string NormalizeSourceUrl(const std::string &url);

// SFS-backed journal file: load / atomic-commit / remove of the sidecar
// `<dest><suffix>` next to the destination (FR-18).  All I/O goes through
// the same SFS the data file uses, so shared-filesystem deployments see the
// journal wherever they see the data (CON-3).
class JournalStore {
public:
    JournalStore(XrdSfsFileSystem *sfs, const std::string &dest_path,
                 const std::string &suffix, const XrdSecEntity *client);

    const std::string &JournalPath() const {return m_journal_path;}

    // True if a sidecar exists (readable via stat).
    bool Exists();

    // Loads and defensively parses the journal.  Returns false with the
    // reason (absent, unreadable, or invalid per JournalRecord::Parse) --
    // callers map that to RESUME_REJECTED / fresh transfer.
    bool Load(JournalRecord &out, std::string &err);

    // Atomically replaces the journal: temp -> write -> sync -> close ->
    // rename (XRD-5 ordering).  The caller must have already sync()ed the
    // DATA file (SUB-1) -- Checkpointer enforces that.
    bool Commit(const JournalRecord &record, std::string &err);

    // Removes the journal (and any stale temp).  Success-path order is
    // sync -> close -> [checksum inject, WP-11] -> Remove -> verdict
    // (FR-23; Remove deliberately LAST among the destructive steps, so a
    // close failure after a provable sync still leaves a resumable journal
    // instead of forcing a from-zero retry -- WP-14/H7).
    bool Remove(std::string &err);

private:
    XrdSfsFileSystem *m_sfs;
    const XrdSecEntity *m_client;
    std::string m_journal_path;
    std::string m_temp_path;
};

// The checkpoint engine (FR-19): decides WHEN to checkpoint (every
// checkpoint_bytes of commit advance or checkpoint_secs, whichever first)
// and executes the SUB-1 ordering (data sync, then journal commit).  A
// failed data sync POISONS the engine permanently (WP-14/C1): on Linux the
// first fsync after a writeback error consumes it and drops the dirty
// pages, so a LATER sync on the same handle can return success without the
// lost bytes ever reaching disk -- retry-and-continue would let W advance
// over a hole and ultimately attest a false success.  Once poisoned, no
// checkpoint, final checkpoint, or success sync will ever run again; the
// caller must fail the transfer.  The last successfully persisted W stays
// trustworthy (every byte below it was proven durable by a sync that
// preceded its commit) and is the correct resumable-from.
class Checkpointer {
public:
    // digest_snapshot (WP-10) mutates the record under the same commit that
    // persists W -- closing the open CRC32C epoch into record.epochs and
    // refreshing record.digest_state -- so digest state and watermark are
    // checkpoint-atomic (SUB-4).  Null = no digests (never in production).
    //
    // prune_tailbytes (WP-14/C4): epochs wholly below W - prune_tailbytes
    // are dropped at each checkpoint -- resume-time tail verification never
    // reads them (it skips epochs ending at or below W - verify.tailbytes),
    // so carrying them only grows the record toward its parse caps.  Pass
    // the configured tpcr.verify.tailbytes.
    using DigestSnapshot = std::function<void(JournalRecord &)>;
    Checkpointer(JournalStore &store, JournalRecord record,
                 uint64_t checkpoint_bytes, unsigned checkpoint_secs,
                 uint64_t prune_tailbytes,
                 XrdSysError &log,
                 DigestSnapshot digest_snapshot = nullptr);

    // Called from the transfer loop after every commit advance.  Performs a
    // checkpoint when due (FR-19); returns true if one was taken.
    bool MaybeCheckpoint(Stream &stream, off_t committed, time_t now);

    // FR-17: unconditional checkpoint on any admitted failure / exit path,
    // taken BEFORE the failure chunk is sent.  Returns true when the journal
    // now durably records `committed` -- the caller may then advertise
    // `resumable-from` in the failure chunk (FR-6).
    bool FinalCheckpoint(Stream &stream, off_t committed);

    // FR-23 success path, first half: the final data sync.  Returns false
    // (with err) if it fails or the engine is poisoned -- the transfer must
    // then be reported as FAILED, since durability of the full content is
    // unproven.  Journal removal is separate (RemoveJournal) and happens
    // only after the close succeeds (WP-14/H7).
    bool FinalDataSync(Stream &stream, std::string &err);

    // FR-23 success path, last half: best-effort journal removal after the
    // data file is synced AND closed.  Failure is non-fatal (lazy GC or the
    // next COPY collects the orphan) but is logged.
    void RemoveJournal();

    // WP-14/C1: true once any data sync has failed.  Permanently sticky;
    // the caller must abort the transfer with a permanent error.
    bool SyncFailed() const {return m_sync_failed;}

    // The last watermark actually persisted to the on-disk journal -- the
    // only value that may be advertised as resumable-from when the current
    // in-memory committed offset can no longer be proven durable.
    off_t PersistedWatermark() const {return m_persisted_committed;}

    // Lease renewal interval = 2 x checkpoint.secs (FR-22, SUB-8).
    int64_t LeaseDuration() const {return 2 * (int64_t)m_checkpoint_secs;}

private:
    // The SUB-1 sequence; shared by periodic and final checkpoints.
    bool Take(Stream &stream, off_t committed, time_t now, const char *why);

    JournalStore &m_store;
    JournalRecord m_record;
    const uint64_t m_checkpoint_bytes;
    const unsigned m_checkpoint_secs;
    const uint64_t m_prune_tailbytes;
    XrdSysError &m_log;
    DigestSnapshot m_digest_snapshot;
    off_t m_last_committed;
    time_t m_last_time;
    off_t m_persisted_committed;
    bool m_sync_failed = false;
};

} // namespace TPCR

#endif // __XRD_TPCR_JOURNAL_HH__
