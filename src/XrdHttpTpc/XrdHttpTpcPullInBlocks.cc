/**
 * A multistream pull in blocks: the stream threads and the thread of the
 * request that coordinates them.  See XrdHttpTpcBlockPull.hh for how a pull
 * in blocks works and for the rules on which thread may touch what.
 */

#include "XrdHttpTpcBlockPull.hh"
#include "XrdHttpTpcPullOptions.hh"
#include "XrdHttpTpcState.hh"
#include "XrdHttpTpcStream.hh"
#include "XrdHttpTpcTPC.hh"

#include "XrdSys/XrdSysError.hh"
#include "XrdSys/XrdSysPthread.hh"

#include <algorithm>
#include <cstring>
#include <memory>
#include <sstream>

using namespace TPC;

namespace {

// Seconds on a clock that a change of the system time does not move: the
// markers and the stall check are timed with it.
long long SteadySeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// "bytes 0-1048575" for the block [start, end), as in a range request.
std::string BlockText(off_t start, off_t end)
{
    std::stringstream ss;
    ss << "bytes " << start << "-" << (end - 1);
    return ss.str();
}

// Where a failure happened, to follow its cause in a message: "; stream 2 of
// 4, block bytes 0-1048575", or only the stream, or nothing when the failure
// is not that of a stream.
std::string Where(const BlockFailure &failure, size_t workers, bool with_block)
{
    if (!failure.stream) {return std::string();}
    std::stringstream ss;
    ss << "; stream " << failure.stream << " of " << workers;
    if (with_block && failure.block_end > failure.block_start) {
        ss << ", block " << BlockText(failure.block_start, failure.block_end);
    }
    return ss.str();
}

// A text from elsewhere, made fit to be followed by "; more": on one line and
// without a full stop at its end.
std::string OneLine(std::string text)
{
    std::replace(text.begin(), text.end(), '\n', ' ');
    while (!text.empty() && (text.back() == '.' || text.back() == ' ')) {text.pop_back();}
    return text;
}

// The progress callback of libcurl: the only way to end a request from
// another thread.  libcurl calls it about once a second, and more often
// while data arrives.  It is not called while the worker is inside a write.
int StopCallback(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return static_cast<BlockPull*>(clientp)->stop.load(std::memory_order_relaxed) ? 1 : 0;
}

// What a worker is started with.  The coordinator keeps these in a vector
// that outlives the threads.
struct WorkerStart {
    State *state;      // owned by this worker until it has been joined
    BlockPull *pull;   // outlives the worker
    size_t stream;     // counted from 1, for the log and the messages
};

// One worker: requests one block after another on its own libcurl handle.
// The entry point of a thread made by XrdSysThread::Run(): it must not let
// an exception out, which would end the server.
void *RunWorker(void *arg) noexcept
{
    WorkerStart *start = static_cast<WorkerStart*>(arg);
    State *state = start->state;
    BlockPull *pull = start->pull;
    const size_t stream = start->stream;
    // The block being fetched; no block between two of them.
    off_t offset = 0, size = 0;
    bool in_block = false;
    try {
        while (pull->NextBlock(offset, size)) {
            in_block = true;
            const std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
            state->SetTransferParameters(offset, size);
            CURLcode result = curl_easy_perform(state->GetHandle());
            bool flushed = true;
            if (result == CURLE_OK) {
                // Write the last, short piece of the block; then check that
                // the body was as long as the range.
                flushed = state->FlushWriteBuffer();
                state->CheckRangeComplete();
            }
            const int error = state->GetErrorCode();
            const int status = state->GetStatusCode();

            if (!error && pull->stop.load()
                && (result == CURLE_ABORTED_BY_CALLBACK || result == CURLE_WRITE_ERROR || !flushed)) {
                // Stopped because of a failure elsewhere, which is already
                // recorded: this is not a cause.  The stop was seen by the
                // progress callback, or before a write to the file, which
                // then was not made and ended the request with a write error.
                break;
            }

            BlockFailure failure;
            failure.stream = stream;
            failure.block_start = offset;
            failure.block_end = offset + size;
            failure.block_received = state->BytesTransferred();

            if (!flushed && !error) {
                // Not reachable as the code stands: a flush that fails
                // without an error is one that saw the stop, and that case
                // has left the loop above.  A block whose end was not written
                // must never count as done.
                failure.error_code = BlockPull::errInternal;
                failure.message = "The end of a block was not written to the file";
                pull->Fail(failure);
                break;
            }
            // libcurl also returns CURLE_OK for a final answer such as 304,
            // which brings no part of the file: see State::CarriesContent().
            const bool failed_status = status >= 400
                || (result == CURLE_OK && !State::CarriesContent(status));
            if (error || failed_status || result != CURLE_OK) {
                failure.error_code = error;
                failure.status_code = failed_status ? status : 0;
                failure.curl_result = result;
                failure.message = state->GetErrorMessage();
                pull->Fail(failure);
                break;
            }

            const std::chrono::duration<double> took = std::chrono::steady_clock::now() - began;
            pull->BlockDone(stream, offset, size, took.count());
            in_block = false;
            state->ResetAfterRequest();
        }
    } catch (...) {
        // No text is built here: building one may throw again, and an
        // exception that leaves a thread ends the server.
        BlockFailure failure;
        failure.error_code = BlockPull::errInternal;
        failure.stream = stream;
        if (in_block) {
            failure.block_start = offset;
            failure.block_end = offset + size;
        }
        pull->Fail(failure);
    }
    pull->WorkerEnded();
    return nullptr;
}

// Stops and joins the workers on every way out of the function that started
// them, and takes back what their handles were given.  After its destructor
// no thread of the pull exists.
//
// The threads are made with XrdSysThread::Run(), as elsewhere in the server,
// which gives them the stack size the server is configured with.  They are
// made to be joined (XRDSYSTHREAD_HOLD).
struct WorkerJoiner {
    BlockPull &pull;
    std::vector<pthread_t> &threads;
    std::vector<std::unique_ptr<State>> &states;

