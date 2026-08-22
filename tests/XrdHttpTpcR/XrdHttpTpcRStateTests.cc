#undef NDEBUG

// T-U2 "State hygiene" (05-TEST-PLAN.md): regression tests for the WP-0
// bug-fix ports in XrdHttpTpcRState.cc.
//   - BUG-4: State::Move must transfer the Repr-Digest map from the source
//     state to the destination (the stock code overwrote the source's map
//     with the destination's empty one, losing the digests).
//   - BUG-5: State::ResetAfterRequest must reconstruct *every* per-request
//     field, including the error buffer/code the stock code left behind.
// Plus header-parser behavior on malformed inputs, which both fixes depend on.

#include "XrdHttpTpcR/XrdHttpTpcRState.hh"

#include <gtest/gtest.h>

#include <curl/curl.h>

#include <string>
#include <vector>

// Named friend of TPCR::State (see XrdHttpTpcRState.hh): lets the tests feed
// raw header lines through the private parser and read private fields without
// production-visible setters.
struct StateTestPeer {
  static int Header(TPCR::State &state, const std::string &line) {
    return state.Header(line);
  }
  static off_t StartOffset(const TPCR::State &state) {
    return state.m_start_offset;
  }
  static off_t ContentLength(const TPCR::State &state) {
    return state.m_content_length;
  }
};

namespace {

// A State needs a real curl easy handle (the constructors install callbacks on
// it); no network activity ever happens in these tests.
class XrdHttpTpcRStateTests : public ::testing::Test {
protected:
  void SetUp() override {
    m_curl = curl_easy_init();
    ASSERT_NE(m_curl, nullptr);
    m_other_curl = curl_easy_init();
    ASSERT_NE(m_other_curl, nullptr);
  }

  void TearDown() override {
    curl_easy_cleanup(m_curl);
    curl_easy_cleanup(m_other_curl);
  }

  // Drives the private header parser as libcurl would: status line first,
  // then individual header lines.
  static void FeedResponse(TPCR::State &state,
                           const std::vector<std::string> &headers) {
    ASSERT_GT(StateTestPeer::Header(state, "HTTP/1.1 206 Partial Content\r\n"),
              0);
    for (const auto &header : headers) {
      ASSERT_GT(StateTestPeer::Header(state, header), 0);
    }
  }

  CURL *m_curl = nullptr;
  CURL *m_other_curl = nullptr;
};

TEST_F(XrdHttpTpcRStateTests, MovePreservesReprDigests) {
  // BUG-4: digests parsed from the source's HEAD response must survive the
  // Move into the long-lived state object.
  TPCR::State source(m_curl, false);
  FeedResponse(source, {"Repr-Digest: adler=:c4Ki0g==:\r\n"});
  ASSERT_FALSE(source.GetReprDigest().empty())
      << "test precondition: the parser must have accepted the digest header";
  const auto expected = source.GetReprDigest();

  TPCR::State destination;
  destination.Move(source);

  EXPECT_EQ(destination.GetReprDigest(), expected)
      << "Move lost the Repr-Digest map (BUG-4 regression)";
  EXPECT_TRUE(source.GetReprDigest().empty())
      << "moved-from state should no longer own the digests";
}

TEST_F(XrdHttpTpcRStateTests, ResetAfterRequestClearsErrorState) {
  // BUG-5: a reused handle must not carry the previous request's error.
  TPCR::State state(m_curl, false);
  state.SetErrorCode(TPCR::State::errWrite);
  state.SetErrorMessage("stale error from a previous range");

  state.ResetAfterRequest();

  EXPECT_EQ(state.GetErrorCode(), TPCR::State::errNone)
      << "error code survived ResetAfterRequest (BUG-5 regression)";
  EXPECT_TRUE(state.GetErrorMessage().empty())
      << "error text survived ResetAfterRequest (BUG-5 regression)";
}

TEST_F(XrdHttpTpcRStateTests, ResetAfterRequestClearsResponseFields) {
  // The rest of the ResetAfterRequest checklist: response-derived fields must
  // return to their constructed values.
  TPCR::State state(m_curl, false);
  FeedResponse(state, {"Content-Length: 12345\r\n",
                       "Repr-Digest: adler=:c4Ki0g==:\r\n"});
  ASSERT_EQ(state.GetStatusCode(), 206);
  ASSERT_EQ(state.GetContentLength(), 12345);
  ASSERT_FALSE(state.GetReprDigest().empty());

  state.ResetAfterRequest();

  EXPECT_EQ(state.GetStatusCode(), -1);
  EXPECT_EQ(state.GetContentLength(), -1);
  EXPECT_TRUE(state.GetReprDigest().empty());
  EXPECT_EQ(state.BytesTransferred(), 0);
}

TEST_F(XrdHttpTpcRStateTests, HeaderParserRejectsMalformedInput) {
  // A non-empty line after the status line with no ':' separator is malformed
  // and must be rejected (parser returns 0, making libcurl abort).
  TPCR::State state(m_curl, false);
  ASSERT_GT(StateTestPeer::Header(state, "HTTP/1.1 200 OK\r\n"), 0);
  EXPECT_EQ(StateTestPeer::Header(state, "this is not a header line\r\n"), 0);

  // Unparseable status code.
  TPCR::State bad_status(m_other_curl, false);
  EXPECT_EQ(StateTestPeer::Header(bad_status, "HTTP/1.1 abc\r\n"), 0);
}

TEST_F(XrdHttpTpcRStateTests, HeaderParserRejectsGarbageContentLength) {
  TPCR::State state(m_curl, false);
  ASSERT_GT(StateTestPeer::Header(state, "HTTP/1.1 200 OK\r\n"), 0);
  EXPECT_EQ(StateTestPeer::Header(state, "Content-Length: banana\r\n"), 0);
}

} // namespace
