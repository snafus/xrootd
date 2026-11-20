/**
 * Implementation of multi-stream HTTP transfers for the TPCHandler
 */

#include "XrdHttpTpcTPC.hh"
#include "XrdHttpTpcState.hh"
#include "XrdHttpTpcExperiment.hh"

#include "XrdSys/XrdSysError.hh"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>


using namespace TPC;

class CurlHandlerSetupError : public std::runtime_error {
public:
    CurlHandlerSetupError(const std::string &msg) :
        std::runtime_error(msg)
    {}

    virtual ~CurlHandlerSetupError() noexcept {}
};

namespace {
class MultiCurlHandler {
public:
    MultiCurlHandler(std::vector<State*> &states, XrdSysError &log) :
        m_handle(curl_multi_init()),
        m_states(states),
        m_log(log),
        m_bytes_transferred(0),
        m_error_code(0),
        m_status_code(0)
    {
        if (m_handle == NULL) {
            throw CurlHandlerSetupError("Failed to initialize a libcurl multi-handle");
        }
        m_avail_handles.reserve(states.size());
        m_active_handles.reserve(states.size());
        for (std::vector<State*>::const_iterator state_iter = states.begin();
             state_iter != states.end();
             state_iter++) {
            m_avail_handles.push_back((*state_iter)->GetHandle());
        }
    }

    ~MultiCurlHandler()
    {
        if (!m_handle) {return;}
        for (std::vector<CURL *>::const_iterator it = m_active_handles.begin();
             it != m_active_handles.end();
             it++) {
            curl_multi_remove_handle(m_handle, *it);
        }
        curl_multi_cleanup(m_handle);
    }

    MultiCurlHandler(const MultiCurlHandler &) = delete;

    CURLM *Get() const {return m_handle;}

    void FinishCurlXfer(CURL *curl, CURLcode result) {
        CURLMcode mres = curl_multi_remove_handle(m_handle, curl);
        if (mres) {
            std::stringstream ss;
            ss << "Failed to remove transfer from set: "
               << curl_multi_strerror(mres);
            throw std::runtime_error(ss.str());
        }
        for (std::vector<State*>::iterator state_iter = m_states.begin();
             state_iter != m_states.end();
             state_iter++) {
            if (curl == (*state_iter)->GetHandle()) {
                m_bytes_transferred += (*state_iter)->BytesTransferred();
                if (result == CURLE_OK) {
                    // EXPERIMENT: write the last, short part of the block.
                    (*state_iter)->FlushWriteBuffer();
                    // Complete as far as libcurl can tell; count the bytes.
                    (*state_iter)->CheckRangeComplete();
                }
                int error_code = (*state_iter)->GetErrorCode();
                if (error_code && !m_error_code) {
                    m_error_code = error_code;
                    m_error_message = (*state_iter)->GetErrorMessage();
                }
                int status_code = (*state_iter)->GetStatusCode();
                if (status_code >= 400 && !m_status_code) {
                    m_status_code = status_code;
                    m_error_message = (*state_iter)->GetErrorMessage();
                }
                (*state_iter)->ResetAfterRequest();
                break;
            }
        }
        for (std::vector<CURL *>::iterator iter = m_active_handles.begin();
             iter != m_active_handles.end();
             ++iter)
        {
            if (*iter == curl) {
                m_active_handles.erase(iter);
                break;
            }
        }
        m_avail_handles.push_back(curl);
    }

    off_t StartTransfers(off_t current_offset, off_t content_length, size_t block_size,
                         int &running_handles) {
         bool started_new_xfer = false;
         do {
             size_t xfer_size = std::min(content_length - current_offset, static_cast<off_t>(block_size));
             if (xfer_size == 0) {return current_offset;}
             if (!(started_new_xfer = StartTransfer(current_offset, xfer_size))) {
                 // In this case, we need to start new transfers but weren't able to.
                 if (running_handles == 0) {
                     if (!CanStartTransfer(true)) {
                         m_log.Emsg("StartTransfers", "Unable to start transfers.");
                     }
                 }
                 break;
             } else {
                 running_handles += 1;
             }
             current_offset += xfer_size;
        } while (true);
        return current_offset;
    }

    // Flush the file to the local filesystem.  All the states share the same
    // underlying stream, hence a single flush is enough -- and required: a
    // second one would be a no-op at best.  Returns State::errNone and leaves
    // error_msg untouched on success; otherwise returns State::errFlush and sets
    // error_msg to the corresponding error message.
    int Flush(std::string &error_msg) {
        if (m_states.empty() || (m_states[0]->Flush() != -1)) {
            return State::errNone;
        }
        error_msg = m_states[0]->GetFinalizeErrorMessage();
        if (error_msg.empty()) {error_msg = "(no error message provided)";}
        return State::errFlush;
    }

    off_t BytesTransferred() const {
        return m_bytes_transferred;
    }