    void Join() {
        for (pthread_t thread : threads) {
            XrdSysThread::Join(thread, NULL);
        }
        threads.clear();
    }

    ~WorkerJoiner() {
        pull.Stop();
        Join();
        for (std::unique_ptr<State> &state : states) {
            state->SetPullCounters(nullptr, nullptr);
            state->SetPullStopFlag(nullptr);
            curl_easy_setopt(state->GetHandle(), CURLOPT_XFERINFOFUNCTION, NULL);
            curl_easy_setopt(state->GetHandle(), CURLOPT_XFERINFODATA, NULL);
            curl_easy_setopt(state->GetHandle(), CURLOPT_NOPROGRESS, 1L);
        }
    }
};

}

/******************************************************************************/
/*            T P C H a n d l e r : : R u n P u l l I n B l o c k s           */
/******************************************************************************/

int TPCHandler::RunPullInBlocks(XrdHttpExtReq &req, State &state, Stream &stream,
    const BlockPlan &plan, size_t streams_asked, StreamThreadBudget::Share &stream_threads,
    TPCLogRecord &rec)
{
    // ProcessPullReq() gave the state the size of the source file; it is not
    // zero here, and the plan has at least two workers.  Packet marking is
    // off: a pull with it is not carried out in blocks yet.
    const off_t file_size = state.GetContentLength();
    const size_t planned = plan.workers;
    const std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();

    {
        std::stringstream ss;
        // The standard fields already give streams=, the number granted.
        ss << "mode=blocks, asked=" << streams_asked << ", workers=" << planned
           << ", blocks=" << plan.blocks << ", block_size=" << plan.block_size
           << ", write_size=" << m_pull_options.write_size << ", file_size=" << file_size;
        logTransferEvent(LogMask::Info, rec, "MULTISTREAM_START", ss.str());
    }

    // One state and one libcurl handle for each worker, all made here before
    // any thread exists.  The copies are released when this function returns:
    // first the states, then their handles.
    std::vector<ManagedCurlHandle> curl_handles;
    std::vector<std::unique_ptr<State>> states;
    try {
        states.emplace_back(new State());
        states[0]->Move(state);
        for (size_t idx = 1; idx < planned; idx++) {
            states.emplace_back(states[0]->Duplicate());
            curl_handles.emplace_back(states.back()->GetHandle());
        }
    } catch (std::exception &e) {
        // The response to the client is not started yet: answer with a
        // complete one.
        rec.status = 500;
        std::stringstream ss;
        ss << "Unable to set up the streams of the transfer: " << e.what();
        logTransferEvent(LogMask::Error, rec, "MULTISTREAM_ERROR", ss.str());
        return req.SendSimpleResp(rec.status, NULL, NULL, generateClientErr(ss, rec).c_str(), 0);
    }

    int retval = req.StartChunkedResp(202, NULL, "Content-Type: text/plain");
    if (retval) {
        logTransferEvent(LogMask::Error, rec, "RESPONSE_FAIL",
            "Failed to send the initial response to the TPC client");
        return retval;
    }
    logTransferEvent(LogMask::Debug, rec, "RESPONSE_START",
        "Initial transfer response sent to the TPC client");

    BlockPull pull(file_size, plan.block_size, planned);

    // A pull that does not succeed leaves only complete blocks in the file.
    // Without this the file could have the size of the source and a hole in
    // it; and the storage could store a checksum for content it does not
    // hold, since its real-time checksum counts a write that the storage cut
    // short as written in full.  A cut to a length other than zero makes that
    // layer give its checksum up.
    //
    // This object does it on the ways out that give no verdict: the client is
    // gone, or an exception was caught below.  It is declared before the
    // joiner, so that it runs after the workers are gone.
    struct CutBack {
        TPCHandler &handler;
        Stream &stream;
        BlockPull &pull;
        TPCLogRecord &rec;
        bool armed;
        ~CutBack() {
            if (!armed) {return;}
            try {
                bool cut_failed = false;
                const std::string left = handler.CutBackFailedPull(stream, pull, rec, cut_failed);
                // A failed cut has been logged; otherwise say here what is
                // left, at a level that is on by default: no verdict line
                // will say it.
                if (!cut_failed) {handler.logTransferEvent(LogMask::Warning, rec, "TRUNCATE", left);}
            } catch (...) {
                // The file stays as it is; the pull is not reported as a success in any case.
            }
        }
    } cut_back{*this, stream, pull, rec, true};

    std::vector<pthread_t> threads;
    std::vector<WorkerStart> starts;
    WorkerJoiner joiner{pull, threads, states};

    for (std::unique_ptr<State> &worker_state : states) {
        worker_state->SetPullCounters(&pull.received, &pull.written);
        worker_state->SetPullStopFlag(&pull.stop);
        curl_easy_setopt(worker_state->GetHandle(), CURLOPT_XFERINFOFUNCTION, StopCallback);
        curl_easy_setopt(worker_state->GetHandle(), CURLOPT_XFERINFODATA, &pull);
        curl_easy_setopt(worker_state->GetHandle(), CURLOPT_NOPROGRESS, 0L);
    }

    // No exception may leave this function from here on without being
    // caught: the server has no handler above it, and without one the
    // workers would not be joined before the process ends.
    bool client_gone = false;
    bool failure_logged = false;
    size_t workers = planned;
    try {
        // Filled before the first thread starts and not changed afterwards:
        // the threads keep pointers into it.
        threads.reserve(planned);
        starts.reserve(planned);
        for (size_t idx = 0; idx < planned; idx++) {
            starts.push_back(WorkerStart{states[idx].get(), &pull, idx + 1});
        }
        for (size_t idx = 0; idx < planned; idx++) {
            pull.WorkerStarting();
            pthread_t thread;
            // No description is given: with one, every stream of every pull
            // would leave a "thread started" line in the log.  Run() returns
            // the error number itself and does not set errno.
            const int reason = XrdSysThread::Run(&thread, RunWorker, &starts[idx], XRDSYSTHREAD_HOLD);
            if (reason) {
                pull.WorkerNotStarted();
                workers = threads.size();
                std::stringstream ss;
                if (workers) {
                    // Go on with the threads there are: any of them can
                    // fetch any block.
                    ss << "Unable to start stream " << (idx + 1) << " of " << planned << ": "
                       << strerror(reason) << "; the transfer goes on with " << workers
                       << (workers == 1 ? " stream" : " streams");
                    logTransferEvent(LogMask::Warning, rec, "STREAM_START_FAIL", ss.str());
                } else {
                    ss << "Unable to start a thread for the transfer: " << strerror(reason);
                    logTransferEvent(LogMask::Error, rec, "STREAM_START_FAIL", ss.str());
                    BlockFailure failure;
                    failure.error_code = BlockPull::errInternal;
                    failure.message = ss.str();
                    pull.Fail(failure);
                    failure_logged = true;
                }
                break;
            }
            threads.push_back(thread);
        }
        // What was not started is given back to the server, and the record
        // says how many streams the pull really has.
        stream_threads.ShrinkTo(static_cast<int>(workers));
        rec.streams = workers;

        // The coordinator: markers to the client, the log, and the watch
        // that the pull moves.  Progress is bytes received plus bytes
        // written: when every byte has arrived and a slow storage is still
        // taking the last writes, the pull is not stalled.
        long long last_marker = 0;
        off_t last_advance_bytes = 0;
        long long last_advance_time = SteadySeconds();
        const long long transfer_start = last_advance_time;
        long long stop_time = 0;
        bool slow_stop_logged = false;
        while (!pull.WaitForWorkers(std::chrono::seconds(1))) {
            const long long now = SteadySeconds();
            const off_t received = pull.received.load();
            const off_t written = pull.written.load();
            if (received + written > last_advance_bytes) {
                last_advance_bytes = received + written;
                last_advance_time = now;
            }
            LogBlockReports(pull, workers, rec);

            if (pull.stop.load()) {
                // A failure is recorded and the workers are stopping.  One
                // that is inside a write to the storage cannot be
                // interrupted, and this thread must wait for it.  No marker
                // is sent meanwhile: a client that watches for markers must
                // not take the pull for alive.
                if (!failure_logged) {
                    LogStreamFailure(pull, workers, rec);
                    failure_logged = true;
                }
                if (!stop_time) {stop_time = now;}
                if (!slow_stop_logged && now > stop_time + m_timeout) {
                    std::stringstream ss;
                    ss << "running=" << pull.Running() << ", seconds=" << (now - stop_time)
                       << "; the streams have not stopped since the transfer failed: a write"
                          " to the storage or a connection attempt may be hanging";
                    logTransferEvent(LogMask::Error, rec, "MULTISTREAM_STOP_SLOW", ss.str());
                    slow_stop_logged = true;
                }
                continue;
            }
            if (now >= last_marker + m_marker_period) {
                // SendPerfMarker() does not use the list of states of a multistream pull.
                std::vector<State*> unused;
                if (SendPerfMarker(req, rec, unused, received)) {
                    logTransferEvent(LogMask::Error, rec, "PERFMARKER_FAIL",
                        "Failed to send a perf marker to the TPC client; stopping the transfer");
                    client_gone = true;
                    // The cause is the line above; nothing more is logged for it.
                    failure_logged = true;
                    BlockFailure failure;
                    failure.error_code = BlockPull::errInternal;
                    failure.message = "The TPC client is gone";
                    pull.Fail(failure);
                    break;
                }
                last_marker = now;
            }
            const int timeout = (transfer_start == last_advance_time) ? m_first_timeout : m_timeout;
            if (now > last_advance_time + timeout) {
                // Every worker stops at its next progress callback; the loop
                // ends when they have.
                BlockFailure failure;
                failure.error_code = State::errTimeout;
                std::stringstream ss;
                // When every worker holds a full write of data, or all the
                // data has arrived, it is the storage that does not move.
                if (received >= file_size
                    || received - written >= static_cast<off_t>(workers) * m_pull_options.write_size) {
                    ss << "Transfer failed because the local storage has not taken any data in "
                       << timeout << " seconds (" << received << " bytes received, " << written
                       << " written).";
                } else {
                    ss << "Transfer failed because no bytes have been received from the source"
                          " (pull mode) in " << timeout << " seconds.";
                }
                failure.message = ss.str();
                pull.Fail(failure);
            }
        }

        // No worker is left after this, so what follows has the pull to itself.
        joiner.Join();
        LogBlockReports(pull, workers, rec);
        if (pull.Failed() && !failure_logged) {LogStreamFailure(pull, workers, rec);}

        rec.bytes_transferred = pull.received.load();
        if (client_gone) {
            // Nobody is left to tell; the file is cut back on the way out.
            return -1;
        }
        const std::chrono::duration<double> took = std::chrono::steady_clock::now() - began;
        return ConcludePullInBlocks(req, *states[0], stream, pull, workers, took.count(),
                                    cut_back.armed, rec);
    } catch (...) {
        // In practice: out of memory.  Nothing that could throw again is done
        // here; the joiner stops and joins the workers and the file is cut
        // back, on the way out.  The response is left unfinished, which the
        // client takes for a failure.
        BlockFailure failure;
        failure.error_code = BlockPull::errInternal;
        pull.Fail(failure);
        return -1;
    }
}

