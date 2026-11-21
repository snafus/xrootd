
#include <algorithm>
#include <sstream>
#include <stdexcept>

#include "XrdVersion.hh"
#include "XrdHttp/XrdHttpExtHandler.hh"
#include "XrdSfs/XrdSfsInterface.hh"
#include "XrdOuc/XrdOucTUtils.hh"

#include <curl/curl.h>

#include "XrdHttpTpcBlockPull.hh"
#include "XrdHttpTpcState.hh"
#include "XrdHttpTpcStream.hh"
#include "XrdHttpTpcUtils.hh"

#include "XrdHttp/XrdHttpHeaderUtils.hh"

using namespace TPC;


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
    other.m_repr_digests = m_repr_digests;
}


bool State::InstallHandlers(CURL *curl) {
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "xrootd-tpc/" XrdVERSION);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &State::HeaderCB);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, this);
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

void State::ResetAfterRequest() {
    m_offset = 0;
    m_status_code = -1;
    m_content_length = -1;
    m_push_length = -1;
    m_range_length = -1;
    m_recv_all_headers = false;
    m_recv_status_line = false;
    m_repr_digests.clear();
    m_wbuf.clear();
    // What a response said belongs to that response: a text kept from one
    // request must not be reported as the cause of the failure of the next.
    m_error_buf.clear();
}

