#undef NDEBUG

// TPCR Stream unit tests.
//
// Ported from the stock XrdHttpTpc Stream tests and extended for the WP-1
// Stream rework (T-U4, T-U5 in 05-TEST-PLAN.md):
//   - the ported scenarios keep their arrival patterns but assert on the new
//     CommittedOffset()/ReorderSpan() surface (the occupancy-based
//     AvailableBuffers() admission signal is gone -- BUG-7 disposition);
//   - T-U4: short and failing backing writes -- Stream::Write must return
//     fully-accepted counts, never a short count with the remainder secretly
//     buffered (BUG-6), and nothing may be written to the file twice;
//   - T-U4: offset-seeded construction (resume groundwork, FR-20) -- writes
//     land at absolute offsets, writes below the seed are rejected;
//   - T-U5: adversarial arrival orders over property-style iterations,
//     including range-boundary straddles and entry-capacity fragmentation
//     (BUG-7, FR-25 groundwork);
//   - WP-1 commit hook: every byte passes the hook exactly once, in order.

#include "XrdHttpTpcR/XrdHttpTpcRStream.hh"
#include "XrdSfs/XrdSfsInterface.hh"
#include "XrdSys/XrdSysError.hh"
#include "XrdSys/XrdSysLogger.hh"

#include <algorithm>
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <numeric>
#include <random>
#include <utility>
#include <vector>
#include <unistd.h>

using namespace testing;

class XrdHttpTpcRStreamTests : public Test {};

// Minimal XrdSfsFile implementation used to observe which writes the Stream
// reordering layer sends to the backing filesystem.
class MemorySfsFile : public XrdSfsFile {
public:
  MemorySfsFile() : XrdSfsFile("test", 0) {}

  int open(const char *, XrdSfsFileOpenMode, mode_t,
           const XrdSecEntity * = 0, const char * = 0) override {
    return SFS_OK;
  }

  int close() override {
    return SFS_OK;
  }

  int fctl(const int, const char *, XrdOucErrInfo &) override {
    return SFS_OK;
  }

  const char *FName() override {
    return "memory";
  }

  int getMmap(void **addr, off_t &size) override {
    *addr = nullptr;
    size = 0;
    return SFS_ERROR;
  }

  XrdSfsXferSize read(XrdSfsFileOffset, XrdSfsXferSize) override {
    return 0;
  }

  XrdSfsXferSize read(XrdSfsFileOffset offset, char *buffer,
                      XrdSfsXferSize size) override {
    if (offset < 0 || static_cast<size_t>(offset) > m_data.size()) {
      return SFS_ERROR;
    }
    size_t available = m_data.size() - static_cast<size_t>(offset);
    size_t to_copy = std::min(static_cast<size_t>(size), available);
    if (!to_copy) {
      return 0;
    }
    memcpy(buffer, &m_data[static_cast<size_t>(offset)], to_copy);
    return static_cast<XrdSfsXferSize>(to_copy);
  }

  int read(XrdSfsAio *) override {
    return SFS_ERROR;
  }

  XrdSfsXferSize write(XrdSfsFileOffset offset, const char *buffer,
                       XrdSfsXferSize size) override {
    if (offset < 0 || m_fail_writes) {
      return SFS_ERROR;
    }
    if (!size) {
      return 0;
    }
    // Emulate a short-writing backend when configured: accepting fewer bytes
    // than offered is legal XrdSfsFile behavior (BUG-6 coverage).
    size_t to_accept = static_cast<size_t>(size);
    if (m_max_write && to_accept > m_max_write) {
      to_accept = m_max_write;
    }
    size_t begin = static_cast<size_t>(offset);
    size_t end = begin + to_accept;
    if (m_data.size() < end) {
      m_data.resize(end);
    }
    memcpy(&m_data[begin], buffer, to_accept);
    m_writes.emplace_back(offset, static_cast<XrdSfsXferSize>(to_accept));
    return static_cast<XrdSfsXferSize>(to_accept);
  }

