/**
 * state.hh:
 *
 * Helper class for managing the state of a single TPC request.
 */
#ifndef __XRD_TPC_STATE_HH__
#define __XRD_TPC_STATE_HH__

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Forward dec'ls
class XrdSfsFile;
class XrdHttpExtReq;
typedef void CURL;
struct curl_slist;

namespace TPC {
class Stream;
class SourceCheck;
struct WorkerStatus;

class State {
public:

    // Error codes recorded on a transfer state.  They are exposed so that the
    // TPC handler is able to tell the various failure modes apart when it
    // composes the error sent back to the client.
    enum ErrorCode {
        errNone    = 0,
        errWrite   = 1,  // Failure while writing the received data to the local file.
        errFlush   = 2,  // Failure while flushing the local file.
        errClose   = 3,  // Failure while closing the local file.
        errRange   = 4,  // The source did not answer with the range that was requested, or not with all of it.
        errSource  = 5,  // The source file changed between two requests of the pull.
        errTimeout = 10  // The transfer did not make any progress within the timeout.
    };

    State() :
        m_push(true),
        m_recv_status_line(false),
        m_recv_all_headers(false),
        m_offset(0),
        m_start_offset(0),
        m_status_code(-1),
        m_error_code(0),
        m_content_length(-1),
        m_stream(NULL),
        m_curl(NULL),
        m_headers(NULL),
        m_is_transfer_state(true)
    {}

    /**
     * Don't use that constructor if you want to do some transfers.
     * @param curl the curl handle
     * @param tpcForwardCreds set to true if the credentials needs to be forwarded for this request, false otherwise
     * @param push set to true if this HEAD request is for a push transfer, false otherwise
     */
    State(CURL * curl, bool tpcForwardCreds):
      m_push(true),
      m_recv_status_line(false),
      m_recv_all_headers(false),
      m_offset(0),
      m_start_offset(0),
      m_status_code(-1),
      m_error_code(0),
      m_content_length(-1),
      m_push_length(-1),
      m_stream(NULL),
      m_curl(curl),
      m_headers(NULL),
      m_is_transfer_state(false),
      tpcForwardCreds(tpcForwardCreds)
    {
        InstallHandlers(curl);
    }

    // Note that we are "borrowing" a reference to the curl handle;
    // it is not owned / freed by the State object.  However, we use it
    // as if there's only one handle per State.
    State (off_t start_offset, Stream &stream, CURL *curl, bool push, bool tpcForwardCreds) :
      m_push(push),
      m_recv_status_line(false),
      m_recv_all_headers(false),
      m_offset(0),
      m_start_offset(start_offset),
      m_status_code(-1),
      m_error_code(0),
      m_content_length(-1),
      m_push_length(-1),
      m_stream(&stream),
      m_curl(curl),
      m_headers(NULL),
      m_is_transfer_state(true),
      tpcForwardCreds(tpcForwardCreds)
    {
        InstallHandlers(curl);
    }

    ~State();

    void SetTransferParameters(off_t offset, size_t size);

    // To be called when a request has completed without a transport error:
    // checks that the body filled the range that was requested, and records
    // the error otherwise.  libcurl cannot tell when the body carries no
    // length of its own (chunked encoding).
    bool CheckRangeComplete();

    void SetupHeaders(XrdHttpExtReq &req);

    void SetupHeadersForHEAD(XrdHttpExtReq & req);

    off_t BytesTransferred() const {return m_offset;}

    void SetContentLength(const off_t content_length) { m_content_length = content_length; }

    off_t GetContentLength() const {return m_content_length;}

    const std::map<std::string, std::string> & GetReprDigest() const { return m_repr_digests; }

    int GetErrorCode() const {return m_error_code;}

    void SetErrorCode(int error_code) {m_error_code = error_code;}

    int GetStatusCode() const {return m_status_code;}

    // To be called with the result of libcurl (a CURLcode) for the request of
    // this state, once the request is over and before the handle is used
    // again.
    //
    // The handle of a pull has CURLOPT_FAILONERROR set, so libcurl ends a
    // request on a status of 400 or more.  An older libcurl (7.61.1, as in
    // EL8) does so before it hands the status line to the header callback:
    // the status code of this state is then still unset, or is that of a
    // redirection that came before.  libcurl has recorded the status all the
    // same, and it is taken from there, so that the pull fails with the
    // status of the source and not with a failure of the HTTP library.  A
    // newer libcurl (7.76.1, as in EL9) hands the status line over first,
    // and this changes nothing.
    void RecordStatusOnFailure(int curl_result);

    // Whether the final status of a GET says that the response carries what
    // was asked for: 200 for the file, 203 for the file as a proxy has passed
    // it on, 206 for a range of it.  Any other status below 400 that ends a
    // request - 204, 304, a redirect that libcurl has not followed - comes
    // without the file, and the pull has failed even though neither the
    // source nor libcurl reports an error.
    static bool CarriesContent(int status_code) {
        return status_code == 200 || status_code == 203 || status_code == 206;
    }

