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

// Pure range-FSM implementation (no libcurl, no server dependencies): the
// curl-driven transfer loop lives in XrdHttpTpcRScheduler.cc.  Keeping this
// half dependency-free is what lets T-U9 drive every transition and timing
// rule directly.

#include "XrdHttpTpcRScheduler.hh"
#include "XrdHttpTpcRState.hh"

#include <curl/curl.h>   // constants only; no link dependency

#include <algorithm>
#include <cassert>

using namespace TPCR;

FailureClass TPCR::ClassifyCurlFailure(int curl_code, int http_status,
                                       int state_error_code)
{
    // 1. Errors our own callbacks recorded take precedence: they describe
    //    exactly what went wrong regardless of how libcurl surfaced it
    //    (usually as CURLE_WRITE_ERROR after the callback aborted).
    switch (state_error_code) {
        case State::errRangeNotHonored:
        case State::errRangeMismatch:
        case State::errLengthMismatch:
            // The source demonstrably misimplements ranged GETs (FR-8).
            return FailureClass::Permanent;
        case State::errWrite:
        case State::errFlush:
        case State::errClose:
            // Local storage failure: retrying the network cannot help.
            return FailureClass::Permanent;
        default:
            break;
    }

    // 2. HTTP status classes (FR-12).
    if (http_status >= 400) {
        switch (http_status) {
            case 408: case 429:
            case 500: case 502: case 503: case 504:
                return FailureClass::Retryable;
            case 401: case 403:
                // Mid-session auth flap: worth one full re-probe (WP-5),
                // then permanent -- the session cannot refresh its token.
                return FailureClass::AuthRetry;
            default:
                return FailureClass::Permanent;
        }
    }

    // 3. Transport-level CURLcodes (FR-12).
    switch (curl_code) {
        case CURLE_COULDNT_CONNECT:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY:
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_PARTIAL_FILE:
        case CURLE_GOT_NOTHING:
        case CURLE_SEND_ERROR:
        case CURLE_RECV_ERROR:
            return FailureClass::Retryable;
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CERTPROBLEM:
        case CURLE_SSL_CIPHER:
        case CURLE_SSL_CACERT_BADFILE:
        case CURLE_SSL_ISSUER_ERROR:
        case CURLE_USE_SSL_FAILED:
            // TLS handshake/verification problems are configuration or
            // security failures, not blips (FR-12).
            return FailureClass::Permanent;
        case CURLE_WRITE_ERROR:
            // Our write callback aborted without recording a reason: with
            // the recorded-error cases handled above, this is the >1 KiB
            // error-body path or an internal inconsistency -- permanent.
            return FailureClass::Permanent;
        default:
            // Unknown transport error: default to retryable (bounded by the
            // per-range cap and recovery budget) -- see the declaration.
            return FailureClass::Retryable;
    }
}

Scheduler::Scheduler(const Limits &limits, Clock clock, uint32_t jitter_seed)
    : m_limits(limits),
      m_clock(std::move(clock)),
      m_rng(jitter_seed),
      m_next_offset(limits.start_offset),
      m_committed(limits.start_offset)
{
    FillWindow();
}

void Scheduler::FillWindow()
{
    // Create PENDING ranges until the table covers the window or the file
    // ends.  Coverage is exact by construction: each new range starts where
    // the previous one ended (FR-10 -- no gaps, no overlaps).
    const off_t window_end =
        std::min<off_t>(m_limits.content_length,
                        m_committed + static_cast<off_t>(m_limits.window_bytes));
    while (m_next_offset < window_end) {
        const size_t length = static_cast<size_t>(
            std::min<off_t>(static_cast<off_t>(m_limits.block_size),
                            m_limits.content_length - m_next_offset));
        Range range;
        range.id = m_next_id++;
        range.offset = m_next_offset;
        range.length = length;
        m_table.push_back(range);
        m_next_offset += static_cast<off_t>(length);
    }
    CheckInvariants();
}

void Scheduler::CheckInvariants() const
{
#ifndef NDEBUG
    // Debug-build assertion of the FR-10 coverage rule: ranges are sorted,
    // non-overlapping, and jointly cover [<first live offset>, m_next_offset).
    off_t cursor = -1;
    for (const auto &range : m_table) {
        assert(range.length > 0 && "zero-length range in table");
        if (cursor >= 0) {
            assert(range.offset >= cursor && "overlapping ranges");
        }
        // A shrink-retry advances offset within the original block, so
        // adjacent ranges may show a gap exactly equal to bytes already
        // delivered -- those bytes are in the Stream, i.e. still covered
        // exactly once over the issue history.
        cursor = range.offset + static_cast<off_t>(range.length);
    }
    if (cursor >= 0) {
        assert(cursor == m_next_offset && "table end != scheduled offset");
    }
#endif
}