    // Number of bytes that have actually been transferred so far: the bytes of
    // the requests that already completed plus the bytes of the requests that
    // are still in flight.  The two are disjoint: FinishCurlXfer() accumulates
    // the counter of a state into m_bytes_transferred and then zeroes it via
    // State::ResetAfterRequest(), so no byte is counted twice.
    // Note this is not the same quantity as the scheduling offset maintained by
    // StartTransfers(), which is advanced as soon as a range request is handed
    // over to libcurl, hence before any byte of that range has been received.
    off_t BytesInFlightAndTransferred() const {
        off_t bytes = m_bytes_transferred;
        for (std::vector<State*>::const_iterator state_iter = m_states.begin();
             state_iter != m_states.end();
             state_iter++) {
            bytes += (*state_iter)->BytesTransferred();
        }
        return bytes;
    }

    int GetStatusCode() const {
        return m_status_code;
    }

    int GetErrorCode() const {
        return m_error_code;
    }

    void SetErrorCode(int error_code) {
        m_error_code = error_code;
    }

    std::string GetErrorMessage() const {
        return m_error_message;
    }

    void SetErrorMessage(const std::string &error_msg) {
        m_error_message = error_msg;
    }

private:

    bool StartTransfer(off_t offset, size_t size) {
        if (!CanStartTransfer(false)) {return false;}
        for (std::vector<CURL*>::const_iterator handle_it = m_avail_handles.begin();
             handle_it != m_avail_handles.end();
             handle_it++) {
            for (std::vector<State*>::iterator state_it = m_states.begin();
                 state_it != m_states.end();
                 state_it++) {
                if ((*state_it)->GetHandle() == *handle_it) {  // This state object represents an idle handle.
                    (*state_it)->SetTransferParameters(offset, size);
                    ActivateHandle(**state_it);
                    return true;
                }
            }
        }
        return false;
    }

    void ActivateHandle(State &state) {
        CURL *curl = state.GetHandle();
        m_active_handles.push_back(curl);
        CURLMcode mres;
        mres = curl_multi_add_handle(m_handle, curl);
        if (mres) {
            std::stringstream ss;
            ss << "Failed to add transfer to libcurl multi-handle"
               << curl_multi_strerror(mres);
            throw std::runtime_error(ss.str());
        }
        for (auto iter = m_avail_handles.begin();
             iter != m_avail_handles.end();
             ++iter)
        {
            if (*iter == curl) {
                m_avail_handles.erase(iter);
                break;
            }
        }
    }

    bool CanStartTransfer(bool log_reason) const {
        size_t idle_handles = m_avail_handles.size();
        size_t transfer_in_progress = 0;
        for (std::vector<State*>::const_iterator state_iter = m_states.begin();
             state_iter != m_states.end();
             state_iter++) {
            for (std::vector<CURL*>::const_iterator handle_iter = m_active_handles.begin();
                 handle_iter != m_active_handles.end();
                 handle_iter++) {
                if (*handle_iter == (*state_iter)->GetHandle()) {
                    transfer_in_progress += (*state_iter)->BodyTransferInProgress();
                    break;
                }
            }
        }
        if (!idle_handles) {
            if (log_reason) {
                m_log.Emsg("CanStartTransfer", "Unable to start transfers as no idle CURL handles are available.");
            }
            return false;
        }
        ssize_t available_buffers = m_states[0]->AvailableBuffers();
        // To be conservative, set aside buffers for any transfers that have been activated
        // but don't have their first responses back yet.
        available_buffers -= (m_active_handles.size() - transfer_in_progress);
        if (log_reason && (available_buffers == 0)) {
            std::stringstream ss;
            ss << "Unable to start transfers as no buffers are available.  Available buffers: " <<
                m_states[0]->AvailableBuffers() << ", Active curl handles: " << m_active_handles.size()
                << ", Transfers in progress: " << transfer_in_progress;
            m_log.Emsg("CanStartTransfer", ss.str().c_str());
            if (m_states[0]->AvailableBuffers() == 0) {
                m_states[0]->DumpBuffers();
            }
        }
        return available_buffers > 0;
    }

    CURLM *m_handle;
    std::vector<CURL *> m_avail_handles;
    std::vector<CURL *> m_active_handles;
    std::vector<State*> &m_states;
    XrdSysError         &m_log;
    off_t                m_bytes_transferred;
    int                  m_error_code;
    int                  m_status_code;
    std::string          m_error_message;
};
}


