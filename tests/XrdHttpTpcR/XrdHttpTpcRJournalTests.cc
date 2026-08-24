#undef NDEBUG

// T-U11 (05-TEST-PLAN.md): the journal record.
//   - serialize/parse round-trip over every field
//   - malformed-journal corpus: truncations, bad CRC, absurd W, oversize
//     fields, bad version -- rejected, never a crash (fuzz-lite, >= 10k
//     mutated buffers; SUB-9: the journal is attacker-adjacent input)
//   - version skew rejection
//   - lease helpers: live/expired/steal identity (FR-22, SUB-8)
//   - source URL normalization (tokens in the query must not defeat the
//     source-identity comparison)
//
// The atomic-update sequence itself (temp+sync+rename under kill -9) is
// covered by the WP-6 backend matrix and the integration crash scenario.

#include "XrdHttpTpcR/XrdHttpTpcRJournal.hh"

#include "XrdOuc/XrdOucCRC.hh"

#include <gtest/gtest.h>

#include <random>
#include <string>

using TPCR::JournalRecord;
using TPCR::NormalizeSourceUrl;

namespace {

JournalRecord SampleRecord() {
  JournalRecord record;
  record.committed = 7 * 1024 * 1024;
  record.source_url = "https://source.example.org:8443/store/file.root";
  record.validators.content_length = 40 * 1024 * 1024;
  record.validators.etag = "\"abc-123\"";
  record.validators.last_modified = "Wed, 01 Jan 2025 00:00:00 GMT";
  record.validators.repr_digests["adler"] = ":c4Ki0g==:";
  record.validators.repr_digests["sha-256"] = ":base64base64:";
  record.lease_owner = JournalRecord::NewLeaseOwner();
  record.lease_expiry = 1900000000;
  record.digest_state = std::string("\x01\x02\x00\x03opaque", 9);
  record.epochs.push_back({0, 4096, 0xdeadbeef});
  record.epochs.push_back({4096, 8192, 0x12345678});
  record.block_size = 16 * 1024 * 1024;
  record.streams = 4;
  record.attempts = 2;
  record.created = 1800000000;
  record.updated = 1800000600;
  return record;
}

} // namespace

TEST(XrdHttpTpcRJournalTests, RoundTripPreservesEveryField) {
  const JournalRecord original = SampleRecord();
  std::string raw, err;
  ASSERT_TRUE(original.Serialize(raw, err)) << err;

  JournalRecord parsed;
  ASSERT_TRUE(JournalRecord::Parse(raw.data(), raw.size(), parsed, err)) << err;

  EXPECT_EQ(original.committed, parsed.committed);
  EXPECT_EQ(original.source_url, parsed.source_url);
  EXPECT_EQ(original.validators.content_length, parsed.validators.content_length);
  EXPECT_EQ(original.validators.etag, parsed.validators.etag);
  EXPECT_EQ(original.validators.last_modified, parsed.validators.last_modified);
  EXPECT_EQ(original.validators.repr_digests, parsed.validators.repr_digests);
  EXPECT_EQ(original.lease_owner, parsed.lease_owner);
  EXPECT_EQ(original.lease_expiry, parsed.lease_expiry);
  EXPECT_EQ(original.digest_state, parsed.digest_state);
  ASSERT_EQ(original.epochs.size(), parsed.epochs.size());
  for (size_t i = 0; i < original.epochs.size(); i++) {
    EXPECT_EQ(original.epochs[i].offset, parsed.epochs[i].offset);
    EXPECT_EQ(original.epochs[i].length, parsed.epochs[i].length);
    EXPECT_EQ(original.epochs[i].crc32c, parsed.epochs[i].crc32c);
  }
  EXPECT_EQ(original.block_size, parsed.block_size);
  EXPECT_EQ(original.streams, parsed.streams);
  EXPECT_EQ(original.attempts, parsed.attempts);
  EXPECT_EQ(original.created, parsed.created);
  EXPECT_EQ(original.updated, parsed.updated);
}

TEST(XrdHttpTpcRJournalTests, TruncationsAreRejectedAtEveryLength) {
  std::string raw, err;
  ASSERT_TRUE(SampleRecord().Serialize(raw, err));
  JournalRecord parsed;
  for (size_t length = 0; length < raw.size(); length++) {
    EXPECT_FALSE(JournalRecord::Parse(raw.data(), length, parsed, err))
        << "truncation to " << length << " bytes was accepted";
  }
}

