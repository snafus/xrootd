#undef NDEBUG

#include "XrdHttpTpc/XrdHttpTpcPullOptions.hh"

#include <gtest/gtest.h>

#include <string>

using namespace TPC;

namespace {

const off_t MiB = PullOptions::MiB;
const off_t GiB = PullOptions::GiB;
const off_t TiB = PullOptions::TiB;

PullOptions Validated() {
  PullOptions options;
  std::string error;
  EXPECT_TRUE(options.Validate(error)) << error;
  return options;
}

}

TEST(XrdHttpTpcPullOptionsTests, DefaultsAreValidAndKeepThePresentBehaviour) {
  PullOptions options;
  std::string error = "unset";
  ASSERT_TRUE(options.Validate(error));
  EXPECT_TRUE(error.empty());
  // Nothing changes for a server that sets no http_multi directive.
  EXPECT_EQ(PullOptions::Mode::Sequential, options.mode);
  EXPECT_EQ(100, options.max_streams_request);
  EXPECT_EQ(256 * MiB, options.block_min);
  EXPECT_EQ(4 * GiB, options.block_max);
  EXPECT_EQ(16 * MiB, options.write_size);
}

TEST(XrdHttpTpcPullOptionsTests, BlockBoundsAreRoundedUpToWholeWrites) {
  PullOptions options;
  options.write_size = 16 * MiB;
  options.block_min = 100 * MiB;       // 6.25 writes
  options.block_max = 1000 * MiB;      // 62.5 writes
  std::string error;
  ASSERT_TRUE(options.Validate(error)) << error;
  EXPECT_EQ(112 * MiB, options.block_min);
  EXPECT_EQ(1008 * MiB, options.block_max);
}

TEST(XrdHttpTpcPullOptionsTests, TheMaximumBlockMayBeRaisedForFastPaths) {
  PullOptions options;
  options.block_max = 64 * GiB;
  std::string error;
  EXPECT_TRUE(options.Validate(error)) << error;
  options.block_max = 1 * TiB;
  EXPECT_TRUE(options.Validate(error)) << error;
  options.block_max = 1 * TiB + 16 * MiB;
  EXPECT_FALSE(options.Validate(error));
  EXPECT_NE(std::string::npos, error.find("http_multi.block_size"));
}

TEST(XrdHttpTpcPullOptionsTests, AMaximumAtTheLimitStaysWithinItWhateverTheWriteSize) {
  // 3 MiB does not divide 1 TiB: rounding the maximum up would pass the limit.
  PullOptions options;
  options.write_size = 3 * MiB;
  options.block_min = 30 * MiB;
  options.block_max = 1 * TiB;
  std::string error;
  ASSERT_TRUE(options.Validate(error)) << error;
  EXPECT_LE(options.block_max, PullOptions::MaxBlockSize);
  EXPECT_EQ(0, options.block_max % options.write_size);
  EXPECT_GT(options.block_max, 1 * TiB - 3 * MiB);
  EXPECT_GE(options.block_max, options.block_min);
}

TEST(XrdHttpTpcPullOptionsTests, TheMessageSaysWhichValueIsWrong) {
  std::string error;
  PullOptions request;
  request.max_streams_request = 101;
  EXPECT_FALSE(request.Validate(error));
  EXPECT_NE(std::string::npos, error.find("streams of one request")) << error;

  PullOptions server;
  server.max_stream_threads = 1;
  EXPECT_FALSE(server.Validate(error));
  EXPECT_NE(std::string::npos, error.find("stream threads of the server")) << error;

  PullOptions attempts;
  attempts.retry_attempts = PullOptions::MaxRetryAttempts + 1;
  EXPECT_FALSE(attempts.Validate(error));
  EXPECT_NE(std::string::npos, error.find("attempts")) << error;

  PullOptions minimum;
  minimum.block_min = 2 * TiB;
  minimum.block_max = 2 * TiB;
  EXPECT_FALSE(minimum.Validate(error));
  EXPECT_NE(std::string::npos, error.find("at most")) << error;

  PullOptions defaults;
  EXPECT_EQ(128, defaults.max_stream_threads);
  EXPECT_EQ(3, defaults.retry_attempts);
}

TEST(XrdHttpTpcPullOptionsTests, SettingsThatCannotBeHonouredAreRefusedWithTheDirectiveNamed) {
  struct Case {const char *directive; void (*spoil)(PullOptions &);};
  const Case cases[] = {
    {"http_multi.max_streams", [](PullOptions &o) {o.max_streams_request = 0;}},
    {"http_multi.max_streams", [](PullOptions &o) {o.max_streams_request = 101;}},
    {"http_multi.max_streams", [](PullOptions &o) {o.max_stream_threads = 1;}},
    {"http_multi.max_streams", [](PullOptions &o) {o.max_stream_threads = 5000;}},
    {"http_multi.write_size",  [](PullOptions &o) {o.write_size = 512 * 1024;}},
    {"http_multi.write_size",  [](PullOptions &o) {o.write_size = 128 * MiB;}},
    {"http_multi.write_size",  [](PullOptions &o) {o.write_size = 16 * MiB + 1;}},
    {"http_multi.block_size",  [](PullOptions &o) {o.block_min = 8 * MiB;}},
    {"http_multi.block_size",  [](PullOptions &o) {o.block_max = 128 * MiB;}},
    {"http_multi.retry",       [](PullOptions &o) {o.retry_attempts = 0;}},
    {"http_multi.retry",       [](PullOptions &o) {o.retry_wait = -1;}},
    {"http_multi.retry",       [](PullOptions &o) {o.retry_wait = 301;}},
  };
  for (const Case &c : cases) {
    PullOptions options;
    c.spoil(options);
    std::string error;
    EXPECT_FALSE(options.Validate(error)) << c.directive;
    EXPECT_NE(std::string::npos, error.find(c.directive)) << error;
  }
}