int TPCHandler::RunCurlWithStreamsImpl(XrdHttpExtReq &req, State &state,
    size_t streams, std::vector<State*> &handles,
    std::vector<ManagedCurlHandle> &curl_handles, TPCLogRecord &rec)
{
    // EXPERIMENT: threads, but not with packet marking, whose bookkeeping of
    // sockets is written for one thread and could not be tested with several.
    if (Experiment::BlockWrites() && Experiment::Threads() && !rec.pmarkManager.isEnabled()) {
        return RunBlocksWithThreads(req, state, streams, handles, curl_handles, rec);
    }

    bool success;
    // The content-length was set thanks to the call to GetRemoteFileInfoTPCPull() before calling this function
    off_t content_size = state.GetContentLength();
    off_t current_offset = 0;

    size_t concurrency = streams * m_pipelining_multiplier;
    size_t block_size = m_block_size;
    if (Experiment::BlockWrites()) {
        // EXPERIMENT: one request per stream at a time, each for a large block.
        concurrency = streams;
        block_size = Experiment::BlockSize(content_size, streams);
        std::stringstream ss;
        ss << "EXPERIMENT block writes: block=" << block_size << " write="
           << Experiment::WriteSize() << " streams=" << streams;
        logTransferEvent(LogMask::Info, rec, "MULTISTREAM_EXPERIMENT", ss.str());
    }

    handles.reserve(concurrency);
    handles.push_back(new State());
    handles[0]->Move(state);
    for (size_t idx = 1; idx < concurrency; idx++) {
        // The response to the client is not started yet: report a failure as a
        // setup error, which is answered with a complete response.
        try {
            handles.push_back(handles[0]->Duplicate());
        } catch (std::runtime_error &e) {
            throw CurlHandlerSetupError(e.what());
        }
        curl_handles.emplace_back(handles.back()->GetHandle());
    }

    // Notify the packet marking manager that the transfer will start after this point
    rec.pmarkManager.startTransfer();

    // Create the multi-handle and add in the current transfer to it.
    MultiCurlHandler mch(handles, m_log);
    CURLM *multi_handle = mch.Get();

    curl_multi_setopt(multi_handle, CURLMOPT_PIPELINING, 1);
    curl_multi_setopt(multi_handle, CURLMOPT_MAX_HOST_CONNECTIONS, streams);

    // Start response to client prior to the first call to curl_multi_perform
    int retval = req.StartChunkedResp(202, NULL, "Content-Type: text/plain");
    if (retval) {
        logTransferEvent(LogMask::Error, rec, "RESPONSE_FAIL",
            "Failed to send the initial response to the TPC client");
        return retval;
    } else {
        logTransferEvent(LogMask::Debug, rec, "RESPONSE_START",
            "Initial transfer response sent to the TPC client");
    }

    // Start assigning transfers
    int running_handles = 0;
    current_offset = mch.StartTransfers(current_offset, content_size, block_size, running_handles);

    // Transfer loop: use curl to actually run the transfer, but periodically
    // interrupt things to send back performance updates to the client.
    time_t last_marker = 0;
    // Track the time since the transfer last made progress
    off_t last_advance_bytes = 0;
    time_t last_advance_time = time(NULL);
    time_t transfer_start = last_advance_time;
    CURLcode res = static_cast<CURLcode>(-1);
    CURLMcode mres = CURLM_OK;
    do {
        time_t now = time(NULL);
        time_t next_marker = last_marker + m_marker_period;
        if (now >= next_marker) {
            // Report - and watch for progress on - the bytes that have really
            // been transferred, not the offset up to which the range requests
            // have been scheduled: the latter runs ahead of the transfer by up
            // to concurrency * m_block_size bytes.
            const off_t bytes_transferred = mch.BytesInFlightAndTransferred();
            if (bytes_transferred > last_advance_bytes) {
                last_advance_bytes = bytes_transferred;
                last_advance_time = now;
            }
            if (SendPerfMarker(req, rec, handles, bytes_transferred)) {
                logTransferEvent(LogMask::Error, rec, "PERFMARKER_FAIL",
                    "Failed to send a perf marker to the TPC client");
                return -1;
            }
            int timeout = (transfer_start == last_advance_time) ? m_first_timeout : m_timeout;
            if (now > last_advance_time + timeout) {
                const char *log_prefix = rec.log_prefix.c_str();
                bool tpc_pull = strncmp("Pull", log_prefix, 4) == 0;

                mch.SetErrorCode(State::errTimeout);
                std::stringstream ss;
                ss << "Transfer failed because no bytes have been "
                   << (tpc_pull ? "received from the source (pull mode) in "
                                : "transmitted to the destination (push mode) in ") << timeout << " seconds.";
                mch.SetErrorMessage(ss.str());
                break;
            }
            last_marker = now;
        }

        mres = curl_multi_perform(multi_handle, &running_handles);
        if (mres == CURLM_CALL_MULTI_PERFORM) {
            // curl_multi_perform should be called again immediately.  On newer
            // versions of curl, this is no longer used.
            continue;
        } else if (mres != CURLM_OK) {
            break;
        }

        rec.pmarkManager.beginPMarks();


        // Harvest any messages, looking for CURLMSG_DONE.
        CURLMsg *msg;
        do {
            int msgq = 0;
            msg = curl_multi_info_read(multi_handle, &msgq);
            if (msg && (msg->msg == CURLMSG_DONE)) {
                CURL *easy_handle = msg->easy_handle;
                res = msg->data.result;
                mch.FinishCurlXfer(easy_handle, res);
                // If any requests fail, cut off the entire transfer.
                if (res != CURLE_OK || mch.GetErrorCode() == State::errRange) {
                    break;
                }
            }
        } while (msg);
        if (mch.GetErrorCode() == State::errRange) {
            logTransferEvent(LogMask::Debug, rec, "MULTISTREAM_RANGE_FAILURE",
                "Breaking loop: " + mch.GetErrorMessage());
            break;
        }
        if (res != static_cast<CURLcode>(-1) && res != CURLE_OK) {
            std::stringstream ss;
            ss << "Breaking loop due to failed curl transfer: " << curl_easy_strerror(res);
            logTransferEvent(LogMask::Debug, rec, "MULTISTREAM_CURL_FAILURE",
                ss.str());
            break;
        }

        if (running_handles < static_cast<int>(concurrency)) {
            // Issue new transfers if there is still pending work to do.
            // Otherwise, continue running until there are no handles left.
            if (current_offset != content_size) {
                current_offset = mch.StartTransfers(current_offset, content_size,
                                                    block_size, running_handles);
                if (!running_handles) {
                    std::stringstream ss;
                    ss << "No handles are able to run.  Streams=" << streams << ", concurrency="
                       << concurrency;
                    
                    logTransferEvent(LogMask::Debug, rec, "MULTISTREAM_IDLE", ss.str());
                }
            } else if (running_handles == 0) {
                logTransferEvent(LogMask::Debug, rec, "MULTISTREAM_IDLE",
                    "All the ranges have been scheduled and all the handles are done; ending the transfer loop.");
                break;
            }
        }

        int64_t max_sleep_time = next_marker - time(NULL);
        if (max_sleep_time <= 0) {
            continue;
        }
        int fd_count;
        mres = curl_multi_wait(multi_handle, NULL, 0, max_sleep_time*1000,
                               &fd_count);
        if (mres != CURLM_OK) {
            break;
        }
    } while (running_handles);

    if (mres != CURLM_OK) {
        std::stringstream ss;
        ss << "Internal libcurl multi-handle error: "
           << curl_multi_strerror(mres);
        logTransferEvent(LogMask::Error, rec, "MULTISTREAM_ERROR", ss.str());
        throw std::runtime_error(ss.str());
    }

    // Harvest any messages, looking for CURLMSG_DONE.
    CURLMsg *msg;
    do {
        int msgq = 0;
        msg = curl_multi_info_read(multi_handle, &msgq);
        if (msg && (msg->msg == CURLMSG_DONE)) {
            CURL *easy_handle = msg->easy_handle;
            mch.FinishCurlXfer(easy_handle, msg->data.result);
            if (res == CURLE_OK || res == static_cast<CURLcode>(-1))
                res = msg->data.result;  // Transfer result will be examined below.
        }
    } while (msg);

    if (!state.GetErrorCode() && res == static_cast<CURLcode>(-1)) { // No transfers returned?!?
        logTransferEvent(LogMask::Error, rec, "MULTISTREAM_ERROR",
            "Internal state error in libcurl");
        throw std::runtime_error("Internal state error in libcurl");
    }

    // A failure to flush the file to the local filesystem is always logged and is
    // appended to the error reported to the client, but it never replaces the
    // transfer failure itself: the flush failure is usually a consequence of it.
    std::string flushErrorMsg;
    const int flushErrorCode = mch.Flush(flushErrorMsg);
    std::string flushErrorSuffix;
    if (flushErrorCode) {
        std::replace(flushErrorMsg.begin(), flushErrorMsg.end(), '\n', ' ');
        flushErrorMsg = "Failed to flush the file to the local filesystem. " + flushErrorMsg;
        logTransferEvent(LogMask::Error, rec, "FLUSH_FAIL", flushErrorMsg);
        flushErrorSuffix = "; " + flushErrorMsg;
    }

    rec.bytes_transferred = mch.BytesTransferred();
    rec.tpc_status = mch.GetStatusCode();

    // Generate the final response back to the client.
    std::stringstream ss;
    success = false;
    if (mch.GetStatusCode() >= 400) {
        std::string err = mch.GetErrorMessage();
        std::stringstream ss2;
        ss2 << "Remote side failed with status code " << mch.GetStatusCode();
        if (!err.empty()) {
            std::replace(err.begin(), err.end(), '\n', ' ');
            ss2 << "; error message: \"" << err << "\"";
        }
        logTransferEvent(LogMask::Error, rec, "MULTISTREAM_FAIL", ss2.str());
        ss2 << flushErrorSuffix;
        ss << generateClientErr(ss2, rec);
    } else if (mch.GetErrorCode() == State::errTimeout || mch.GetErrorCode() == State::errRange) {
        // The stall detector fired, or the source answered a range request
        // with something else than that range; the message already describes
        // precisely what happened, report it as-is.
        std::stringstream ss2;
        ss2 << mch.GetErrorMessage();
        logTransferEvent(LogMask::Error, rec, "MULTISTREAM_FAIL", ss2.str());
        ss2 << flushErrorSuffix;
        ss << generateClientErr(ss2, rec);
    } else if (mch.GetErrorCode()) {
        std::string err = mch.GetErrorMessage();
        if (err.empty()) {err = "(no error message provided)";}
        else {std::replace(err.begin(), err.end(), '\n', ' ');}
        std::stringstream ss2;
        ss2 << "Error when interacting with local filesystem: " << err;
        logTransferEvent(LogMask::Error, rec, "MULTISTREAM_FAIL", ss2.str());
        ss2 << flushErrorSuffix;
        ss << generateClientErr(ss2, rec);
    } else if (res != CURLE_OK) {
        std::stringstream ss2;
        ss2 << "Request failed when processing";
        std::stringstream ss3;
        ss3 << ss2.str() << ":" << curl_easy_strerror(res);
        logTransferEvent(LogMask::Error, rec, "MULTISTREAM_FAIL", ss3.str());
        ss2 << flushErrorSuffix;
        ss << generateClientErr(ss2, rec, res);
    } else if (current_offset != content_size) {
        std::stringstream ss2;
        ss2 << "Internal logic error led to early abort; current offset is " <<
              current_offset << " while full size is " << content_size;
        logTransferEvent(LogMask::Error, rec, "MULTISTREAM_FAIL", ss2.str());
        ss2 << flushErrorSuffix;
        ss << generateClientErr(ss2, rec);
    } else if (flushErrorCode) {
        // Nothing else went wrong: the flush failure is the reason of the failure.
        std::stringstream ss2;
        ss2 << flushErrorMsg;
        ss << generateClientErr(ss2, rec);
    } else {
        if (!handles[0]->Finalize()) {
            std::stringstream ss2;
            ss2 << "Failed to finalize and close file handle.";
            std::string handleErrMsg = handles[0]->GetFinalizeErrorMessage();
            if(handleErrMsg.size()) {
              std::replace(handleErrMsg.begin(), handleErrMsg.end(), '\n', ' ');
              ss2 << " " << handleErrMsg;
            }
            ss << generateClientErr(ss2, rec);
            logTransferEvent(LogMask::Error, rec, "MULTISTREAM_ERROR",
                ss2.str());
        } else {
            ss << "success: Created";
            success = true;
        }
    }

    if ((retval = req.ChunkResp(ss.str().c_str(), 0))) {
        logTransferEvent(LogMask::Error, rec, "TRANSFER_ERROR",
            "Failed to send last update to remote client");
        return retval;
    } else if (success) {
        logTransferEvent(LogMask::Info, rec, "TRANSFER_SUCCESS");
        rec.status = 0;
    }
    return req.ChunkResp(NULL, 0);
}


