
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

#include <strings.h>

#include "XrdVersion.hh"
#include "XrdHttp/XrdHttpExtHandler.hh"
#include "XrdSfs/XrdSfsInterface.hh"
#include "XrdOuc/XrdOucTUtils.hh"

#include <curl/curl.h>

#include "XrdHttpTpcRState.hh"
#include "XrdHttpTpcRStream.hh"

#include "XrdHttp/XrdHttpHeaderUtils.hh"

using namespace TPCR;


State::~State() {
    if (m_headers) {
            curl_slist_free_all(m_headers);
            m_headers = NULL;
            if (m_curl) {curl_easy_setopt(m_curl, CURLOPT_HTTPHEADER, m_headers);}
    }
}


void State::Move(State &other)
{
    m_push = other.m_push;
    m_recv_status_line = other.m_recv_status_line;
    m_recv_all_headers = other.m_recv_all_headers;
    m_offset = other.m_offset;
    m_start_offset = other.m_start_offset;
    m_status_code = other.m_status_code;
    m_content_length = other.m_content_length;
    m_push_length = other.m_push_length;
    m_expected_length = other.m_expected_length;
    m_reported_length = other.m_reported_length;
    m_resp_range_start = other.m_resp_range_start;
    m_resp_range_end = other.m_resp_range_end;
    m_range_request = other.m_range_request;
    m_if_range = other.m_if_range;
    m_if_range_etag = std::move(other.m_if_range_etag);
    m_seen_content_range = other.m_seen_content_range;
    m_body_validated = other.m_body_validated;
    m_etag = other.m_etag;
    m_last_modified = other.m_last_modified;
    m_stream = other.m_stream;
    m_curl = other.m_curl;
    m_headers = other.m_headers;
    m_headers_copy = other.m_headers_copy;
    m_resp_protocol = other.m_resp_protocol;
    m_is_transfer_state = other.m_is_transfer_state;
    curl_easy_setopt(m_curl, CURLOPT_HEADERDATA, this);
    if (m_is_transfer_state) {
        if (m_push) {
            curl_easy_setopt(m_curl, CURLOPT_READDATA, this);
        } else {
            curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, this);
        }
    }
    tpcForwardCreds = other.tpcForwardCreds;
    other.m_headers_copy.clear();
    other.m_curl = NULL;
    other.m_headers = NULL;
    other.m_stream = NULL;
    // BUG-4 fix: the stock code assigned this the wrong way around
    // (other.m_repr_digests = m_repr_digests), overwriting the source's
    // digests with our own (empty) map and losing them.  Move them over,
    // matching the copy-then-clear treatment of the other fields.
    m_repr_digests = std::move(other.m_repr_digests);
    other.m_repr_digests.clear();
}


bool State::InstallHandlers(CURL *curl) {
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "xrootd-tpc/" XrdVERSION);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &State::HeaderCB);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, this);
    // Handle -> State lookup for the scheduler loop.  Also the one handle
    // option that is introspectable (CURLINFO_PRIVATE), which T-U8 uses to
    // prove the reset/reinstall cycle actually restores options (SUB-5).
    curl_easy_setopt(curl, CURLOPT_PRIVATE, this);
    if(m_is_transfer_state) {
        if (m_push) {
            curl_easy_setopt(curl, CURLOPT_UPLOAD, 1);
            curl_easy_setopt(curl, CURLOPT_READFUNCTION, &State::ReadCB);
            curl_easy_setopt(curl, CURLOPT_READDATA, this);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &State::PushRespCB);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
            struct stat buf;
            if (SFS_OK == m_stream->Stat(&buf)) {
                m_push_length = buf.st_size;
                curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, buf.st_size);
            }
        } else {
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &State::WriteCB);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
        }
    }
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if(tpcForwardCreds) {
      curl_easy_setopt(curl,CURLOPT_UNRESTRICTED_AUTH,1L);
    }

    return true;
}

/**
 * Setup any headers necessary for the GET/PUT operation
 *
 * Currently includes:
 * - Handle the 'Copy-Headers' feature
 * - Adding `Expect: 100-continue` to get around a libcurl bug on uploads.
 */