TEST(XrdHttpTpcRJournalTests, SingleBitFlipsAreRejected) {
  // Any single-bit corruption must trip the record CRC (SUB-9).
  std::string raw, err;
  ASSERT_TRUE(SampleRecord().Serialize(raw, err));
  JournalRecord parsed;
  std::mt19937 rng(99);
  std::uniform_int_distribution<size_t> pick_byte(0, raw.size() - 1);
  std::uniform_int_distribution<int> pick_bit(0, 7);
  for (int i = 0; i < 2000; i++) {
    std::string mutated = raw;
    mutated[pick_byte(rng)] ^= char(1 << pick_bit(rng));
    EXPECT_FALSE(JournalRecord::Parse(mutated.data(), mutated.size(), parsed, err))
        << "bit-flipped record accepted at iteration " << i;
  }
}

TEST(XrdHttpTpcRJournalTests, FuzzedBuffersNeverCrash) {
  // Fuzz-lite (>= 10k iterations): random slices, splices, and garbage.
  // The only requirement is no crash and no acceptance of junk; the loop
  // itself passing IS the assertion of "never crash".
  std::string raw, err;
  ASSERT_TRUE(SampleRecord().Serialize(raw, err));
  std::mt19937 rng(1234);
  JournalRecord parsed;
  int accepted = 0;
  for (int i = 0; i < 12000; i++) {
    std::string mutated = raw;
    const int mode = int(rng() % 4);
    if (mode == 0 && !mutated.empty()) {          // random corruption burst
      size_t start = rng() % mutated.size();
      size_t count = 1 + rng() % 64;
      for (size_t j = start; j < std::min(mutated.size(), start + count); j++) {
        mutated[j] = char(rng());
      }
    } else if (mode == 1) {                       // random truncation
      mutated.resize(rng() % (mutated.size() + 1));
    } else if (mode == 2) {                       // random growth
      size_t extra = 1 + rng() % 256;
      for (size_t j = 0; j < extra; j++) {mutated.push_back(char(rng()));}
    } else {                                      // pure garbage
      mutated.resize(16 + rng() % 512);
      for (auto &byte : mutated) {byte = char(rng());}
    }
    if (mutated == raw) {continue;}  // the mutation happened to be a no-op
    if (JournalRecord::Parse(mutated.data(), mutated.size(), parsed, err)) {
      accepted++;
    }
  }
  // Random mutation can only be accepted by colliding a CRC32C -- with 12k
  // trials the expected count is ~0.  Anything more means the parser is
  // not actually validating.
  EXPECT_LE(accepted, 1) << "fuzzer produced accepted records";
}

TEST(XrdHttpTpcRJournalTests, VersionSkewIsRejected) {
  // A future format version must be rejected for its VERSION, not merely
  // because the CRC no longer matches: patch the version field (offset 8,
  // little-endian) and recompute a valid trailing CRC.
  std::string raw, err;
  ASSERT_TRUE(SampleRecord().Serialize(raw, err));
  raw[8] = char(JournalRecord::kFormatVersion + 1);
  const uint32_t crc =
      XrdOucCRC::Calc32C(raw.data(), raw.size() - 4);
  for (int i = 0; i < 4; i++) {
    raw[raw.size() - 4 + i] = char((crc >> (8 * i)) & 0xff);
  }
  JournalRecord parsed;
  EXPECT_FALSE(JournalRecord::Parse(raw.data(), raw.size(), parsed, err));
  EXPECT_NE(std::string::npos, err.find("version"))
      << "rejection reason should name the version, got: " << err;
}

TEST(XrdHttpTpcRJournalTests, OversizeFieldsRefuseToSerialize) {
  JournalRecord record = SampleRecord();
  record.source_url.assign(JournalRecord::kMaxUrl + 1, 'x');
  std::string raw, err;
  EXPECT_FALSE(record.Serialize(raw, err));
  EXPECT_NE(std::string::npos, err.find("bound"));
}

