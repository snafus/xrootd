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

// The curl-driven transfer loop of the TPCR range scheduler (WP-4).  This
// replaces the stock multistream implementation: ALL pull transfers --
// including streams=1 (FR-7) -- run through here, driving the pure FSM in
// XrdHttpTpcRSchedulerCore.cc against a curl multi handle.
//
// Responsibilities: handle-slot management with a single ConfigureHandle
// (SUB-5), range issue with slab reservation (NFR-1), harvest with FR-12
// classification and FR-8 completion validation, per-range stall cancel
// (WP-4), perf markers from received bytes (SUB-3), the stock global stall
// backstop (SUB-11), and the FR-11 completion gate.

#include "XrdHttpTpcRTPC.hh"
#include "XrdHttpTpcRState.hh"
#include "XrdHttpTpcRStream.hh"
#include "XrdHttpTpcRScheduler.hh"
#include "XrdHttpTpcRJournal.hh"

#include "XrdSys/XrdSysError.hh"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace TPCR;

namespace {

// RAII wrapper for the curl multi handle.
class MultiHandle {
public:
    MultiHandle() : m_handle(curl_multi_init()) {}
    ~MultiHandle() {if (m_handle) {curl_multi_cleanup(m_handle);}}
    MultiHandle(const MultiHandle &) = delete;
    CURLM *Get() const {return m_handle;}
    explicit operator bool() const {return m_handle != nullptr;}
private:
    CURLM *m_handle;
};

// One easy handle + its State, and the range it currently serves.
struct HandleSlot {
    State *state = nullptr;
    bool busy = false;
    uint64_t range_id = 0;
};

// Outcome of the degraded-state recovery procedure (FR-14..FR-16).
enum class DegradedResult {
    Recovered,        // probe succeeded; restore full parallelism
    AdmitFailure,     // recovery budget exhausted with zero commit progress
    PermanentFailure, // source changed mid-session, or a permanent error
    ClientGone,       // chunk write failed (SUB-6): exit without verdict
};

} // namespace

bool TPCRHandler::ConfigureHandle(CURL *curl, State &state, TPCLogRecord &rec,
                                  const std::string &resource_url,
                                  const std::string &interface_ip)
{
    // SUB-5: THE single place transfer-handle options live -- applied at
    // handle creation and re-applied after every curl_easy_reset.  A reset
    // clears everything (callbacks, CA configuration, socket callbacks);
    // an option applied anywhere else would silently vanish on the first
    // per-range cancel/retry cycle.  CURLOPT_RANGE is intentionally NOT set
    // here: every issue sets it explicitly via SetTransferParameters, so a
    // stale range on a reused handle is impossible.
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long) CURL_HTTP_VERSION_1_1);
#if CURL_AT_LEAST_VERSION(7, 85, 0)
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR,       "https,http");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https,http");
#else
    long protocols = CURLPROTO_HTTP | CURLPROTO_HTTPS;
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, protocols);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, protocols);
#endif
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, opensocket_callback);
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETDATA, &rec);
    curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, sockopt_callback);
    curl_easy_setopt(curl, CURLOPT_SOCKOPTDATA, &rec);
    curl_easy_setopt(curl, CURLOPT_CLOSESOCKETFUNCTION, closesocket_callback);
    curl_easy_setopt(curl, CURLOPT_CLOSESOCKETDATA, &rec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT);
    if (!interface_ip.empty()) {
        // tpc.fixed_route: pin the local end to the IP the client used.
        curl_easy_setopt(curl, CURLOPT_INTERFACE, interface_ip.c_str());
    }
    ConfigureCurlLowSpeed(curl);
    curl_easy_setopt(curl, CURLOPT_URL, resource_url.c_str());
    state.InstallHandlers(curl);   // callbacks, WRITEDATA, CURLOPT_PRIVATE
    state.RebindHeaders();         // Copy-Header/TransferHeader slist
    if (!ConfigureCurlCA(curl, rec)) {
        return false;
    }
    return true;
}

int TPCRHandler::RunPullScheduler(XrdHttpExtReq &req, State &state,
                                  Stream &stream, size_t streams,
                                  const std::string &resource_url,
                                  const std::string &interface_ip,
                                  const SourceValidators &baseline,
                                  Checkpointer *checkpointer,
                                  TPCLogRecord &rec)
{
    std::vector<State*> states;
    std::vector<ManagedCurlHandle> owned_handles;
    try {
        int retval = RunPullSchedulerImpl(req, state, stream, streams,
                                          resource_url, interface_ip,
                                          baseline, checkpointer,
                                          states, owned_handles, rec);
        for (auto *each : states) {delete each;}
        return retval;
    } catch (std::runtime_error &e) {
        for (auto *each : states) {delete each;}
        logTransferEvent(LogMask::Error, rec, "SCHEDULER_ERROR", e.what());
        std::stringstream ss;
        ss << e.what();
        std::stringstream err_ss;
        err_ss << generateClientErr(ss, rec);
        int retval;
        if ((retval = req.ChunkResp(err_ss.str().c_str(), 0))) {
            return retval;
        }
        return req.ChunkResp(NULL, 0);
    }
}

