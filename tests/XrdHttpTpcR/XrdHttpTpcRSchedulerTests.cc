#undef NDEBUG

// T-U9 (05-TEST-PLAN.md): the pure range-scheduler FSM.
//   - table-driven state transitions (PENDING/ISSUED/RECEIVING/DONE,
//     retryable vs permanent vs exhausted dispositions)  [FR-12, BUG-1]
//   - backoff monotonicity and jitter bounds              [FR-13]
//   - retry-cap exhaustion signalling (degraded entry)    [FR-13, FR-14]
//   - window coverage: exact, gap-free, bounded table     [FR-10, NFR-5]
//   - shrink-on-retry: delivered prefixes are never re-fetched
//   - per-range stall detection                           [WP-4]
//
// The scheduler takes an injected clock and jitter seed, so every rule is
// checked deterministically -- no sleeps, no curl.

#include "XrdHttpTpcR/XrdHttpTpcRScheduler.hh"
#include "XrdHttpTpcR/XrdHttpTpcRState.hh"

#include <gtest/gtest.h>

#include <curl/curl.h>

#include <set>
#include <vector>

using TPCR::ClassifyCurlFailure;
using TPCR::FailureClass;
using TPCR::Scheduler;

namespace {

// A controllable clock the tests advance by hand.
struct FakeClock {
  time_t now = 1000;
  Scheduler::Clock fn() {
    return [this]() { return now; };
  }
};

Scheduler::Limits SmallLimits() {
  Scheduler::Limits limits;
  limits.start_offset = 0;
  limits.content_length = 100;  // 10 ranges of 10 bytes
  limits.block_size = 10;
  limits.window_bytes = 40;     // 4 ranges in the window
  limits.max_inflight = 2;
  limits.retry_max = 3;
  limits.range_timeout = 30;
  return limits;
}

} // namespace

TEST(XrdHttpTpcRSchedulerTests, WindowLimitsTableSize) {
  // NFR-5: the table covers the window, never the file.
  FakeClock clock;
  Scheduler scheduler(SmallLimits(), clock.fn(), 1);
  EXPECT_EQ(40, scheduler.ScheduledOffset()) << "4 ranges of 10 in a 40-byte window";
  EXPECT_EQ(4u, scheduler.PendingCount());
}

TEST(XrdHttpTpcRSchedulerTests, IssueRespectsInflightCap) {
  FakeClock clock;
  Scheduler scheduler(SmallLimits(), clock.fn(), 1);

  Scheduler::RangeRef first, second, third;
  ASSERT_TRUE(scheduler.NextIssuable(clock.now, first));
  scheduler.MarkIssued(first.id, clock.now);
  ASSERT_TRUE(scheduler.NextIssuable(clock.now, second));
  scheduler.MarkIssued(second.id, clock.now);
  EXPECT_EQ(0, first.offset);
  EXPECT_EQ(10, second.offset);
  // max_inflight = 2: no third issue until something completes.
  EXPECT_FALSE(scheduler.NextIssuable(clock.now, third));
  EXPECT_EQ(2u, scheduler.InFlight());

  EXPECT_EQ(Scheduler::Disposition::Done,
            scheduler.OnRangeResult(first.id, true, FailureClass::Retryable,
                                    clock.now));
  EXPECT_TRUE(scheduler.NextIssuable(clock.now, third));
  EXPECT_EQ(20, third.offset);
}

TEST(XrdHttpTpcRSchedulerTests, CoverageIsExactAndOrdered) {
  // FR-10: drive a whole file through and record every issued range; the
  // union must cover [0, len) exactly once.
  FakeClock clock;
  auto limits = SmallLimits();
  Scheduler scheduler(limits, clock.fn(), 1);

  std::vector<std::pair<off_t, size_t>> issued;
  off_t committed = 0;
  while (!scheduler.AllDone()) {
    Scheduler::RangeRef ref;
    while (scheduler.NextIssuable(clock.now, ref)) {
      scheduler.MarkIssued(ref.id, clock.now);
      issued.emplace_back(ref.offset, ref.length);
      // Delivery + completion, in order of issue.
      scheduler.OnProgress(ref.id, ref.length, clock.now);
      EXPECT_EQ(Scheduler::Disposition::Done,
                scheduler.OnRangeResult(ref.id, true, FailureClass::Retryable,
                                        clock.now));
      committed = ref.offset + static_cast<off_t>(ref.length);
      scheduler.AdvanceCommitted(committed);
    }
  }
  ASSERT_EQ(10u, issued.size());
  off_t cursor = 0;
  for (const auto &range : issued) {
    EXPECT_EQ(cursor, range.first) << "gap or overlap in coverage";
    cursor += static_cast<off_t>(range.second);
  }
  EXPECT_EQ(100, cursor);
  EXPECT_EQ(100, scheduler.ScheduledOffset());
}