void State::SetupHeaders(XrdHttpExtReq &req) {
    struct curl_slist *list = NULL;
    for (const auto & [header,value]: req.headers) {
        if (!strncasecmp(header.c_str(),"copy-header", 11)) {
            list = curl_slist_append(list, value.c_str());
            m_headers_copy.emplace_back(value);
        }
        // Note: len("TransferHeader") == 14
        if (!strncasecmp(header.c_str(),"transferheader",14)) {
            std::stringstream ss;
            ss << header.substr(14) << ": " << value;
            list = curl_slist_append(list, ss.str().c_str());
            m_headers_copy.emplace_back(ss.str());
        }
    }

    if(m_is_transfer_state && !m_push && !req.mReprDigest.empty()) {
      size_t reprDigestSize = req.mReprDigest.size();
      std::stringstream ss;
      ss << "Want-Repr-Digest: ";
      size_t cpt = 1;
      for (const auto &kv: req.mReprDigest) {
        // We put the same weight for the digest names as we do not have any way, according to the specs,
        // to give priority to a digest name in particular
        ss << kv.first << '=' << 5;
        if(cpt < reprDigestSize) {
          ss << ',';
        }
        cpt++;
      }
      list = curl_slist_append(list, ss.str().c_str());
      m_headers_copy.emplace_back(ss.str());
    }

    if (m_is_transfer_state && m_push && m_push_length > 0) {
        // On libcurl 8.5.0 - 8.9.1, we've observed bugs causing failures whenever
        // `Expect: 100-continue` is not used.  Older versions of libcurl unconditionally
        // set `Expect` whenever PUT is used (likely an older bug).  To workaround the issue,
        // we force `Expect` to be set, triggering the older libcurl behavior.
        // See: https://github.com/xrootd/xrootd/issues/2470
        // See: https://github.com/curl/curl/issues/17004
        list = curl_slist_append(list, "Expect: 100-continue");
        // Add Repr-Digest header to PUT request (PUSH)
        auto reprDigest = XrdOucTUtils::caseInsensitiveFind(req.headers,"repr-digest");
        if(reprDigest != req.headers.end()) {
          std::string reprDigestHeader {"Repr-Digest: " + reprDigest->second};
          curl_slist_append(list,reprDigestHeader.c_str());
        }
    }

    if (list != nullptr) {
        curl_easy_setopt(m_curl, CURLOPT_HTTPHEADER, list);
        m_headers = list;
    }
}

void State::SetIfRange(const std::string &etag) {
    // Appended to the persistent custom-header list so RebindHeaders
    // carries it across every curl_easy_reset (SUB-5).  Transfer-level
    // state: deliberately NOT cleared by ResetAfterRequest.
    const std::string header = "If-Range: " + etag;
    m_headers = curl_slist_append(m_headers, header.c_str());
    m_headers_copy.push_back(header);
    m_if_range = true;
    m_if_range_etag = etag;
    curl_easy_setopt(m_curl, CURLOPT_HTTPHEADER, m_headers);
}

void State::SetupHeadersForHEAD(XrdHttpExtReq &req) {
  struct curl_slist *list = NULL;
  for (const auto & [header,value]: req.headers) {
    if (!strncasecmp(header.c_str(),"copy-header", 11)) {
      list = curl_slist_append(list, value.c_str());
    }
    // Note: len("TransferHeader") == 14
    if (!strncasecmp(header.c_str(),"transferheader",14)) {
      std::stringstream ss;
      ss << header.substr(14) << ": " << value;
      list = curl_slist_append(list, ss.str().c_str());
    }
  }
  if(!req.mReprDigest.empty()) {
    size_t reprDigestSize = req.mReprDigest.size();
    std::stringstream ss;
    ss << "Want-Repr-Digest: ";
    size_t cpt = 1;
    for (const auto &kv: req.mReprDigest) {
      // We put the same weight for the digest names as we do not have any way, according to the specs,
      // to give priority to a digest name in particular
      ss << kv.first << '=' << 5;
      if(cpt < reprDigestSize) {
        ss << ',';
      }
      cpt++;
    }
    list = curl_slist_append(list, ss.str().c_str());
  }

  if (list != nullptr) {
    curl_easy_setopt(m_curl, CURLOPT_HTTPHEADER, list);
  }
}

