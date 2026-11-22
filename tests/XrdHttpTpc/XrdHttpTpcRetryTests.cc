#undef NDEBUG

#include "XrdHttpTpc/XrdHttpTpcRetry.hh"

#include "XrdHttpTpc/XrdHttpTpcBlockPull.hh"
#include "XrdHttpTpc/XrdHttpTpcState.hh"

#include <gtest/gtest.h>

using namespace TPC;

namespace {

RequestResult Result(CURLcode curl_result, int status_code, int error_code = 0) {
  RequestResult result;
  result.curl_result = curl_result;
  result.status_code = status_code;
  result.error_code = error_code;
  return result;
}

}

// -- which failures are tried again ------------------------------------------

TEST(XrdHttpTpcRetryTests, ARequestThatBroughtItsRangeIsDone) {
  EXPECT_EQ(NextStep::Done, AfterRequest(Result(CURLE_OK, 206)));
  // A 200 or 203 that the request code let pass: the whole file, where the
  // range asked for was the whole file.
  EXPECT_EQ(NextStep::Done, AfterRequest(Result(CURLE_OK, 200)));
  EXPECT_EQ(NextStep::Done, AfterRequest(Result(CURLE_OK, 203)));
}

TEST(XrdHttpTpcRetryTests, AFailureOfTheConnectionIsRetried) {
  // No response at all, and a response cut part way.
  for (CURLcode result : {CURLE_COULDNT_CONNECT, CURLE_COULDNT_RESOLVE_HOST, CURLE_RECV_ERROR,
                          CURLE_SEND_ERROR, CURLE_GOT_NOTHING, CURLE_PARTIAL_FILE,
                          CURLE_OPERATION_TIMEDOUT, CURLE_SSL_CONNECT_ERROR,
                          CURLE_HTTP2, CURLE_WEIRD_SERVER_REPLY, CURLE_UNSUPPORTED_PROTOCOL}) {
    EXPECT_EQ(NextStep::Retry, AfterRequest(Result(result, -1))) << curl_easy_strerror(result);
    EXPECT_EQ(NextStep::Retry, AfterRequest(Result(result, 206))) << curl_easy_strerror(result);
  }
  // Headers or a body that the request code refused without recording why.
  EXPECT_EQ(NextStep::Retry, AfterRequest(Result(CURLE_WRITE_ERROR, 206)));
  EXPECT_EQ(NextStep::Retry, AfterRequest(Result(CURLE_WRITE_ERROR, -1)));
}

TEST(XrdHttpTpcRetryTests, AStatusIsRetriedUnlessItIsKnownToBeFinal) {
  // The body of an error is read, so libcurl reports no error; or it is cut
  // off when it is long, which libcurl reports as a write error; or libcurl
  // was told to fail on such a status, and says so.
  for (int status : {408, 409, 421, 423, 425, 429, 500, 502, 503, 504, 507, 509, 520, 599,
                     418, 499}) {
    EXPECT_EQ(NextStep::Retry, AfterRequest(Result(CURLE_OK, status))) << status;
    EXPECT_EQ(NextStep::Retry, AfterRequest(Result(CURLE_WRITE_ERROR, status))) << status;
    EXPECT_EQ(NextStep::Retry, AfterRequest(Result(CURLE_HTTP_RETURNED_ERROR, status))) << status;
    EXPECT_FALSE(StatusIsFinal(status)) << status;
  }
  for (int status : {400, 401, 403, 404, 405, 406, 407, 410, 411, 412, 413, 414, 415, 416,
                     417, 426, 431, 451, 501, 505}) {
    EXPECT_EQ(NextStep::Fail, AfterRequest(Result(CURLE_OK, status))) << status;
    EXPECT_EQ(NextStep::Fail, AfterRequest(Result(CURLE_WRITE_ERROR, status))) << status;
    EXPECT_EQ(NextStep::Fail, AfterRequest(Result(CURLE_HTTP_RETURNED_ERROR, status))) << status;
    EXPECT_TRUE(StatusIsFinal(status)) << status;
  }
}

TEST(XrdHttpTpcRetryTests, AFinalStatusWithoutTheFileIsRetried) {
  // libcurl reports no error for these; the cause is not known, so it is
  // not taken for permanent.
  for (int status : {204, 205, 300, 302, 304, 307}) {
    EXPECT_EQ(NextStep::Retry, AfterRequest(Result(CURLE_OK, status))) << status;
  }
  // A redirection that was followed, and the next server then failed: it is
  // the failure of the connection that counts.
  EXPECT_EQ(NextStep::Retry, AfterRequest(Result(CURLE_COULDNT_CONNECT, 307)));
}