TEST(XrdHttpTpcRJournalTests, AbsurdWatermarkIsRejected) {
  // W beyond the content length can never be legitimate (FR-25 corollary);
  // a forged journal must not be able to claim bytes that cannot exist.
  JournalRecord record = SampleRecord();
  record.committed = record.validators.content_length + 1;
  std::string raw, err;
  ASSERT_TRUE(record.Serialize(raw, err));
  JournalRecord parsed;
  EXPECT_FALSE(JournalRecord::Parse(raw.data(), raw.size(), parsed, err));
  EXPECT_NE(std::string::npos, err.find("watermark"));
}

TEST(XrdHttpTpcRJournalTests, LeaseLifecycle) {
  // FR-22 / SUB-8: live lease excludes others; expired lease is stealable
  // by rewriting owner+expiry (the atomic rewrite is JournalStore::Commit).
  JournalRecord record = SampleRecord();
  const time_t now = 1900000000 - 100;
  EXPECT_TRUE(record.LeaseLive(now)) << "expiry is in the future";
  EXPECT_FALSE(record.LeaseLive(1900000000 + 1)) << "expiry passed";

  const auto me = JournalRecord::NewLeaseOwner();
  const auto other = JournalRecord::NewLeaseOwner();
  EXPECT_NE(me, other) << "lease owner ids must be distinct";
  record.lease_owner = me;
  EXPECT_TRUE(record.LeaseOwnedBy(me));
  EXPECT_FALSE(record.LeaseOwnedBy(other));

  // Steal: new owner, fresh expiry (SUB-8: only after expiry, and the
  // journal rewrite must happen before any data write -- XRD-6).
  record.lease_owner = other;
  record.lease_expiry = int64_t(now) + 120;
  EXPECT_TRUE(record.LeaseOwnedBy(other));
  EXPECT_TRUE(record.LeaseLive(now));
}

TEST(XrdHttpTpcRJournalTests, SourceUrlNormalizationStripsQuery) {
  EXPECT_EQ("https://host:8443/path/file",
            NormalizeSourceUrl("https://host:8443/path/file?authz=SECRET&x=1"));
  EXPECT_EQ("https://host/file", NormalizeSourceUrl("https://host/file"));
  EXPECT_EQ("", NormalizeSourceUrl("?onlyquery"));
}

// ---------------------------------------------------------------------------
// WP-14: Checkpointer-level tests (sync poison, epoch pruning, cap
// consistency).  These use the in-memory SFS so the full JournalStore
// commit/load path runs, not a stub.
// ---------------------------------------------------------------------------

#include "XrdHttpTpcR/XrdHttpTpcRStream.hh"
#include "XrdHttpTpcRMockSfs.hh"
#include "XrdHttpTpcRMockSfsFile.hh"

#include "XrdSys/XrdSysError.hh"
#include "XrdSys/XrdSysLogger.hh"

#include <ctime>
#include <memory>
#include <vector>

TEST(XrdHttpTpcRJournalTests, EpochCapsAreMutuallyConsistent) {
  // WP-14/C4 regression: a record AT the epoch cap must both serialize and
  // re-parse.  The original caps allowed Serialize/Commit to emit a record
  // that the next session's Parse rejected as oversized -- silently
  // forfeiting weeks of resume equity on a slow transfer.
  JournalRecord record = SampleRecord();
  record.epochs.clear();
  uint64_t off = 0;
  for (size_t i = 0; i < JournalRecord::kMaxEpochs; i++) {
    record.epochs.push_back({off, 10, 0x1u});
    off += 10;
  }
  record.committed = off_t(off);
  record.validators.content_length = int64_t(off) + 100;
  std::string raw, err;
  ASSERT_TRUE(record.Serialize(raw, err)) << err;
  EXPECT_LE(raw.size(), size_t(JournalRecord::kMaxRecordBytes));
  JournalRecord parsed;
  EXPECT_TRUE(JournalRecord::Parse(raw.data(), raw.size(), parsed, err))
      << err;
  // One past the cap: refused at WRITE time, never at the next load.
  record.epochs.push_back({off, 10, 0x1u});
  EXPECT_FALSE(record.Serialize(raw, err));
}