Scheduler::Range *Scheduler::Find(uint64_t id)
{
    for (auto &range : m_table) {
        if (range.id == id) {return &range;}
    }
    return nullptr;
}

const Scheduler::Range *Scheduler::Find(uint64_t id) const
{
    for (const auto &range : m_table) {
        if (range.id == id) {return &range;}
    }
    return nullptr;
}

bool Scheduler::NextIssuable(time_t now, RangeRef &out)
{
    if (m_inflight >= m_limits.max_inflight) {return false;}
    for (const auto &range : m_table) {
        if (range.state != RState::PENDING) {continue;}
        if (range.not_before > now) {continue;}  // backing off (FR-13)
        out.id = range.id;
        out.offset = range.offset;
        out.length = range.length;
        return true;
    }
    return false;
}

void Scheduler::MarkIssued(uint64_t id, time_t now)
{
    Range *range = Find(id);
    assert(range && range->state == RState::PENDING &&
           "MarkIssued on a range that is not pending");
    if (!range || range->state != RState::PENDING) {return;}  // NFR-7 release path
    range->state = RState::ISSUED;
    range->delivered = 0;
    range->last_progress = now;
    m_inflight++;
}

void Scheduler::OnProgress(uint64_t id, size_t delivered_total, time_t now)
{
    Range *range = Find(id);
    if (!range) {return;}
    if (range->state == RState::ISSUED && delivered_total > 0) {
        range->state = RState::RECEIVING;
    }
    if (delivered_total > range->delivered) {
        range->delivered = delivered_total;
        range->last_progress = now;
    }
}

Scheduler::Disposition Scheduler::OnRangeResult(uint64_t id, bool ok,
                                                FailureClass failure,
                                                time_t now)
{
    Range *range = Find(id);
    assert(range && "OnRangeResult for unknown range id");
    if (!range) {return Disposition::Permanent;}  // NFR-7: fail, don't guess
    assert((range->state == RState::ISSUED || range->state == RState::RECEIVING) &&
           "OnRangeResult on a range that is not in flight");
    if (m_inflight > 0) {m_inflight--;}

    if (ok) {
        range->state = RState::DONE;
        return Disposition::Done;
    }

    if (failure == FailureClass::Permanent) {
        // Prompt abort (FR-16 exception): stock code kept scheduling for
        // hours after a permanent failure (BUG-1); the loop stops now.
        return Disposition::Permanent;
    }

    // Retryable (and AuthRetry, which shares the mechanics): keep the
    // delivered prefix -- it was validated, in-order data the Stream already
    // accepted -- and re-queue only the remainder.
    range->offset += static_cast<off_t>(range->delivered);
    range->length -= range->delivered;
    range->delivered = 0;
    range->attempts++;
    if (range->length == 0) {
        // Everything arrived before the connection died: the range is done
        // in every way that matters.
        range->state = RState::DONE;
        return Disposition::Done;
    }
    if (range->attempts > m_limits.retry_max) {
        // Out of per-range patience: the caller escalates (degraded state,
        // WP-5) instead of silently retrying forever.
        range->state = RState::PENDING;   // remains re-issuable post-recovery
        range->not_before = now;          // degraded logic decides the pacing
        return Disposition::Exhausted;
    }
    range->state = RState::PENDING;
    range->not_before =
        now + static_cast<time_t>(BackoffDelay(range->attempts, m_rng) + 0.5);
    CheckInvariants();
    return Disposition::Retry;
}

bool SourceValidators::CompatibleMidSession(const SourceValidators &fresh,
                                            std::string &reason) const
{
    // Length must match whenever both sides know it -- a changed length is
    // always a changed file.
    if (content_length >= 0 && fresh.content_length >= 0 &&
        content_length != fresh.content_length) {
        reason = "content length changed (" + std::to_string(content_length) +
                 " -> " + std::to_string(fresh.content_length) + ")";
        return false;
    }
    if (!etag.empty() && !fresh.etag.empty() && etag != fresh.etag) {
        reason = "ETag changed (" + etag + " -> " + fresh.etag + ")";
        return false;
    }
    if (!last_modified.empty() && !fresh.last_modified.empty() &&
        last_modified != fresh.last_modified) {
        reason = "Last-Modified changed (" + last_modified + " -> " +
                 fresh.last_modified + ")";
        return false;
    }
    for (const auto &[algorithm, value] : repr_digests) {
        auto match = fresh.repr_digests.find(algorithm);
        if (match != fresh.repr_digests.end() && match->second != value) {
            reason = "Repr-Digest (" + algorithm + ") changed";
            return false;
        }
    }
    return true;
}