/******************************************************************************/
/*      E X P E R I M E N T :   o n e   t h r e a d   p e r   s t r e a m       */
/******************************************************************************/

namespace {

// A failure that is not one of the State error codes; its message is reported as it is.
const int errStreamInternal = 100;

// What the stream threads of one pull share.  It lives on the stack of the
// thread of the request, which joins every stream thread before it leaves.
struct ThreadedPull {
    ThreadedPull(off_t size, off_t block) : file_size(size), block_size(block) {}

    // Hands out the next block.  False when none is left or the pull is being stopped.
    bool NextBlock(off_t &offset, off_t &size) {
        std::lock_guard<std::mutex> lock(mtx);
        if (stop.load() || next_offset >= file_size) {return false;}
        offset = next_offset;
        size = std::min(block_size, file_size - next_offset);
        next_offset += size;
        return true;
    }

    // Records the first failure and tells every stream to stop.  It is called
    // from catch blocks too and must not throw: the text is the one thing in
    // it that can, and a failure without its text is still a failure.
    void Fail(int error, int status, CURLcode result, const std::string &message) noexcept {
        std::lock_guard<std::mutex> lock(mtx);
        if (!failed) {
            failed = true;
            error_code = error;
            status_code = status;
            curl_result = result;
            try {error_message = message;} catch (...) {}
        }
        stop.store(true);
    }