// Reconstruct every field of transient (per-request) state, as if freshly
// constructed.  A State object is reused across many range requests, and under
// retry the same curl handle serves ranges that previously failed -- any field
// missed here leaks state from one request into the next.
//
// BUG-5 fix: the stock code did not clear the error fields, so a handle that
// had recorded an error once would misattribute that stale error text to
// whichever later range happened to fail.
//
// Checklist of transient fields (keep in sync with the State data members;
// when adding a member, decide explicitly whether it belongs here):
//   m_offset             bytes received in the current request       -> 0
//   m_status_code        HTTP status of the current response        -> -1
//   m_content_length     length claimed by the current response     -> -1
//   m_push_length        push-mode file size probe                  -> -1
//   m_recv_all_headers   header-parser progress flag                -> false
//   m_recv_status_line   header-parser progress flag                -> false
//   m_repr_digests       digests parsed from the current response   -> clear
//   m_error_buf          error text of the current request          -> clear
//   m_error_code         error class of the current request        -> errNone
//   m_expected_length    range length of the current request        -> -1
//   m_reported_length    Content-Length claimed by the response     -> -1
//   m_resp_range_start   parsed Content-Range start                 -> -1
//   m_resp_range_end     parsed Content-Range end                   -> -1
//   m_range_request      a Range was set for the current request    -> false
//   m_seen_content_range response carried Content-Range             -> false
//   m_body_validated     body-start validation already ran          -> false
//   m_etag               ETag from the current response             -> clear
//   m_last_modified      Last-Modified from the current response    -> clear
// NOT reset (they describe the transfer, not the request):
//   m_start_offset, m_stream, m_curl, m_headers*, m_push,
//   m_is_transfer_state, tpcForwardCreds, m_finalize_error_*,
//   m_content_length (the transfer's total length, set by the handler)
void State::ResetAfterRequest() {
    m_offset = 0;
    m_status_code = -1;
    m_push_length = -1;
    m_recv_all_headers = false;
    m_recv_status_line = false;
    m_repr_digests.clear();
    m_error_buf.clear();
    m_error_code = errNone;
    m_expected_length = -1;
    m_reported_length = -1;
    m_resp_range_start = -1;
    m_resp_range_end = -1;
    m_range_request = false;
    m_seen_content_range = false;
    m_body_validated = false;
    m_etag.clear();
    m_last_modified.clear();
}

void State::RebindHeaders() {
    if (m_curl && m_headers) {
        curl_easy_setopt(m_curl, CURLOPT_HTTPHEADER, m_headers);
    }
}

size_t State::HeaderCB(char *buffer, size_t size, size_t nitems, void *userdata)
{
    State *obj = static_cast<State*>(userdata);
    std::string header(buffer, size*nitems);
    return obj->Header(header);
}

