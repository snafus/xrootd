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

#include "XrdSys/XrdSysError.hh"

#include <curl/curl.h>

#include <algorithm>
#include <random>
#include <sstream>
#include <stdexcept>
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
                                  TPCLogRecord &rec)
{
    std::vector<State*> states;
    std::vector<ManagedCurlHandle> owned_handles;
    try {
        int retval = RunPullSchedulerImpl(req, state, stream, streams,
                                          resource_url, interface_ip,
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
        if (disposition == Scheduler::Disposition::Exhausted && !aborted) {
            // WP-5 turns this into the degraded-state entry; until then,
            // exhaustion is an admitted failure with a distinct message.
            aborted = true;
            abort_msg = "range retries exhausted while the source is unresponsive";
        }
    };

    time_t last_marker = 0;
    off_t last_advance_bytes = 0;
    time_t last_advance_time = time(NULL);
    const time_t transfer_start = last_advance_time;

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
                // Stop scheduling and exit WITHOUT further client writes.
                // (FR-17 final checkpoint runs here once WP-7 lands.)
                logTransferEvent(LogMask::Error, rec, "PERFMARKER_FAIL",
                    "Failed to send a perf marker to the TPC client; client disconnected");
                return -1;
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
                // WP-4 interim: the one-shot re-probe for auth flaps is
                // WP-5's degraded machinery; treat as permanent until then.
                if (failure == FailureClass::AuthRetry) {
                    failure = FailureClass::Permanent;
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
                    aborted = true;
                    abort_msg = "range retries exhausted while the source is unresponsive";
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

        // Commit advance drives the window (and, from WP-7, checkpoints).
        sched.AdvanceCommitted(stream.CommittedOffset());
        // Return excess slab reservations to the pool.
        while (slab_stash.size() > sched.InFlight()) {slab_stash.pop_back();}

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

    // FR-17: on any admitted failure a final checkpoint (data sync + journal
    // update) is taken before the failure chunk.  No-op until WP-7 lands the
    // journal; the call site is deliberately in place already.

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