  int write(XrdSfsAio *) override {
    return SFS_ERROR;
  }

  int stat(struct stat *buf) override {
    memset(buf, 0, sizeof(*buf));
    buf->st_size = m_data.size();
    return SFS_OK;
  }

  int sync() override {
    return SFS_OK;
  }

  int sync(XrdSfsAio *) override {
    return SFS_OK;
  }

  int truncate(XrdSfsFileOffset size) override {
    if (size < 0) {
      return SFS_ERROR;
    }
    m_data.resize(static_cast<size_t>(size));
    return SFS_OK;
  }

  int getCXinfo(char cxtype[4], int &cxrsz) override {
    memset(cxtype, 0, 4);
    cxrsz = 0;
    return SFS_OK;
  }

  const std::vector<char> &Data() const {
    return m_data;
  }

  const std::vector<std::pair<XrdSfsFileOffset, XrdSfsXferSize>> &Writes() const {
    return m_writes;
  }

  // Total bytes the backing file has accepted; if this ever exceeds the file
  // size for gap-free content, something was written twice.
  size_t BytesWritten() const {
    size_t total = 0;
    for (const auto &w : m_writes) {
      total += static_cast<size_t>(w.second);
    }
    return total;
  }

  // Makes every subsequent write() fail, to check error propagation.
  void FailWrites() {
    m_fail_writes = true;
  }

  // Caps the number of bytes a single write() call accepts (0 = no cap).
  void SetMaxWriteSize(size_t max_write) {
    m_max_write = max_write;
  }

  // Pre-populates the file, emulating a partial left by a previous session.
  void Preload(const std::vector<char> &data) {
    m_data = data;
  }

private:
  std::vector<char> m_data;
  std::vector<std::pair<XrdSfsFileOffset, XrdSfsXferSize>> m_writes;
  bool m_fail_writes = false;
  size_t m_max_write = 0;
};

namespace {

std::vector<char> AsBytes(const std::string &str) {
  return std::vector<char>(str.begin(), str.end());
}

} // namespace

// ---------------------------------------------------------------------------
// Ported scenarios (arrival patterns unchanged; assertions moved to the
// CommittedOffset()/ReorderSpan() surface).
// ---------------------------------------------------------------------------

TEST_F(XrdHttpTpcRStreamTests, FlushesExactlyFullEmptyBuffer) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  auto file = std::make_unique<MemorySfsFile>();
  auto raw_file = file.get();
  TPCR::Stream stream(std::move(file), 0, 8, log);

  // Reproduce the case where a single callback exactly fills an empty reorder
  // buffer.  The buffer is contiguous with the stream offset, so it must be
  // written immediately, leaving nothing buffered.
  ASSERT_EQ(8, stream.Write(0, "abcdefgh", 8, false));
  EXPECT_EQ(8, stream.CommittedOffset());
  EXPECT_EQ(0u, stream.ReorderSpan());
  ASSERT_EQ(1u, raw_file->Writes().size());
  EXPECT_EQ(0, raw_file->Writes()[0].first);
  EXPECT_EQ(8, raw_file->Writes()[0].second);
  EXPECT_EQ(AsBytes("abcdefgh"), raw_file->Data());
}

