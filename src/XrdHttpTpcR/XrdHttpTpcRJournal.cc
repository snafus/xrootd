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

#include "XrdHttpTpcRJournal.hh"
#include "XrdHttpTpcRStream.hh"

#include "XrdOuc/XrdOucCRC.hh"
#include "XrdOuc/XrdOucErrInfo.hh"
#include "XrdSfs/XrdSfsInterface.hh"
#include "XrdSys/XrdSysError.hh"

#include <cstring>
#include <memory>
#include <random>
#include <sstream>

using namespace TPCR;

// ---------------------------------------------------------------------------
// On-disk format (little-endian throughout):
//
//   8   magic "XRDTPCR\n"
//   u32 format version
//   u32 flags (reserved, must be 0)
//   u64 committed watermark W
//   i64 content length (source size; -1 if unknown)
//   i64 lease expiry (unix secs)
//   16  lease owner (opaque id)
//   i64 created (unix secs)
//   i64 updated (unix secs)
//   u32 attempts
//   u32 streams (informational)
//   u64 block size (informational)
//   str source_url            (u32 length + bytes; <= kMaxUrl)
//   str etag                  (<= kMaxETag)
//   str last_modified         (<= kMaxLastModified)
//   str digest_state          (<= kMaxDigestState; opaque, WP-10)
//   u32 repr digest count     (<= kMaxReprDigests)
//     { str name (<= kMaxDigestName), str value (<= kMaxDigestValue) } ...
//   u32 epoch count           (<= kMaxEpochs)
//     { u64 offset, u64 length, u32 crc32c } ...
//   u32 CRC32C of everything above
//
// Every length is validated against both its own cap and the remaining
// buffer before it is consumed (SUB-9).
// ---------------------------------------------------------------------------

namespace {

const char kMagic[8] = {'X', 'R', 'D', 'T', 'P', 'C', 'R', '\n'};

void PutU32(std::string &out, uint32_t value) {
    for (int i = 0; i < 4; i++) {out.push_back(char((value >> (8 * i)) & 0xff));}
}
void PutU64(std::string &out, uint64_t value) {
    for (int i = 0; i < 8; i++) {out.push_back(char((value >> (8 * i)) & 0xff));}
}
void PutI64(std::string &out, int64_t value) {PutU64(out, uint64_t(value));}
void PutString(std::string &out, const std::string &value) {
    PutU32(out, uint32_t(value.size()));
    out.append(value);
}

// Bounded little-endian reader over the raw record.
class Reader {
public:
    Reader(const unsigned char *data, size_t size)
        : m_data(data), m_size(size), m_pos(0) {}

    bool Bytes(void *out, size_t count) {
        if (count > m_size - m_pos) {return false;}
        memcpy(out, m_data + m_pos, count);
        m_pos += count;
        return true;
    }
    bool U32(uint32_t &out) {
        unsigned char b[4];
        if (!Bytes(b, 4)) {return false;}
        out = uint32_t(b[0]) | uint32_t(b[1]) << 8 |
              uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
        return true;
    }
    bool U64(uint64_t &out) {
        unsigned char b[8];
        if (!Bytes(b, 8)) {return false;}
        out = 0;
        for (int i = 7; i >= 0; i--) {out = (out << 8) | b[i];}
        return true;
    }
    bool I64(int64_t &out) {
        uint64_t raw;
        if (!U64(raw)) {return false;}
        out = int64_t(raw);
        return true;
    }
    bool String(std::string &out, size_t cap) {
        uint32_t length;
        if (!U32(length)) {return false;}
        if (length > cap || length > m_size - m_pos) {return false;}
        out.assign(reinterpret_cast<const char *>(m_data + m_pos), length);
        m_pos += length;
        return true;
    }
    size_t Position() const {return m_pos;}

private:
    const unsigned char *m_data;
    size_t m_size;
    size_t m_pos;
};

} // namespace

