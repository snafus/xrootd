#undef NDEBUG

#include "XrdHttpTpc/XrdHttpTpcBlockPull.hh"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

using namespace TPC;

namespace {

const off_t MiB = 1024 * 1024;

// Whether `done` becomes true within the time given.
bool Within(const std::atomic<bool> &done, std::chrono::milliseconds patience) {
  const auto deadline = std::chrono::steady_clock::now() + patience;
  while (!done.load()) {
    if (std::chrono::steady_clock::now() > deadline) {return false;}
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

}

TEST(XrdHttpTpcBlockPullTests, BlocksAreHandedOutInFileOrderAndCoverTheFile) {
  // 3.5 blocks; a window wide enough not to matter.
  BlockPull pull(7 * MiB, 2 * MiB, 4);
  EXPECT_EQ(4u, pull.Blocks());

  off_t offset = -1, size = -1, expected = 0;
  std::vector<std::pair<off_t, off_t>> blocks;
  while (pull.NextBlock(offset, size)) {
    EXPECT_EQ(expected, offset);
    blocks.emplace_back(offset, size);
    expected += size;
  }
  ASSERT_EQ(4u, blocks.size());
  EXPECT_EQ(2 * MiB, blocks[0].second);
  EXPECT_EQ(1 * MiB, blocks[3].second);   // the last block is what is left
  EXPECT_EQ(7 * MiB, expected);
  EXPECT_TRUE(pull.AllHandedOut());
  EXPECT_EQ(0u, pull.BlocksDone());
}

TEST(XrdHttpTpcBlockPullTests, CompleteUpToIsTheStartOfTheOldestBlockThatIsNotDone) {
  BlockPull pull(8 * MiB, 2 * MiB, 4);
  off_t offset, size;
  EXPECT_EQ(0, pull.CompleteUpTo());
  for (int i = 0; i < 4; i++) {ASSERT_TRUE(pull.NextBlock(offset, size));}

  // Blocks finish out of order: nothing is complete from the start until
  // the first one is.
  pull.BlockDone(3, 4 * MiB, 2 * MiB, 0.1);
  pull.BlockDone(2, 2 * MiB, 2 * MiB, 0.1);
  EXPECT_EQ(0, pull.CompleteUpTo());
  pull.BlockDone(1, 0, 2 * MiB, 0.1);
  EXPECT_EQ(6 * MiB, pull.CompleteUpTo());
  pull.BlockDone(4, 6 * MiB, 2 * MiB, 0.1);
  EXPECT_EQ(8 * MiB, pull.CompleteUpTo());
  EXPECT_EQ(4u, pull.BlocksDone());

  const std::vector<BlockReport> reports = pull.TakeReports();
  ASSERT_EQ(4u, reports.size());
  EXPECT_EQ(3u, reports[0].stream);
  EXPECT_EQ(4 * MiB, reports[0].start);
  EXPECT_EQ(6 * MiB, reports[0].end);
  EXPECT_TRUE(pull.TakeReports().empty());
}

TEST(XrdHttpTpcBlockPullTests, TheHandOutWaitsTwoRoundsAheadOfTheOldestUnfinishedBlock) {
  // One worker: the window is two blocks.
  BlockPull pull(10 * MiB, 1 * MiB, 1);
  off_t offset, size;
  ASSERT_TRUE(pull.NextBlock(offset, size));   // block at 0
  ASSERT_TRUE(pull.NextBlock(offset, size));   // block at 1 MiB
  // Block 0 is not done: the third block must not be handed out.
  std::atomic<bool> returned{false};
  off_t third = -1, third_size = 0;
  bool got = false;
  std::thread taker([&] {
    got = pull.NextBlock(third, third_size);
    returned.store(true);
  });
  EXPECT_FALSE(Within(returned, std::chrono::milliseconds(200)));
  // The second block being done changes nothing: the oldest is still block 0.
  pull.BlockDone(1, 1 * MiB, 1 * MiB, 0.1);
  EXPECT_FALSE(Within(returned, std::chrono::milliseconds(200)));
  pull.BlockDone(1, 0, 1 * MiB, 0.1);
  EXPECT_TRUE(Within(returned, std::chrono::seconds(5)));
  taker.join();
  EXPECT_TRUE(got);
  EXPECT_EQ(2 * MiB, third);
}

TEST(XrdHttpTpcBlockPullTests, AStopEndsAWaitAndTheHandOut) {
  BlockPull pull(10 * MiB, 1 * MiB, 1);
  off_t offset, size;
  ASSERT_TRUE(pull.NextBlock(offset, size));
  ASSERT_TRUE(pull.NextBlock(offset, size));
  std::atomic<bool> returned{false};
  bool got = true;
  std::thread taker([&] {
    off_t o, s;
    got = pull.NextBlock(o, s);
    returned.store(true);
  });
  EXPECT_FALSE(Within(returned, std::chrono::milliseconds(200)));
  pull.Stop();
  EXPECT_TRUE(Within(returned, std::chrono::seconds(5)));
  taker.join();
  EXPECT_FALSE(got);
  EXPECT_TRUE(pull.stop.load());
  EXPECT_FALSE(pull.NextBlock(offset, size));
  EXPECT_FALSE(pull.Failed());   // a stop alone is not a failure
}

TEST(XrdHttpTpcBlockPullTests, TheFirstFailureIsKeptAndStopsThePull) {
  BlockPull pull(10 * MiB, 1 * MiB, 4);
  EXPECT_FALSE(pull.Failed());

  BlockFailure first;
  first.status_code = 503;
  first.stream = 2;
  first.block_start = 1 * MiB;
  first.block_end = 2 * MiB;
  first.block_received = 12345;
  first.stall_limit = 60;
  first.stall_idle = 61;
  first.message = "first";
  pull.Fail(first);

  BlockFailure second;
  second.error_code = BlockPull::errInternal;
  second.stream = 3;
  second.message = "second";
  pull.Fail(second);

  EXPECT_TRUE(pull.Failed());
  EXPECT_TRUE(pull.stop.load());
  const BlockFailure kept = pull.Failure();
  EXPECT_EQ(503, kept.status_code);
  EXPECT_EQ(0, kept.error_code);
  EXPECT_EQ(2u, kept.stream);
  EXPECT_EQ(1 * MiB, kept.block_start);
  EXPECT_EQ(2 * MiB, kept.block_end);
  EXPECT_EQ(12345, kept.block_received);
  // Every field of the failure is kept, not only those the verdict needs most.
  EXPECT_EQ(60, kept.stall_limit);
  EXPECT_EQ(61, kept.stall_idle);
  EXPECT_EQ(CURLE_OK, kept.curl_result);
  EXPECT_EQ("first", kept.message);
}

TEST(XrdHttpTpcBlockPullTests, TheCoordinatorWaitsForTheWorkers) {
  BlockPull pull(4 * MiB, 1 * MiB, 2);
  EXPECT_TRUE(pull.WaitForWorkers(std::chrono::seconds(0)));   // none started
  pull.WorkerStarting();
  pull.WorkerStarting();
  pull.WorkerNotStarted();
  EXPECT_EQ(1, pull.Running());
  EXPECT_FALSE(pull.WaitForWorkers(std::chrono::seconds(0)));
  std::thread worker([&] {pull.WorkerEnded();});
  EXPECT_TRUE(pull.WaitForWorkers(std::chrono::seconds(5)));
  worker.join();
  EXPECT_EQ(0, pull.Running());
}

TEST(XrdHttpTpcBlockPullTests, TheBudgetGivesWhatIsLeftAndTakesItBack) {
  ASSERT_EQ(0, StreamThreadBudget::InUse());
  {
    StreamThreadBudget::Share first = StreamThreadBudget::Take(4, 6);
    EXPECT_EQ(4, first.Count());
    EXPECT_EQ(4, StreamThreadBudget::InUse());

    // Two are left below the limit of six.
    StreamThreadBudget::Share second = StreamThreadBudget::Take(4, 6);
    EXPECT_EQ(2, second.Count());
    EXPECT_EQ(6, StreamThreadBudget::InUse());

    // None is left.
    StreamThreadBudget::Share third = StreamThreadBudget::Take(4, 6);
    EXPECT_EQ(0, third.Count());
    EXPECT_EQ(6, StreamThreadBudget::InUse());

    // A handler with a higher limit of its own still gets some.
    StreamThreadBudget::Share other = StreamThreadBudget::Take(4, 8);
    EXPECT_EQ(2, other.Count());

    // Giving a part back, and never taking more by "shrinking" upwards.
    first.ShrinkTo(1);
    EXPECT_EQ(1, first.Count());
    EXPECT_EQ(5, StreamThreadBudget::InUse());
    first.ShrinkTo(3);
    EXPECT_EQ(1, first.Count());

    // A share that is moved is counted once.
    StreamThreadBudget::Share moved = std::move(second);
    EXPECT_EQ(2, moved.Count());
    EXPECT_EQ(0, second.Count());
    EXPECT_EQ(5, StreamThreadBudget::InUse());
    third = std::move(moved);
    EXPECT_EQ(2, third.Count());
    EXPECT_EQ(5, StreamThreadBudget::InUse());
  }
  // Every share gives back what it holds when it goes.
  EXPECT_EQ(0, StreamThreadBudget::InUse());
}

TEST(XrdHttpTpcBlockPullTests, AStreamThatWaitsForTheSourceTooLongIsStalled) {
  // A first byte may take 6 s, more data 3 s.
  StallWatch watch;
  const int receiving = WorkerStatus::Receiving;
  EXPECT_EQ(0, watch.Stalled(100, 0, 0, receiving, 1, 6, 3));   // first look: the time starts
  EXPECT_EQ(0, watch.Stalled(106, 0, 0, receiving, 1, 6, 3));   // exactly the time allowed
  EXPECT_EQ(7, watch.Stalled(107, 0, 0, receiving, 1, 6, 3));   // longer: stalled, for 7 s

  // Once data has come, the shorter time applies, counted from the last byte.
  StallWatch moving;
  EXPECT_EQ(0, moving.Stalled(100, 0, 0, receiving, 1, 6, 3));
  EXPECT_EQ(0, moving.Stalled(101, 500, 0, receiving, 1, 6, 3));
  EXPECT_EQ(0, moving.Stalled(104, 500, 0, receiving, 1, 6, 3));
  EXPECT_EQ(4, moving.Stalled(105, 500, 0, receiving, 1, 6, 3));
  // A byte more and it is alive again.
  EXPECT_EQ(0, moving.Stalled(106, 501, 0, receiving, 1, 6, 3));
  EXPECT_EQ(0, moving.Stalled(109, 501, 0, receiving, 1, 6, 3));
  EXPECT_EQ(4, moving.Stalled(110, 501, 0, receiving, 1, 6, 3));
}

TEST(XrdHttpTpcBlockPullTests, TimeSpentWritingOrBetweenRequestsIsNotAStall) {
  StallWatch watch;
  EXPECT_EQ(0, watch.Stalled(100, 0, 0, WorkerStatus::Receiving, 1, 6, 3));
  EXPECT_EQ(0, watch.Stalled(101, 900, 0, WorkerStatus::Receiving, 1, 6, 3));
  // A write that takes a minute, looked at every second as the coordinator
  // does: nothing is received meanwhile, and that is not the source's doing.
  for (long long now = 102; now <= 162; now++) {
    EXPECT_EQ(0, watch.Stalled(now, 900, 0, WorkerStatus::Writing, 1, 6, 3));
  }
  // Back to waiting for the source: the time counts from the last look at
  // which it was still writing.
  EXPECT_EQ(0, watch.Stalled(163, 900, 0, WorkerStatus::Receiving, 1, 6, 3));
  EXPECT_EQ(0, watch.Stalled(165, 900, 0, WorkerStatus::Receiving, 1, 6, 3));
  EXPECT_EQ(4, watch.Stalled(166, 900, 0, WorkerStatus::Receiving, 1, 6, 3));

  // Waiting for a block to be handed out.
  StallWatch idle;
  EXPECT_EQ(0, idle.Stalled(100, 0, 0, WorkerStatus::Idle, 0, 6, 3));
  EXPECT_EQ(0, idle.Stalled(500, 0, 0, WorkerStatus::Idle, 0, 6, 3));
}

TEST(XrdHttpTpcBlockPullTests, ANewRequestStartsItsOwnTime) {
  StallWatch watch;
  const int receiving = WorkerStatus::Receiving;
  EXPECT_EQ(0, watch.Stalled(100, 0, 0, receiving, 1, 6, 3));
  EXPECT_EQ(0, watch.Stalled(101, 1000, 0, receiving, 1, 6, 3));
  // The next request has received nothing yet: it has the time of a first
  // byte, although the worker has received bytes before.
  EXPECT_EQ(0, watch.Stalled(103, 1000, 1000, receiving, 2, 6, 3));
  EXPECT_EQ(0, watch.Stalled(109, 1000, 1000, receiving, 2, 6, 3));
  EXPECT_EQ(7, watch.Stalled(110, 1000, 1000, receiving, 2, 6, 3));
}

namespace {

SourceCheck::Seen Response(const char *host, off_t total, const char *etag, const char *modified) {
  SourceCheck::Seen seen;
  seen.host = host;
  seen.total = total;
  seen.etag = etag;
  seen.last_modified = modified;
  return seen;
}

}

TEST(XrdHttpTpcBlockPullTests, ResponsesOfTheSameVersionAgree) {
  SourceCheck check(1000);
  std::string error = "unset";
  EXPECT_TRUE(check.Agrees(Response("a", 1000, "\"v1\"", "Mon, 01 Jan 2024 00:00:00 GMT"), error));
  EXPECT_TRUE(check.Agrees(Response("a", 1000, "\"v1\"", "Mon, 01 Jan 2024 00:00:00 GMT"), error));
  // A response that gives less is not a mismatch.
  EXPECT_TRUE(check.Agrees(Response("a", -1, "", ""), error));
  EXPECT_TRUE(check.Agrees(Response("a", 1000, "", "Mon, 01 Jan 2024 00:00:00 GMT"), error));
  EXPECT_EQ("unset", error);
}

TEST(XrdHttpTpcBlockPullTests, AChangedSizeOrValidatorOfOneHostIsAChangedFile) {
  std::string error;
  {
    SourceCheck check(1000);
    EXPECT_TRUE(check.Agrees(Response("a", 1000, "", ""), error));
    EXPECT_FALSE(check.Agrees(Response("a", 1001, "", ""), error));
    EXPECT_NE(std::string::npos, error.find("its size was 1000 bytes and is now 1001")) << error;
    EXPECT_NE(std::string::npos, error.find("(server a)")) << error;
  }
  {
    SourceCheck check(1000);
    EXPECT_TRUE(check.Agrees(Response("a", 1000, "\"v1\"", ""), error));
    EXPECT_FALSE(check.Agrees(Response("a", 1000, "\"v2\"", ""), error));
    EXPECT_NE(std::string::npos, error.find("its ETag was \"v1\" and is now \"v2\"")) << error;
  }
  {
    // No strong ETag: Last-Modified decides.
    SourceCheck check(1000);
    EXPECT_TRUE(check.Agrees(Response("a", 1000, "W/\"v1\"", "Mon, 01 Jan 2024 00:00:00 GMT"), error));
    EXPECT_FALSE(check.Agrees(Response("a", 1000, "W/\"v1\"", "Tue, 02 Jan 2024 00:00:00 GMT"), error));
    EXPECT_NE(std::string::npos, error.find("Last-Modified")) << error;
  }
}

TEST(XrdHttpTpcBlockPullTests, WhatDoesNotProveAChangeIsNotOne) {
  std::string error;
  // A weak ETag says "equivalent", not "the same bytes": it is not compared.
  SourceCheck weak(1000);
  EXPECT_TRUE(weak.Agrees(Response("a", 1000, "W/\"v1\"", ""), error));
  EXPECT_TRUE(weak.Agrees(Response("a", 1000, "W/\"v2\"", ""), error));

  // Where the host gives a strong ETag and it is the same, a Last-Modified
  // that differs does not decide.
  SourceCheck both(1000);
  EXPECT_TRUE(both.Agrees(Response("a", 1000, "\"v1\"", "Mon, 01 Jan 2024 00:00:00 GMT"), error));
  EXPECT_TRUE(both.Agrees(Response("a", 1000, "\"v1\"", "Tue, 02 Jan 2024 00:00:00 GMT"), error));

  // Two servers that hold copies of the file have validators of their own.
  SourceCheck replicas(1000);
  EXPECT_TRUE(replicas.Agrees(Response("a", 1000, "\"v1\"", "Mon, 01 Jan 2024 00:00:00 GMT"), error));
  EXPECT_TRUE(replicas.Agrees(Response("b", 1000, "\"other\"", "Wed, 03 Jan 2024 00:00:00 GMT"), error));
  // ... but each must agree with itself, and both with the size.
  EXPECT_FALSE(replicas.Agrees(Response("b", 1000, "\"changed\"", ""), error));
  EXPECT_FALSE(replicas.Agrees(Response("c", 999, "", ""), error));
}

TEST(XrdHttpTpcBlockPullTests, LastModifiedDecidesWheneverNoETagWasCompared) {
  std::string error;
  // The server gave a strong ETag once; a later response without one is
  // judged by its Last-Modified.
  SourceCheck later(1000);
  EXPECT_TRUE(later.Agrees(Response("a", 1000, "\"v1\"", "Mon, 01 Jan 2024 00:00:00 GMT"), error));
  EXPECT_FALSE(later.Agrees(Response("a", 1000, "", "Tue, 02 Jan 2024 00:00:00 GMT"), error));
  EXPECT_NE(std::string::npos, error.find("Last-Modified")) << error;

  // The first strong ETag of a server has nothing to be compared with: the
  // Last-Modified that comes with it is.
  SourceCheck first(1000);
  EXPECT_TRUE(first.Agrees(Response("a", 1000, "", "Mon, 01 Jan 2024 00:00:00 GMT"), error));
  EXPECT_FALSE(first.Agrees(Response("a", 1000, "\"v1\"", "Tue, 02 Jan 2024 00:00:00 GMT"), error));
}

TEST(XrdHttpTpcBlockPullTests, WithoutValidatorsOnlyTheSizeIsCompared) {
  std::string error;
  SourceCheck check(1000, false);
  EXPECT_TRUE(check.Agrees(Response("a", 1000, "\"v1\"", "Mon, 01 Jan 2024 00:00:00 GMT"), error));
  EXPECT_TRUE(check.Agrees(Response("a", 1000, "\"v2\"", "Tue, 02 Jan 2024 00:00:00 GMT"), error));
  EXPECT_FALSE(check.Agrees(Response("a", 1001, "\"v2\"", ""), error));
}

TEST(XrdHttpTpcBlockPullTests, WhatTheSourceSentIsMadeFitForAMessage) {
  std::string error;
  SourceCheck check(1000);
  const std::string first = "\"" + std::string(500, 'a') + "\"";
  const std::string second = "\"x\ry\tz\"";
  EXPECT_TRUE(check.Agrees(Response("", 1000, first.c_str(), ""), error));
  EXPECT_FALSE(check.Agrees(Response("", 1000, second.c_str(), ""), error));
  // Long values are cut, control characters replaced, and a server that is
  // not known is not named.
  EXPECT_LT(error.size(), 400u) << error;
  EXPECT_NE(std::string::npos, error.find("...")) << error;
  EXPECT_NE(std::string::npos, error.find("\"x?y?z\"")) << error;
  EXPECT_EQ(std::string::npos, error.find("server")) << error;
  EXPECT_EQ(std::string::npos, error.find('\r')) << error;
}

TEST(XrdHttpTpcBlockPullTests, ARequestIsAllowedLongerUntilItsFirstByte) {
  EXPECT_EQ(120, StallWatch::Allowed(5000, 5000, 120, 60));
  EXPECT_EQ(60, StallWatch::Allowed(5001, 5000, 120, 60));
}