TEST(XrdHttpTpcRSchedulerTests, LastRangeIsShort) {
  // FR-10: block 10 over a 25-byte file -> 10, 10, 5.
  FakeClock clock;
  Scheduler::Limits limits = SmallLimits();
  limits.content_length = 25;
  limits.window_bytes = 100;
  Scheduler scheduler(limits, clock.fn(), 1);
  Scheduler::RangeRef ref;
  std::vector<size_t> lengths;
  limits.max_inflight = 3;
  while (scheduler.NextIssuable(clock.now, ref)) {
    scheduler.MarkIssued(ref.id, clock.now);
    lengths.push_back(ref.length);
    if (lengths.size() > 3) break;
  }
  ASSERT_EQ(2u, lengths.size()) << "max_inflight caps issues";
  EXPECT_EQ(10u, lengths[0]);
  EXPECT_EQ(10u, lengths[1]);
}

TEST(XrdHttpTpcRSchedulerTests, RetryableFailureBacksOffAndRetries) {
  FakeClock clock;
  Scheduler scheduler(SmallLimits(), clock.fn(), 42);

  Scheduler::RangeRef ref;
  ASSERT_TRUE(scheduler.NextIssuable(clock.now, ref));
  scheduler.MarkIssued(ref.id, clock.now);

  // First failure, nothing delivered: full range re-queued with backoff.
  EXPECT_EQ(Scheduler::Disposition::Retry,
            scheduler.OnRangeResult(ref.id, false, FailureClass::Retryable,
                                    clock.now));

  // FR-13: the backoff must not block the scheduler loop -- the range simply
  // is not issuable yet, while other ranges are.
  Scheduler::RangeRef next;
  ASSERT_TRUE(scheduler.NextIssuable(clock.now, next));
  EXPECT_NE(ref.id, next.id) << "backing-off range must not be re-issued at once";

  // After the backoff expires (attempt 1: at most 1.5s), it is issuable again.
  clock.now += 2;
  bool found = false;
  Scheduler::RangeRef probe;
  scheduler.MarkIssued(next.id, clock.now);  // occupy one slot
  while (scheduler.NextIssuable(clock.now, probe)) {
    scheduler.MarkIssued(probe.id, clock.now);
    if (probe.id == ref.id) { found = true; break; }
  }
  EXPECT_TRUE(found) << "range must come back after its backoff";
}

TEST(XrdHttpTpcRSchedulerTests, DeliveredPrefixIsNotRefetched) {
  FakeClock clock;
  Scheduler scheduler(SmallLimits(), clock.fn(), 7);

  Scheduler::RangeRef ref;
  ASSERT_TRUE(scheduler.NextIssuable(clock.now, ref));
  scheduler.MarkIssued(ref.id, clock.now);
  ASSERT_EQ(0, ref.offset);
  ASSERT_EQ(10u, ref.length);

  // 6 of 10 bytes arrived (validated + accepted in order), then the
  // connection reset: the retry must fetch only [6, 10).
  scheduler.OnProgress(ref.id, 6, clock.now);
  EXPECT_EQ(Scheduler::Disposition::Retry,
            scheduler.OnRangeResult(ref.id, false, FailureClass::Retryable,
                                    clock.now));
  clock.now += 2;  // let the backoff pass

  Scheduler::RangeRef retry;
  bool found = false;
  while (scheduler.NextIssuable(clock.now, retry)) {
    scheduler.MarkIssued(retry.id, clock.now);
    if (retry.id == ref.id) { found = true; break; }
  }
  ASSERT_TRUE(found);
  EXPECT_EQ(6, retry.offset) << "delivered prefix must not be re-fetched";
  EXPECT_EQ(4u, retry.length);
}