    std::string GetErrorMessage() const {return m_error_buf;}

    void SetErrorMessage(const std::string &error_msg) {m_error_buf = error_msg;}

    // Error recorded while flushing and closing the local file at the end of the
    // transfer (see Flush() and Finalize()).  It is deliberately kept apart from
    // the transfer error above: a failure to flush or close the file must not
    // hide the reason why the transfer itself failed, e.g. a libcurl error or a
    // stalled transfer.
    int GetFinalizeErrorCode() const {return m_finalize_error_code;}

    std::string GetFinalizeErrorMessage() const {return m_finalize_error_buf;}

    void ResetAfterRequest();

    // -- Retry of a range ---------------------------------------------------

    /**
     * Sets up the next request for the part of the range that the last one
     * did not bring: bytes [Position(), end of the range).  Everything the
     * last request and its response left behind is reset, the error
     * included; the data collected for the next write is kept, and the new
     * request goes on filling it.
     *
     * Nothing is asked for twice.  It is refused, with the reason in
     * `error`, when the range is complete, or when the data collected does
     * not end where the new request starts: going on would then write a
     * byte twice or leave one out.
     *
     * Blocks mode only; called by the stream thread that owns the state.
     */
    bool ContinueRange(std::string &error);

    // The offset in the file of the next byte the request in progress, or
    // the last one, is to receive.
    off_t Position() const {return m_start_offset + m_offset;}

    // Whether every byte of the range has arrived.
    bool RangeFilled() const {return m_range_length >= 0 && m_offset == m_range_length;}

    // Whether the source sent more than the range in answer to the last
    // request.  Such a request is not continued: the source does not keep to
    // the range it announced.
    bool BodyOverran() const {return m_body_overran;}

    // Seconds the source asked to wait before the next request, from the
    // Retry-After header of the last response; -1 if it gave none, or gave a
    // date.
    long GetRetryAfter() const {return m_resp_retry_after;}

    // -- A pull in blocks ---------------------------------------------------
    //
    // When the stream writes at offsets (Stream::EnableOffsetWrites), this
    // state does not hand its data to the reorder buffers.  It collects one
    // write of data and writes it at the offset it belongs to; the stream
    // thread that owns the state calls FlushWriteBuffer() at the end of a
    // block for what is left.
    //
    // Threads: a state belongs to one stream thread from the moment that
    // thread starts until it has been joined.  The three pointers below lead
    // to objects shared by all the streams of the pull, which outlive them.

    // Writes what has been collected.  False if the storage did not take all
    // of it, with the reason recorded as an errWrite; false with no error
    // recorded if the pull is stopping, in which case the data is dropped:
    // another stream has already recorded why the pull fails.
    bool FlushWriteBuffer();

    // Counters, shared by the streams of a pull, of the bytes received from
    // the source and of the bytes the storage has taken.  The thread of the
    // request reads them for the markers and to see whether the pull moves.
    void SetPullCounters(std::atomic<off_t> *received, std::atomic<off_t> *written) {
        m_pull_received = received;
        m_pull_written = written;
    }

    // The flag that tells every stream of a pull to stop.  It is looked at
    // before each write, so that a stream that is stopping does not queue
    // for the storage first.
    void SetPullStopFlag(const std::atomic<bool> *stop) {m_pull_stop = stop;}

    // Where this state tells what its stream is doing, for the stall check
    // of the thread of the request: bytes received, and whether it is inside
    // a write.  See WorkerStatus.
    void SetWorkerStatus(WorkerStatus *status) {m_worker_status = status;}

    // The check that every response of the pull comes from the same version
    // of the source file; shared by the streams.  See SourceCheck.
    void SetSourceCheck(SourceCheck *check) {m_source_check = check;}

    CURL *GetHandle() const {return m_curl;}

    int AvailableBuffers() const;

    void DumpBuffers() const;

    // Returns true if at least one byte of the response has been received,
    // but not the entire contents of the response.
    bool BodyTransferInProgress() const {return m_offset && (m_offset != m_content_length);}

    // Duplicate the current state; all settings are copied over, but those
    // related to the transient state are reset as if from a constructor.
    State *Duplicate();

    // Move the contents of a State object.  To be replaced by a move
    // constructor once C++11 is allowed in XRootD.
    void Move (State &other);

    // Flush and finalize a transfer state.  Eventually calls close() on the underlying
    // file handle, which should hopefully synchronize the file metadata across
    // all readers (even other load-balanced servers on the same distributed file
    // system).
    //
    // Returns true on success; false otherwise.  Failures can happen, for example, if
    // not all buffers have been reordered by the underlying stream.  A failure is
    // recorded in the finalization error (GetFinalizeErrorCode()), not in the
    // transfer error.
    bool Finalize();