TEST(XrdHttpTpcPullOptionsTests, PlanGivesOneBlockPerStreamWithinTheBounds) {
  const PullOptions options = Validated();

  // Smaller than the minimum block: one request for the whole file.
  BlockPlan plan = PlanBlocks(100 * MiB, 4, options);
  EXPECT_EQ(256 * MiB, plan.block_size);
  EXPECT_EQ(1u, plan.blocks);
  EXPECT_EQ(1u, plan.workers);

  // The minimum applies: fewer workers than streams.
  plan = PlanBlocks(1 * GiB, 8, options);
  EXPECT_EQ(256 * MiB, plan.block_size);
  EXPECT_EQ(4u, plan.blocks);
  EXPECT_EQ(4u, plan.workers);

  // One block per stream, rounded up to a whole number of 16 MiB writes.
  plan = PlanBlocks(10024648704LL, 8, options);
  EXPECT_EQ(1200 * MiB, plan.block_size);
  EXPECT_EQ(8u, plan.blocks);
  EXPECT_EQ(8u, plan.workers);

  // The maximum applies: several blocks per stream.
  plan = PlanBlocks(1 * TiB, 8, options);
  EXPECT_EQ(4 * GiB, plan.block_size);
  EXPECT_EQ(256u, plan.blocks);
  EXPECT_EQ(8u, plan.workers);

  plan = PlanBlocks(10 * TiB, 8, options);
  EXPECT_EQ(4 * GiB, plan.block_size);
  EXPECT_EQ(2560u, plan.blocks);
  EXPECT_EQ(8u, plan.workers);
}

TEST(XrdHttpTpcPullOptionsTests, PlanCoversTheFileExactly) {
  const PullOptions options = Validated();
  const off_t sizes[] = {1, 16 * MiB - 1, 16 * MiB, 256 * MiB, 256 * MiB + 1, 512 * MiB,
                         3 * GiB + 12345, 32 * GiB, 32 * GiB + 1, 100 * GiB + 7};
  for (off_t size : sizes) {
    for (size_t streams : {1u, 2u, 3u, 4u, 8u, 10u, 100u}) {
      const BlockPlan plan = PlanBlocks(size, streams, options);
      ASSERT_GT(plan.block_size, 0) << size << " " << streams;
      // A whole number of writes, within the bounds.
      EXPECT_EQ(0, plan.block_size % options.write_size);
      EXPECT_GE(plan.block_size, options.block_min);
      EXPECT_LE(plan.block_size, options.block_max);
      // The blocks cover the file and the last one is not empty.
      EXPECT_GE(static_cast<off_t>(plan.blocks) * plan.block_size, size);
      EXPECT_LT(static_cast<off_t>(plan.blocks - 1) * plan.block_size, size);
      // Never more workers than streams or than blocks, and at least one.
      EXPECT_GE(plan.workers, 1u);
      EXPECT_LE(plan.workers, streams);
      EXPECT_LE(plan.workers, plan.blocks);
    }
  }
}

TEST(XrdHttpTpcPullOptionsTests, PlanOfNothingIsEmpty) {
  const PullOptions options = Validated();
  for (const BlockPlan &plan : {PlanBlocks(0, 4, options), PlanBlocks(-1, 4, options),
                                PlanBlocks(1 * GiB, 0, options)}) {
    EXPECT_EQ(0, plan.block_size);
    EXPECT_EQ(0u, plan.blocks);
    EXPECT_EQ(0u, plan.workers);
  }
}

TEST(XrdHttpTpcPullOptionsTests, PlanFollowsOtherWriteAndBlockSizes) {
  PullOptions options;
  options.write_size = 4 * MiB;
  options.block_min = 64 * MiB;
  options.block_max = 16 * GiB;
  std::string error;
  ASSERT_TRUE(options.Validate(error)) << error;

  // 100 GiB over 8 streams: 12.5 GiB each, a whole number of 4 MiB writes.
  BlockPlan plan = PlanBlocks(100 * GiB, 8, options);
  EXPECT_EQ(12800 * MiB, plan.block_size);
  EXPECT_EQ(8u, plan.blocks);
  EXPECT_EQ(8u, plan.workers);

  // 1 TiB over 8 streams would be 128 GiB each: held to 16 GiB.
  plan = PlanBlocks(1 * TiB, 8, options);
  EXPECT_EQ(16 * GiB, plan.block_size);
  EXPECT_EQ(64u, plan.blocks);
}