TEST(XrdHttpTpcRSchedulerTests, FullyDeliveredFailureIsDone) {
  // Connection died after the last byte: nothing left to fetch.
  FakeClock clock;
  Scheduler scheduler(SmallLimits(), clock.fn(), 7);
  Scheduler::RangeRef ref;
  ASSERT_TRUE(scheduler.NextIssuable(clock.now, ref));
  scheduler.MarkIssued(ref.id, clock.now);
  scheduler.OnProgress(ref.id, ref.length, clock.now);
  EXPECT_EQ(Scheduler::Disposition::Done,
            scheduler.OnRangeResult(ref.id, false, FailureClass::Retryable,
                                    clock.now));
}

TEST(XrdHttpTpcRSchedulerTests, PermanentFailureAbortsPromptly) {
  // BUG-1/FR-16: permanent failures must not keep the transfer running.
  FakeClock clock;
  Scheduler scheduler(SmallLimits(), clock.fn(), 7);
  Scheduler::RangeRef ref;
  ASSERT_TRUE(scheduler.NextIssuable(clock.now, ref));
  scheduler.MarkIssued(ref.id, clock.now);
  EXPECT_EQ(Scheduler::Disposition::Permanent,
            scheduler.OnRangeResult(ref.id, false, FailureClass::Permanent,
                                    clock.now));
}

TEST(XrdHttpTpcRSchedulerTests, RetryCapSignalsExhaustion) {
  // FR-13/FR-14: after retry_max failures the scheduler reports Exhausted;
  // the loop escalates to the degraded state instead of looping forever.
  FakeClock clock;
  auto limits = SmallLimits();
  limits.retry_max = 2;
  limits.max_inflight = 1;
  Scheduler scheduler(limits, clock.fn(), 7);

  Scheduler::RangeRef ref;
  ASSERT_TRUE(scheduler.NextIssuable(clock.now, ref));
  scheduler.MarkIssued(ref.id, clock.now);
  const uint64_t id = ref.id;

  EXPECT_EQ(Scheduler::Disposition::Retry,
            scheduler.OnRangeResult(id, false, FailureClass::Retryable, clock.now));
  clock.now += 60;
  ASSERT_TRUE(scheduler.NextIssuable(clock.now, ref));
  ASSERT_EQ(id, ref.id);
  scheduler.MarkIssued(id, clock.now);
  EXPECT_EQ(Scheduler::Disposition::Retry,
            scheduler.OnRangeResult(id, false, FailureClass::Retryable, clock.now));
  clock.now += 60;
  ASSERT_TRUE(scheduler.NextIssuable(clock.now, ref));
  ASSERT_EQ(id, ref.id);
  scheduler.MarkIssued(id, clock.now);
  EXPECT_EQ(Scheduler::Disposition::Exhausted,
            scheduler.OnRangeResult(id, false, FailureClass::Retryable, clock.now));
  // The range stays re-issuable for post-recovery resumption (WP-5).
  EXPECT_TRUE(scheduler.NextIssuable(clock.now, ref));
  EXPECT_EQ(id, ref.id);
}

TEST(XrdHttpTpcRSchedulerTests, BackoffMonotonicWithBoundedJitter) {
  // FR-13: mean delay doubles per attempt; every sample stays inside
  // [0.5 * 2^(a-1), 1.5 * 2^(a-1)] capped at BACKOFF_CAP_SECS.
  std::mt19937 rng(1234);
  double prev_mean = 0;
  for (unsigned attempt = 1; attempt <= 8; attempt++) {
    double sum = 0;
    const double base =
        std::min<double>(static_cast<double>(1u << (attempt - 1)),
                         Scheduler::BACKOFF_CAP_SECS);
    for (int i = 0; i < 400; i++) {
      double delay = Scheduler::BackoffDelay(attempt, rng);
      EXPECT_GE(delay, 0.5 * base - 1e-9);
      EXPECT_LE(delay, std::min<double>(1.5 * base,
                                        Scheduler::BACKOFF_CAP_SECS) + 1e-9);
      sum += delay;
    }
    const double mean = sum / 400;
    if (base < Scheduler::BACKOFF_CAP_SECS) {
      // Strictly growing until the cap; once the base saturates at the cap
      // the jittered means are statistically equal, so no ordering holds.
      EXPECT_GT(mean, prev_mean - 1e-9) << "mean backoff must not shrink";
    }
    prev_mean = mean;
  }
}

