#undef NDEBUG

#include "XrdHttpTpc/XrdHttpTpcStream.hh"
#include "XrdSfs/XrdSfsInterface.hh"
#include "XrdSys/XrdSysError.hh"
#include "XrdSys/XrdSysLogger.hh"

#include <algorithm>
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

using namespace testing;

class XrdHttpTpcStreamTests : public Test {};

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
    size_t begin = static_cast<size_t>(offset);
    size_t end = begin + static_cast<size_t>(size);
    if (m_data.size() < end) {
      m_data.resize(end);
    }
    memcpy(&m_data[begin], buffer, static_cast<size_t>(size));
    m_writes.emplace_back(offset, size);
    return size;
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

  // Makes every subsequent write() fail, to check error propagation.
  void FailWrites() {
    m_fail_writes = true;
  }

private:
  std::vector<char> m_data;
  std::vector<std::pair<XrdSfsFileOffset, XrdSfsXferSize>> m_writes;
  bool m_fail_writes = false;
};

namespace {

std::vector<char> AsBytes(const std::string &str) {
  return std::vector<char>(str.begin(), str.end());
}

}

TEST_F(XrdHttpTpcStreamTests, FlushesExactlyFullEmptyBuffer) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  auto file = std::make_unique<MemorySfsFile>();
  auto raw_file = file.get();
  TPC::Stream stream(std::move(file), 1, 8, log);

  // Reproduce the case where a single callback exactly fills an empty reorder
  // buffer.  The buffer is contiguous with the stream offset, so it must be
  // written immediately and returned to the available-buffer pool.
  ASSERT_EQ(8, stream.Write(0, "abcdefgh", 8, false));
  ASSERT_EQ(1u, stream.AvailableBuffers());
  ASSERT_EQ(1u, raw_file->Writes().size());
  EXPECT_EQ(0, raw_file->Writes()[0].first);
  EXPECT_EQ(8, raw_file->Writes()[0].second);
  EXPECT_EQ(AsBytes("abcdefgh"), raw_file->Data());
}

TEST_F(XrdHttpTpcStreamTests, FlushesExactlyFullCurrentBuffer) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  auto file = std::make_unique<MemorySfsFile>();
  auto raw_file = file.get();
  TPC::Stream stream(std::move(file), 1, 8, log);

  // Reproduce the multi-stream pull stall from xrootd/xrootd#2108 in miniature:
  // a first callback partially fills the only reorder buffer, then a later
  // callback exactly completes it.  If exact fills are not flushed immediately,
  // the transfer has no active curl handles and no available buffers, so no new
  // range requests can be started.
  ASSERT_EQ(4, stream.Write(0, "abcd", 4, false));
  ASSERT_EQ(0u, stream.AvailableBuffers());
  ASSERT_TRUE(raw_file->Writes().empty());

  // Completing the buffer should trigger a backing write and make the buffer
  // available again without requiring another callback to enter Stream::Write().
  ASSERT_EQ(4, stream.Write(4, "efgh", 4, false));
  ASSERT_EQ(1u, stream.AvailableBuffers());
  ASSERT_EQ(1u, raw_file->Writes().size());
  EXPECT_EQ(0, raw_file->Writes()[0].first);
  EXPECT_EQ(8, raw_file->Writes()[0].second);
  EXPECT_EQ(AsBytes("abcdefgh"), raw_file->Data());
}

TEST_F(XrdHttpTpcStreamTests, FlushesCascadeAfterOutOfOrderArrival) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  auto file = std::make_unique<MemorySfsFile>();
  auto raw_file = file.get();
  TPC::Stream stream(std::move(file), 4, 8, log);

  // Four ranges arrive in reverse-ish order; none of them can be written until
  // the one starting at offset 0 shows up.  Writing that one advances the stream
  // offset and must in turn unblock the buffer holding offset 8, then 16, then
  // 24 -- a single flush pass over the buffers is not enough.
  ASSERT_EQ(8, stream.Write(24, "dddddddd", 8, false));
  ASSERT_EQ(3u, stream.AvailableBuffers());
  ASSERT_EQ(8, stream.Write(8, "bbbbbbbb", 8, false));
  ASSERT_EQ(2u, stream.AvailableBuffers());
  ASSERT_EQ(8, stream.Write(16, "cccccccc", 8, false));
  ASSERT_EQ(1u, stream.AvailableBuffers());
  ASSERT_TRUE(raw_file->Writes().empty());

  ASSERT_EQ(8, stream.Write(0, "aaaaaaaa", 8, false));

  // Every buffer must have been handed back, otherwise the multi-stream
  // scheduler cannot start any further range request.
  EXPECT_EQ(4u, stream.AvailableBuffers());
  ASSERT_EQ(4u, raw_file->Writes().size());
  for (size_t idx = 0; idx < 4; idx++) {
    EXPECT_EQ(static_cast<XrdSfsFileOffset>(idx * 8), raw_file->Writes()[idx].first);
    EXPECT_EQ(8, raw_file->Writes()[idx].second);
  }
  EXPECT_EQ(AsBytes("aaaaaaaabbbbbbbbccccccccdddddddd"), raw_file->Data());
}