    void BlockDone() {
        std::lock_guard<std::mutex> lock(mtx);
        blocks_done++;
    }

    void StreamEnded() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            running--;
        }
        ended.notify_all();
    }

    const off_t file_size;
    const off_t block_size;

    std::mutex mtx;  // protects everything down to error_message
    off_t next_offset{0};
    size_t blocks_done{0};  // blocks received and written in full
    int running{0};
    bool failed{false};
    int error_code{0};
    int status_code{0};
    CURLcode curl_result{CURLE_OK};
    std::string error_message;

    std::condition_variable ended;   // a stream thread has ended
    std::atomic<bool> stop{false};   // every stream is to stop
    std::atomic<off_t> received{0};  // bytes received from the source by all the streams
    std::atomic<off_t> written{0};   // bytes written to the file by all the streams
};

// The progress callback of libcurl: the only way to end a transfer from
// another thread.  libcurl calls it about once a second, and more often
// while data arrives.
int StopCallback(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return static_cast<ThreadedPull*>(clientp)->stop.load(std::memory_order_relaxed) ? 1 : 0;
}

// One stream: requests one block after another on its own libcurl handle.
// Nothing of libcurl is shared with another thread.
void StreamThread(State *state, ThreadedPull *pull)
{
    try {
        off_t offset = 0, size = 0;
        while (pull->NextBlock(offset, size)) {
            state->SetTransferParameters(offset, size);
            CURLcode result = curl_easy_perform(state->GetHandle());
            if (result == CURLE_OK) {
                // Write the last, short part of the block, then count the bytes.
                state->FlushWriteBuffer();
                state->CheckRangeComplete();
            }
            int error = state->GetErrorCode();
            int status = state->GetStatusCode();
            if (result == CURLE_ABORTED_BY_CALLBACK && !error && pull->stop.load()) {
                // Stopped because of a failure elsewhere, which is already
                // recorded: this is not a cause.
                break;
            }
            // A block is only done after an answer of the 2xx class.  libcurl
            // also returns CURLE_OK for a final answer such as 304, or a
            // redirection without a Location, which carries no part of the file.
            const bool wrong_status = (result == CURLE_OK) && (status < 200 || status >= 300);
            if (error || status >= 400 || wrong_status || result != CURLE_OK) {
                pull->Fail(error, (status >= 400 || wrong_status) ? status : 0, result, state->GetErrorMessage());
                break;
            }
            pull->BlockDone();
            state->ResetAfterRequest();
        }
    } catch (...) {
        // No text is built here: building one may throw again, and an
        // exception that leaves a thread ends the server.
        pull->Fail(errStreamInternal, 0, CURLE_OK, std::string());
    }
    pull->StreamEnded();
}