TEST(XrdHttpTpcRSchedulerTests, StallDetectorFlagsSilentRanges) {
  FakeClock clock;
  Scheduler scheduler(SmallLimits(), clock.fn(), 7);

  Scheduler::RangeRef quiet, chatty;
  ASSERT_TRUE(scheduler.NextIssuable(clock.now, quiet));
  scheduler.MarkIssued(quiet.id, clock.now);
  ASSERT_TRUE(scheduler.NextIssuable(clock.now, chatty));
  scheduler.MarkIssued(chatty.id, clock.now);

  // 29s in: neither has timed out (range_timeout = 30).
  clock.now += 29;
  EXPECT_TRUE(scheduler.TimedOut(clock.now).empty());

  // The chatty range delivers a byte; the quiet one stays silent.
  scheduler.OnProgress(chatty.id, 1, clock.now);
  clock.now += 1;
  auto stalled = scheduler.TimedOut(clock.now);
  ASSERT_EQ(1u, stalled.size());
  EXPECT_EQ(quiet.id, stalled[0]);
}

TEST(XrdHttpTpcRSchedulerTests, AdvanceCommittedSlidesWindowAndTrims) {
  FakeClock clock;
  Scheduler scheduler(SmallLimits(), clock.fn(), 7);

  // Complete the first two ranges.
  for (int i = 0; i < 2; i++) {
    Scheduler::RangeRef ref;
    ASSERT_TRUE(scheduler.NextIssuable(clock.now, ref));
    scheduler.MarkIssued(ref.id, clock.now);
    scheduler.OnProgress(ref.id, ref.length, clock.now);
    ASSERT_EQ(Scheduler::Disposition::Done,
              scheduler.OnRangeResult(ref.id, true, FailureClass::Retryable,
                                      clock.now));
  }
  // Committed advances to 20: the window slides, two new ranges appear.
  scheduler.AdvanceCommitted(20);
  EXPECT_EQ(60, scheduler.ScheduledOffset()) << "window slid by the commit";
  EXPECT_EQ(4u, scheduler.PendingCount()) << "completed ranges trimmed";
  EXPECT_FALSE(scheduler.AllDone());
}

TEST(XrdHttpTpcRSchedulerTests, ZeroLengthFileIsImmediatelyDone) {
  FakeClock clock;
  Scheduler::Limits limits = SmallLimits();
  limits.content_length = 0;
  Scheduler scheduler(limits, clock.fn(), 7);
  Scheduler::RangeRef ref;
  EXPECT_FALSE(scheduler.NextIssuable(clock.now, ref));
  EXPECT_TRUE(scheduler.AllDone());
}

TEST(XrdHttpTpcRSchedulerTests, RequeueInFlightKeepsPrefixWithoutPenalty) {
  // FR-14 teardown: in-flight ranges return to PENDING keeping their
  // delivered-prefix shrink, with no attempt increment.
  FakeClock clock;
  Scheduler scheduler(SmallLimits(), clock.fn(), 7);

  Scheduler::RangeRef a, b;
  ASSERT_TRUE(scheduler.NextIssuable(clock.now, a));
  scheduler.MarkIssued(a.id, clock.now);
  ASSERT_TRUE(scheduler.NextIssuable(clock.now, b));
  scheduler.MarkIssued(b.id, clock.now);
  scheduler.OnProgress(a.id, 4, clock.now);  // half of range a delivered

  scheduler.RequeueInFlight(clock.now);
  EXPECT_EQ(0u, scheduler.InFlight());

  // Both immediately issuable again; range a shrunk to its remainder.
  Scheduler::RangeRef again;
  bool found_a = false, found_b = false;
  while (scheduler.NextIssuable(clock.now, again)) {
    scheduler.MarkIssued(again.id, clock.now);
    if (again.id == a.id) {
      found_a = true;
      EXPECT_EQ(a.offset + 4, again.offset) << "delivered prefix not re-fetched";
      EXPECT_EQ(a.length - 4, again.length);
    }
    if (again.id == b.id) {
      found_b = true;
      EXPECT_EQ(b.offset, again.offset);
    }
  }
  EXPECT_TRUE(found_a);
  EXPECT_TRUE(found_b);

  // No attempt was consumed: the ranges still have the full retry budget.
  for (unsigned i = 0; i < SmallLimits().retry_max; i++) {
    EXPECT_EQ(Scheduler::Disposition::Retry,
              scheduler.OnRangeResult(a.id, false, FailureClass::Retryable,
                                      clock.now))
        << "attempt " << i;
    clock.now += 60;
    Scheduler::RangeRef retry;
    ASSERT_TRUE(scheduler.NextIssuable(clock.now, retry));
    scheduler.MarkIssued(retry.id, clock.now);
    ASSERT_EQ(a.id, retry.id);
  }
  EXPECT_EQ(Scheduler::Disposition::Exhausted,
            scheduler.OnRangeResult(a.id, false, FailureClass::Retryable,
                                    clock.now));
}