int State::Header(const std::string &header) {
    //printf("Received remote header (%d, %d): %s", m_recv_all_headers, m_recv_status_line, header.c_str());
    if (m_recv_all_headers) {  // This is the second request -- maybe processed a redirect?
        m_recv_all_headers = false;
        m_recv_status_line = false;
    }
    if (!m_recv_status_line) {
        std::stringstream ss(header);
        std::string item;
        if (!std::getline(ss, item, ' ')) return 0;
        m_resp_protocol = item;
        //printf("\n\nResponse protocol: %s\n", m_resp_protocol.c_str());
        if (!std::getline(ss, item, ' ')) return 0;
        try {
            m_status_code = std::stol(item);
        } catch (...) {
            return 0;
        }
        m_recv_status_line = true;
    } else if (header.size() == 0 || header == "\n" || header == "\r\n") {
        m_recv_all_headers = true;
    }
    else if (header != "\r\n") {
        // Parse the header
        std::size_t found = header.find(":");
        if (found != std::string::npos) {
            std::string header_name = header.substr(0, found);
            std::transform(header_name.begin(), header_name.end(), header_name.begin(), ::tolower);
            std::string header_value = header.substr(found+1);
            if (header_name == "content-length")
            {
                try {
                    // BUG-3 fix: this records what the response *claims*
                    // (m_reported_length); it must never overwrite the
                    // expected range length set by SetTransferParameters.
                    // ValidateRangeResponse compares the two.
                    m_reported_length = std::stoll(header_value);
                } catch (...) {
                    // Header unparseable -- not a great sign, fail request.
                    //printf("Content-length header unparseable\n");
                    return 0;
                }
            }
            if (header_name == "etag" || header_name == "last-modified")
            {
                // Source validators (SUB-7): captured at the session-start
                // HEAD as the baseline the degraded-state re-probe (FR-14)
                // and the resume ladder (FR-21) compare against.
                std::string value = header_value;
                const char *whitespace = " \t\r\n";
                value.erase(0, value.find_first_not_of(whitespace));
                const auto tail = value.find_last_not_of(whitespace);
                if (tail != std::string::npos) {value.erase(tail + 1);}
                if (header_name == "etag") {
                    m_etag = value;
                } else {
                    m_last_modified = value;
                }
            }
            if (header_name == "content-range")
            {
                // FR-8: parse "bytes <start>-<end>/<total|*>" strictly; a
                // Content-Range we cannot parse is a malformed response and
                // fails the request (returning 0 aborts the transfer in
                // libcurl).  The parsed values are checked against the
                // request in ValidateRangeResponse.
                if (!ParseContentRange(header_value)) {
                    return 0;
                }
            }
            if(header_name == "repr-digest") {
              XrdHttpHeaderUtils::parseReprDigest(header_value,m_repr_digests);
            }
        } else {
            // Non-empty header that isn't the status line, but no ':' present --
            // malformed request?
            //printf("Malformed header: %s\n", header.c_str());
            return 0;
        }
    }
    return header.size();
}

size_t State::WriteCB(void *buffer, size_t size, size_t nitems, void *userdata) {
    State *obj = static_cast<State*>(userdata);
    if (obj->GetStatusCode() < 0) {
        return 0;
     }  // malformed request - got body before headers.
    if (obj->GetStatusCode() >= 400) {
        obj->m_error_buf += std::string(static_cast<char*>(buffer),
                                        std::min(static_cast<size_t>(1024), size*nitems));
        // Record error messages until we hit a KB; at that point, fail out.
        if (obj->m_error_buf.size() >= 1024)
            return 0;
        else
            return size*nitems;
    }  // Status indicates failure.
    // FR-8: before the first body byte of a ranged request is accepted, the
    // response must prove it honors the range (206, echoed Content-Range,
    // consistent Content-Length).  Returning 0 aborts the transfer before
    // any misdirected byte can reach the reorder buffers.
    if (!obj->m_body_validated) {
        obj->m_body_validated = true;
        if (!obj->ValidateRangeResponse(false)) {
            return 0;
        }
    }
    return obj->Write(static_cast<char*>(buffer), size*nitems);
}

/**
 * This callback is used to give users the error message returns by the passive server of
 * the TPC PUSH
 * It is a write callback! --> the error message is written to the buffer
 */
size_t State::PushRespCB(void *buffer, size_t size, size_t nitems, void *userdata) {
  State *obj = static_cast<State*>(userdata);
  // Note: The obj's status code is set by the HeaderCB once there's a reply from the passive server
  if (obj->GetStatusCode() < 0) {
    return 0;
  }  // malformed request - got body before headers.
  if (obj->GetStatusCode() >= 400) {
    obj->m_error_buf += std::string(static_cast<char*>(buffer),
                                    std::min(static_cast<size_t>(1024), size*nitems));
    // Record error messages until we hit a KB; at that point, fail out.
    if (obj->m_error_buf.size() >= 1024)
      return 0;
    else
      return size*nitems;
  }
  return size*nitems;
}