TEST(XrdHttpTpcRJournalTests, SyncFailurePoisonsTheCheckpointer) {
  // WP-14/C1 (fsyncgate): after ONE failed data sync, no later "successful"
  // sync may advance the journal or attest anything -- on Linux the first
  // fsync after a writeback error consumes it and drops the lost pages, so
  // the later success proves nothing.
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "JournalTest");
  MemorySfs sfs;
  TPCR::JournalStore store(&sfs, "/data/file.bin", ".xrdtpcr", nullptr);
  JournalRecord record;
  record.source_url = "https://src/file";
  record.committed = 0;
  record.validators.content_length = 1 << 20;
  std::string err;
  ASSERT_TRUE(store.Commit(record, err)) << err;

  auto *data_file = new MemorySfsFile();
  data_file->FailSyncs(1);   // one transient failure, then "success"
  TPCR::Stream stream(std::unique_ptr<XrdSfsFile>(data_file), 0, 4096, log);
  TPCR::Checkpointer ckpt(store, record, /*bytes*/ 1, /*secs*/ 1000000,
                          /*prune tail*/ 1 << 20, log);

  std::vector<char> buf(4096, 'a');
  ASSERT_EQ(stream.Write(0, buf.data(), buf.size(), true),
            ssize_t(buf.size()));

  // First checkpoint: the sync fails -> poisoned, journal untouched.
  EXPECT_FALSE(ckpt.MaybeCheckpoint(stream, 4096, time(NULL)));
  EXPECT_TRUE(ckpt.SyncFailed());
  EXPECT_EQ(ckpt.PersistedWatermark(), 0);

  // The mock now syncs "successfully" -- the poison must hold anyway.
  EXPECT_FALSE(ckpt.MaybeCheckpoint(stream, 4096, time(NULL) + 999999));
  EXPECT_FALSE(ckpt.FinalCheckpoint(stream, 4096));
  std::string sync_err;
  EXPECT_FALSE(ckpt.FinalDataSync(stream, sync_err));
  EXPECT_FALSE(sync_err.empty());
  EXPECT_TRUE(ckpt.SyncFailed());

  // The on-disk journal still records the last PROVABLY durable watermark.
  JournalRecord reloaded;
  ASSERT_TRUE(store.Load(reloaded, err)) << err;
  EXPECT_EQ(reloaded.committed, 0);
}

TEST(XrdHttpTpcRJournalTests, CheckpointerPrunesEpochsBelowTheVerifyTail) {
  // WP-14/C4: epochs wholly below W - verify.tailbytes are never read by
  // resume-time tail verification; carrying them only grows the record
  // toward its parse caps.  The Checkpointer must drop them per commit.
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "JournalTest");
  MemorySfs sfs;
  TPCR::JournalStore store(&sfs, "/data/big.bin", ".xrdtpcr", nullptr);
  JournalRecord record;
  record.source_url = "https://src/big";
  record.committed = 0;
  record.validators.content_length = 1 << 20;
  std::string err;
  ASSERT_TRUE(store.Commit(record, err)) << err;

  TPCR::Stream stream(std::unique_ptr<XrdSfsFile>(new MemorySfsFile()), 0,
                      4096, log);
  off_t epoch_start = 0;
  TPCR::Checkpointer ckpt(
      store, record, /*bytes*/ 1, /*secs*/ 1000000, /*prune tail*/ 150, log,
      [&epoch_start](JournalRecord &snapshot) {
        // Emulate the digest snapshot: one epoch per checkpoint covering
        // [previous W, new W).
        snapshot.epochs.push_back(
            {uint64_t(epoch_start),
             uint64_t(snapshot.committed - epoch_start), 0xabcu});
        epoch_start = snapshot.committed;
      });

  time_t now = time(NULL);
  for (off_t w = 100; w <= 400; w += 100) {
    ASSERT_TRUE(ckpt.MaybeCheckpoint(stream, w, now++));
  }

  // W=400, tail=150 -> floor 250: epochs [0,100) and [100,200) end at or
  // below the floor and must be gone; [200,300) and [300,400) remain and
  // tile the tail contiguously up to exactly W.
  JournalRecord reloaded;
  ASSERT_TRUE(store.Load(reloaded, err)) << err;
  EXPECT_EQ(reloaded.committed, 400);
  ASSERT_EQ(reloaded.epochs.size(), 2u);
  EXPECT_EQ(reloaded.epochs[0].offset, 200u);
  EXPECT_EQ(reloaded.epochs[0].length, 100u);
  EXPECT_EQ(reloaded.epochs[1].offset, 300u);
  EXPECT_EQ(reloaded.epochs[1].length, 100u);
}