bool SourceValidators::ResumeAccepts(const SourceValidators &journal,
                                     const SourceValidators &fresh,
                                     Policy policy, std::string &reason)
{
    // Length is the unconditional gate (FR-21: "length must always match").
    if (journal.content_length < 0 || fresh.content_length < 0) {
        reason = "content length unavailable";
        return false;
    }
    if (journal.content_length != fresh.content_length) {
        reason = "content length mismatch (" +
                 std::to_string(journal.content_length) + " -> " +
                 std::to_string(fresh.content_length) + ")";
        return false;
    }
    if (policy == Policy::LengthOnly) {
        return true;
    }

    // Rung 1: a digest algorithm present on both sides decides outright.
    for (const auto &[algorithm, value] : journal.repr_digests) {
        auto match = fresh.repr_digests.find(algorithm);
        if (match == fresh.repr_digests.end()) {continue;}
        if (match->second == value) {return true;}
        reason = "Repr-Digest (" + algorithm + ") mismatch";
        return false;
    }

    // Rung 2: strong ETags (weak "W/" validators are ignored -- they only
    // promise semantic, not byte, equivalence).
    auto strong = [](const std::string &etag) {
        return !etag.empty() && etag.rfind("W/", 0) != 0;
    };
    if (strong(journal.etag) && strong(fresh.etag)) {
        if (journal.etag == fresh.etag) {return true;}
        reason = "ETag mismatch";
        return false;
    }

    // Rung 3: Last-Modified (already backed by the length gate above).
    if (!journal.last_modified.empty() && !fresh.last_modified.empty()) {
        if (journal.last_modified == fresh.last_modified) {return true;}
        reason = "Last-Modified mismatch";
        return false;
    }

    reason = "no acceptable validator offered (policy strong)";
    return false;
}

void Scheduler::RequeueInFlight(time_t now)
{
    for (auto &range : m_table) {
        if (range.state != RState::ISSUED && range.state != RState::RECEIVING) {
            continue;
        }
        // Keep the delivered prefix (validated, in-order, accepted by the
        // Stream) exactly as a retryable failure would; no attempt penalty.
        range.offset += static_cast<off_t>(range.delivered);
        range.length -= range.delivered;
        range.delivered = 0;
        range.state = range.length ? RState::PENDING : RState::DONE;
        range.not_before = now;
    }
    m_inflight = 0;
    CheckInvariants();
}

void Scheduler::ResetAttempts()
{
    for (auto &range : m_table) {
        range.attempts = 0;
        if (range.state == RState::PENDING) {range.not_before = 0;}
    }
}

std::vector<uint64_t> Scheduler::TimedOut(time_t now) const
{
    std::vector<uint64_t> stalled;
    for (const auto &range : m_table) {
        if (range.state != RState::ISSUED && range.state != RState::RECEIVING) {
            continue;
        }
        if (now - range.last_progress >= m_limits.range_timeout) {
            stalled.push_back(range.id);
        }
    }
    return stalled;
}

void Scheduler::AdvanceCommitted(off_t committed)
{
    assert(committed >= m_committed && "committed offset moved backwards");
    m_committed = committed;
    // Drop DONE ranges wholly below the committed offset: their bytes are in
    // order in the Stream, nothing can reference them again.  This is what
    // keeps the table O(window/block) for a 10 TB file (NFR-5).
    while (!m_table.empty()) {
        const Range &head = m_table.front();
        if (head.state == RState::DONE &&
            head.offset + static_cast<off_t>(head.length) <= committed) {
            m_table.pop_front();
        } else {
            break;
        }
    }
    FillWindow();
}

bool Scheduler::AllDone() const
{
    if (m_next_offset < m_limits.content_length) {return false;}
    for (const auto &range : m_table) {
        if (range.state != RState::DONE) {return false;}
    }
    return true;
}

size_t Scheduler::PendingCount() const
{
    size_t count = 0;
    for (const auto &range : m_table) {
        if (range.state == RState::PENDING) {count++;}
    }
    return count;
}

time_t Scheduler::NextNotBefore(time_t now) const
{
    time_t earliest = 0;
    for (const auto &range : m_table) {
        if (range.state != RState::PENDING) {continue;}
        if (range.not_before <= now) {return 0;}  // ready right now
        if (!earliest || range.not_before < earliest) {
            earliest = range.not_before;
        }
    }
    return earliest;
}

double Scheduler::BackoffDelay(unsigned attempt, std::mt19937 &rng)
{
    if (attempt == 0) {return 0.0;}
    const unsigned shift = std::min(attempt - 1, 16u);  // no overflow games
    double base = static_cast<double>(1u << shift);
    if (base > BACKOFF_CAP_SECS) {base = BACKOFF_CAP_SECS;}
    // Jitter in [0.5, 1.5]: desynchronizes ranges that failed together
    // (typically all of them, when a source blips).
    std::uniform_real_distribution<double> jitter(0.5, 1.5);
    double delay = base * jitter(rng);
    return std::min<double>(delay, BACKOFF_CAP_SECS);
}