// Stops and joins the stream threads on every way out of the function that
// started them, and takes back what the handles were given.
struct StreamJoiner {
    ThreadedPull &pull;
    std::vector<std::thread> &threads;
    std::vector<State*> &handles;

    ~StreamJoiner() {
        pull.stop.store(true);
        for (std::thread &thread : threads) {
            if (thread.joinable()) {thread.join();}
        }
        for (State *handle : handles) {
            handle->SetCounters(nullptr, nullptr);
            curl_easy_setopt(handle->GetHandle(), CURLOPT_NOPROGRESS, 1L);
            curl_easy_setopt(handle->GetHandle(), CURLOPT_XFERINFODATA, NULL);
        }
    }
};

}


int TPCHandler::RunBlocksWithThreads(XrdHttpExtReq &req, State &state,
    size_t streams, std::vector<State*> &handles,
    std::vector<ManagedCurlHandle> &curl_handles, TPCLogRecord &rec)
{
    // The content-length was set by GetRemoteFileInfoTPCPull(); it is not zero here.
    const off_t content_size = state.GetContentLength();
    const off_t block_size = Experiment::BlockSize(content_size, streams);
    const size_t blocks = static_cast<size_t>((content_size + block_size - 1) / block_size);
    const size_t workers = std::max(static_cast<size_t>(1), std::min(streams, blocks));
    {
        std::stringstream ss;
        ss << "EXPERIMENT block writes with threads: block=" << block_size << " write="
           << Experiment::WriteSize() << " streams=" << streams << " threads=" << workers
           << " write_lock=" << Experiment::WriteLock();
        logTransferEvent(LogMask::Info, rec, "MULTISTREAM_EXPERIMENT", ss.str());
    }

    // One handle for each thread, all made here before any thread exists.
    handles.reserve(workers);
    handles.push_back(new State());
    handles[0]->Move(state);
    for (size_t idx = 1; idx < workers; idx++) {
        try {
            handles.push_back(handles[0]->Duplicate());
        } catch (std::runtime_error &e) {
            throw CurlHandlerSetupError(e.what());
        }
        curl_handles.emplace_back(handles.back()->GetHandle());
    }

    // Notify the packet marking manager that the transfer will start after this point
    rec.pmarkManager.startTransfer();

    int retval = req.StartChunkedResp(202, NULL, "Content-Type: text/plain");
    if (retval) {
        logTransferEvent(LogMask::Error, rec, "RESPONSE_FAIL",
            "Failed to send the initial response to the TPC client");
        return retval;
    }
    logTransferEvent(LogMask::Debug, rec, "RESPONSE_START",
        "Initial transfer response sent to the TPC client");

    ThreadedPull pull(content_size, block_size);
    std::vector<std::thread> threads;
    StreamJoiner joiner{pull, threads, handles};

    for (State *handle : handles) {
        handle->SetCounters(&pull.received, &pull.written);
        curl_easy_setopt(handle->GetHandle(), CURLOPT_XFERINFOFUNCTION, StopCallback);
        curl_easy_setopt(handle->GetHandle(), CURLOPT_XFERINFODATA, &pull);
        curl_easy_setopt(handle->GetHandle(), CURLOPT_NOPROGRESS, 0L);
    }

    threads.reserve(workers);
    for (size_t idx = 0; idx < workers; idx++) {
        {
            std::lock_guard<std::mutex> lock(pull.mtx);
            pull.running++;
        }
        try {
            threads.emplace_back(StreamThread, handles[idx], &pull);
        } catch (std::system_error &e) {
            {
                std::lock_guard<std::mutex> lock(pull.mtx);
                pull.running--;
            }
            // Go on with the threads there are; with none, the pull fails.
            logTransferEvent(LogMask::Warning, rec, "MULTISTREAM_EXPERIMENT",
                std::string("Unable to start a stream thread: ") + e.what());
            break;
        }
    }
    if (threads.empty()) {
        pull.Fail(errStreamInternal, 0, CURLE_OK, "Unable to start a thread for the transfer");
    }

    // The thread of the request: markers to the client and the stall detector.
    // Progress is bytes received plus bytes written: when every byte has
    // arrived and a slow storage is still taking the last writes, the pull is
    // not stalled.
    time_t last_marker = 0;
    off_t last_advance_bytes = 0;
    time_t last_advance_time = time(NULL);
    const time_t transfer_start = last_advance_time;
    time_t stop_time = 0;
    bool stop_reported = false;
    bool client_gone = false;
    while (true) {
        {
            std::unique_lock<std::mutex> lock(pull.mtx);
            // wait_until() on the system clock, not wait_for(): the latter ends in
            // pthread_cond_clockwait(), which the thread sanitizer of GCC 11
            // does not know, so that it reports this lock as held twice.  A
            // step of the clock only makes one wait shorter or longer.
            if (pull.ended.wait_until(lock, std::chrono::system_clock::now() + std::chrono::seconds(1),
                                      [&pull]{return pull.running == 0;})) {
                break;
            }
        }
        const time_t now = time(NULL);
        const off_t received = pull.received.load();
        const off_t progress = received + pull.written.load();
        if (progress > last_advance_bytes) {
            last_advance_bytes = progress;
            last_advance_time = now;
        }
        if (pull.stop.load()) {
            // A failure is recorded and the streams are stopping.  A stream
            // inside a write to the storage cannot be interrupted, and this
            // thread must wait for it.  No marker is sent meanwhile: a client
            // that watches for markers must not take the pull for alive.
            if (!stop_time) {stop_time = now;}
            if (!stop_reported && now > stop_time + m_timeout) {
                logTransferEvent(LogMask::Error, rec, "MULTISTREAM_EXPERIMENT",
                    "The streams have not stopped; a write to the storage may be hanging");
                stop_reported = true;
            }
            continue;
        }
        if (now >= last_marker + m_marker_period) {
            if (SendPerfMarker(req, rec, handles, received)) {
                logTransferEvent(LogMask::Error, rec, "PERFMARKER_FAIL",
                    "Failed to send a perf marker to the TPC client");
                client_gone = true;
                pull.Fail(errStreamInternal, 0, CURLE_OK, "The TPC client is gone");
                break;
            }
            last_marker = now;
        }
        const int timeout = (transfer_start == last_advance_time) ? m_first_timeout : m_timeout;
        if (now > last_advance_time + timeout) {
            std::stringstream ss;
            ss << "Transfer failed because no bytes have been received from the source (pull mode) in "
               << timeout << " seconds.";
            // Every stream stops at its next progress callback; the loop ends when they have.
            pull.Fail(State::errTimeout, 0, CURLE_OK, ss.str());
        }
        rec.pmarkManager.beginPMarks();
    }

    // No stream thread is left after this, so what follows has the pull to itself.
    for (std::thread &thread : threads) {
        if (thread.joinable()) {thread.join();}
    }
    if (client_gone) {
        return -1;
    }

    rec.bytes_transferred = pull.received.load();
    rec.tpc_status = pull.status_code;

    std::stringstream ss;
    bool success = false;
    if (pull.failed && pull.status_code != 0) {
        std::string err = pull.error_message;
        std::stringstream ss2;
        ss2 << "Remote side failed with status code " << pull.status_code;
        if (!err.empty()) {
            std::replace(err.begin(), err.end(), '\n', ' ');
            ss2 << "; error message: \"" << err << "\"";
        }
        logTransferEvent(LogMask::Error, rec, "MULTISTREAM_FAIL", ss2.str());
        ss << generateClientErr(ss2, rec);
    } else if (pull.failed && (pull.error_code == State::errTimeout || pull.error_code == State::errRange
                               || pull.error_code == errStreamInternal)) {
        std::stringstream ss2;
        ss2 << (pull.error_message.empty() ? "Internal error in a stream" : pull.error_message);
        logTransferEvent(LogMask::Error, rec, "MULTISTREAM_FAIL", ss2.str());
        ss << generateClientErr(ss2, rec);
    } else if (pull.failed && pull.error_code) {
        std::string err = pull.error_message;
        if (err.empty()) {err = "(no error message provided)";}
        else {std::replace(err.begin(), err.end(), '\n', ' ');}
        std::stringstream ss2;
        ss2 << "Error when interacting with local filesystem: " << err;
        logTransferEvent(LogMask::Error, rec, "MULTISTREAM_FAIL", ss2.str());
        ss << generateClientErr(ss2, rec);
    } else if (pull.failed) {
        std::stringstream ss2;
        ss2 << "Request failed when processing";
        std::stringstream ss3;
        ss3 << ss2.str() << ":" << curl_easy_strerror(pull.curl_result);
        logTransferEvent(LogMask::Error, rec, "MULTISTREAM_FAIL", ss3.str());
        ss << generateClientErr(ss2, rec, pull.curl_result);
    } else if (pull.next_offset != content_size || pull.blocks_done != blocks
               || pull.received.load() != content_size) {
        // Never close as complete a file of which a block is missing.
        std::stringstream ss2;
        ss2 << "Internal logic error led to early abort; " << pull.blocks_done << " of " << blocks
            << " blocks and " << pull.received.load() << " of " << content_size << " bytes were received";
        logTransferEvent(LogMask::Error, rec, "MULTISTREAM_FAIL", ss2.str());
        ss << generateClientErr(ss2, rec);
    } else if (handles[0]->Flush() == -1 || !handles[0]->Finalize()) {
        std::stringstream ss2;
        ss2 << "Failed to finalize and close file handle.";
        std::string handleErrMsg = handles[0]->GetFinalizeErrorMessage();
        if (handleErrMsg.size()) {
            std::replace(handleErrMsg.begin(), handleErrMsg.end(), '\n', ' ');
            ss2 << " " << handleErrMsg;
        }
        ss << generateClientErr(ss2, rec);
        logTransferEvent(LogMask::Error, rec, "MULTISTREAM_ERROR", ss2.str());
    } else {
        ss << "success: Created";
        success = true;
    }

    if ((retval = req.ChunkResp(ss.str().c_str(), 0))) {
        logTransferEvent(LogMask::Error, rec, "TRANSFER_ERROR",
            "Failed to send last update to remote client");
        return retval;
    } else if (success) {
        logTransferEvent(LogMask::Info, rec, "TRANSFER_SUCCESS");
        rec.status = 0;
    }
    return req.ChunkResp(NULL, 0);
}