TEST(XrdHttpTpcRetryTests, WhatLibcurlCannotDoIsNotRetried) {
  for (CURLcode result : {CURLE_OUT_OF_MEMORY, CURLE_FAILED_INIT, CURLE_URL_MALFORMAT,
                          CURLE_NOT_BUILT_IN, CURLE_TOO_MANY_REDIRECTS, CURLE_RANGE_ERROR,
                          CURLE_PEER_FAILED_VERIFICATION, CURLE_SSL_CERTPROBLEM,
                          CURLE_SSL_CACERT_BADFILE, CURLE_LOGIN_DENIED}) {
    EXPECT_EQ(NextStep::Fail, AfterRequest(Result(result, -1))) << curl_easy_strerror(result);
    EXPECT_TRUE(CurlResultIsFinal(result)) << curl_easy_strerror(result);
  }
  EXPECT_FALSE(CurlResultIsFinal(CURLE_OK));
  EXPECT_FALSE(CurlResultIsFinal(CURLE_SSL_CONNECT_ERROR));
}

TEST(XrdHttpTpcRetryTests, WhatTheRequestCodeFoundDecidesBeforeTheStatusAndLibcurl) {
  // The stall check of the stream ended the request.
  EXPECT_EQ(NextStep::Retry, AfterRequest(Result(CURLE_ABORTED_BY_CALLBACK, 206, State::errTimeout)));
  EXPECT_EQ(NextStep::Retry, AfterRequest(Result(CURLE_ABORTED_BY_CALLBACK, -1, State::errTimeout)));
  // A body shorter than the range, or another range than asked.  (A body
  // longer than its range has the same code; the stream thread ends it.)
  EXPECT_EQ(NextStep::Retry, AfterRequest(Result(CURLE_OK, 206, State::errRange)));
  EXPECT_EQ(NextStep::Retry, AfterRequest(Result(CURLE_WRITE_ERROR, 206, State::errRange)));
  // A 204 in answer to a range: not the range, and the cause is not known.
  EXPECT_EQ(NextStep::Retry, AfterRequest(Result(CURLE_WRITE_ERROR, 204, State::errRange)));
  // The whole file in answer to a range: the source does not serve ranges.
  EXPECT_EQ(NextStep::Fail, AfterRequest(Result(CURLE_WRITE_ERROR, 200, State::errRange)));
  EXPECT_EQ(NextStep::Fail, AfterRequest(Result(CURLE_WRITE_ERROR, 203, State::errRange)));
  // The source file changed.
  EXPECT_EQ(NextStep::Fail, AfterRequest(Result(CURLE_WRITE_ERROR, 206, State::errSource)));
  // This server: the storage, or an internal error.
  EXPECT_EQ(NextStep::Fail, AfterRequest(Result(CURLE_WRITE_ERROR, 206, State::errWrite)));
  EXPECT_EQ(NextStep::Fail, AfterRequest(Result(CURLE_OK, 206, State::errFlush)));
  EXPECT_EQ(NextStep::Fail, AfterRequest(Result(CURLE_OK, 206, State::errClose)));
  EXPECT_EQ(NextStep::Fail, AfterRequest(Result(CURLE_OK, 206, BlockPull::errInternal)));
  // A code that is not known is not taken for something the source can cure.
  EXPECT_EQ(NextStep::Fail, AfterRequest(Result(CURLE_OK, 206, 77)));
}

// -- how many tries a block gets ---------------------------------------------

TEST(XrdHttpTpcRetryTests, ABlockGetsItsAttemptsWithoutProgress) {
  RetryPolicy tries(3, 10, 2);
  EXPECT_EQ(0, tries.Tries());
  EXPECT_TRUE(tries.Another(false));   // the first try failed: a second
  EXPECT_TRUE(tries.Another(false));   // the second failed: a third
  EXPECT_FALSE(tries.Another(false));  // the third failed: no more
  EXPECT_EQ(3, tries.Tries());
}

TEST(XrdHttpTpcRetryTests, OneAttemptMeansNoRetry) {
  RetryPolicy tries(1, 10, 2);
  EXPECT_FALSE(tries.Another(false));
  EXPECT_EQ(1, tries.Tries());
  // Not even when the try brought data, and whatever the cap.
  RetryPolicy with_progress(1, 10, 2);
  EXPECT_FALSE(with_progress.Another(true));
}

TEST(XrdHttpTpcRetryTests, ProgressSetsTheCountBack) {
  RetryPolicy tries(3, 100, 2);
  EXPECT_TRUE(tries.Another(false));
  EXPECT_TRUE(tries.Another(false));
  EXPECT_TRUE(tries.Another(true));    // the third brought data
  EXPECT_TRUE(tries.Another(false));
  EXPECT_TRUE(tries.Another(false));
  EXPECT_FALSE(tries.Another(false));  // three in a row without
  EXPECT_EQ(6, tries.Tries());
}

