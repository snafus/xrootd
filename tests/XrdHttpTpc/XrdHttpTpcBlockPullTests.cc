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