TEST_F(XrdHttpTpcRStreamTests, FlushesExactlyFullCurrentBuffer) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  auto file = std::make_unique<MemorySfsFile>();
  auto raw_file = file.get();
  TPCR::Stream stream(std::move(file), 0, 8, log);

  // Reproduce the multi-stream pull stall from xrootd/xrootd#2108 in miniature:
  // a first callback partially fills a reorder buffer, then a later callback
  // exactly completes it.  Exact fills must be flushed immediately, without
  // requiring another callback to enter Stream::Write().
  ASSERT_EQ(4, stream.Write(0, "abcd", 4, false));
  EXPECT_EQ(0, stream.CommittedOffset());
  EXPECT_EQ(4u, stream.ReorderSpan());
  ASSERT_TRUE(raw_file->Writes().empty());

  ASSERT_EQ(4, stream.Write(4, "efgh", 4, false));
  EXPECT_EQ(8, stream.CommittedOffset());
  EXPECT_EQ(0u, stream.ReorderSpan());
  ASSERT_EQ(1u, raw_file->Writes().size());
  EXPECT_EQ(0, raw_file->Writes()[0].first);
  EXPECT_EQ(8, raw_file->Writes()[0].second);
  EXPECT_EQ(AsBytes("abcdefgh"), raw_file->Data());
}

TEST_F(XrdHttpTpcRStreamTests, FlushesCascadeAfterOutOfOrderArrival) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  auto file = std::make_unique<MemorySfsFile>();
  auto raw_file = file.get();
  TPCR::Stream stream(std::move(file), 0, 8, log);

  // Four ranges arrive in reverse-ish order; none of them can be written until
  // the one starting at offset 0 shows up.  Writing that one advances the stream
  // offset and must in turn unblock the buffer holding offset 8, then 16, then
  // 24 -- a single flush pass over the buffers is not enough.
  ASSERT_EQ(8, stream.Write(24, "dddddddd", 8, false));
  EXPECT_EQ(32u, stream.ReorderSpan());
  ASSERT_EQ(8, stream.Write(8, "bbbbbbbb", 8, false));
  ASSERT_EQ(8, stream.Write(16, "cccccccc", 8, false));
  EXPECT_EQ(0, stream.CommittedOffset());
  EXPECT_EQ(32u, stream.ReorderSpan());
  ASSERT_TRUE(raw_file->Writes().empty());

  ASSERT_EQ(8, stream.Write(0, "aaaaaaaa", 8, false));

  // Everything must have cascaded out, otherwise the scheduler would see the
  // reorder window as still occupied and stop admitting ranges.
  EXPECT_EQ(32, stream.CommittedOffset());
  EXPECT_EQ(0u, stream.ReorderSpan());
  ASSERT_EQ(4u, raw_file->Writes().size());
  for (size_t idx = 0; idx < 4; idx++) {
    EXPECT_EQ(static_cast<XrdSfsFileOffset>(idx * 8), raw_file->Writes()[idx].first);
    EXPECT_EQ(8, raw_file->Writes()[idx].second);
  }
  EXPECT_EQ(AsBytes("aaaaaaaabbbbbbbbccccccccdddddddd"), raw_file->Data());
}

TEST_F(XrdHttpTpcRStreamTests, PartialAcceptSpansTwoBuffers) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  auto file = std::make_unique<MemorySfsFile>();
  auto raw_file = file.get();
  TPCR::Stream stream(std::move(file), 0, 8, log);

  ASSERT_EQ(4, stream.Write(0, "abcd", 4, false));
  EXPECT_EQ(4u, stream.ReorderSpan());

  // This one completes the first buffer and spills the rest into a second one:
  // the first four bytes go to disk, the last four stay buffered.
  ASSERT_EQ(8, stream.Write(4, "efghijkl", 8, false));
  EXPECT_EQ(8, stream.CommittedOffset());
  EXPECT_EQ(4u, stream.ReorderSpan());
  ASSERT_EQ(1u, raw_file->Writes().size());
  EXPECT_EQ(0, raw_file->Writes()[0].first);
  EXPECT_EQ(8, raw_file->Writes()[0].second);
  EXPECT_EQ(AsBytes("abcdefgh"), raw_file->Data());

  // Completing the spill-over buffer releases it too.
  ASSERT_EQ(4, stream.Write(12, "mnop", 4, false));
  EXPECT_EQ(16, stream.CommittedOffset());
  EXPECT_EQ(0u, stream.ReorderSpan());
  ASSERT_EQ(2u, raw_file->Writes().size());
  EXPECT_EQ(8, raw_file->Writes()[1].first);
  EXPECT_EQ(8, raw_file->Writes()[1].second);
  EXPECT_EQ(AsBytes("abcdefghijklmnop"), raw_file->Data());
}