// HeaderCB and WriteCB are callbacks of libcurl, which is C: no exception may
// pass through them.  In blocks mode they run in a stream thread, where an
// exception that escapes ends the server.  Returning a number other than the
// one libcurl handed over makes it end the request with an error.
size_t State::HeaderCB(char *buffer, size_t size, size_t nitems, void *userdata)
{
    try {
        State *obj = static_cast<State*>(userdata);
        std::string header(buffer, size*nitems);
        return obj->Header(header);
    } catch (...) {
        return 0;
    }
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
        // A new response: what an earlier one of this request said (a
        // redirection that was followed) is not about this one.
        m_error_buf.clear();
        m_resp_content_length = -1;
        m_resp_range_first = -1;
        m_resp_range_last = -1;
        m_resp_range_total = -1;
        m_resp_etag.clear();
        m_resp_last_modified.clear();
    } else if (header.size() == 0 || header == "\n" || header == "\r\n") {
        m_recv_all_headers = true;
        if (!CheckRangeResponse() || !CheckSourceUnchanged()) {
            return 0;
        }
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
                    m_content_length = std::stoll(header_value);
                } catch (...) {
                    // Header unparseable -- not a great sign, fail request.
                    //printf("Content-length header unparseable\n");
                    return 0;
                }
                m_resp_content_length = m_content_length;
            }
            if (header_name == "content-range") {
                // An unparseable value leaves the range unset; it is reported
                // once all the headers are in.
                XrdHttpTpcUtils::parseContentRange(header_value, m_resp_range_first, m_resp_range_last,
                                                   &m_resp_range_total);
            }
            if (header_name == "etag" || header_name == "last-modified") {
                // Kept as sent, without the white space around it: they are
                // only compared with those of other responses.
                static const char blanks[] = " \t\r\n";
                const size_t begin = header_value.find_first_not_of(blanks);
                const std::string value = begin == std::string::npos ? std::string()
                    : header_value.substr(begin, header_value.find_last_not_of(blanks) - begin + 1);
                (header_name == "etag" ? m_resp_etag : m_resp_last_modified) = value;
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

bool State::CheckRangeResponse() {
    // Only a successful answer to a range request is checked here; errors and
    // intermediate responses (redirections) are handled elsewhere.
    if (m_push || m_range_length < 0 || m_status_code < 200 || m_status_code >= 300) {
        return true;
    }
    const long long first = m_start_offset;
    const long long last = first + m_range_length - 1;
    if (m_status_code == 206 && m_resp_range_first == first && m_resp_range_last == last) {
        return true;
    }
    // A server may ignore the Range header and send the whole file. That is
    // what was requested when the range covers the whole file.
    if (m_status_code == 200 && first == 0 && m_resp_content_length == m_range_length) {
        return true;
    }
    std::stringstream ss;
    ss << "The source did not return the requested range (bytes " << first << "-" << last
       << "): it answered with status " << m_status_code;
    if (m_resp_range_first >= 0) {
        ss << " and the range bytes " << m_resp_range_first << "-" << m_resp_range_last;
    } else if (m_status_code == 206) {
        ss << " without a valid Content-Range header";
    } else if (m_status_code == 200) {
        ss << "; it must support range requests for a transfer with several streams";
    }
    m_error_buf = ss.str();
    m_error_code = errRange;
    return false;
}

bool State::CheckSourceUnchanged() {
    // Only a response that brings a part of the file says something about
    // the file; a redirection or an error does not.
    if (!m_source_check || m_push || !CarriesContent(m_status_code)) {
        return true;
    }
    SourceCheck::Seen seen;
    seen.total = m_resp_range_total;
    seen.etag = m_resp_etag;
    seen.last_modified = m_resp_last_modified;
    // The server that answered, in the form the performance markers use:
    // behind a redirector the requests of a pull may reach different servers.
    seen.host = GetConnectionDescription();
    std::string error;
    if (!m_source_check->Agrees(seen, error)) {
        m_error_buf = error;
        m_error_code = errSource;
        return false;
    }
    return true;
}

size_t State::WriteCB(void *buffer, size_t size, size_t nitems, void *userdata) {
    try {
        return static_cast<State*>(userdata)->Body(buffer, size, nitems);
    } catch (...) {
        // Not size*nitems: libcurl ends the request with a write error.
        return 0;
    }
}

size_t State::Body(void *buffer, size_t size, size_t nitems) {
    if (GetStatusCode() < 0) {
        return 0;
    }  // malformed request - got body before headers.
    if (GetStatusCode() >= 400) {
        m_error_buf += std::string(static_cast<char*>(buffer),
                                   std::min(static_cast<size_t>(1024), size*nitems));
        // Record error messages until we hit a KB; at that point, fail out.
        if (m_error_buf.size() >= 1024)
            return 0;
        else
            return size*nitems;
    }  // Status indicates failure.
    if (!CarriesContent(GetStatusCode())) {
        // Not the file: keep the beginning for the error message and drop
        // the rest.  The request is left to end normally, so that the pull
        // fails on the status and not on an aborted transfer.
        if (m_error_buf.size() < 1024) {
            m_error_buf.append(static_cast<char*>(buffer),
                               std::min(1024 - m_error_buf.size(), size*nitems));
        }
        return size*nitems;
    }
    return Write(static_cast<char*>(buffer), size*nitems);
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

bool State::CheckRangeComplete() {
    if (m_push || m_range_length < 0 || m_error_code || m_offset == m_range_length
        || m_status_code < 200 || m_status_code >= 300) {
        return true;
    }
    std::stringstream ss;
    ss << "The source sent " << m_offset << " bytes for the range bytes " << m_start_offset
       << "-" << (m_start_offset + m_range_length - 1) << ", which is " << m_range_length
       << " bytes long";
    m_error_buf = ss.str();
    m_error_code = errRange;
    return false;
}

ssize_t State::Write(char *buffer, size_t size) {
    if (m_range_length >= 0 && m_offset + static_cast<off_t>(size) > m_range_length) {
        // Never write past the end of the range: these bytes belong to another request.
        std::stringstream ss;
        ss << "The source sent more than " << m_range_length << " bytes for the range bytes "
           << m_start_offset << "-" << (m_start_offset + m_range_length - 1);
        m_error_buf = ss.str();
        m_error_code = errRange;
        return -1;
    }
    const size_t write_size = m_stream->OffsetWriteSize();
    if (write_size) {
        // Blocks mode: collect the data and write it at its own offset each
        // time one write is complete.  libcurl hands the data over in pieces
        // of at most its buffer, 16 KiB unless set otherwise; written as they
        // come, these pieces would each be a system call and a record for the
        // real-time checksum of the storage.
        size_t done = 0;
        try {
            while (done < size) {
                if (m_wbuf.empty()) {
                    m_wbuf.reserve(write_size);
                    m_wbuf_offset = m_start_offset + m_offset + done;
                }
                size_t count = std::min(size - done, write_size - m_wbuf.size());
                m_wbuf.insert(m_wbuf.end(), buffer + done, buffer + done + count);
                done += count;
                if (m_wbuf.size() == write_size && !FlushWriteBuffer()) {
                    return -1;
                }
            }
        } catch (std::exception &e) {
            m_error_buf = std::string("Failed to buffer the data received: ") + e.what();
            m_error_code = errWrite;
            return -1;
        }
        // Only now: the whole piece is in the buffer or in the file.
        m_offset += size;
        if (m_pull_received) {m_pull_received->fetch_add(size, std::memory_order_relaxed);}
        if (m_worker_status) {m_worker_status->received.fetch_add(size, std::memory_order_relaxed);}
        return size;
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

bool State::FlushWriteBuffer() {
    if (m_wbuf.empty()) {return true;}
    if (m_pull_stop && m_pull_stop->load(std::memory_order_relaxed)) {
        // The pull is being stopped for a reason that is already recorded,
        // by another stream or by the thread of the request.  Nothing more is
        // written, and this is not a cause.
        m_wbuf.clear();
        return false;
    }
    const off_t offset = m_wbuf_offset;
    const size_t size = m_wbuf.size();
    std::string error;
    // While this stream writes, or waits for its turn to write, it is not
    // waiting for the source.
    if (m_worker_status) {m_worker_status->phase.store(WorkerStatus::Writing);}
    ssize_t retval = m_stream->WriteAt(offset, m_wbuf.data(), size, error);
    if (m_worker_status) {m_worker_status->phase.store(WorkerStatus::Receiving);}
    m_wbuf.clear();
    if (retval != static_cast<ssize_t>(size)) {
        // A short write counts as a failure: the bytes the storage did not
        // take are not in the file, whatever the layers above it believe.
        std::stringstream ss;
        if (retval >= 0) {
            ss << "The storage took only " << retval << " of the " << size
               << " bytes of a write at offset " << offset;
        } else {
            ss << (error.empty() ? "(no error message provided)" : error)
               << "; write of " << size << " bytes at offset " << offset;
        }
        m_error_buf = ss.str();
        m_error_code = errWrite;
        return false;
    }
    if (m_pull_written) {m_pull_written->fetch_add(retval, std::memory_order_relaxed);}
    return true;
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

State *State::Duplicate() {
    CURL *curl = curl_easy_duphandle(m_curl);
    if (!curl) {
        throw std::runtime_error("Failed to duplicate existing curl handle.");
    }

    State *state = new State(0, *m_stream, curl, m_push, tpcForwardCreds);

    if (m_headers) {
        state->m_headers_copy.reserve(m_headers_copy.size());
        for (std::vector<std::string>::const_iterator header_iter = m_headers_copy.begin();
             header_iter != m_headers_copy.end();
             header_iter++) {
            state->m_headers = curl_slist_append(state->m_headers, header_iter->c_str());
            state->m_headers_copy.push_back(*header_iter);
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, state->m_headers);
    }

    return state;
}

void State::SetTransferParameters(off_t offset, size_t size) {
    m_start_offset = offset;
    m_offset = 0;
    m_content_length = size;
    m_range_length = size;
    std::stringstream ss;
    ss << offset << "-" << (offset+size-1);
    curl_easy_setopt(m_curl, CURLOPT_RANGE, ss.str().c_str());
}

int State::AvailableBuffers() const
{
    return m_stream->AvailableBuffers();
}

void State::DumpBuffers() const
{
    m_stream->DumpBuffers();
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