TEST_F(XrdHttpTpcStreamTests, PartialAcceptSpansTwoBuffers) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  auto file = std::make_unique<MemorySfsFile>();
  auto raw_file = file.get();
  TPC::Stream stream(std::move(file), 2, 8, log);

  ASSERT_EQ(4, stream.Write(0, "abcd", 4, false));
  ASSERT_EQ(1u, stream.AvailableBuffers());

  // This one completes the first buffer and spills the rest into a second one:
  // the first four bytes go to disk, the last four stay buffered.
  ASSERT_EQ(8, stream.Write(4, "efghijkl", 8, false));
  EXPECT_EQ(1u, stream.AvailableBuffers());
  ASSERT_EQ(1u, raw_file->Writes().size());
  EXPECT_EQ(0, raw_file->Writes()[0].first);
  EXPECT_EQ(8, raw_file->Writes()[0].second);
  EXPECT_EQ(AsBytes("abcdefgh"), raw_file->Data());

  // Completing the spill-over buffer releases it too.
  ASSERT_EQ(4, stream.Write(12, "mnop", 4, false));
  EXPECT_EQ(2u, stream.AvailableBuffers());
  ASSERT_EQ(2u, raw_file->Writes().size());
  EXPECT_EQ(8, raw_file->Writes()[1].first);
  EXPECT_EQ(8, raw_file->Writes()[1].second);
  EXPECT_EQ(AsBytes("abcdefghijklmnop"), raw_file->Data());
}

TEST_F(XrdHttpTpcStreamTests, AvailableCountReflectsBufferOccupancy) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  auto file = std::make_unique<MemorySfsFile>();
  auto raw_file = file.get();
  TPC::Stream stream(std::move(file), 3, 8, log);

  // AvailableBuffers() is what gates new range requests, so it must match the
  // number of empty buffers after every single call, not eventually.
  ASSERT_EQ(3u, stream.AvailableBuffers());
  ASSERT_EQ(8, stream.Write(16, "cccccccc", 8, false));
  EXPECT_EQ(2u, stream.AvailableBuffers());
  ASSERT_EQ(4, stream.Write(8, "bbbb", 4, false));
  EXPECT_EQ(1u, stream.AvailableBuffers());

  // Writes out the buffer holding offset 0 and hands it straight back; the
  // half-filled buffer at offset 8 is not full yet, so it stays put and keeps
  // blocking the one holding offset 16.
  ASSERT_EQ(8, stream.Write(0, "aaaaaaaa", 8, false));
  EXPECT_EQ(1u, stream.AvailableBuffers());
  ASSERT_EQ(1u, raw_file->Writes().size());

  // Completing the half-filled buffer cascades: offset 8, then offset 16.
  ASSERT_EQ(4, stream.Write(12, "bbbb", 4, false));
  EXPECT_EQ(3u, stream.AvailableBuffers());
  EXPECT_EQ(3u, raw_file->Writes().size());
  EXPECT_EQ(AsBytes("aaaaaaaabbbbbbbbcccccccc"), raw_file->Data());
}

TEST_F(XrdHttpTpcStreamTests, ZeroSizeWriteForcesFlushOfPartialBuffer) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  auto file = std::make_unique<MemorySfsFile>();
  auto raw_file = file.get();
  TPC::Stream stream(std::move(file), 2, 8, log);

  ASSERT_EQ(4, stream.Write(0, "abcd", 4, false));
  ASSERT_TRUE(raw_file->Writes().empty());

  // This is what TPC::State::Flush() does at the end of a transfer: a forced,
  // zero-sized write that must push out buffers that never got filled.
  ASSERT_EQ(0, stream.Write(4, nullptr, 0, true));
  EXPECT_EQ(2u, stream.AvailableBuffers());
  ASSERT_EQ(1u, raw_file->Writes().size());
  EXPECT_EQ(0, raw_file->Writes()[0].first);
  EXPECT_EQ(4, raw_file->Writes()[0].second);
  EXPECT_EQ(AsBytes("abcd"), raw_file->Data());
  EXPECT_TRUE(stream.Finalize());
}

TEST_F(XrdHttpTpcStreamTests, RepeatedFlushesPushOutBufferedDataOnce) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  auto file = std::make_unique<MemorySfsFile>();
  auto raw_file = file.get();
  TPC::Stream stream(std::move(file), 2, 8, log);

  ASSERT_EQ(4, stream.Write(0, "abcd", 4, false));
  ASSERT_TRUE(raw_file->Writes().empty());

  // TPC::State::Flush() ends up here.  In the multistream case every transfer
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

TEST_F(XrdHttpTpcStreamTests, FlushSucceedsWhenNothingIsBuffered) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  auto file = std::make_unique<MemorySfsFile>();
  auto raw_file = file.get();
  TPC::Stream stream(std::move(file), 2, 8, log);

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