TEST_F(XrdHttpTpcRStreamTests, ReorderSpanTracksBufferedData) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  auto file = std::make_unique<MemorySfsFile>();
  auto raw_file = file.get();
  TPCR::Stream stream(std::move(file), 0, 8, log);

  // ReorderSpan() is what the scheduler bounds by the reorder window when
  // admitting ranges (NFR-1), so it must be exact after every single call.
  EXPECT_EQ(0u, stream.ReorderSpan());
  ASSERT_EQ(8, stream.Write(16, "cccccccc", 8, false));
  EXPECT_EQ(24u, stream.ReorderSpan());
  ASSERT_EQ(4, stream.Write(8, "bbbb", 4, false));
  EXPECT_EQ(24u, stream.ReorderSpan());

  // Writes out the buffer holding offset 0 straight away; the half-filled
  // buffer at offset 8 is not full yet, so it stays put and keeps blocking
  // the one holding offset 16.
  ASSERT_EQ(8, stream.Write(0, "aaaaaaaa", 8, false));
  EXPECT_EQ(8, stream.CommittedOffset());
  EXPECT_EQ(16u, stream.ReorderSpan());
  ASSERT_EQ(1u, raw_file->Writes().size());

  // Completing the half-filled buffer cascades: offset 8, then offset 16.
  ASSERT_EQ(4, stream.Write(12, "bbbb", 4, false));
  EXPECT_EQ(24, stream.CommittedOffset());
  EXPECT_EQ(0u, stream.ReorderSpan());
  EXPECT_EQ(3u, raw_file->Writes().size());
  EXPECT_EQ(AsBytes("aaaaaaaabbbbbbbbcccccccc"), raw_file->Data());
}

TEST_F(XrdHttpTpcRStreamTests, ZeroSizeWriteForcesFlushOfPartialBuffer) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  auto file = std::make_unique<MemorySfsFile>();
  auto raw_file = file.get();
  TPCR::Stream stream(std::move(file), 0, 8, log);

  ASSERT_EQ(4, stream.Write(0, "abcd", 4, false));
  ASSERT_TRUE(raw_file->Writes().empty());

  // This is what TPCR::State::Flush() does at the end of a transfer: a forced,
  // zero-sized write that must push out buffers that never got filled.
  ASSERT_EQ(0, stream.Write(4, nullptr, 0, true));
  EXPECT_EQ(4, stream.CommittedOffset());
  EXPECT_EQ(0u, stream.ReorderSpan());
  ASSERT_EQ(1u, raw_file->Writes().size());
  EXPECT_EQ(0, raw_file->Writes()[0].first);
  EXPECT_EQ(4, raw_file->Writes()[0].second);
  EXPECT_EQ(AsBytes("abcd"), raw_file->Data());
  EXPECT_TRUE(stream.Finalize());
}

TEST_F(XrdHttpTpcRStreamTests, RepeatedFlushesPushOutBufferedDataOnce) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  auto file = std::make_unique<MemorySfsFile>();
  auto raw_file = file.get();
  TPCR::Stream stream(std::move(file), 0, 8, log);

  ASSERT_EQ(4, stream.Write(0, "abcd", 4, false));
  ASSERT_TRUE(raw_file->Writes().empty());

  // TPCR::State::Flush() ends up here.  In the multistream case every transfer
  // state shares this stream and each of them flushes it, hence the repeated
  // calls; none of them may fail, whatever offset the states stopped at.
  EXPECT_EQ(0, stream.Flush());
  EXPECT_EQ(0, stream.Flush());
  ASSERT_EQ(1u, raw_file->Writes().size());
  EXPECT_EQ(0, raw_file->Writes()[0].first);
  EXPECT_EQ(4, raw_file->Writes()[0].second);
  EXPECT_EQ(AsBytes("abcd"), raw_file->Data());
  EXPECT_TRUE(stream.GetErrorMessage().empty());
  EXPECT_TRUE(stream.Finalize());
}