bool JournalRecord::Serialize(std::string &out, std::string &err) const
{
    if (source_url.size() > kMaxUrl || validators.etag.size() > kMaxETag ||
        validators.last_modified.size() > kMaxLastModified ||
        digest_state.size() > kMaxDigestState ||
        validators.repr_digests.size() > kMaxReprDigests ||
        epochs.size() > kMaxEpochs) {
        err = "journal field exceeds its bound";
        return false;
    }
    out.clear();
    out.reserve(512 + source_url.size() + digest_state.size() +
                epochs.size() * 20);
    out.append(kMagic, sizeof(kMagic));
    PutU32(out, kFormatVersion);
    PutU32(out, 0);  // flags
    PutU64(out, uint64_t(committed));
    PutI64(out, int64_t(validators.content_length));
    PutI64(out, lease_expiry);
    out.append(reinterpret_cast<const char *>(lease_owner.data()),
               lease_owner.size());
    PutI64(out, created);
    PutI64(out, updated);
    PutU32(out, attempts);
    PutU32(out, streams);
    PutU64(out, block_size);
    PutString(out, source_url);
    PutString(out, validators.etag);
    PutString(out, validators.last_modified);
    PutString(out, digest_state);
    PutU32(out, uint32_t(validators.repr_digests.size()));
    for (const auto &[name, value] : validators.repr_digests) {
        if (name.size() > kMaxDigestName || value.size() > kMaxDigestValue) {
            err = "repr digest field exceeds its bound";
            return false;
        }
        PutString(out, name);
        PutString(out, value);
    }
    PutU32(out, uint32_t(epochs.size()));
    for (const auto &epoch : epochs) {
        PutU64(out, epoch.offset);
        PutU64(out, epoch.length);
        PutU32(out, epoch.crc32c);
    }
    // Self-CRC (SUB-9) over everything so far.
    PutU32(out, XrdOucCRC::Calc32C(out.data(), out.size()));
    // WP-14/C4: never emit a record the parser would refuse -- committing
    // one would silently forfeit resume at the NEXT session's Load.
    if (out.size() > kMaxRecordBytes) {
        err = "serialized journal exceeds kMaxRecordBytes";
        return false;
    }
    return true;
}

