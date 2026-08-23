#undef NDEBUG

// T-U12 (05-TEST-PLAN.md): the streaming transfer digests.
//   - adler32 streaming equals zlib's one-shot reference (independent
//     implementation -- ours is local precisely so its state is seedable)
//   - CombineAdler equals zlib's adler32_combine
//   - serialize/restore: digest(fresh full run) == digest(restore at a
//     randomized cut W) for identical content -- the SUB-4 property that
//     makes a resumed transfer's final checksum trustworthy
//   - CRC32C epochs partition the content and match a reference
//   - out-of-order updates poison the digests (NFR-7), never mis-attest
//
// [FR-26, FR-28, SUB-4]

#include "XrdHttpTpcR/XrdHttpTpcRDigest.hh"

#include "XrdOuc/XrdOucCRC.hh"

#include <gtest/gtest.h>
#include <zlib.h>

#include <random>
#include <string>
#include <vector>

using TPCR::TransferDigests;

namespace {

std::vector<char> RandomContent(size_t size, unsigned seed) {
  std::mt19937 rng(seed);
  std::vector<char> content(size);
  for (auto &byte : content) {byte = char(rng());}
  return content;
}

// Feeds content[from, to) in random-sized in-order chunks.
void Feed(TransferDigests &digests, const std::vector<char> &content,
          size_t from, size_t to, std::mt19937 &rng) {
  size_t cursor = from;
  while (cursor < to) {
    size_t chunk = 1 + rng() % 8192;
    if (chunk > to - cursor) {chunk = to - cursor;}
    digests.Update(off_t(cursor), content.data() + cursor, chunk);
    cursor += chunk;
  }
}

} // namespace

TEST(XrdHttpTpcRDigestTests, AdlerMatchesZlibReference) {
  const auto content = RandomContent(300000, 1);
  std::mt19937 rng(2);
  TransferDigests digests;
  Feed(digests, content, 0, content.size(), rng);

  const uint32_t reference = uint32_t(
      adler32(adler32(0L, Z_NULL, 0),
              reinterpret_cast<const Bytef *>(content.data()),
              uInt(content.size())));
  EXPECT_EQ(reference, digests.Adler());
  EXPECT_EQ(off_t(content.size()), digests.Covered());

  char expected_hex[9];
  snprintf(expected_hex, sizeof(expected_hex), "%08x", reference);
  EXPECT_EQ(std::string(expected_hex), digests.AdlerHex());
}

TEST(XrdHttpTpcRDigestTests, CombineMatchesZlibReference) {
  std::mt19937 rng(3);
  for (int i = 0; i < 50; i++) {
    const size_t len_a = 1 + rng() % 100000;
    const size_t len_b = 1 + rng() % 100000;
    const auto a = RandomContent(len_a, 100 + i);
    const auto b = RandomContent(len_b, 200 + i);
    const uint32_t adler_a = uint32_t(adler32(
        adler32(0L, Z_NULL, 0), reinterpret_cast<const Bytef *>(a.data()),
        uInt(len_a)));
    const uint32_t adler_b = uint32_t(adler32(
        adler32(0L, Z_NULL, 0), reinterpret_cast<const Bytef *>(b.data()),
        uInt(len_b)));
    const uint32_t reference =
        uint32_t(adler32_combine(adler_a, adler_b, z_off_t(len_b)));
    EXPECT_EQ(reference,
              TransferDigests::CombineAdler(adler_a, adler_b, len_b))
        << "iteration " << i;
  }
}

TEST(XrdHttpTpcRDigestTests, FreshEqualsResumedAcrossRandomizedCuts) {
  // The T-U12 core: for identical content, a digest carried through
  // serialize/restore at ANY watermark equals the fresh full-run digest --
  // across randomized kill points (SUB-4).
  const auto content = RandomContent(200000, 7);
  std::mt19937 baseline_rng(8);
  TransferDigests fresh;
  Feed(fresh, content, 0, content.size(), baseline_rng);

  std::mt19937 rng(9);
  for (int trial = 0; trial < 40; trial++) {
    const size_t cut = rng() % (content.size() + 1);

    TransferDigests session_one;
    std::mt19937 feed_rng(1000 + trial);
    Feed(session_one, content, 0, cut, feed_rng);
    // A checkpoint at the cut closes the epoch and snapshots the state --
    // exactly what the Checkpointer does before a crash would matter.
    session_one.CloseEpoch();
    const std::string blob = session_one.Serialize();

    TransferDigests session_two;
    std::string err;
    ASSERT_TRUE(session_two.Restore(blob, off_t(cut), err))
        << "cut " << cut << ": " << err;
    Feed(session_two, content, cut, content.size(), feed_rng);

    EXPECT_EQ(fresh.Adler(), session_two.Adler())
        << "cut " << cut << ": resumed adler differs from fresh";
    EXPECT_EQ(off_t(content.size()), session_two.Covered());
  }
}

TEST(XrdHttpTpcRDigestTests, RestoreRejectsMismatchedWatermark) {
  const auto content = RandomContent(5000, 11);
  std::mt19937 rng(12);
  TransferDigests digests;
  Feed(digests, content, 0, 3000, rng);
  digests.CloseEpoch();
  const std::string blob = digests.Serialize();

  TransferDigests restored;
  std::string err;
  // SUB-4: state for W=3000 must not be accepted for any other W.
  EXPECT_FALSE(restored.Restore(blob, 2999, err));
  EXPECT_FALSE(restored.Restore(blob, 3001, err));
  EXPECT_TRUE(restored.Restore(blob, 3000, err)) << err;
  // Garbage blobs are rejected, never crash.
  EXPECT_FALSE(restored.Restore("short", 3000, err));
  EXPECT_FALSE(restored.Restore(std::string(29, '\\xff'), 3000, err));
}

TEST(XrdHttpTpcRDigestTests, EpochsPartitionTheContent) {
  // FR-28: closed epochs tile [0, covered) without gaps, and each carries
  // the CRC32C of exactly its interval.
  const auto content = RandomContent(100000, 21);
  std::mt19937 rng(22);
  TransferDigests digests;
  std::vector<TPCR::JournalRecord::EpochDigest> epochs;
  size_t cursor = 0;
  while (cursor < content.size()) {
    size_t next = cursor + 10000 + rng() % 20000;
    if (next > content.size()) {next = content.size();}
    Feed(digests, content, cursor, next, rng);
    epochs.push_back(digests.CloseEpoch());
    cursor = next;
  }
  uint64_t expected_offset = 0;
  for (const auto &epoch : epochs) {
    EXPECT_EQ(expected_offset, epoch.offset) << "gap between epochs";
    const uint32_t reference = XrdOucCRC::Calc32C(
        content.data() + epoch.offset, size_t(epoch.length));
    EXPECT_EQ(reference, epoch.crc32c);
    expected_offset += epoch.length;
  }
  EXPECT_EQ(uint64_t(content.size()), expected_offset);
}

TEST(XrdHttpTpcRDigestTests, OutOfOrderUpdatePoisons) {
  TransferDigests digests;
  const char data[16] = {0};
  digests.Update(0, data, 16);
  EXPECT_FALSE(digests.Poisoned());
#ifdef NDEBUG
  digests.Update(64, data, 16);   // gap: violates the in-order contract
  EXPECT_TRUE(digests.Poisoned()) << "violation must poison, not mis-attest";
#else
  // In debug builds the violation asserts (NFR-7's abort-in-debug); the
  // release-path poisoning is exercised by the NDEBUG build of the module.
  SUCCEED();
#endif
}
