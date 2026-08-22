/**
 * state.hh:
 *
 * Helper class for managing the state of a single TPC request.
 */
#ifndef __XRD_TPCR_STATE_HH__
#define __XRD_TPCR_STATE_HH__

#include <map>
#include <memory>
#include <string>
#include <vector>

// Forward dec'ls
struct StateTestPeer;
class XrdSfsFile;
class XrdHttpExtReq;
typedef void CURL;
struct curl_slist;

namespace TPCR {
class Stream;

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
        // Range-response validation failures (FR-8/FR-9; WP-3).  All of them
        // are permanent-class: the source is misbehaving, retrying cannot
        // help (FR-12 classification consumes these at WP-4).
        errRangeNotHonored = 4,  // Non-206 success status to a ranged request.
        errRangeMismatch   = 5,  // Content-Range absent or not echoing the request.
        errLengthMismatch  = 6,  // Reported or delivered length != requested range length.
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

    void SetupHeaders(XrdHttpExtReq &req);

    void SetupHeadersForHEAD(XrdHttpExtReq & req);

    off_t BytesTransferred() const {return m_offset;}

    void SetContentLength(const off_t content_length) { m_content_length = content_length; }

    off_t GetContentLength() const {return m_content_length;}

    // Content-Length as claimed by the current HTTP response.  Kept strictly
    // separate from both the transfer's known content length and the
    // requested range length (FR-9 / BUG-3: the stock code let the response
    // header overwrite the expectation it was supposed to be checked
    // against).
    off_t GetReportedLength() const {return m_reported_length;}

    // Source validators from the current response (SUB-7): the ETag exactly
    // as sent (including quotes / a W/ prefix) and the Last-Modified string.
    // Empty when the response did not carry them.
    const std::string &GetETag() const {return m_etag;}
    const std::string &GetLastModified() const {return m_last_modified;}

    // Validates the current response against the requested range (FR-8).
    // Called once when the body starts (completion=false: status must be
    // 206, Content-Range must exactly echo the request, a reported
    // Content-Length must equal the range length) and once when libcurl
    // reports the request complete (completion=true: additionally the
    // delivered byte count must equal the range length).  On violation,
    // records a permanent-class error code and a distinct, greppable
    // message, and returns false.  No-op (true) for non-ranged requests.
    bool ValidateRangeResponse(bool completion);

    // True once this state has a pending ranged request (SetTransferParameters
    // was called for the current issue).
    bool RangeRequested() const {return m_range_request;}

    const std::map<std::string, std::string> & GetReprDigest() const { return m_repr_digests; }

    int GetErrorCode() const {return m_error_code;}

    void SetErrorCode(int error_code) {m_error_code = error_code;}

    int GetStatusCode() const {return m_status_code;}

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

    // Installs the libcurl callbacks (header/write/read), the data
    // pointers, and CURLOPT_PRIVATE for this state.  Public because
    // TPCRHandler::ConfigureHandle re-runs it after curl_easy_reset as part
    // of the single-place handle configuration (SUB-5).
    bool InstallHandlers(CURL *curl);

    // Re-binds the stored custom header list to the curl handle.  Required
    // after curl_easy_reset (which wipes every option) as part of the
    // ConfigureHandle sequence (SUB-5).
    void RebindHeaders();

    CURL *GetHandle() const {return m_curl;}

    // Returns true if at least one byte of the response has been received,
    // but not the entire contents of the response.  For ranged requests the
    // yardstick is the length we requested, never the length the response
    // claimed (BUG-3).
    bool BodyTransferInProgress() const {
        return m_offset &&
               (m_offset != (m_range_request ? m_expected_length
                                             : m_content_length));
    }

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
    // Test seam: the unit tests (T-U2 State hygiene, T-U7 response validation)
    // must drive the private header parser and inspect per-request state.
    // Production code must not gain setters for these, so the tests get a
    // single named friend instead.
    friend struct ::StateTestPeer;

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
    // Strict parser for "Content-Range: bytes X-Y/Z|*" (FR-8); false on
    // malformed input, which fails the request.
    bool ParseContentRange(const std::string &header_value);
    static size_t WriteCB(void *buffer, size_t size, size_t nitems, void *userdata);
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
    int m_status_code;  // status code from HTTP response.
    int m_error_code; // error code from underlying stream operations.
    off_t m_content_length;  // known total content length of the transfer (set by the handler).
    off_t m_push_length; // For push transfers, the size of the file on our server.

    // --- Per-request response validation state (WP-3, FR-8/FR-9). ---
    // All of these are transient and reconstructed by ResetAfterRequest.
    off_t m_expected_length = -1;   // range length we asked for (SetTransferParameters).
    off_t m_reported_length = -1;   // Content-Length claimed by the response.
    off_t m_resp_range_start = -1;  // parsed Content-Range first byte.
    off_t m_resp_range_end = -1;    // parsed Content-Range last byte (inclusive).
    bool m_range_request = false;   // a Range header was set for this request.
    bool m_seen_content_range = false;  // response carried a Content-Range.
    bool m_body_validated = false;  // ValidateRangeResponse(false) already ran.
    std::string m_etag;             // ETag from the current response (SUB-7).
    std::string m_last_modified;    // Last-Modified from the current response.
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