TEST_F(XrdHttpTpcRStreamTests, FlushSucceedsWhenNothingIsBuffered) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  auto file = std::make_unique<MemorySfsFile>();
  auto raw_file = file.get();
  TPCR::Stream stream(std::move(file), 0, 8, log);

  // A buffer that gets exactly filled is written out straight away, so nothing
  // is left in memory.  This is what the end of a transfer looks like when the
  // size of the file is a multiple of the block size.
  ASSERT_EQ(8, stream.Write(0, "abcdefgh", 8, false));
  ASSERT_EQ(1u, raw_file->Writes().size());

  EXPECT_EQ(0, stream.Flush());
  EXPECT_EQ(0, stream.Flush());
  EXPECT_EQ(1u, raw_file->Writes().size());
  EXPECT_EQ(AsBytes("abcdefgh"), raw_file->Data());
  EXPECT_TRUE(stream.GetErrorMessage().empty());
  EXPECT_TRUE(stream.Finalize());
}

TEST_F(XrdHttpTpcRStreamTests, FilesystemWriteFailureIsPropagated) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  auto file = std::make_unique<MemorySfsFile>();
  auto raw_file = file.get();
  TPCR::Stream stream(std::move(file), 0, 8, log);

  raw_file->FailWrites();
  EXPECT_EQ(SFS_ERROR, stream.Write(0, "abcdefgh", 8, false));
  EXPECT_FALSE(stream.GetErrorMessage().empty());
}

// ---------------------------------------------------------------------------
// T-U4: offset-seeded construction (resume groundwork).
// ---------------------------------------------------------------------------

TEST_F(XrdHttpTpcRStreamTests, NonzeroSeedWritesLandAtAbsoluteOffsets) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  auto file = std::make_unique<MemorySfsFile>();
  auto raw_file = file.get();
  // A previous session already committed 16 bytes; the resumed stream is
  // seeded at W=16 and must never touch [0, 16).
  raw_file->Preload(AsBytes("0123456789abcdef"));
  TPCR::Stream stream(std::move(file), 16, 8, log);

  EXPECT_EQ(16, stream.CommittedOffset());
  ASSERT_EQ(8, stream.Write(16, "ghijklmn", 8, false));
  EXPECT_EQ(24, stream.CommittedOffset());
  ASSERT_EQ(1u, raw_file->Writes().size());
  EXPECT_EQ(16, raw_file->Writes()[0].first) << "write must land at the absolute offset";
  EXPECT_EQ(AsBytes("0123456789abcdefghijklmn"), raw_file->Data());

  // Out-of-order arrival relative to the seed works the same as from zero:
  // both fragments are buffered (neither fills an entry, so nothing flushes
  // until forced), then the final Flush drains them in order.
  ASSERT_EQ(4, stream.Write(28, "rstu", 4, false));
  EXPECT_EQ(8u, stream.ReorderSpan());
  ASSERT_EQ(4, stream.Write(24, "opqr", 4, false));
  EXPECT_EQ(24, stream.CommittedOffset());
  EXPECT_EQ(8u, stream.ReorderSpan());
  EXPECT_EQ(0, stream.Flush());
  EXPECT_EQ(32, stream.CommittedOffset());
  EXPECT_EQ(AsBytes("0123456789abcdefghijklmnopqrrstu"), raw_file->Data());
}

