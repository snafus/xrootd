#undef NDEBUG

// T-U3 (05-TEST-PLAN.md): tpcr.* configuration parsing (FR-30).
//   - Every directive parses with its documented default.
//   - Unknown tpcr.* directives and invalid values are rejected (the handler
//     treats a false return from Set() as fatal at init -- fail fast).
//   - The X-Number-Of-Streams clamp (NFR-1, BUG-10) behaves per spec.
//
// Directives are added to these tests in the same WP that implements them,
// so tests and implementation cannot drift apart; the trace_check gate keeps
// FR-30 referenced here.

#include "XrdHttpTpcR/XrdHttpTpcRConfig.hh"

#include <gtest/gtest.h>

#include <string>

using TPCR::Config;

TEST(XrdHttpTpcRConfigTests, DocumentedDefaults) {
  Config config;
  EXPECT_EQ(16u * 1024 * 1024, config.block_size);
  EXPECT_EQ(4ULL * 1024 * 1024 * 1024, config.mempool_max);
  EXPECT_EQ(16u, config.streams_max);
  EXPECT_EQ(256u * 1024 * 1024, config.window_bytes);
  EXPECT_EQ(5u, config.retry_max);
  EXPECT_EQ(60u, config.range_timeout);
  EXPECT_EQ(120u, config.recovery_maxsecs);  // FR-16 default
}

TEST(XrdHttpTpcRConfigTests, SetParsesEveryDirective) {
  Config config;
  std::string err;
  EXPECT_TRUE(config.Set("tpcr.blocksize", "32m", err)) << err;
  EXPECT_EQ(32u * 1024 * 1024, config.block_size);
  EXPECT_TRUE(config.Set("tpcr.mempool.max", "8g", err)) << err;
  EXPECT_EQ(8ULL * 1024 * 1024 * 1024, config.mempool_max);
  EXPECT_TRUE(config.Set("tpcr.streams.max", "32", err)) << err;
  EXPECT_EQ(32u, config.streams_max);
  EXPECT_TRUE(config.Set("tpcr.window.bytes", "64m", err)) << err;
  EXPECT_EQ(64u * 1024 * 1024, config.window_bytes);
  EXPECT_TRUE(config.Set("tpcr.retry.max", "3", err)) << err;
  EXPECT_EQ(3u, config.retry_max);
  EXPECT_TRUE(config.Set("tpcr.range.timeout", "2m", err)) << err;
  EXPECT_EQ(120u, config.range_timeout);
  EXPECT_TRUE(config.Set("tpcr.recovery.maxsecs", "90s", err)) << err;
  EXPECT_EQ(90u, config.recovery_maxsecs);
}

TEST(XrdHttpTpcRConfigTests, UnknownDirectiveIsFatal) {
  Config config;
  std::string err;
  // FR-30: a typo must fail init, not silently fall back to a default.
  EXPECT_FALSE(config.Set("tpcr.blocksze", "32m", err));
  EXPECT_NE(std::string::npos, err.find("unknown"));
}

TEST(XrdHttpTpcRConfigTests, InvalidValuesAreFatal) {
  Config config;
  std::string err;
  EXPECT_FALSE(config.Set("tpcr.blocksize", "banana", err));
  EXPECT_FALSE(config.Set("tpcr.blocksize", "12x", err));
  EXPECT_FALSE(config.Set("tpcr.blocksize", "-4m", err));
  EXPECT_FALSE(config.Set("tpcr.blocksize", "4k", err))
      << "below the 1 MiB floor (MB-aligned direct writes)";
  EXPECT_FALSE(config.Set("tpcr.blocksize", "2g", err))
      << "above the 1 GiB sanity ceiling";
  EXPECT_FALSE(config.Set("tpcr.mempool.max", "0", err));
  EXPECT_FALSE(config.Set("tpcr.streams.max", "0", err));
  EXPECT_FALSE(config.Set("tpcr.streams.max", "4096", err));
  EXPECT_FALSE(config.Set("tpcr.retry.max", "999", err));
  EXPECT_FALSE(config.Set("tpcr.range.timeout", "1s", err));
  EXPECT_FALSE(config.Set("tpcr.recovery.maxsecs", "2s", err));
  // Defaults must survive every failed Set.
  EXPECT_EQ(16u * 1024 * 1024, config.block_size);
  EXPECT_EQ(16u, config.streams_max);
}

TEST(XrdHttpTpcRConfigTests, SizeParser) {
  uint64_t out = 0;
  EXPECT_TRUE(Config::ParseSize("1024", out));
  EXPECT_EQ(1024u, out);
  EXPECT_TRUE(Config::ParseSize("16m", out));
  EXPECT_EQ(16u * 1024 * 1024, out);
  EXPECT_TRUE(Config::ParseSize("16M", out));
  EXPECT_EQ(16u * 1024 * 1024, out);
  EXPECT_TRUE(Config::ParseSize("2k", out));
  EXPECT_EQ(2048u, out);
  EXPECT_TRUE(Config::ParseSize("4G", out));
  EXPECT_EQ(4ULL * 1024 * 1024 * 1024, out);
  EXPECT_FALSE(Config::ParseSize("", out));
  EXPECT_FALSE(Config::ParseSize("m", out));
  EXPECT_FALSE(Config::ParseSize("1.5g", out));
  EXPECT_FALSE(Config::ParseSize("12mm", out));
  EXPECT_FALSE(Config::ParseSize("99999999999999999999999", out)) << "overflow";
}

TEST(XrdHttpTpcRConfigTests, DurationParser) {
  uint64_t out = 0;
  EXPECT_TRUE(Config::ParseDuration("120", out));
  EXPECT_EQ(120u, out);
  EXPECT_TRUE(Config::ParseDuration("90s", out));
  EXPECT_EQ(90u, out);
  EXPECT_TRUE(Config::ParseDuration("2m", out));
  EXPECT_EQ(120u, out);
  EXPECT_TRUE(Config::ParseDuration("24h", out));
  EXPECT_EQ(86400u, out);
  EXPECT_FALSE(Config::ParseDuration("", out));
  EXPECT_FALSE(Config::ParseDuration("h", out));
  EXPECT_FALSE(Config::ParseDuration("1d", out));
}

TEST(XrdHttpTpcRConfigTests, BoolParser) {
  bool out = false;
  EXPECT_TRUE(Config::ParseBool("yes", out));
  EXPECT_TRUE(out);
  EXPECT_TRUE(Config::ParseBool("0", out));
  EXPECT_FALSE(out);
  EXPECT_TRUE(Config::ParseBool("true", out));
  EXPECT_TRUE(out);
  EXPECT_FALSE(Config::ParseBool("maybe", out));
}

TEST(XrdHttpTpcRConfigTests, StreamsClamp) {
  Config config;
  bool clamped = true;
  // 0 = "use the default": one stream, not a clamp.
  EXPECT_EQ(1u, config.ClampStreams(0, clamped));
  EXPECT_FALSE(clamped);
  EXPECT_EQ(8u, config.ClampStreams(8, clamped));
  EXPECT_FALSE(clamped);
  EXPECT_EQ(16u, config.ClampStreams(16, clamped));
  EXPECT_FALSE(clamped);
  // BUG-10: 100 used to allocate 25.6 GiB; now it clamps to the cap, and the
  // clamp is reported so the handler can log it.
  EXPECT_EQ(16u, config.ClampStreams(100, clamped));
  EXPECT_TRUE(clamped);
}