int TPCRHandler::RunPullSchedulerImpl(XrdHttpExtReq &req, State &main_state,
                                      Stream &stream, size_t streams,
                                      const std::string &resource_url,
                                      const std::string &interface_ip,
                                      const SourceValidators &baseline,
                                      Checkpointer *checkpointer,
                                      std::vector<State*> &states,
                                      std::vector<ManagedCurlHandle> &owned_handles,
                                      TPCLogRecord &rec)
{
    const off_t content_length = main_state.GetContentLength();

    // Handle count: the effective parallelism is `streams` HTTP/1.1
    // connections (CURLMOPT_MAX_HOST_CONNECTIONS below); a small margin of
    // spare handles keeps a cancel/reset cycle from stalling an issue slot
    // (BUG-8: the stock streams x 16 handle pool served a pipelining
    // mechanism libcurl removed years ago).
    const size_t margin = 2;
    const size_t nhandles = streams + margin;

    // Slot 0 inherits the already-configured main handle (used for the HEAD)
    // and its headers; the extra handles are fresh and run through the same
    // ConfigureHandle path (SUB-5).
    states.reserve(nhandles);
    states.push_back(new State());
    states[0]->Move(main_state);
    for (size_t idx = 1; idx < nhandles; idx++) {
        CURL *curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("Failed to initialize internal transfer resources");
        }
        owned_handles.emplace_back(curl);
        State *extra = new State(0, stream, curl, /*push=*/false,
                                 req.tpcForwardCreds);
        states.push_back(extra);
        if (!ConfigureHandle(curl, *extra, rec, resource_url, interface_ip)) {
            throw std::runtime_error("Failed to configure the certificate authorities for the transfer");
        }
        extra->SetupHeaders(req);
    }

    std::vector<HandleSlot> slots(nhandles);
    for (size_t idx = 0; idx < nhandles; idx++) {
        slots[idx].state = states[idx];
    }

    rec.pmarkManager.startTransfer();

    MultiHandle multi;
    if (!multi) {
        throw std::runtime_error("Failed to initialize a libcurl multi-handle");
    }
    // No CURLMOPT_PIPELINING: dead since libcurl 7.62 (BUG-8).  The host
    // connection cap is what actually limits parallel connections.
    curl_multi_setopt(multi.Get(), CURLMOPT_MAX_HOST_CONNECTIONS,
                      static_cast<long>(streams));

    // The scheduler FSM: seeded at the stream's committed offset (0 for a
    // fresh transfer; the WP-8 resume path seeds W before we get here).
    Scheduler::Limits limits;
    limits.start_offset = stream.CommittedOffset();
    limits.content_length = content_length;
    limits.block_size = m_tpcr.block_size;
    limits.window_bytes = m_tpcr.window_bytes;
    limits.max_inflight = streams;
    limits.retry_max = m_tpcr.retry_max;
    limits.range_timeout = static_cast<int>(m_tpcr.range_timeout);
    Scheduler sched(limits, []() {return time(NULL);},
                    std::random_device{}());

    // Slab reservation (NFR-1): one slab acquired per issued range, parked
    // in the stash the Stream draws entries from.  If the pool has nothing,
    // the range simply is not issued yet -- backpressure, never failure.
    // The stash is trimmed to the in-flight count each round so reservations
    // for ranges that never needed buffering (in-order arrival writes
    // straight through) flow back to the pool.
    std::shared_ptr<SlabPool::Client> slab_client;
    if (m_slab_pool) {slab_client = m_slab_pool->RegisterClient();}
    std::vector<SlabPool::Slab> slab_stash;
    stream.SetSlabSource([&]() -> SlabPool::Slab {
        if (!slab_stash.empty()) {
            auto slab = std::move(slab_stash.back());
            slab_stash.pop_back();
            return slab;
        }
        if (slab_client) {return m_slab_pool->Acquire(slab_client);}
        return SlabPool::Slab();
    });

    // Start response to client prior to the first call to curl_multi_perform
    int retval = req.StartChunkedResp(202, NULL, "Content-Type: text/plain");
    if (retval) {
        logTransferEvent(LogMask::Error, rec, "RESPONSE_FAIL",
            "Failed to send the initial response to the TPC client");
        return retval;
    }
    logTransferEvent(LogMask::Debug, rec, "RESPONSE_START",
        "Initial transfer response sent to the TPC client");

    // Received-bytes accounting (SUB-3: markers report *received*, the
    // journal will record *committed*, the FSM tracks *scheduled*).
    off_t harvested_bytes = 0;
    auto received_bytes = [&]() -> off_t {
        off_t bytes = harvested_bytes;
        for (const auto &slot : slots) {
            if (slot.busy) {bytes += slot.state->BytesTransferred();}
        }
        return bytes;
    };

    int running_handles = 0;

    // Issues every range the FSM offers while a slot, a slab, and the
    // in-flight budget allow.  Returns false only on internal errors.
    auto issue_ready = [&](time_t now) {
        Scheduler::RangeRef ref;
        while (sched.NextIssuable(now, ref)) {
            // Find a free slot.
            HandleSlot *slot = nullptr;
            for (auto &candidate : slots) {
                if (!candidate.busy) {slot = &candidate; break;}
            }
            if (!slot) {break;}
            // Reserve a slab for this range's buffering needs (NFR-1).
            if (slab_client) {
                auto slab = m_slab_pool->Acquire(slab_client);
                if (!slab) {
                    // Pool saturated: defer, the range stays PENDING.
                    logTransferEvent(LogMask::Debug, rec, "SLAB_DEFER",
                        "Slab pool saturated; deferring range issue");
                    break;
                }
                slab_stash.push_back(std::move(slab));
            }
            // SUB-5: the range is set explicitly for every single issue.
            slot->state->SetTransferParameters(ref.offset, ref.length);
            CURLMcode mres = curl_multi_add_handle(multi.Get(),
                                                   slot->state->GetHandle());
            if (mres) {
                std::stringstream ss;
                ss << "Failed to add transfer to libcurl multi-handle: "
                   << curl_multi_strerror(mres);
                throw std::runtime_error(ss.str());
            }
            sched.MarkIssued(ref.id, now);
            slot->busy = true;
            slot->range_id = ref.id;
            running_handles++;
            if (m_log.getMsgMask() & LogMask::Debug) {
                std::stringstream ss;
                ss << "Issued range " << ref.offset << "-"
                   << (ref.offset + static_cast<off_t>(ref.length) - 1)
                   << " (id " << ref.id << ")";
                logTransferEvent(LogMask::Debug, rec, "RANGE_ISSUE", ss.str());
            }
        }
    };

    auto find_slot = [&](CURL *curl) -> HandleSlot * {
        for (auto &slot : slots) {
            if (slot.busy && slot.state->GetHandle() == curl) {return &slot;}
        }
        return nullptr;
    };

    // Failure verdicts collected for the final chunk.
    bool aborted = false;             // permanent/exhausted/stall failure
    std::string abort_msg;
    int abort_status = 0;             // HTTP status to report, if any
    CURLcode abort_curl_code = CURLE_OK;

    // Degraded-state machinery (FR-14..FR-16, WP-5).
    bool enter_degraded = false;
    std::string degraded_reason;
    bool auth_probe_used = false;     // FR-12: one re-probe for 401/403
    bool client_gone = false;         // SUB-6: stop writing to the client
    time_t last_commit_time = time(NULL);
    off_t last_committed_offset = stream.CommittedOffset();

    // Cancels an in-flight range's connection and re-queues (or aborts).
    // The handle is reset and reconfigured -- its connection state is
    // unknown after a cancel, so it must be rebuilt from scratch (SUB-5).
    auto cancel_and_requeue = [&](HandleSlot &slot, time_t now,
                                  const char *why) {
        State *state = slot.state;
        sched.OnProgress(slot.range_id, state->BytesTransferred(), now);
        curl_multi_remove_handle(multi.Get(), state->GetHandle());
        running_handles--;
        harvested_bytes += state->BytesTransferred();
        {
            std::stringstream ss;
            ss << "Cancelled in-flight range (id " << slot.range_id
               << "): " << why;
            logTransferEvent(LogMask::Info, rec, "RANGE_RETRY", ss.str());
        }
        auto disposition = sched.OnRangeResult(slot.range_id, false,
                                               FailureClass::Retryable, now);
        state->ResetAfterRequest();
        curl_easy_reset(state->GetHandle());
        if (!ConfigureHandle(state->GetHandle(), *state, rec, resource_url,
                             interface_ip)) {
            throw std::runtime_error("Failed to reconfigure a transfer handle after reset");
        }
        slot.busy = false;
        if (disposition == Scheduler::Disposition::Exhausted) {
            // FR-14: retry exhaustion escalates to the degraded state.
            enter_degraded = true;
            degraded_reason = "per-range retries exhausted";
        }
    };

    time_t last_marker = 0;
    off_t last_advance_bytes = 0;
    time_t last_advance_time = time(NULL);
    const time_t transfer_start = last_advance_time;

    // Refreshes the zero-commit-progress clock (FR-16's budget yardstick).
    auto refresh_commit_clock = [&]() {
        if (stream.CommittedOffset() > last_committed_offset) {
            last_committed_offset = stream.CommittedOffset();
            last_commit_time = time(NULL);
        }
    };

    // Marker upkeep usable from the recovery paths (FR-15: markers continue
    // throughout the degraded state, reporting the stalled byte count).
    // Returns false when the chunk write failed -- the client is gone.
    auto marker_tick = [&]() -> bool {
        const time_t tick_now = time(NULL);
        if (tick_now >= last_marker + m_marker_period) {
            if (SendPerfMarker(req, rec, states, received_bytes())) {
                return false;
            }
            last_marker = tick_now;
        }
        return true;
    };

    // ------------------------------------------------------------------ //
    //             Degraded state: Tier-1 recovery (FR-14..FR-16)          //
    // ------------------------------------------------------------------ //
    // Entered when per-range retries exhaust or an auth flap needs its
    // one-shot re-probe.  Tears down every connection, then repeatedly:
    // re-HEADs the source on a dedicated handle (SUB-7), revalidates the
    // session baseline (a definite change is a permanent failure), and runs
    // a single-connection probe range.  Success restores full parallelism
    // with fresh retry patience; the budget is recovery_maxsecs of ZERO
    // commit progress -- any committed byte restarts the clock.
    auto RunDegraded = [&]() -> DegradedResult {
        logTransferEvent(LogMask::Warning, rec, "DEGRADED_ENTER",
                         degraded_reason);
        time_t now = time(NULL);

        // Teardown (FR-14): dismantle every connection.  Ranges keep their
        // delivered prefixes and return to PENDING without attempt penalty.
        for (auto &slot : slots) {
            if (!slot.busy) {continue;}
            sched.OnProgress(slot.range_id, slot.state->BytesTransferred(), now);
            curl_multi_remove_handle(multi.Get(), slot.state->GetHandle());
            harvested_bytes += slot.state->BytesTransferred();
            slot.state->ResetAfterRequest();
            curl_easy_reset(slot.state->GetHandle());
            if (!ConfigureHandle(slot.state->GetHandle(), *slot.state, rec,
                                 resource_url, interface_ip)) {
                throw std::runtime_error("Failed to reconfigure a transfer handle after reset");
            }
            slot.busy = false;
        }
        running_handles = 0;
        sched.RequeueInFlight(now);

        const time_t degraded_enter = now;
        auto budget_deadline = [&]() -> time_t {
            return std::max(degraded_enter, last_commit_time) +
                   static_cast<time_t>(m_tpcr.recovery_maxsecs);
        };
        int probe_pause = 2;  // secs between re-probes, doubling up to 15

        while (true) {
            if (!marker_tick()) {return DegradedResult::ClientGone;}
            now = time(NULL);
            if (now > budget_deadline()) {return DegradedResult::AdmitFailure;}

            SourceValidators fresh;
            if (!ProbeSourceValidators(req, rec, resource_url, interface_ip,
                                       fresh)) {
                logTransferEvent(LogMask::Debug, rec, "DEGRADED_PROBE",
                    "source re-HEAD failed; continuing to wait out the outage");
            } else {
                std::string reason;
                if (!baseline.CompatibleMidSession(fresh, reason)) {
                    // FR-14: a mid-session source change is permanent.
                    degraded_reason = "source changed mid-transfer: " + reason;
                    logTransferEvent(LogMask::Error, rec, "SOURCE_CHANGED",
                                     reason);
                    return DegradedResult::PermanentFailure;
                }
                // Single-connection probe: prove a real data path end to
                // end before restoring parallelism.
                Scheduler::RangeRef probe_ref;
                if (!sched.NextIssuable(now, probe_ref)) {
                    logTransferEvent(LogMask::Info, rec, "DEGRADED_EXIT",
                        "no pending work after re-probe");
                    sched.ResetAttempts();
                    last_advance_time = time(NULL);
                    return DegradedResult::Recovered;
                }
                HandleSlot &slot = slots[0];
                slot.state->SetTransferParameters(probe_ref.offset,
                                                  probe_ref.length);
                curl_multi_add_handle(multi.Get(), slot.state->GetHandle());
                sched.MarkIssued(probe_ref.id, now);
                slot.busy = true;
                slot.range_id = probe_ref.id;
                running_handles = 1;

                bool settled = false, probe_ok = false;
                while (!settled) {
                    if (!marker_tick()) {
                        curl_multi_remove_handle(multi.Get(),
                                                 slot.state->GetHandle());
                        return DegradedResult::ClientGone;
                    }
                    refresh_commit_clock();
                    now = time(NULL);
                    if (now > budget_deadline()) {
                        curl_multi_remove_handle(multi.Get(),
                                                 slot.state->GetHandle());
                        sched.OnProgress(slot.range_id,
                                         slot.state->BytesTransferred(), now);
                        harvested_bytes += slot.state->BytesTransferred();
                        sched.RequeueInFlight(now);
                        slot.state->ResetAfterRequest();
                        slot.busy = false;
                        return DegradedResult::AdmitFailure;
                    }
                    curl_multi_perform(multi.Get(), &running_handles);
                    CURLMsg *msg;
                    int msgq = 0;
                    while ((msg = curl_multi_info_read(multi.Get(), &msgq))) {
                        if (msg->msg != CURLMSG_DONE) {continue;}
                        const CURLcode res = msg->data.result;
                        curl_multi_remove_handle(multi.Get(),
                                                 slot.state->GetHandle());
                        now = time(NULL);
                        sched.OnProgress(slot.range_id,
                                         slot.state->BytesTransferred(), now);
                        harvested_bytes += slot.state->BytesTransferred();
                        bool ok = (res == CURLE_OK) &&
                                  (slot.state->GetStatusCode() < 400) &&
                                  slot.state->ValidateRangeResponse(true);
                        FailureClass fclass = FailureClass::Retryable;
                        if (!ok) {
                            fclass = ClassifyCurlFailure(
                                static_cast<int>(res),
                                slot.state->GetStatusCode(),
                                slot.state->GetErrorCode());
                            // The one-shot auth re-probe is this very
                            // procedure: a second auth failure is final.
                            if (fclass == FailureClass::AuthRetry) {
                                fclass = FailureClass::Permanent;
                            }
                        }
                        const auto disposition = sched.OnRangeResult(
                            slot.range_id, ok, fclass, now);
                        probe_ok = (disposition == Scheduler::Disposition::Done);
                        if (!ok && fclass == FailureClass::Permanent) {
                            degraded_reason =
                                slot.state->GetErrorMessage().empty()
                                    ? std::string(curl_easy_strerror(res))
                                    : slot.state->GetErrorMessage();
                            slot.state->ResetAfterRequest();
                            slot.busy = false;
                            return DegradedResult::PermanentFailure;
                        }
                        slot.state->ResetAfterRequest();
                        if (!probe_ok) {
                            // Connection state unknown after the failure:
                            // rebuild the handle (SUB-5).
                            curl_easy_reset(slot.state->GetHandle());
                            if (!ConfigureHandle(slot.state->GetHandle(),
                                                 *slot.state, rec,
                                                 resource_url, interface_ip)) {
                                throw std::runtime_error("Failed to reconfigure a transfer handle after reset");
                            }
                        }
                        slot.busy = false;
                        settled = true;
                    }
                    if (!settled) {MultiWait(multi.Get(), 1000);}
                }
                running_handles = 0;
                refresh_commit_clock();
                if (probe_ok) {
                    logTransferEvent(LogMask::Warning, rec, "DEGRADED_EXIT",
                        "single-connection probe succeeded; restoring parallelism");
                    // Fresh patience against the recovered source, and a
                    // fresh steady-state stall clock: the outage window was
                    // governed by the recovery budget, not the global stall.
                    sched.ResetAttempts();
                    last_advance_bytes = received_bytes();
                    last_advance_time = time(NULL);
                    return DegradedResult::Recovered;
                }
                // Probe failed retryably: pause, then re-HEAD again.
            }
            // Pause between probes, keeping markers flowing (FR-15) and the
            // lease renewed (FR-22: expiry is 2 x checkpoint.secs; the timed
            // checkpoint trigger renews it even with zero commit progress,
            // and the data sync it implies is a no-op on a quiet handle).
            const time_t pause_end = time(NULL) + probe_pause;
            while (time(NULL) < pause_end) {
                if (!marker_tick()) {return DegradedResult::ClientGone;}
                if (checkpointer) {
                    checkpointer->MaybeCheckpoint(stream,
                                                  stream.CommittedOffset(),
                                                  time(NULL));
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            probe_pause = std::min(probe_pause * 2, 15);
        }
    };

    issue_ready(time(NULL));

    // ------------------------------------------------------------------ //
    //                          The transfer loop                         //
    // ------------------------------------------------------------------ //
    while (!aborted) {
        time_t now = time(NULL);

        // Feed per-range progress into the FSM's stall detector.
        for (auto &slot : slots) {
            if (slot.busy) {
                sched.OnProgress(slot.range_id,
                                 slot.state->BytesTransferred(), now);
            }
        }

        // Perf markers at the stock cadence, reporting received bytes
        // (absolute; on resume they include the seeded prefix -- FR-5).
        time_t next_marker = last_marker + m_marker_period;
        if (now >= next_marker) {
            const off_t bytes = received_bytes();
            if (bytes > last_advance_bytes) {
                last_advance_bytes = bytes;
                last_advance_time = now;
            }
            if (SendPerfMarker(req, rec, states, bytes)) {
                // SUB-6: a failed chunk write means the client is gone.
                // Stop scheduling; the client_gone path below flushes,
                // takes the FR-17 final checkpoint, and exits WITHOUT
                // further client writes.
                logTransferEvent(LogMask::Error, rec, "PERFMARKER_FAIL",
                    "Failed to send a perf marker to the TPC client; client disconnected");
                client_gone = true;
                break;
            }
            // Global stall backstop, stock semantics (SUB-11: outermost of
            // the three timers; operators' existing tuning still applies).
            int timeout = (transfer_start == last_advance_time)
                              ? m_first_timeout : m_timeout;
            if (now > last_advance_time + timeout) {
                aborted = true;
                std::stringstream ss;
                ss << "Transfer failed because no bytes have been received "
                   << "from the source (pull mode) in " << timeout << " seconds.";
                abort_msg = ss.str();
                break;
            }
            last_marker = now;
        }

        // Per-range stall detector: cancel and re-queue silent ranges.
        for (uint64_t id : sched.TimedOut(now)) {
            for (auto &slot : slots) {
                if (slot.busy && slot.range_id == id) {
                    cancel_and_requeue(slot, now, "no progress within tpcr.range.timeout");
                    break;
                }
            }
        }
        if (aborted) {break;}

        CURLMcode mres = curl_multi_perform(multi.Get(), &running_handles);
        if (mres != CURLM_OK && mres != CURLM_CALL_MULTI_PERFORM) {
            std::stringstream ss;
            ss << "Internal libcurl multi-handle error: "
               << curl_multi_strerror(mres);
            throw std::runtime_error(ss.str());
        }

        rec.pmarkManager.beginPMarks();

        // Harvest finished requests.
        CURLMsg *msg;
        do {
            int msgq = 0;
            msg = curl_multi_info_read(multi.Get(), &msgq);
            if (!msg || msg->msg != CURLMSG_DONE) {continue;}
            CURL *easy = msg->easy_handle;
            const CURLcode res = msg->data.result;
            HandleSlot *slot = find_slot(easy);
            if (!slot) {
                // A DONE message for a handle we do not track is an internal
                // inconsistency: fail rather than continue (NFR-7).
                throw std::runtime_error("libcurl reported completion of an unknown handle");
            }
            State *state = slot->state;
            curl_multi_remove_handle(multi.Get(), easy);
            running_handles--;

            const time_t harvest_now = time(NULL);
            sched.OnProgress(slot->range_id, state->BytesTransferred(),
                             harvest_now);
            harvested_bytes += state->BytesTransferred();

            // FR-8 completion gate + FR-12 classification at harvest time
            // (BUG-1: HTTP failures act here, immediately, not at the end).
            bool ok = (res == CURLE_OK) && (state->GetStatusCode() < 400);
            if (ok) {
                ok = state->ValidateRangeResponse(true);
            }
            FailureClass failure = FailureClass::Retryable;
            if (!ok) {
                failure = ClassifyCurlFailure(static_cast<int>(res),
                                              state->GetStatusCode(),
                                              state->GetErrorCode());
                if (failure == FailureClass::AuthRetry) {
                    // FR-12: 401/403 mid-session gets exactly one full
                    // re-probe (the source side may be a flapping gateway);
                    // a second occurrence is permanent -- this session
                    // cannot refresh its own token.
                    if (auth_probe_used) {
                        failure = FailureClass::Permanent;
                    } else {
                        auth_probe_used = true;
                        enter_degraded = true;
                        degraded_reason = "authorization rejected mid-session (one re-probe)";
                    }
                }
            }

            const auto disposition =
                sched.OnRangeResult(slot->range_id, ok, failure, harvest_now);

            switch (disposition) {
                case Scheduler::Disposition::Done:
                    break;
                case Scheduler::Disposition::Retry: {
                    std::stringstream ss;
                    ss << "Range request failed (";
                    if (res != CURLE_OK) {
                        ss << curl_easy_strerror(res);
                    } else if (state->GetStatusCode() >= 400) {
                        ss << "HTTP " << state->GetStatusCode();
                    } else {
                        ss << state->GetErrorMessage();
                    }
                    ss << "); re-queued with backoff";
                    logTransferEvent(LogMask::Info, rec, "RANGE_RETRY", ss.str());
                    break;
                }
                case Scheduler::Disposition::Exhausted:
                    // FR-14: escalate to the degraded state instead of
                    // admitting failure (which forfeits the partial).
                    enter_degraded = true;
                    degraded_reason = "per-range retries exhausted";
                    break;
                case Scheduler::Disposition::Permanent: {
                    aborted = true;
                    if (!state->GetErrorMessage().empty()) {
                        abort_msg = state->GetErrorMessage();
                    } else if (state->GetStatusCode() >= 400) {
                        std::stringstream ss;
                        ss << "Remote side failed with status code "
                           << state->GetStatusCode();
                        // The <=1 KiB error body accumulated by WriteCB.
                        abort_msg = ss.str();
                        abort_status = state->GetStatusCode();
                    } else {
                        abort_msg = curl_easy_strerror(res);
                        abort_curl_code = res;
                    }
                    if (state->GetStatusCode() >= 400) {
                        abort_status = state->GetStatusCode();
                    }
                    break;
                }
            }

            // The handle completed a full HTTP request cycle: its connection
            // is healthy and reusable, so do NOT reset it (a reset would
            // discard the connection and force a new TLS handshake per
            // range).  Per-request state is reconstructed instead; a fresh
            // CURLOPT_RANGE is set at the next issue (SUB-5).
            state->ResetAfterRequest();
            slot->busy = false;
        } while (msg);

        if (aborted) {break;}

        // Commit advance drives the window and the checkpoint engine
        // (FR-19; the SUB-1 data-sync-then-journal ordering lives inside).
        refresh_commit_clock();
        sched.AdvanceCommitted(stream.CommittedOffset());
        if (checkpointer) {
            checkpointer->MaybeCheckpoint(stream, stream.CommittedOffset(),
                                          time(NULL));
        }
        // Return excess slab reservations to the pool.
        while (slab_stash.size() > sched.InFlight()) {slab_stash.pop_back();}

        if (enter_degraded) {
            enter_degraded = false;
            switch (RunDegraded()) {
                case DegradedResult::Recovered:
                    break;  // main loop resumes with restored parallelism
                case DegradedResult::AdmitFailure: {
                    aborted = true;
                    std::stringstream ss;
                    ss << "no commit progress within the recovery budget ("
                       << m_tpcr.recovery_maxsecs
                       << "s); source unavailable, admitting failure";
                    abort_msg = ss.str();
                    break;
                }
                case DegradedResult::PermanentFailure:
                    aborted = true;
                    abort_msg = degraded_reason;
                    break;
                case DegradedResult::ClientGone:
                    client_gone = true;
                    break;
            }
            if (aborted || client_gone) {break;}
        }

        issue_ready(time(NULL));

        if (sched.AllDone()) {break;}

        if (running_handles == 0 && sched.InFlight() == 0) {
            // Nothing on the wire: either every pending range is backing
            // off, or admission is deferred (slab pressure).  Sleep until
            // the earliest backoff expiry or the next marker, whichever is
            // first -- never spin (BUG-9, T-U10).
            now = time(NULL);
            time_t wake = last_marker + m_marker_period;
            const time_t horizon = sched.NextNotBefore(now);
            if (horizon && horizon < wake) {wake = horizon;}
            int64_t sleep_ms = static_cast<int64_t>(wake - now) * 1000;
            if (sleep_ms < 100) {sleep_ms = 100;}
            if (MultiWait(multi.Get(), static_cast<int>(sleep_ms)) != CURLM_OK) {
                throw std::runtime_error("libcurl multi-wait failure");
            }
            continue;
        }

        int64_t max_sleep_ms =
            static_cast<int64_t>(next_marker - time(NULL)) * 1000;
        if (max_sleep_ms <= 0) {continue;}
        if (MultiWait(multi.Get(), static_cast<int>(max_sleep_ms)) != CURLM_OK) {
            throw std::runtime_error("libcurl multi-wait failure");
        }
    }

    if (client_gone) {
        // SUB-6: the client connection is dead.  Stop entirely, flush what
        // the reorder buffers hold, take the FR-17 final checkpoint, and
        // exit WITHOUT attempting any further write to the client -- this
        // is a distinct event, not a transfer failure: no verdict was
        // sent, so a shared-filesystem retry can resume from W.
        stream.Flush();
        if (checkpointer) {
            checkpointer->FinalCheckpoint(stream, stream.CommittedOffset());
        }
        logTransferEvent(LogMask::Info, rec, "CLIENT_DISCONNECT",
            "Client connection lost; checkpointed and exiting without verdict");
        return -1;
    }

    // ------------------------------------------------------------------ //
    //                             Wrap-up                                //
    // ------------------------------------------------------------------ //

    // Flush whatever the reorder buffers still hold.  A flush failure never
    // masks the transfer failure that usually caused it (stock behavior).
    std::string flush_error;
    if (stream.Flush() == SFS_ERROR) {
        flush_error = stream.GetErrorMessage();
        if (flush_error.empty()) {flush_error = "(no error message provided)";}
        std::replace(flush_error.begin(), flush_error.end(), '\n', ' ');
        flush_error = "Failed to flush the file to the local filesystem. " + flush_error;
        logTransferEvent(LogMask::Error, rec, "FLUSH_FAIL", flush_error);
    }

    rec.bytes_transferred = received_bytes();
    if (abort_status) {rec.tpc_status = abort_status;}

    // FR-17: on any admitted failure the final checkpoint (data sync +
    // journal update) is taken BEFORE the failure chunk goes out, so a
    // shared-filesystem retry can resume even though orchestrator cleanup
    // may render it moot.
    if (aborted && checkpointer) {
        checkpointer->FinalCheckpoint(stream, stream.CommittedOffset());
    }

    std::stringstream final_ss;
    bool success = false;
    if (aborted) {
        std::stringstream ss2;
        ss2 << abort_msg;
        logTransferEvent(LogMask::Error, rec, "SCHEDULER_FAIL", abort_msg);
        if (!flush_error.empty()) {ss2 << "; " << flush_error;}
        final_ss << generateClientErr(ss2, rec, abort_curl_code);
    } else if (!flush_error.empty()) {
        std::stringstream ss2;
        ss2 << flush_error;
        final_ss << generateClientErr(ss2, rec);
    } else if (stream.CommittedOffset() != content_length || !sched.AllDone()) {
        // FR-11 completion gate: never report success unless the destination
        // provably holds every byte.  Reaching here is an internal
        // inconsistency (NFR-7) -- fail loudly.
        std::stringstream ss2;
        ss2 << "Internal completion gate failure: committed offset "
            << stream.CommittedOffset() << " of " << content_length;
        logTransferEvent(LogMask::Error, rec, "SCHEDULER_FAIL", ss2.str());
        final_ss << generateClientErr(ss2, rec);
    } else if (checkpointer && !([&]() {
                   // FR-23 success ordering: final data sync -> [checksum
                   // injection, WP-11] -> journal delete; only then close
                   // and only then the success chunk.
                   std::string cleanup_err;
                   if (!checkpointer->SuccessCleanup(stream, cleanup_err)) {
                       std::stringstream ss2;
                       ss2 << cleanup_err;
                       logTransferEvent(LogMask::Error, rec, "SCHEDULER_FAIL",
                                        cleanup_err);
                       final_ss << generateClientErr(ss2, rec);
                       return false;
                   }
                   return true;
               })()) {
        // final_ss already carries the failure message.
    } else if (!states[0]->Finalize()) {
        std::stringstream ss2;
        ss2 << "Failed to finalize and close file handle.";
        std::string finalize_error = states[0]->GetFinalizeErrorMessage();
        if (finalize_error.size()) {
            std::replace(finalize_error.begin(), finalize_error.end(), '\n', ' ');
            ss2 << " " << finalize_error;
        }
        logTransferEvent(LogMask::Error, rec, "SCHEDULER_FAIL", ss2.str());
        final_ss << generateClientErr(ss2, rec);
    } else {
        final_ss << "success: Created";
        success = true;
    }

    if (stream.OverflowEntries()) {
        std::stringstream ss;
        ss << stream.OverflowEntries()
           << " reorder entries fell back to heap allocation (slab pool pressure)";
        logTransferEvent(LogMask::Debug, rec, "SLAB_OVERFLOW", ss.str());
    }

    if ((retval = req.ChunkResp(final_ss.str().c_str(), 0))) {
        logTransferEvent(LogMask::Error, rec, "TRANSFER_ERROR",
            "Failed to send last update to remote client");
        return retval;
    }
    if (success) {
        logTransferEvent(LogMask::Info, rec, "TRANSFER_SUCCESS");
        rec.status = 0;
    }
    return req.ChunkResp(NULL, 0);
}