bool JournalRecord::Parse(const void *data, size_t size, JournalRecord &out,
                          std::string &err)
{
    // Never trust anything before the CRC has vouched for the whole record.
    const auto *bytes = static_cast<const unsigned char *>(data);
    if (size < sizeof(kMagic) + 8 || size > kMaxRecordBytes) {
        err = "journal size implausible";
        return false;
    }
    if (memcmp(bytes, kMagic, sizeof(kMagic)) != 0) {
        err = "journal magic mismatch";
        return false;
    }
    {
        Reader crc_reader(bytes + size - 4, 4);
        uint32_t stored = 0;
        crc_reader.U32(stored);
        const uint32_t computed = XrdOucCRC::Calc32C(bytes, size - 4);
        if (stored != computed) {
            err = "journal CRC mismatch";
            return false;
        }
    }

    Reader reader(bytes, size - 4);   // CRC excluded from field parsing
    char magic[8];
    uint32_t version = 0, flags = 0;
    uint64_t committed_raw = 0;
    JournalRecord record;
    if (!reader.Bytes(magic, sizeof(magic)) || !reader.U32(version) ||
        !reader.U32(flags)) {
        err = "journal header truncated";
        return false;
    }
    if (version != kFormatVersion) {
        std::ostringstream reason;
        reason << "journal format version " << version << " not supported";
        err = reason.str();
        return false;
    }
    if (flags != 0) {
        err = "journal flags not recognized";
        return false;
    }
    int64_t content_length = -1;
    if (!reader.U64(committed_raw) || !reader.I64(content_length) ||
        !reader.I64(record.lease_expiry) ||
        !reader.Bytes(record.lease_owner.data(), record.lease_owner.size()) ||
        !reader.I64(record.created) || !reader.I64(record.updated) ||
        !reader.U32(record.attempts) || !reader.U32(record.streams) ||
        !reader.U64(record.block_size)) {
        err = "journal fixed fields truncated";
        return false;
    }
    // Sanity (SUB-9): a forged/absurd W must not survive parsing.
    if (committed_raw > uint64_t(INT64_MAX)) {
        err = "journal watermark implausible";
        return false;
    }
    record.committed = off_t(committed_raw);
    record.validators.content_length = off_t(content_length);
    if (content_length < -1 ||
        (content_length >= 0 && record.committed > content_length)) {
        err = "journal watermark exceeds content length";
        return false;
    }
    if (!reader.String(record.source_url, kMaxUrl) ||
        !reader.String(record.validators.etag, kMaxETag) ||
        !reader.String(record.validators.last_modified, kMaxLastModified) ||
        !reader.String(record.digest_state, kMaxDigestState)) {
        err = "journal string field invalid";
        return false;
    }
    uint32_t digest_count = 0;
    if (!reader.U32(digest_count) || digest_count > kMaxReprDigests) {
        err = "journal digest count invalid";
        return false;
    }
    for (uint32_t i = 0; i < digest_count; i++) {
        std::string name, value;
        if (!reader.String(name, kMaxDigestName) ||
            !reader.String(value, kMaxDigestValue) || name.empty()) {
            err = "journal digest entry invalid";
            return false;
        }
        record.validators.repr_digests[name] = value;
    }
    uint32_t epoch_count = 0;
    if (!reader.U32(epoch_count) || epoch_count > kMaxEpochs) {
        err = "journal epoch count invalid";
        return false;
    }
    record.epochs.reserve(epoch_count);
    for (uint32_t i = 0; i < epoch_count; i++) {
        EpochDigest epoch;
        if (!reader.U64(epoch.offset) || !reader.U64(epoch.length) ||
            !reader.U32(epoch.crc32c)) {
            err = "journal epoch entry truncated";
            return false;
        }
        record.epochs.push_back(epoch);
    }
    if (reader.Position() != size - 4) {
        err = "journal has trailing garbage";
        return false;
    }
    out = std::move(record);
    return true;
}

std::array<unsigned char, 16> JournalRecord::NewLeaseOwner()
{
    std::array<unsigned char, 16> owner;
    std::random_device device;
    for (size_t i = 0; i < owner.size(); i += 4) {
        const uint32_t word = device();
        memcpy(owner.data() + i, &word, 4);
    }
    return owner;
}

std::string TPCR::NormalizeSourceUrl(const std::string &url)
{
    // Authorization tokens ride in the query and change between sessions;
    // the source's identity is scheme://host/path.
    const size_t query = url.find('?');
    return query == std::string::npos ? url : url.substr(0, query);
}

// ---------------------------------------------------------------------------
//                              JournalStore
// ---------------------------------------------------------------------------

JournalStore::JournalStore(XrdSfsFileSystem *sfs, const std::string &dest_path,
                           const std::string &suffix,
                           const XrdSecEntity *client)
    : m_sfs(sfs), m_client(client),
      m_journal_path(dest_path + suffix),
      m_temp_path(dest_path + suffix + ".tmp")
{
}

bool JournalStore::Exists()
{
    struct stat buffer;
    XrdOucErrInfo error;
    return m_sfs->stat(m_journal_path.c_str(), &buffer, error, m_client) ==
           SFS_OK;
}

bool JournalStore::Load(JournalRecord &out, std::string &err)
{
    struct stat buffer;
    XrdOucErrInfo error;
    if (m_sfs->stat(m_journal_path.c_str(), &buffer, error, m_client) != SFS_OK) {
        err = "journal absent";
        return false;
    }
    if (buffer.st_size <= 0 ||
        size_t(buffer.st_size) > JournalRecord::kMaxRecordBytes) {
        err = "journal size implausible";
        return false;
    }
    std::unique_ptr<XrdSfsFile> file(m_sfs->newFile());
    if (!file) {
        err = "journal open: no file object";
        return false;
    }
    if (file->open(m_journal_path.c_str(), SFS_O_RDONLY, 0, m_client, "") !=
        SFS_OK) {
        err = "journal open failed";
        return false;
    }
    std::string raw;
    raw.resize(size_t(buffer.st_size));
    XrdSfsXferSize got =
        file->read(0, raw.data(), XrdSfsXferSize(raw.size()));
    file->close();
    if (got != XrdSfsXferSize(raw.size())) {
        err = "journal short read";
        return false;
    }
    return JournalRecord::Parse(raw.data(), raw.size(), out, err);
}