    // Flush the data in memory to disk, even if it may cause unaligned or short
    // writes.  Typically, only done while shutting down the transfer (note some
    // backends may be unable to handle unaligned writes unless it's the last write).
    // Returns -1 on failure, in which case the error is recorded in the
    // finalization error (GetFinalizeErrorCode()), not in the transfer error.
    int Flush();

    // Retrieve the description of the remote connection; is of the form:
    //   tcp:129.93.3.4:1234
    //   tcp:[2600:900:6:1301:268a:7ff:fef6:a590]:2345
    // This is meant to facilitate the monitoring via the performance markers.
    std::string GetConnectionDescription();

private:
    bool InstallHandlers(CURL *curl);

    // Record a failure that happened while flushing or closing the local file.
    // Only the first failure is kept: the ones that follow are almost always a
    // consequence of it.
    void RecordFinalizeError(int error_code, const std::string &error_msg);

    State(const State&);
    // Add back once C++11 is available
    //State(State &&) noexcept;

    // libcurl callback functions, along with the corresponding class methods.
    static size_t HeaderCB(char *buffer, size_t size, size_t nitems,
                           void *userdata);
    int Header(const std::string &header);
    // Whether the response whose headers were just received carries the range
    // that was requested; records the error otherwise.
    bool CheckRangeResponse();
    static size_t WriteCB(void *buffer, size_t size, size_t nitems, void *userdata);
    // What WriteCB does with a piece of the response body; may throw.
    size_t Body(void *buffer, size_t size, size_t nitems);
    ssize_t Write(char *buffer, size_t size);
    /**
    * This callback is used to give users the error message returns by the passive server of
    * the TPC PUSH
    */
    static size_t PushRespCB(void *buffer, size_t size, size_t nitems, void *userdata);
    static size_t ReadCB(void *buffer, size_t size, size_t nitems, void *userdata);
    int Read(char *buffer, size_t size);

    bool m_push;  // whether we are transferring in "push-mode"
    bool m_recv_status_line;  // whether we have received a status line in the response from the remote host.
    bool m_recv_all_headers;  // true if we have seen the end of headers.
    off_t m_offset;  // number of bytes we have received.
    off_t m_start_offset;  // offset where we started in the file.
    std::vector<char> m_wbuf;  // blocks mode: data collected for the next write.
    off_t m_wbuf_offset{0};  // blocks mode: offset in the file of the first byte of m_wbuf.
    std::atomic<off_t> *m_pull_received{nullptr};  // blocks mode: see SetPullCounters().
    std::atomic<off_t> *m_pull_written{nullptr};  // blocks mode: see SetPullCounters().
    const std::atomic<bool> *m_pull_stop{nullptr};  // blocks mode: see SetPullStopFlag().
    int m_status_code;  // status code from HTTP response.
    int m_error_code; // error code from underlying stream operations.
    off_t m_content_length;  // value of Content-Length header, if we received one.
    off_t m_range_length{-1};  // number of bytes of the range we requested; -1 if we requested no range.
    off_t m_resp_content_length{-1};  // Content-Length of the current response; -1 if it has none.
    long long m_resp_range_first{-1};  // first byte of the Content-Range of the current response; -1 if it has none.
    long long m_resp_range_last{-1};  // last byte of the Content-Range of the current response.
    long long m_resp_range_total{-1};  // total length in the Content-Range of the current response; -1 if none.
    std::string m_resp_etag;  // ETag of the current response, as sent.
    std::string m_resp_last_modified;  // Last-Modified of the current response, as sent.
    long m_resp_retry_after{-1};  // Retry-After of the current response, in seconds; -1 if none.
    bool m_body_overran{false};  // the body of the last response was longer than the range.
    WorkerStatus *m_worker_status{nullptr};  // blocks mode: see SetWorkerStatus().
    SourceCheck *m_source_check{nullptr};  // blocks mode: see SetSourceCheck().
    // Compares the response whose headers have all arrived with what earlier
    // responses of the pull said of the source file.  False, with an
    // errSource recorded, if the file has changed.
    bool CheckSourceUnchanged();
    off_t m_push_length; // For push transfers, the size of the file on our server.
    Stream *m_stream;  // stream corresponding to this transfer.
    CURL *m_curl;  // libcurl handle
    struct curl_slist *m_headers; // any headers we set as part of the libcurl request.
    std::vector<std::string> m_headers_copy; // Copies of custom headers.
    std::string m_resp_protocol;  // Response protocol in the HTTP status line.
    std::string m_error_buf;  // Any error associated with a response.
    int m_finalize_error_code = 0; // error code from flushing / closing the local file.
    std::string m_finalize_error_buf; // error message from flushing / closing the local file.
    bool m_is_transfer_state; // If set to true, this state will be used to perform some transfers
    bool tpcForwardCreds = false; // if set to true, the redirection will send user credentials to the redirection host
    std::map<std::string, std::string> m_repr_digests; // Repr-Digest values received from the passive server (PULL)
};

};

#endif
