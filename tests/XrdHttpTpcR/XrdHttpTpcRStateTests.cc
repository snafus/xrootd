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
#include "XrdHttpTpcR/XrdHttpTpcRStream.hh"
#include "XrdHttpTpcRMockSfsFile.hh"

#include "XrdSys/XrdSysError.hh"
#include "XrdSys/XrdSysLogger.hh"

#include <gtest/gtest.h>

#include <curl/curl.h>

#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

// Named friend of TPCR::State (see XrdHttpTpcRState.hh): lets the tests feed
// raw header lines through the private parser, deliver body bytes through the
// libcurl write callback, and read private fields without production-visible
// setters.
struct StateTestPeer {
  static int Header(TPCR::State &state, const std::string &line) {
    return state.Header(line);
  }
  // Delivers body bytes exactly as libcurl would (through WriteCB).
  static size_t WriteBody(TPCR::State &state, const std::string &body) {
    // libcurl hands a non-const buffer; the callback never modifies it.
    return TPCR::State::WriteCB(const_cast<char *>(body.data()), 1,
                                body.size(), &state);
  }
  static off_t StartOffset(const TPCR::State &state) {
    return state.m_start_offset;
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
  // return to their constructed values.  (The transfer-level content length
  // set by the handler is NOT per-request state and survives -- see the
  // checklist comment in XrdHttpTpcRState.cc.)
  TPCR::State state(m_curl, false);
  state.SetContentLength(999);
  FeedResponse(state, {"Content-Length: 12345\r\n",
                       "Repr-Digest: adler=:c4Ki0g==:\r\n"});
  ASSERT_EQ(state.GetStatusCode(), 206);
  ASSERT_EQ(state.GetReportedLength(), 12345);
  ASSERT_EQ(state.GetContentLength(), 999)
      << "response Content-Length must not overwrite the transfer's length (BUG-3)";
  ASSERT_FALSE(state.GetReprDigest().empty());

  state.ResetAfterRequest();

  EXPECT_EQ(state.GetStatusCode(), -1);
  EXPECT_EQ(state.GetReportedLength(), -1);
  EXPECT_EQ(state.GetContentLength(), 999) << "transfer-level, not per-request";
  EXPECT_TRUE(state.GetReprDigest().empty());
  EXPECT_EQ(state.BytesTransferred(), 0);
  EXPECT_FALSE(state.RangeRequested());
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

TEST_F(XrdHttpTpcRStateTests, InstallHandlersRestoresOptionsAfterReset) {
  // T-U8 (SUB-5): curl_easy_reset wipes EVERY option; the ConfigureHandle
  // sequence (of which InstallHandlers + RebindHeaders is the State-owned
  // part) must restore them.  CURLOPT_PRIVATE is the one introspectable
  // option, so it stands proxy for the set here; the full behavioral check
  // (callbacks actually firing after a reset) runs in the integration
  // fault-injection tests, where handles are reset by real retries.
  // Stale CURLOPT_RANGE is impossible structurally: the only place a range
  // is set is SetTransferParameters, called at every single issue.
  TPCR::State state(m_curl, false);
  char *priv = nullptr;
  ASSERT_EQ(CURLE_OK, curl_easy_getinfo(m_curl, CURLINFO_PRIVATE, &priv));
  EXPECT_EQ(reinterpret_cast<TPCR::State *>(priv), &state)
      << "InstallHandlers must set CURLOPT_PRIVATE";

  curl_easy_reset(m_curl);
  priv = nullptr;
  ASSERT_EQ(CURLE_OK, curl_easy_getinfo(m_curl, CURLINFO_PRIVATE, &priv));
  EXPECT_EQ(nullptr, priv) << "reset must have wiped the options";

  state.InstallHandlers(m_curl);
  state.RebindHeaders();
  ASSERT_EQ(CURLE_OK, curl_easy_getinfo(m_curl, CURLINFO_PRIVATE, &priv));
  EXPECT_EQ(reinterpret_cast<TPCR::State *>(priv), &state)
      << "re-install must restore the options";
}

// ---------------------------------------------------------------------------
// T-U7: range-response validation (FR-8, FR-9; BUG-2, BUG-3).
//
// These drive a real transfer State (with a memory-backed Stream) through
// the same callbacks libcurl uses, for each malformed-source scenario.
// ---------------------------------------------------------------------------

class XrdHttpTpcRStateRangeTests : public XrdHttpTpcRStateTests {
protected:
  void SetUp() override {
    XrdHttpTpcRStateTests::SetUp();
    m_logger = std::make_unique<XrdSysLogger>(STDERR_FILENO, 0);
    m_log = std::make_unique<XrdSysError>(m_logger.get(), "StateRangeTest");
    auto file = std::make_unique<MemorySfsFile>();
    m_file = file.get();
    m_stream = std::make_unique<TPCR::Stream>(std::move(file), 0, 64, *m_log);
  }

  // A transfer state with a pending ranged request [offset, offset+length).
  std::unique_ptr<TPCR::State> RangedState(off_t offset, size_t length) {
    auto state = std::make_unique<TPCR::State>(0, *m_stream, m_curl,
                                               /*push=*/false,
                                               /*tpcForwardCreds=*/false);
    state->SetTransferParameters(offset, length);
    return state;
  }

  // Feeds a status line + headers, mimicking a source's range response.
  static void Respond(TPCR::State &state, const std::string &status_line,
                      const std::vector<std::string> &headers) {
    ASSERT_GT(StateTestPeer::Header(state, status_line), 0);
    for (const auto &header : headers) {
      ASSERT_GT(StateTestPeer::Header(state, header), 0);
    }
  }

  std::unique_ptr<XrdSysLogger> m_logger;
  std::unique_ptr<XrdSysError> m_log;
  MemorySfsFile *m_file = nullptr;
  std::unique_ptr<TPCR::Stream> m_stream;
};

TEST_F(XrdHttpTpcRStateRangeTests, Status200ToRangedRequestIsRejected) {
  // BUG-2's worst case: the source ignores Range and streams the whole file.
  // Not one body byte may reach the stream (FR-8).
  auto state = RangedState(0, 8);
  Respond(*state, "HTTP/1.1 200 OK\r\n", {"Content-Length: 4096\r\n"});

  EXPECT_EQ(0u, StateTestPeer::WriteBody(*state, "garbagegarbage"));
  EXPECT_EQ(TPCR::State::errRangeNotHonored, state->GetErrorCode());
  EXPECT_NE(std::string::npos,
            state->GetErrorMessage().find("does not honor Range requests"));
  EXPECT_TRUE(m_file->Writes().empty()) << "no byte may reach the file";
}

TEST_F(XrdHttpTpcRStateRangeTests, MissingContentRangeIsRejected) {
  auto state = RangedState(0, 8);
  Respond(*state, "HTTP/1.1 206 Partial Content\r\n", {"Content-Length: 8\r\n"});

  EXPECT_EQ(0u, StateTestPeer::WriteBody(*state, "abcdefgh"));
  EXPECT_EQ(TPCR::State::errRangeMismatch, state->GetErrorCode());
  EXPECT_NE(std::string::npos,
            state->GetErrorMessage().find("no Content-Range"));
}

TEST_F(XrdHttpTpcRStateRangeTests, MismatchedContentRangeIsRejected) {
  // The source answered a *different* range: writing it at our start offset
  // would corrupt the file (FR-8: exact echo required).
  auto state = RangedState(0, 8);
  Respond(*state, "HTTP/1.1 206 Partial Content\r\n",
          {"Content-Range: bytes 8-15/4096\r\n", "Content-Length: 8\r\n"});

  EXPECT_EQ(0u, StateTestPeer::WriteBody(*state, "abcdefgh"));
  EXPECT_EQ(TPCR::State::errRangeMismatch, state->GetErrorCode());
  EXPECT_NE(std::string::npos,
            state->GetErrorMessage().find("does not echo the requested range"));
}

TEST_F(XrdHttpTpcRStateRangeTests, ContentLengthMismatchIsRejected) {
  // FR-9 / BUG-3: the response's Content-Length must be validated against
  // the expectation -- never adopted as the expectation.
  auto state = RangedState(0, 8);
  Respond(*state, "HTTP/1.1 206 Partial Content\r\n",
          {"Content-Range: bytes 0-7/4096\r\n", "Content-Length: 4096\r\n"});

  EXPECT_EQ(0u, StateTestPeer::WriteBody(*state, "abcdefgh"));
  EXPECT_EQ(TPCR::State::errLengthMismatch, state->GetErrorCode());
  EXPECT_NE(std::string::npos,
            state->GetErrorMessage().find("does not match the requested range length"));
}

TEST_F(XrdHttpTpcRStateRangeTests, MalformedContentRangeFailsHeaderParse) {
  auto state = RangedState(0, 8);
  ASSERT_GT(StateTestPeer::Header(*state, "HTTP/1.1 206 Partial Content\r\n"), 0);
  // Returning 0 from the header callback aborts the request in libcurl.
  EXPECT_EQ(StateTestPeer::Header(*state, "Content-Range: bytes x-y/z\r\n"), 0);
  EXPECT_EQ(StateTestPeer::Header(*state, "Content-Range: bytes 5-2/10\r\n"), 0)
      << "end below start";
  EXPECT_EQ(StateTestPeer::Header(*state, "Content-Range: bytes 0-7\r\n"), 0)
      << "missing complete-length part";
  EXPECT_EQ(StateTestPeer::Header(*state, "Content-Range: elephants 0-7/8\r\n"), 0)
      << "unknown unit";
}

TEST_F(XrdHttpTpcRStateRangeTests, ValidRangeResponseIsAcceptedAndCompletes) {
  auto state = RangedState(0, 8);
  Respond(*state, "HTTP/1.1 206 Partial Content\r\n",
          {"Content-Range: bytes 0-7/4096\r\n", "Content-Length: 8\r\n"});

  EXPECT_EQ(8u, StateTestPeer::WriteBody(*state, "abcdefgh"));
  EXPECT_TRUE(state->ValidateRangeResponse(true))
      << state->GetErrorMessage();
  EXPECT_EQ(TPCR::State::errNone, state->GetErrorCode());
  // The bytes sit in the reorder buffer until flushed (entry not full).
  EXPECT_EQ(0, m_stream->Flush());
  ASSERT_EQ(1u, m_file->Writes().size());
  EXPECT_EQ(0, m_file->Writes()[0].first);
}

TEST_F(XrdHttpTpcRStateRangeTests, ContentRangeWithWildcardLengthIsAccepted) {
  // "bytes 0-7/*" is valid per RFC 9110; the total is not our yardstick.
  auto state = RangedState(0, 8);
  Respond(*state, "HTTP/1.1 206 Partial Content\r\n",
          {"Content-Range: bytes 0-7/*\r\n"});
  EXPECT_EQ(8u, StateTestPeer::WriteBody(*state, "abcdefgh"));
  EXPECT_TRUE(state->ValidateRangeResponse(true)) << state->GetErrorMessage();
}

TEST_F(XrdHttpTpcRStateRangeTests, OverDeliveryIsRejectedMidBody) {
  auto state = RangedState(0, 8);
  Respond(*state, "HTTP/1.1 206 Partial Content\r\n",
          {"Content-Range: bytes 0-7/4096\r\n"});

  // 12 bytes against an 8-byte range: the excess must never reach the file.
  // The callback signals abort to libcurl by returning anything other than
  // the byte count it was handed.
  EXPECT_NE(12u, StateTestPeer::WriteBody(*state, "abcdefghijkl"));
  EXPECT_EQ(TPCR::State::errLengthMismatch, state->GetErrorCode());
  EXPECT_NE(std::string::npos,
            state->GetErrorMessage().find("more bytes than the requested range"));
  EXPECT_TRUE(m_file->Writes().empty());
}

TEST_F(XrdHttpTpcRStateRangeTests, UnderDeliveryIsCaughtAtCompletion) {
  auto state = RangedState(0, 8);
  Respond(*state, "HTTP/1.1 206 Partial Content\r\n",
          {"Content-Range: bytes 0-7/4096\r\n"});

  EXPECT_EQ(4u, StateTestPeer::WriteBody(*state, "abcd"));
  // Body-start validation passed; the completion check must catch the gap
  // (this is what FinishCurlXfer runs when libcurl reports the request done).
  EXPECT_FALSE(state->ValidateRangeResponse(true));
  EXPECT_EQ(TPCR::State::errLengthMismatch, state->GetErrorCode());
  EXPECT_NE(std::string::npos,
            state->GetErrorMessage().find("different byte count than requested"));
}

TEST_F(XrdHttpTpcRStateRangeTests, NonRangedRequestAccepts200) {
  // The transitional single-stream pull is a whole-object GET: a 200 with
  // no Content-Range is the correct response there.
  auto state = std::make_unique<TPCR::State>(0, *m_stream, m_curl, false, false);
  Respond(*state, "HTTP/1.1 200 OK\r\n", {"Content-Length: 8\r\n"});
  EXPECT_EQ(8u, StateTestPeer::WriteBody(*state, "abcdefgh"));
  EXPECT_TRUE(state->ValidateRangeResponse(true));
  EXPECT_EQ(TPCR::State::errNone, state->GetErrorCode());
}

} // namespace