ssize_t State::Write(char *buffer, size_t size) {
    // FR-8: over-delivery on a ranged request is a source malfunction; stop
    // before a byte beyond the requested range can land in the stream.
    if (m_range_request &&
        m_offset + static_cast<off_t>(size) > m_expected_length) {
        m_error_buf = "source delivered more bytes than the requested range";
        m_error_code = errLengthMismatch;
        return -1;
    }
    ssize_t retval = m_stream->Write(m_start_offset + m_offset, buffer, size, false);
    if (retval == SFS_ERROR) {
        m_error_buf = m_stream->GetErrorMessage();
        m_error_code = errWrite;
        return -1;
    }
    m_offset += retval;
    return retval;
}

void State::RecordFinalizeError(int error_code, const std::string &error_msg) {
    if (m_finalize_error_code) {
        return;
    }
    m_finalize_error_code = error_code;
    m_finalize_error_buf = error_msg;
}

int State::Flush() {
    if (m_push) {
        return 0;
    }

    if (m_stream->Flush() == SFS_ERROR) {
        RecordFinalizeError(errFlush, m_stream->GetErrorMessage());
        return -1;
    }
    return 0;
}

size_t State::ReadCB(void *buffer, size_t size, size_t nitems, void *userdata) {
    State *obj = static_cast<State*>(userdata);
    if (obj->GetStatusCode() < 0) {return 0;}  // malformed request - got body before headers.
    if (obj->GetStatusCode() >= 400) {return 0;}  // Status indicates failure.
    return obj->Read(static_cast<char*>(buffer), size*nitems);
}

int State::Read(char *buffer, size_t size) {
    int retval = m_stream->Read(m_start_offset + m_offset, buffer, size);
    if (retval == SFS_ERROR) {
        return -1;
    }
    m_offset += retval;
    //printf("Read a total of %ld bytes.\n", m_offset);
    return retval;
}

void State::SetTransferParameters(off_t offset, size_t size) {
    m_start_offset = offset;
    m_offset = 0;
    // BUG-3 fix: the requested range length lives in its own field; the
    // response's Content-Length is recorded separately (m_reported_length)
    // and validated against this, never allowed to overwrite it.
    m_expected_length = static_cast<off_t>(size);
    m_range_request = true;
    std::stringstream ss;
    ss << offset << "-" << (offset+size-1);
    curl_easy_setopt(m_curl, CURLOPT_RANGE, ss.str().c_str());
}

// Parses a Content-Range header value of the form
//   bytes <start>-<end>/<total|*>
// (leading whitespace tolerated, as header values arrive with the separator
// space).  Returns false on anything it cannot parse strictly.
bool State::ParseContentRange(const std::string &header_value) {
    const char *cursor = header_value.c_str();
    while (*cursor == ' ' || *cursor == '\t') {cursor++;}
    if (strncasecmp(cursor, "bytes", 5)) {return false;}
    cursor += 5;
    while (*cursor == ' ' || *cursor == '\t') {cursor++;}

    char *end = nullptr;
    errno = 0;
    long long start = strtoll(cursor, &end, 10);
    if (errno || end == cursor || start < 0 || *end != '-') {return false;}
    cursor = end + 1;
    long long last = strtoll(cursor, &end, 10);
    if (errno || end == cursor || last < start || *end != '/') {return false;}
    cursor = end + 1;
    // The complete length is either "*" or a number; it is not used for
    // validation (the requested range is the yardstick), but a malformed
    // field still fails the parse.
    if (*cursor == '*') {
        cursor++;
    } else {
        strtoll(cursor, &end, 10);
        if (errno || end == cursor) {return false;}
        cursor = end;
    }
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {cursor++;}
    if (*cursor != '\0') {return false;}

    m_resp_range_start = static_cast<off_t>(start);
    m_resp_range_end = static_cast<off_t>(last);
    m_seen_content_range = true;
    return true;
}

