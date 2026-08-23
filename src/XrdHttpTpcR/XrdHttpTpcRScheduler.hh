//------------------------------------------------------------------------------
// This file is part of XrdHttpTpcR: the resumable HTTP-TPC handler.
//
// XRootD is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// XRootD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with XRootD.  If not, see <http://www.gnu.org/licenses/>.
//------------------------------------------------------------------------------

#ifndef __XRD_TPCR_SCHEDULER_HH__
#define __XRD_TPCR_SCHEDULER_HH__

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <random>
#include <map>
#include <string>
#include <vector>

#include <sys/types.h>

namespace TPCR {

// Failure classification per FR-12.  The mapping from (CURLcode, HTTP status,
// State error code) lives in ClassifyCurlFailure(); the scheduler itself only
// consumes the class.
enum class FailureClass {
    Retryable,   // connect/reset/timeout/partial classes, HTTP 408/429/5xx-transient
    Permanent,   // other 4xx, TLS failures, Range-not-honored, malformed responses
    AuthRetry,   // 401/403 mid-session: one re-probe, then permanent (FR-12, WP-5)
};

// FR-12 classification of a failed range request.  `curl_code` is the
// CURLcode as an int (kept as int so this header stays curl-free),
// `http_status` the response status (or <= 0 if none), `state_error_code`
// the TPCR::State ErrorCode recorded by the callbacks (errNone if none).
// Precedence: recorded validation/local-write errors, then HTTP status,
// then the curl transport code.  Unknown transport errors default to
// Retryable -- the per-range cap and the recovery budget bound the cost,
// and prematurely declaring a transient fault permanent forfeits the
// transfer (priority order: reliability over efficiency).
FailureClass ClassifyCurlFailure(int curl_code, int http_status,
                                 int state_error_code);

// Source validators captured at the session-start HEAD (SUB-7) and
// re-checked by the degraded-state re-probe (FR-14).  Also the baseline the
// cross-session resume ladder (FR-21, WP-8) persists in the journal.
struct SourceValidators {
    off_t content_length = -1;
    std::string etag;                       // verbatim, may be weak (W/...)
    std::string last_modified;
    std::map<std::string, std::string> repr_digests;

    // Cross-session resume ladder (FR-21), strongest rung first:
    //   matching Repr-Digest (same algorithm, same value)
    //   > matching strong ETag (weak W/ ETags are ignored)
    //   > matching Last-Modified AND length.
    // Length must match in ALL cases.  Policy `strong` (default) refuses to
    // resume when no rung is available; `length-only` accepts a bare length
    // match (for controlled environments with known-immutable sources).
    // `journal` is what the previous session recorded; `fresh` is this
    // session's HEAD.  Returns true to resume; otherwise fills `reason`
    // (the RESUME_REJECTED reason code, FR-31).
    enum class Policy { Strong, LengthOnly };
    static bool ResumeAccepts(const SourceValidators &journal,
                              const SourceValidators &fresh, Policy policy,
                              std::string &reason);

    // Mid-session comparison (FR-14): a *definite* change -- differing
    // length, ETag, Last-Modified, or any common digest algorithm's value --
    // makes the transfer a permanent failure.  A validator merely missing
    // from one side is logged by the caller but is not evidence of change
    // (the strict presence policy belongs to the resume ladder, FR-21).
    // Returns true when compatible; otherwise fills `reason`.
    bool CompatibleMidSession(const SourceValidators &fresh,
                              std::string &reason) const;
};

// Pure range-scheduling state machine (WP-4; 02-ARCHITECTURE §6).
//
// Owns the range table over the active window
// [committed, committed + window_bytes) -- never the whole file, so state
// stays O(window/block), not O(file/block) (NFR-5).  Each range moves
//
//     PENDING -> ISSUED -> RECEIVING -> DONE
//                   \__________________/
//                    FAILED(retryable) -> PENDING (backoff, attempts++)
//                    FAILED(permanent) -> transfer aborts
//
// The scheduler is deliberately curl-free and side-effect-free: the transfer
// loop asks it what to do (NextIssuable, TimedOut, OnRangeResult) and reports
// what happened.  The clock and the backoff jitter RNG are injected, so every
// transition and timing rule is unit-testable (T-U9).
//
// Three counters, three meanings (SUB-3) -- never mix them:
//   ScheduledOffset()  bytes handed to range requests so far ("scheduled")
//   received bytes     live in the transfer loop / States ("received";
//                      perf markers report these)
//   committed offset   bytes durably in order in the Stream ("committed";
//                      pushed in via AdvanceCommitted, drives the window)
//
// Retry model: bytes a failed range already delivered were validated and
// accepted in order into the Stream, so a retry re-fetches only the
// remainder -- the range's offset advances by the delivered count (recorded
// via OnProgress).  Nothing is ever delivered twice, which is exactly the
// invariant the Stream enforces (FR-10: every byte scheduled once).
class Scheduler {
public:
    struct Limits {
        off_t  start_offset = 0;      // W: first byte to fetch (0 = fresh)
        off_t  content_length = 0;    // total file length (absolute)
        size_t block_size = 16 * 1024 * 1024;   // FR-10: tpcr.blocksize
        size_t window_bytes = 256 * 1024 * 1024;  // reorder/admission window
        size_t max_inflight = 1;      // = streams (effective parallelism)
        unsigned retry_max = 5;       // per-range attempt cap (FR-13)
        int    range_timeout = 60;    // secs without per-range progress (WP-4)
    };