int TPCHandler::RunCurlWithStreams(XrdHttpExtReq &req, State &state,
    size_t streams, TPCLogRecord &rec)
{
    std::vector<ManagedCurlHandle> curl_handles;
    std::vector<State*> handles;
    std::stringstream err_ss;
    try {
        int retval = RunCurlWithStreamsImpl(req, state, streams, handles, curl_handles, rec);
        for (std::vector<State*>::iterator state_iter = handles.begin();
             state_iter != handles.end();
             state_iter++) {
            delete *state_iter;
        }
        return retval;
    } catch (CurlHandlerSetupError &e) {
        for (std::vector<State*>::iterator state_iter = handles.begin();
             state_iter != handles.end();
             state_iter++) {
            delete *state_iter;
        }

        rec.status = 500;
        logTransferEvent(LogMask::Error, rec, "MULTISTREAM_ERROR", e.what());
        std::stringstream ss;
        ss << e.what();
        err_ss << generateClientErr(ss, rec);
        return req.SendSimpleResp(rec.status, NULL, NULL, e.what(), 0);
    } catch (std::runtime_error &e) {
        for (std::vector<State*>::iterator state_iter = handles.begin();
             state_iter != handles.end();
             state_iter++) {
            delete *state_iter;
        }

        logTransferEvent(LogMask::Error, rec, "MULTISTREAM_ERROR", e.what());
        std::stringstream ss;
        ss << e.what();
        err_ss << generateClientErr(ss, rec);
        int retval;
        if ((retval = req.ChunkResp(err_ss.str().c_str(), 0))) {
            return retval;
        }
        return req.ChunkResp(NULL, 0);
    }
}