bool JournalStore::Commit(const JournalRecord &record, std::string &err)
{
    std::string raw;
    if (!record.Serialize(raw, err)) {return false;}

    // XRD-5 sequence: temp -> write -> sync -> close -> rename.  The temp's
    // CONTENTS must be durable before the rename, or some filesystems could
    // expose a torn record under power loss; the missing directory fsync is
    // an accepted bounded risk (worst case: the previous journal reappears,
    // i.e. an older W and some re-fetch -- the safe direction).
    std::unique_ptr<XrdSfsFile> file(m_sfs->newFile());
    if (!file) {
        err = "journal temp open: no file object";
        return false;
    }
    if (file->open(m_temp_path.c_str(),
                   SFS_O_CREAT | SFS_O_TRUNC | SFS_O_WRONLY, 0644, m_client,
                   "") != SFS_OK) {
        err = "journal temp open failed: " +
              std::string(file->error.getErrText());
        return false;
    }
    size_t written = 0;
    while (written < raw.size()) {
        XrdSfsXferSize put = file->write(XrdSfsFileOffset(written),
                                         raw.data() + written,
                                         XrdSfsXferSize(raw.size() - written));
        if (put <= 0) {
            err = "journal temp write failed";
            file->close();
            return false;
        }
        written += size_t(put);
    }
    if (file->sync() != SFS_OK) {
        err = "journal temp sync failed";
        file->close();
        return false;
    }
    if (file->close() != SFS_OK) {
        err = "journal temp close failed";
        return false;
    }
    XrdOucErrInfo error;
    if (m_sfs->rename(m_temp_path.c_str(), m_journal_path.c_str(), error,
                      m_client) != SFS_OK) {
        err = "journal rename failed: " + std::string(error.getErrText());
        return false;
    }
    return true;
}