bool State::ValidateRangeResponse(bool completion) {
    if (!m_range_request) {
        // Whole-object requests (push responses, the transitional
        // single-stream pull) validate nothing here.
        return true;
    }
    // Every error below is permanent-class (FR-12): the source demonstrably
    // does not implement ranged GETs correctly, so retrying is pointless and
    // continuing risks corrupting the destination.
    if (m_status_code != 206) {
        std::stringstream ss;
        if (m_status_code == 200 && m_if_range && m_etag != m_if_range_etag) {
            // WP-14/H6: with the If-Range guard armed, a 200 whose response
            // ETag differs from the one we sent is the source SAYING the
            // entity no longer matches -- the file changed in place with no
            // transport fault to catch it.  (A 200 whose ETag still MATCHES
            // is a range-dishonoring source, handled below: it ignored both
            // Range and If-Range.)
            ss << "source entity changed during the transfer "
                  "(If-Range guard: 200 to a ranged request)";
            m_error_buf = ss.str();
            m_error_code = errSourceChanged;
            return false;
        }
        if (m_status_code == 200) {
            // The canonical failure: source ignored Range and is streaming
            // the whole file to every connection (BUG-2's silent corruption
            // path in the stock handler).
            ss << "source does not honor Range requests (status 200 to a ranged request)";
        } else {
            ss << "source returned unexpected status " << m_status_code
               << " to a ranged request";
        }
        m_error_buf = ss.str();
        m_error_code = errRangeNotHonored;
        return false;
    }
    if (!m_seen_content_range) {
        m_error_buf = "source response has no Content-Range header for a ranged request";
        m_error_code = errRangeMismatch;
        return false;
    }
    if (m_resp_range_start != m_start_offset ||
        m_resp_range_end != m_start_offset + m_expected_length - 1) {
        std::stringstream ss;
        ss << "source Content-Range does not echo the requested range"
           << " (requested " << m_start_offset << "-"
           << (m_start_offset + m_expected_length - 1)
           << ", response " << m_resp_range_start << "-" << m_resp_range_end << ")";
        m_error_buf = ss.str();
        m_error_code = errRangeMismatch;
        return false;
    }
    if (m_reported_length >= 0 && m_reported_length != m_expected_length) {
        std::stringstream ss;
        ss << "source response Content-Length " << m_reported_length
           << " does not match the requested range length " << m_expected_length;
        m_error_buf = ss.str();
        m_error_code = errLengthMismatch;
        return false;
    }
    if (completion && m_offset != m_expected_length) {
        std::stringstream ss;
        ss << "source delivered a different byte count than requested"
           << " (got " << m_offset << ", requested " << m_expected_length << ")";
        m_error_buf = ss.str();
        m_error_code = errLengthMismatch;
        return false;
    }
    return true;
}

bool State::Finalize()
{
    if (!m_stream->Finalize()) {
        RecordFinalizeError(errClose, m_stream->GetErrorMessage());
        return false;
    }
    return true;
}

std::string State::GetConnectionDescription()
{
    // CURLINFO_PRIMARY_PORT is only defined for 7.21.0 or later; on older
    // library versions, simply omit this information.
#if LIBCURL_VERSION_NUM >= 0x071500
    char *curl_ip = NULL;
    CURLcode rc = curl_easy_getinfo(m_curl, CURLINFO_PRIMARY_IP, &curl_ip);
    if ((rc != CURLE_OK) || !curl_ip) {
        return "";
    }
    long curl_port = 0;
    rc = curl_easy_getinfo(m_curl, CURLINFO_PRIMARY_PORT, &curl_port);
    if ((rc != CURLE_OK) || !curl_port) {
        return "";
    }
    std::stringstream ss;
    // libcurl returns IPv6 addresses of the form:
    //    2600:900:6:1301:5054:ff:fe0b:9cba:8000
    // However the HTTP-TPC spec says to use the form
    //   [2600:900:6:1301:5054:ff:fe0b:9cba]:8000
    // Hence, we add '[' and ']' whenever a ':' is seen.
    if (NULL == strchr(curl_ip, ':'))
        ss << "tcp:" << curl_ip << ":" << curl_port;
    else
        ss << "tcp:[" << curl_ip << "]:" << curl_port;
    return ss.str();
#else
    return "";
#endif
}