/******************************************************************************/
/*       T P C H a n d l e r : : C o n c l u d e P u l l I n B l o c k s      */
/******************************************************************************/

// The verdict of a pull in blocks whose workers have all been joined: what
// is logged, what the client is told, and the close of the file.
//
// The first words of each message to the client are those of the sequential
// mode, so that a transfer client that looks for them finds them; the stream
// and the block follow.
int TPCHandler::ConcludePullInBlocks(XrdHttpExtReq &req, State &first_state, Stream &stream,
    BlockPull &pull, size_t workers, double seconds, bool &cut_back_armed, TPCLogRecord &rec)
{
    const off_t file_size = pull.FileSize();
    const BlockFailure failure = pull.Failure();
    const bool failed = pull.Failed();
    const size_t blocks_done = pull.BlocksDone();
    const off_t received = pull.received.load();
    const off_t written = pull.written.load();
    // Never close as complete a file of which a part is missing.  The bytes
    // written are checked as well as the bytes received: they differ if data
    // that was received was dropped before its write.
    const bool complete = !failed && pull.AllHandedOut() && blocks_done == pull.Blocks()
                          && received == file_size && written == file_size;
    // The status of the source, when one failed the pull.  Without one the
    // field is left out of the log line: 0 is not an HTTP status.
    if (failure.status_code) {rec.tpc_status = failure.status_code;}

    // What the log line of a failure says about the file.  It is cut back
    // and closed now, before the client is told: a client that removes the
    // file of a failed transfer must not find it open.
    std::string left;
    if (!complete) {
        bool cut_failed = false;
        left = "; " + CutBackFailedPull(stream, pull, rec, cut_failed);
        cut_back_armed = false;
        first_state.Finalize();
    }

    std::stringstream ss;
    bool success = false;
    if (failed && failure.status_code != 0) {
        std::stringstream ss2;
        ss2 << "Remote side failed with status code " << failure.status_code;
        if (!failure.message.empty()) {
            ss2 << "; error message: \"" << OneLine(failure.message) << "\"";
        }
        ss2 << Where(failure, workers, true);
        logTransferEvent(LogMask::Error, rec, "MULTISTREAM_FAIL", ss2.str() + left);
        ss << generateClientErr(ss2, rec);
    } else if (failed && failure.error_code == State::errTimeout) {
        // The stall check of the coordinator: the message says what did not
        // move, and no stream is to blame more than another.
        std::stringstream ss2;
        ss2 << failure.message;
        logTransferEvent(LogMask::Error, rec, "MULTISTREAM_FAIL", OneLine(failure.message) + left);
        ss << generateClientErr(ss2, rec);
    } else if (failed && failure.error_code == State::errRange) {
        // The message names the range that was asked for and what came.
        std::stringstream ss2;
        ss2 << OneLine(failure.message) << Where(failure, workers, false);
        logTransferEvent(LogMask::Error, rec, "MULTISTREAM_FAIL", ss2.str() + left);
        ss << generateClientErr(ss2, rec);
    } else if (failed && failure.error_code == BlockPull::errInternal) {
        std::stringstream ss2;
        ss2 << (failure.message.empty() ? std::string("Internal error in a stream")
                                        : OneLine(failure.message))
            << Where(failure, workers, true);
        logTransferEvent(LogMask::Error, rec, "MULTISTREAM_FAIL", ss2.str() + left);
        ss << generateClientErr(ss2, rec);
    } else if (failed && failure.error_code) {
        // The message names the write and its offset.
        std::stringstream ss2;
        ss2 << "Error when interacting with local filesystem: "
            << (failure.message.empty() ? std::string("(no error message provided)")
                                        : OneLine(failure.message))
            << Where(failure, workers, false);
        logTransferEvent(LogMask::Error, rec, "MULTISTREAM_FAIL", ss2.str() + left);
        ss << generateClientErr(ss2, rec);
    } else if (failed) {
        // generateClientErr() adds the text of libcurl for the client.
        std::stringstream ss2;
        ss2 << "Request failed when processing" << Where(failure, workers, true);
        std::stringstream ss3;
        ss3 << "Request failed when processing: " << curl_easy_strerror(failure.curl_result)
            << Where(failure, workers, true);
        logTransferEvent(LogMask::Error, rec, "MULTISTREAM_FAIL", ss3.str() + left);
        ss << generateClientErr(ss2, rec, failure.curl_result);
    } else if (!complete) {
        std::stringstream ss2;
        ss2 << "Internal logic error led to early abort; " << blocks_done << " of " << pull.Blocks()
            << " blocks, " << received << " of " << file_size << " bytes received, "
            << written << " of " << file_size << " bytes written";
        logTransferEvent(LogMask::Error, rec, "MULTISTREAM_FAIL", ss2.str() + left);
        ss << generateClientErr(ss2, rec);
    } else {
        // Every block is in the file: there is nothing to cut back, whatever
        // the close says.
        cut_back_armed = false;
        if (first_state.Flush() == -1 || !first_state.Finalize()) {
            std::stringstream ss2;
            ss2 << "Failed to finalize and close file handle.";
            std::string handleErrMsg = first_state.GetFinalizeErrorMessage();
            if (handleErrMsg.size()) {
                std::replace(handleErrMsg.begin(), handleErrMsg.end(), '\n', ' ');
                ss2 << " " << handleErrMsg;
            }
            ss << generateClientErr(ss2, rec);
            logTransferEvent(LogMask::Error, rec, "MULTISTREAM_ERROR", ss2.str());
        } else {
            // The pull has the whole file: for a client that is a complete
            // 200 (OK) response (RFC 9110, Section 15.3.7.3), and 200 is what
            // a single-stream pull logs for a success.
            rec.tpc_status = 200;
            ss << "success: Created";
            success = true;
        }
    }

    int retval;
    if ((retval = req.ChunkResp(ss.str().c_str(), 0))) {
        logTransferEvent(LogMask::Error, rec, "TRANSFER_ERROR",
            "Failed to send last update to remote client");
        return retval;
    } else if (success) {
        std::stringstream ss2;
        ss2 << "mode=blocks, workers=" << workers << ", blocks=" << pull.Blocks();
        ss2.setf(std::ios::fixed);
        ss2.precision(1);
        ss2 << ", seconds=" << seconds;
        if (seconds > 0) {
            ss2 << ", rate_MBps=" << (static_cast<double>(file_size) / 1e6 / seconds);
        }
        logTransferEvent(LogMask::Info, rec, "TRANSFER_SUCCESS", ss2.str());
        rec.status = 0;
    }
    return req.ChunkResp(NULL, 0);
}