TEST_F(XrdHttpTpcRStreamTests, WriteBelowSeedIsRejected) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  auto file = std::make_unique<MemorySfsFile>();
  TPCR::Stream stream(std::move(file), 16, 8, log);

  // The committed prefix [0, W) belongs to the previous session; writing into
  // it is the prior-offset logic error, exactly as for a fresh stream.
  EXPECT_EQ(SFS_ERROR, stream.Write(0, "xxxxxxxx", 8, false));
  EXPECT_FALSE(stream.GetErrorMessage().empty());
}

TEST_F(XrdHttpTpcRStreamTests, DuplicateDeliveryBelowCommitIsRejected) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  auto file = std::make_unique<MemorySfsFile>();
  TPCR::Stream stream(std::move(file), 0, 8, log);

  ASSERT_EQ(8, stream.Write(0, "abcdefgh", 8, false));
  ASSERT_EQ(8, stream.CommittedOffset());
  // A retried range that re-delivers already-committed bytes must be caught:
  // the scheduler purges buffered data before re-queueing (WP-4), and the
  // stream guards the committed prefix (FR-25 groundwork).
  EXPECT_EQ(SFS_ERROR, stream.Write(0, "abcdefgh", 8, false));
}

// ---------------------------------------------------------------------------
// T-U4: short backing writes (BUG-6).
// ---------------------------------------------------------------------------

TEST_F(XrdHttpTpcRStreamTests, ShortDirectWriteAcceptsFullSize) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  auto file = std::make_unique<MemorySfsFile>();
  auto raw_file = file.get();
  raw_file->SetMaxWriteSize(3);
  TPCR::Stream stream(std::move(file), 0, 8, log);

  // BUG-6: a forced write at the committed offset hits the direct path; the
  // backend accepts only 3 of the 8 bytes.  Stream::Write must still report
  // all 8 accepted (the remainder is buffered), never the short count that
  // made the caller re-send bytes and double-write.
  ASSERT_EQ(8, stream.Write(0, "abcdefgh", 8, true));
  EXPECT_EQ(3, stream.CommittedOffset());
  EXPECT_EQ(5u, stream.ReorderSpan());

  // Draining the remainder takes several short backing writes.
  EXPECT_EQ(0, stream.Flush());
  EXPECT_EQ(8, stream.CommittedOffset());
  EXPECT_EQ(0u, stream.ReorderSpan());
  EXPECT_EQ(AsBytes("abcdefgh"), raw_file->Data());
  // No byte may reach the file twice.
  EXPECT_EQ(8u, raw_file->BytesWritten());
  EXPECT_TRUE(stream.Finalize());
}

TEST_F(XrdHttpTpcRStreamTests, ShortWritesDrainBufferedEntriesWithoutLoss) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  auto file = std::make_unique<MemorySfsFile>();
  auto raw_file = file.get();
  raw_file->SetMaxWriteSize(3);
  TPCR::Stream stream(std::move(file), 0, 8, log);

  // Out-of-order arrival, so everything goes through the reorder buffers;
  // the short-writing backend then drains each entry in several passes.
  ASSERT_EQ(8, stream.Write(8, "ijklmnop", 8, false));
  ASSERT_EQ(8, stream.Write(0, "abcdefgh", 8, false));
  EXPECT_EQ(0, stream.Flush());
  EXPECT_EQ(16, stream.CommittedOffset());
  EXPECT_EQ(AsBytes("abcdefghijklmnop"), raw_file->Data());
  EXPECT_EQ(16u, raw_file->BytesWritten()) << "double-write detected";
  EXPECT_TRUE(stream.Finalize());
}

// ---------------------------------------------------------------------------
// WP-1 commit hook: in-order, exactly-once.
// ---------------------------------------------------------------------------