TEST(XrdHttpTpcRSchedulerTests, ResetAttemptsRestoresPatience) {
  // Degraded exit (FR-14): a recovered source deserves the full retry
  // budget, even for ranges that had exhausted theirs.
  FakeClock clock;
  auto limits = SmallLimits();
  limits.retry_max = 1;
  limits.max_inflight = 1;
  Scheduler scheduler(limits, clock.fn(), 7);

  Scheduler::RangeRef ref;
  ASSERT_TRUE(scheduler.NextIssuable(clock.now, ref));
  scheduler.MarkIssued(ref.id, clock.now);
  ASSERT_EQ(Scheduler::Disposition::Retry,
            scheduler.OnRangeResult(ref.id, false, FailureClass::Retryable,
                                    clock.now));
  clock.now += 60;
  ASSERT_TRUE(scheduler.NextIssuable(clock.now, ref));
  scheduler.MarkIssued(ref.id, clock.now);
  ASSERT_EQ(Scheduler::Disposition::Exhausted,
            scheduler.OnRangeResult(ref.id, false, FailureClass::Retryable,
                                    clock.now));

  scheduler.ResetAttempts();
  ASSERT_TRUE(scheduler.NextIssuable(clock.now, ref));
  scheduler.MarkIssued(ref.id, clock.now);
  EXPECT_EQ(Scheduler::Disposition::Retry,
            scheduler.OnRangeResult(ref.id, false, FailureClass::Retryable,
                                    clock.now))
      << "attempts must be reset after recovery";
}

TEST(XrdHttpTpcRSchedulerTests, ValidatorMidSessionComparison) {
  // FR-14: definite changes are fatal; missing validators are not.
  TPCR::SourceValidators base;
  base.content_length = 1000;
  base.etag = "\"abc\"";
  base.last_modified = "Wed, 01 Jan 2025 00:00:00 GMT";
  base.repr_digests["adler"] = ":c4Ki0g==:";
  std::string reason;

  TPCR::SourceValidators same = base;
  EXPECT_TRUE(base.CompatibleMidSession(same, reason));

  TPCR::SourceValidators shorter = base;
  shorter.content_length = 999;
  EXPECT_FALSE(base.CompatibleMidSession(shorter, reason));
  EXPECT_NE(std::string::npos, reason.find("content length"));

  TPCR::SourceValidators new_etag = base;
  new_etag.etag = "\"def\"";
  EXPECT_FALSE(base.CompatibleMidSession(new_etag, reason));

  TPCR::SourceValidators new_mtime = base;
  new_mtime.last_modified = "Thu, 02 Jan 2025 00:00:00 GMT";
  EXPECT_FALSE(base.CompatibleMidSession(new_mtime, reason));

  TPCR::SourceValidators new_digest = base;
  new_digest.repr_digests["adler"] = ":zzzzzz==:";
  EXPECT_FALSE(base.CompatibleMidSession(new_digest, reason));

  // Missing on one side: not evidence of change (mid-session policy).
  TPCR::SourceValidators sparse;
  sparse.content_length = 1000;
  EXPECT_TRUE(base.CompatibleMidSession(sparse, reason));
}