/******************************************************************************/
/*          T P C H a n d l e r : : L o g B l o c k R e p o r t s             */
/******************************************************************************/

void TPCHandler::LogBlockReports(BlockPull &pull, size_t workers, TPCLogRecord &rec)
{
    // Taken in any case, so that they do not pile up when they are not logged.
    const std::vector<BlockReport> reports = pull.TakeReports();
    if (!(m_log.getMsgMask() & LogMask::Debug)) {return;}
    for (const BlockReport &report : reports) {
        std::stringstream ss;
        ss << "stream=" << report.stream << ", workers=" << workers
           << ", block=" << report.start << "-" << (report.end - 1);
        ss.setf(std::ios::fixed);
        ss.precision(1);
        ss << ", seconds=" << report.seconds;
        if (report.seconds > 0) {
            ss << ", rate_MBps=" << (static_cast<double>(report.end - report.start) / 1e6 / report.seconds);
        }
        logTransferEvent(LogMask::Debug, rec, "BLOCK_DONE", ss.str());
    }
}

/******************************************************************************/
/*         T P C H a n d l e r : : L o g S t r e a m F a i l u r e            */
/******************************************************************************/

// Logs the failure that stops a pull, when it is first seen and with
// everything known about it.  The verdict that follows once the streams have
// stopped repeats the cause.  The event is STREAM_FAIL when a stream failed,
// MULTISTREAM_STALL when it was the stall check of the coordinator.
void TPCHandler::LogStreamFailure(BlockPull &pull, size_t workers, TPCLogRecord &rec)
{
    const BlockFailure failure = pull.Failure();
    std::stringstream ss;
    if (!failure.stream) {
        ss << "running=" << pull.Running() << ", received=" << pull.received.load()
           << ", written=" << pull.written.load() << ", cause="
           << (failure.message.empty() ? std::string("internal") : OneLine(failure.message))
           << "; stopping the streams";
        logTransferEvent(LogMask::Error, rec,
                         failure.error_code == State::errTimeout ? "MULTISTREAM_STALL" : "MULTISTREAM_ERROR",
                         ss.str());
        return;
    }
    ss << "stream=" << failure.stream << ", workers=" << workers;
    if (failure.block_end > failure.block_start) {
        ss << ", block=" << failure.block_start << "-" << (failure.block_end - 1)
           << ", received=" << failure.block_received;
    }
    // Free text comes last, so that the fields before it can be picked out.
    ss << ", cause=";
    if (failure.status_code) {
        ss << "status " << failure.status_code;
    } else if (failure.error_code == State::errRange) {
        ss << "wrong range";
    } else if (failure.error_code == BlockPull::errInternal) {
        ss << "internal";
    } else if (failure.error_code) {
        ss << "local filesystem";
    } else {
        ss << "libcurl: " << curl_easy_strerror(failure.curl_result);
    }
    if (!failure.message.empty()) {ss << "; " << OneLine(failure.message);}
    ss << "; stopping the other streams";
    logTransferEvent(LogMask::Error, rec, "STREAM_FAIL", ss.str());
}