TEST_F(XrdHttpTpcRStreamTests, CommitHookSeesEveryByteInOrderExactlyOnce) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  auto file = std::make_unique<MemorySfsFile>();
  auto raw_file = file.get();
  raw_file->SetMaxWriteSize(5);  // Short writes make the hook cadence uneven.
  TPCR::Stream stream(std::move(file), 0, 8, log);

  std::vector<char> hooked;
  off_t expected_offset = 0;
  bool in_order = true;
  stream.SetCommitHook([&](off_t offset, const char *buffer, size_t size) {
    if (offset != expected_offset) {
      in_order = false;
    }
    expected_offset = offset + static_cast<off_t>(size);
    hooked.insert(hooked.end(), buffer, buffer + size);
  });

  // Adversarial-ish arrival: tail first, then head, then the middle.
  ASSERT_EQ(8, stream.Write(16, "qrstuvwx", 8, false));
  ASSERT_EQ(8, stream.Write(0, "abcdefgh", 8, false));
  ASSERT_EQ(8, stream.Write(8, "ijklmnop", 8, false));
  EXPECT_EQ(0, stream.Flush());

  EXPECT_TRUE(in_order) << "commit hook observed a gap or out-of-order range";
  EXPECT_EQ(AsBytes("abcdefghijklmnopqrstuvwx"), hooked)
      << "hook must see exactly the committed byte stream";
  EXPECT_EQ(raw_file->Data(), hooked);
  EXPECT_TRUE(stream.Finalize());
}

// ---------------------------------------------------------------------------
// T-U5: adversarial arrival orders, property-style.
// ---------------------------------------------------------------------------

TEST_F(XrdHttpTpcRStreamTests, AdversarialArrivalOrdersProduceExactContent) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");

  // Emulates the real arrival pattern: the file is split into ranges (as the
  // scheduler would), each range's bytes arrive strictly in order (curl
  // delivers a response body sequentially), but the ranges interleave
  // arbitrarily.  Fragment sizes deliberately straddle the 8-byte entry
  // capacity so entries fragment (BUG-7's trigger).  Deterministic seeds:
  // failures are reproducible by seed.
  const size_t file_size = 512;
  const size_t range_size = 32;
  std::vector<char> reference(file_size);
  for (size_t i = 0; i < file_size; i++) {
    reference[i] = static_cast<char>('A' + (i % 23));
  }

  for (unsigned int seed = 0; seed < 25; seed++) {
    std::mt19937 rng(seed);
    auto file = std::make_unique<MemorySfsFile>();
    auto raw_file = file.get();
    TPCR::Stream stream(std::move(file), 0, 8, log);

    // Per-range delivery cursors.
    struct RangeCursor { size_t start, delivered, length; };
    std::vector<RangeCursor> ranges;
    for (size_t off = 0; off < file_size; off += range_size) {
      ranges.push_back({off, 0, std::min(range_size, file_size - off)});
    }

    std::uniform_int_distribution<size_t> frag_size(1, 13);
    while (!ranges.empty()) {
      std::uniform_int_distribution<size_t> pick(0, ranges.size() - 1);
      size_t idx = pick(rng);
      RangeCursor &range = ranges[idx];
      size_t chunk = std::min(frag_size(rng), range.length - range.delivered);
      off_t offset = static_cast<off_t>(range.start + range.delivered);
      ASSERT_EQ(static_cast<ssize_t>(chunk),
                stream.Write(offset, &reference[range.start + range.delivered],
                             chunk, false))
          << "seed=" << seed << " offset=" << offset << " chunk=" << chunk;
      range.delivered += chunk;
      if (range.delivered == range.length) {
        ranges.erase(ranges.begin() + idx);
      }
    }

    ASSERT_EQ(0, stream.Flush()) << "seed=" << seed;
    EXPECT_EQ(static_cast<off_t>(file_size), stream.CommittedOffset())
        << "seed=" << seed;
    EXPECT_EQ(reference, raw_file->Data()) << "seed=" << seed;
    EXPECT_EQ(file_size, raw_file->BytesWritten())
        << "seed=" << seed << ": double-write detected";
    EXPECT_TRUE(stream.Finalize()) << "seed=" << seed;
  }
}