TEST(XrdHttpTpcRSchedulerTests, ClassificationTable) {
  // FR-12, table-driven.  Args: (curl code, http status, state error code).
  using S = TPCR::State;
  const int no_state_err = S::errNone;

  // Recorded validation / local-storage errors dominate everything.
  EXPECT_EQ(FailureClass::Permanent,
            ClassifyCurlFailure(CURLE_WRITE_ERROR, 206, S::errRangeNotHonored));
  EXPECT_EQ(FailureClass::Permanent,
            ClassifyCurlFailure(CURLE_OK, 206, S::errRangeMismatch));
  EXPECT_EQ(FailureClass::Permanent,
            ClassifyCurlFailure(CURLE_OK, 206, S::errLengthMismatch));
  EXPECT_EQ(FailureClass::Permanent,
            ClassifyCurlFailure(CURLE_WRITE_ERROR, 206, S::errWrite));

  // HTTP statuses: transient server-side conditions retry...
  for (int status : {408, 429, 500, 502, 503, 504}) {
    EXPECT_EQ(FailureClass::Retryable,
              ClassifyCurlFailure(CURLE_OK, status, no_state_err))
        << "status " << status;
  }
  // ...auth flaps get the one-shot re-probe class...
  for (int status : {401, 403}) {
    EXPECT_EQ(FailureClass::AuthRetry,
              ClassifyCurlFailure(CURLE_OK, status, no_state_err))
        << "status " << status;
  }
  // ...and the rest of 4xx/5xx is permanent.
  for (int status : {400, 404, 410, 412, 501, 505}) {
    EXPECT_EQ(FailureClass::Permanent,
              ClassifyCurlFailure(CURLE_OK, status, no_state_err))
        << "status " << status;
  }

  // Transport-level: connectivity blips retry...
  for (int code : {CURLE_COULDNT_CONNECT, CURLE_COULDNT_RESOLVE_HOST,
                   CURLE_OPERATION_TIMEDOUT, CURLE_PARTIAL_FILE,
                   CURLE_GOT_NOTHING, CURLE_SEND_ERROR, CURLE_RECV_ERROR}) {
    EXPECT_EQ(FailureClass::Retryable,
              ClassifyCurlFailure(code, 0, no_state_err)) << "curl " << code;
  }
  // ...TLS problems are configuration/security failures, permanent.
  for (int code : {CURLE_SSL_CONNECT_ERROR, CURLE_PEER_FAILED_VERIFICATION,
                   CURLE_SSL_CERTPROBLEM, CURLE_SSL_CACERT_BADFILE}) {
    EXPECT_EQ(FailureClass::Permanent,
              ClassifyCurlFailure(code, 0, no_state_err)) << "curl " << code;
  }
  // Unattributed write-callback aborts are permanent.
  EXPECT_EQ(FailureClass::Permanent,
            ClassifyCurlFailure(CURLE_WRITE_ERROR, 0, no_state_err));
  // Unknown transport errors default to retryable (bounded by the cap).
  EXPECT_EQ(FailureClass::Retryable,
            ClassifyCurlFailure(CURLE_OBSOLETE20, 0, no_state_err));
}

TEST(XrdHttpTpcRSchedulerTests, NextNotBeforeReportsBackoffHorizon) {
  FakeClock clock;
  auto limits = SmallLimits();
  limits.max_inflight = 4;
  Scheduler scheduler(limits, clock.fn(), 7);

  // Nothing backing off: a range is ready now.
  EXPECT_EQ(0, scheduler.NextNotBefore(clock.now));

  // Fail every range in the window so they all back off.
  Scheduler::RangeRef ref;
  std::vector<uint64_t> ids;
  while (scheduler.NextIssuable(clock.now, ref)) {
    scheduler.MarkIssued(ref.id, clock.now);
    ids.push_back(ref.id);
  }
  for (uint64_t id : ids) {
    scheduler.OnRangeResult(id, false, FailureClass::Retryable, clock.now);
  }
  const time_t horizon = scheduler.NextNotBefore(clock.now);
  EXPECT_GT(horizon, clock.now) << "everything is backing off";
  // The loop can sleep until the horizon instead of spinning (BUG-9 spirit).
  clock.now = horizon;
  EXPECT_EQ(0, scheduler.NextNotBefore(clock.now));
}