/******************************************************************************/
/*        T P C H a n d l e r : : C u t B a c k F a i l e d P u l l           */
/******************************************************************************/

// Cuts the file of a pull that did not succeed back to its complete leading
// blocks.  To be called once the workers have been joined, so that nothing
// writes to the file any more.  Returns what the file is left with, in the
// form of the log; a cut that fails is logged here and reported in
// `cut_failed`.
std::string TPCHandler::CutBackFailedPull(Stream &stream, BlockPull &pull, TPCLogRecord &rec,
    bool &cut_failed)
{
    cut_failed = false;
    const off_t keep = pull.CompleteUpTo();
    std::stringstream ss;
    ss << "blocks_done=" << pull.BlocksDone() << ", blocks=" << pull.Blocks()
       << ", written=" << pull.written.load();
    std::string error;
    if (keep < pull.FileSize() && !stream.Truncate(keep, error)) {
        // The file may be left with the size of the source and a hole in it.
        cut_failed = true;
        ss << ", left=unknown, file_size=" << pull.FileSize();
        logTransferEvent(LogMask::Warning, rec, "TRUNCATE_FAIL", ss.str()
            + "; unable to cut the file back to its complete leading blocks: " + error);
        return ss.str();
    }
    ss << ", left=" << keep << ", file_size=" << pull.FileSize();
    return ss.str();
}
