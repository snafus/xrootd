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