TEST_F(XrdHttpTpcStreamTests, FilesystemWriteFailureIsPropagated) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  auto file = std::make_unique<MemorySfsFile>();
  auto raw_file = file.get();
  TPC::Stream stream(std::move(file), 2, 8, log);

  raw_file->FailWrites();
  EXPECT_EQ(SFS_ERROR, stream.Write(0, "abcdefgh", 8, false));
  EXPECT_FALSE(stream.GetErrorMessage().empty());
}

// The backing file must see one sequential stream of writes that adds up to
// the expected content, whatever the order of arrival was.
static void ExpectSequentialWrites(const MemorySfsFile &file,
                                   const std::vector<char> &expected,
                                   const std::string &context) {
  XrdSfsFileOffset next = 0;
  for (const auto &write : file.Writes()) {
    ASSERT_EQ(next, write.first) << "write out of sequence, " << context;
    next += write.second;
  }
  EXPECT_EQ(static_cast<XrdSfsFileOffset>(expected.size()), next) << context;
  EXPECT_EQ(expected, file.Data()) << context;
}

TEST_F(XrdHttpTpcStreamTests, EveryArrivalOrderOfFourRangesIsWrittenInSequence) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  const std::string content = "aaaaaaaabbbbbbbbccccccccdddddddd";

  std::vector<int> order = {0, 1, 2, 3};
  do {
    std::string context = "order";
    for (int range : order) {context += " " + std::to_string(range);}

    auto file = std::make_unique<MemorySfsFile>();
    auto raw_file = file.get();
    TPC::Stream stream(std::move(file), 4, 8, log);

    for (int range : order) {
      ASSERT_EQ(8, stream.Write(range * 8, &content[range * 8], 8, false)) << context;
    }
    EXPECT_EQ(4u, stream.AvailableBuffers()) << context;
    EXPECT_EQ(4u, raw_file->Writes().size()) << context;
    ExpectSequentialWrites(*raw_file, AsBytes(content), context);
    EXPECT_TRUE(stream.Finalize()) << context;
  } while (std::next_permutation(order.begin(), order.end()));
}

// Ranges are handed out the way the multi-stream scheduler does it (see
// MultiCurlHandler::CanStartTransfer): a range starts only when a buffer is
// free for it, counting one buffer for each range that has started but has not
// delivered a byte yet.  The ranges in flight then deliver pieces of odd sizes
// in a random order.
TEST_F(XrdHttpTpcStreamTests, RandomInterleavingOfRangesIsWrittenInSequence) {
  XrdSysLogger logger(STDERR_FILENO, 0);
  XrdSysError log(&logger, "StreamTest");
  const size_t buffer_size = 64, buffers = 4, ranges = 23;
  // The last range is short, as at the end of a real file.
  const size_t file_size = (ranges - 1) * buffer_size + 13;

  std::vector<char> expected(file_size);
  for (size_t idx = 0; idx < file_size; idx++) {
    expected[idx] = static_cast<char>('A' + (idx * 7 + idx / buffer_size) % 53);
  }

  struct Range {size_t start; size_t size; size_t done;};

  for (unsigned seed = 1; seed <= 200; seed++) {
    const std::string context = "seed " + std::to_string(seed);
    std::mt19937 random(seed);
    auto file = std::make_unique<MemorySfsFile>();
    auto raw_file = file.get();
    TPC::Stream stream(std::move(file), buffers, buffer_size, log);

    std::vector<Range> flight;
    size_t next = 0;
    auto start_ranges = [&]() {
      while (next < ranges && flight.size() < buffers) {
        size_t waiting = std::count_if(flight.begin(), flight.end(),
                                       [](const Range &r) {return r.done == 0;});
        if (stream.AvailableBuffers() <= waiting) {break;}
        size_t start = next * buffer_size;
        flight.push_back({start, std::min(buffer_size, file_size - start), 0});
        next++;
      }
    };

    start_ranges();
    while (!flight.empty()) {
      size_t pick = random() % flight.size();
      Range &range = flight[pick];
      size_t piece = std::min<size_t>(1 + random() % 17, range.size - range.done);
      ASSERT_EQ(static_cast<ssize_t>(piece),
                stream.Write(range.start + range.done,
                             &expected[range.start + range.done], piece, false))
          << context << ": " << stream.GetErrorMessage();
      range.done += piece;
      if (range.done == range.size) {flight.erase(flight.begin() + pick);}

      // The scheduler relies on this: a buffer is in use only for a range
      // that has started and is not yet completely in the file.
      size_t in_use = buffers - stream.AvailableBuffers();
      size_t unwritten = next - raw_file->Data().size() / buffer_size;
      ASSERT_LE(in_use, unwritten) << context;
      start_ranges();
      // With ranges left to fetch, something must always be in flight:
      // otherwise the real transfer would stall here.
      ASSERT_FALSE(flight.empty() && next < ranges) << "stalled, " << context;
    }

    EXPECT_EQ(0, stream.Flush()) << context;
    EXPECT_EQ(buffers, stream.AvailableBuffers()) << context;
    ExpectSequentialWrites(*raw_file, expected, context);
    EXPECT_TRUE(stream.Finalize()) << context;
  }
}