TEST(XrdHttpTpcRetryTests, TheCapHoldsWhateverTheProgress) {
  RetryPolicy tries(3, 5, 2);
  for (int idx = 0; idx < 4; idx++) {EXPECT_TRUE(tries.Another(true)) << idx;}
  EXPECT_FALSE(tries.Another(true));
  EXPECT_EQ(5, tries.Tries());
}

TEST(XrdHttpTpcRetryTests, TheWaitGrowsWithoutProgressAndStopsGrowing) {
  RetryPolicy tries(20, 100, 2);
  const long expected[] = {2000, 4000, 8000, 16000, 30000, 30000, 30000};
  for (long wait : expected) {
    ASSERT_TRUE(tries.Another(false));
    EXPECT_EQ(wait, tries.WaitMilliseconds(-1, 0.0));
  }
  // Progress: back to the first wait.
  ASSERT_TRUE(tries.Another(true));
  EXPECT_EQ(2000, tries.WaitMilliseconds(-1, 0.0));
  ASSERT_TRUE(tries.Another(false));
  EXPECT_EQ(2000, tries.WaitMilliseconds(-1, 0.0));
  ASSERT_TRUE(tries.Another(false));
  EXPECT_EQ(4000, tries.WaitMilliseconds(-1, 0.0));
}

TEST(XrdHttpTpcRetryTests, TheRandomPartAddsUpToAQuarter) {
  RetryPolicy tries(3, 10, 4);
  ASSERT_TRUE(tries.Another(false));
  EXPECT_EQ(4000, tries.WaitMilliseconds(-1, 0.0));
  EXPECT_EQ(4500, tries.WaitMilliseconds(-1, 0.5));
  EXPECT_EQ(5000, tries.WaitMilliseconds(-1, 1.0));
  // A chance outside 0 to 1 is brought back into it.
  EXPECT_EQ(5000, tries.WaitMilliseconds(-1, 7.0));
  EXPECT_EQ(4000, tries.WaitMilliseconds(-1, -3.0));
}

TEST(XrdHttpTpcRetryTests, NoWaitWhenNoneIsConfigured) {
  RetryPolicy tries(5, 10, 0);
  ASSERT_TRUE(tries.Another(false));
  EXPECT_EQ(0, tries.WaitMilliseconds(-1, 0.9));
  ASSERT_TRUE(tries.Another(false));
  EXPECT_EQ(0, tries.WaitMilliseconds(-1, 0.9));
  // The source may still ask for one.
  EXPECT_EQ(3000, tries.WaitMilliseconds(3, 0.0));
}

TEST(XrdHttpTpcRetryTests, RetryAfterLengthensTheWaitUpToTheMaximum) {
  RetryPolicy tries(3, 10, 2);
  ASSERT_TRUE(tries.Another(false));
  EXPECT_EQ(7000, tries.WaitMilliseconds(7, 0.0));
  // It does not shorten the wait this server would make anyway.
  EXPECT_EQ(2000, tries.WaitMilliseconds(1, 0.0));
  EXPECT_EQ(2000, tries.WaitMilliseconds(0, 0.0));
  // Longer than this server's own longest wait, if the source asks for it.
  EXPECT_EQ(45000, tries.WaitMilliseconds(45, 0.0));
  // A source that asks for an hour is not waited for that long.
  EXPECT_EQ(RetryPolicy::MaxRetryAfter * 1000L, tries.WaitMilliseconds(3600, 0.0));
}

TEST(XrdHttpTpcRetryTests, AConfiguredWaitAboveTheMaximumIsKeptAndNotDoubled) {
  RetryPolicy tries(5, 10, 120);
  ASSERT_TRUE(tries.Another(false));
  EXPECT_EQ(120000, tries.WaitMilliseconds(-1, 0.0));
  ASSERT_TRUE(tries.Another(false));
  EXPECT_EQ(120000, tries.WaitMilliseconds(-1, 0.0));
  // Just below the maximum: doubled once, to the maximum.
  RetryPolicy near(5, 10, 20);
  ASSERT_TRUE(near.Another(false));
  EXPECT_EQ(20000, near.WaitMilliseconds(-1, 0.0));
  ASSERT_TRUE(near.Another(false));
  EXPECT_EQ(30000, near.WaitMilliseconds(-1, 0.0));
  ASSERT_TRUE(near.Another(false));
  EXPECT_EQ(30000, near.WaitMilliseconds(-1, 0.0));
}