bool JournalStore::Remove(std::string &err)
{
    XrdOucErrInfo error;
    // Stale temp files are best-effort cleanup; the journal itself must go.
    m_sfs->rem(m_temp_path.c_str(), error, m_client);
    if (m_sfs->rem(m_journal_path.c_str(), error, m_client) != SFS_OK) {
        err = "journal remove failed: " + std::string(error.getErrText());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
//                              Checkpointer
// ---------------------------------------------------------------------------

Checkpointer::Checkpointer(JournalStore &store, JournalRecord record,
                           uint64_t checkpoint_bytes, unsigned checkpoint_secs,
                           uint64_t prune_tailbytes,
                           XrdSysError &log,
                           DigestSnapshot digest_snapshot)
    : m_store(store), m_record(std::move(record)),
      m_checkpoint_bytes(checkpoint_bytes), m_checkpoint_secs(checkpoint_secs),
      m_prune_tailbytes(prune_tailbytes),
      m_log(log), m_digest_snapshot(std::move(digest_snapshot)),
      m_last_committed(m_record.committed), m_last_time(time(NULL)),
      m_persisted_committed(m_record.committed)
{
}

bool Checkpointer::MaybeCheckpoint(Stream &stream, off_t committed, time_t now)
{
    const bool bytes_due =
        committed - m_last_committed >= off_t(m_checkpoint_bytes);
    // The timed trigger only matters when there is commit progress to
    // persist or a lease to renew; renewing on cadence keeps the lease
    // alive through slow stretches (FR-22).
    const bool time_due = now - m_last_time >= time_t(m_checkpoint_secs);
    if (!bytes_due && !time_due) {return false;}
    return Take(stream, committed, now, bytes_due ? "bytes" : "timed");
}

bool Checkpointer::FinalCheckpoint(Stream &stream, off_t committed)
{
    // FR-17: every admitted failure / exit path checkpoints first, so a
    // shared-filesystem retry can resume even though orchestrator cleanup
    // may render it moot (a free option -- 02 §11).
    return Take(stream, committed, time(NULL), "final");
}

bool Checkpointer::FinalDataSync(Stream &stream, std::string &err)
{
    // FR-23 first half: sync -> (caller closes, injects checksum) ->
    // RemoveJournal -> success chunk.  An unprovable final sync means the
    // transfer CANNOT be reported successful (priority 1: correctness).
    if (m_sync_failed) {
        err = "a checkpoint data sync previously failed; durability of the "
              "content cannot be attested (WP-14/C1)";
        return false;
    }
    if (stream.Sync() != SFS_OK) {
        m_sync_failed = true;
        err = "final data sync failed; success cannot be attested";
        return false;
    }
    return true;
}

void Checkpointer::RemoveJournal()
{
    std::string remove_err;
    if (!m_store.Remove(remove_err)) {
        // Not fatal: the orphan journal is caught by lazy GC (FR-24), and
        // a resume attempt would fail validator checks against a complete
        // file anyway.  Log loudly, continue.
        m_log.Emsg("Checkpoint", "journal cleanup failed:", remove_err.c_str());
    }
}

bool Checkpointer::Take(Stream &stream, off_t committed, time_t now,
                        const char *why)
{
    // WP-14/C1: a single failed data sync poisons the engine.  The first
    // fsync after a writeback error consumes the error and the kernel drops
    // the failed pages, so a LATER sync returning success proves nothing --
    // advancing W on it would journal bytes the disk never kept.  The
    // caller observes SyncFailed() and fails the transfer; the on-disk
    // journal keeps the last provably durable watermark.
    if (m_sync_failed) {return false;}
    // SUB-1: the data sync comes FIRST; if it fails the journal must not
    // advance (the old record stays valid: its W is still <= durable data).
    if (stream.Sync() != SFS_OK) {
        m_sync_failed = true;
        m_log.Emsg("Checkpoint",
                   "data sync failed; checkpoint engine poisoned -- the "
                   "transfer must fail (resumable from the last durable "
                   "watermark)");
        return false;
    }
    m_record.committed = committed;
    m_record.updated = int64_t(now);
    m_record.lease_expiry = int64_t(now) + LeaseDuration();
    if (m_digest_snapshot) {
        // SUB-4: the digest state (and the closed epoch) is checkpoint-
        // atomic with W -- same record, same atomic rename.
        m_digest_snapshot(m_record);
    }
    // WP-14/C4: epochs wholly below W - prune_tailbytes are never read
    // again (resume tail verification skips them), so drop them before
    // they grow the record toward its parse caps.  With verify disabled
    // (prune_tailbytes 0) everything below W prunes -- verification will
    // not want any of it.
    if (!m_record.epochs.empty()) {
        const off_t floor =
            committed > off_t(m_prune_tailbytes)
                ? committed - off_t(m_prune_tailbytes) : 0;
        auto keep = m_record.epochs.begin();
        while (keep != m_record.epochs.end() &&
               off_t(keep->offset + keep->length) <= floor) {
            ++keep;
        }
        m_record.epochs.erase(m_record.epochs.begin(), keep);
    }
    std::string err;
    if (!m_store.Commit(m_record, err)) {
        m_log.Emsg("Checkpoint", "journal commit failed:", err.c_str());
        m_last_time = now;
        return false;
    }
    m_persisted_committed = committed;
    m_last_committed = committed;
    m_last_time = now;
    if (m_log.getMsgMask() & 0x01 /* Debug */) {
        std::ostringstream ss;
        ss << "checkpoint (" << why << ") W=" << committed;
        m_log.Emsg("Checkpoint", ss.str().c_str());
    }
    return true;
}