    // What the transfer loop should do about a completed/failed range.
    enum class Disposition {
        Done,        // range complete; nothing to do
        Retry,       // re-queued with backoff; issue again when ready
        Exhausted,   // per-range retries exhausted -> degraded state (WP-5)
        Permanent,   // permanent failure -> abort the transfer promptly
    };

    struct RangeRef {
        uint64_t id = 0;
        off_t offset = 0;
        size_t length = 0;
    };

    using Clock = std::function<time_t()>;

    Scheduler(const Limits &limits, Clock clock, uint32_t jitter_seed);

    // --- Issue side -------------------------------------------------------

    // True if a PENDING range inside the window is ready (backoff expired)
    // and the in-flight cap has room; fills `out`.  The caller reserves its
    // buffer (slab) BEFORE MarkIssued -- if reservation fails, simply do not
    // issue: the range stays PENDING and is offered again later (the
    // never-fail backpressure path, NFR-1).
    bool NextIssuable(time_t now, RangeRef &out);

    // The range was handed to a connection.
    void MarkIssued(uint64_t id, time_t now);

    // Delivery progress: `delivered_total` is the byte count of this range's
    // current issue accepted into the Stream so far.  Also moves
    // ISSUED -> RECEIVING on first bytes and feeds the per-range stall
    // detector.
    void OnProgress(uint64_t id, size_t delivered_total, time_t now);

    // The range's request finished.  `failure` is ignored when ok=true.
    // On a retryable failure the delivered prefix is kept (see class
    // comment) and the remainder re-queued with exponential backoff +
    // jitter (FR-13).  AuthRetry counts against the same cap but is
    // reported distinctly so the loop can trigger the WP-5 re-probe.
    Disposition OnRangeResult(uint64_t id, bool ok, FailureClass failure,
                              time_t now);

    // Ranges whose current issue has shown no delivery progress for
    // range_timeout seconds; the loop cancels their connections and calls
    // OnRangeResult(id, false, Retryable).
    std::vector<uint64_t> TimedOut(time_t now) const;

    // --- Degraded-state support (FR-14, WP-5) ----------------------------

    // Teardown: every in-flight range returns to PENDING, immediately
    // issuable, keeping its delivered-prefix shrink but WITHOUT an attempt
    // increment -- the teardown is the session's decision, not the range's
    // failure.  Called when the loop dismantles the connections on entering
    // the degraded state.
    void RequeueInFlight(time_t now);

    // Fresh patience after a successful recovery (degraded exit): attempt
    // counters and backoffs reset, so previously-exhausted ranges get the
    // full retry budget against the recovered source.
    void ResetAttempts();

    // --- Window / bookkeeping --------------------------------------------

    // Slide the window: `committed` is Stream::CommittedOffset().  Completed
    // ranges below it are dropped from the table and new PENDING ranges are
    // created up to committed + window_bytes (exact, gap-free, overlap-free
    // coverage of [start, content_length) -- asserted in debug builds).
    void AdvanceCommitted(off_t committed);

    // --- Completion / observability --------------------------------------

    // FR-11 completion gate, scheduler side: every byte of
    // [start, content_length) was scheduled and every range is DONE.
    bool AllDone() const;

    // First byte not yet covered by any created range ("scheduled" counter).
    off_t ScheduledOffset() const {return m_next_offset;}

    size_t InFlight() const {return m_inflight;}
    size_t PendingCount() const;

    // The earliest not-before among PENDING ranges (for loop wait sizing);
    // 0 if a range is ready now or none is pending.
    time_t NextNotBefore(time_t now) const;

    // --- Static policy helpers (pure; unit-tested directly) ---------------

    // FR-13: exponential backoff with jitter.  attempt is 1 for the first
    // retry.  Returns seconds in [0.5 * 2^(a-1), 1.5 * 2^(a-1)] capped at
    // BACKOFF_CAP_SECS, never negative.
    static double BackoffDelay(unsigned attempt, std::mt19937 &rng);

    static const unsigned BACKOFF_CAP_SECS = 30;

private:
    enum class RState { PENDING, ISSUED, RECEIVING, DONE };

    struct Range {
        uint64_t id;
        off_t offset;        // current fetch start (advances on shrink-retry)
        size_t length;       // current remaining length
        RState state = RState::PENDING;
        unsigned attempts = 0;       // failures so far
        time_t not_before = 0;       // earliest re-issue (backoff)
        time_t last_progress = 0;    // last delivery advance (stall detector)
        size_t delivered = 0;        // bytes of current issue accepted
    };

    Range *Find(uint64_t id);
    const Range *Find(uint64_t id) const;

    // Extend the table while it fits the window; debug-checks coverage.
    void FillWindow();
    void CheckInvariants() const;

    const Limits m_limits;
    Clock m_clock;
    std::mt19937 m_rng;

    std::deque<Range> m_table;   // ordered by offset; O(window/block) entries
    off_t m_next_offset;         // next byte to cover with a new range
    off_t m_committed;           // latest committed offset from the Stream
    size_t m_inflight = 0;       // ISSUED + RECEIVING
    uint64_t m_next_id = 1;
};

} // namespace TPCR

#endif // __XRD_TPCR_SCHEDULER_HH__
