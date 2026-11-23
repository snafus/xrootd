/**
 * Which failures of a request of a pull in blocks are tried again, and how
 * many tries a block gets.  See XrdHttpTpcRetry.hh for the rule.
 */

#include "XrdHttpTpcRetry.hh"

#include "XrdHttpTpcBlockPull.hh"
#include "XrdHttpTpcState.hh"

#include <algorithm>

using namespace TPC;

bool TPC::StatusIsFinal(int status_code)
{
    switch (status_code) {
        case 400:  // Bad Request
        case 401:  // Unauthorized: the token is refused, or has run out
        case 403:  // Forbidden
        case 404:  // Not Found: the file is gone
        case 405:  // Method Not Allowed
        case 406:  // Not Acceptable
        case 407:  // Proxy Authentication Required
        case 410:  // Gone
        case 411:  // Length Required
        case 412:  // Precondition Failed
        case 413:  // Content Too Large
        case 414:  // URI Too Long
        case 415:  // Unsupported Media Type
        case 416:  // Range Not Satisfiable: the file is shorter than it was
        case 417:  // Expectation Failed
        case 426:  // Upgrade Required
        case 431:  // Request Header Fields Too Large
        case 451:  // Unavailable For Legal Reasons
        case 501:  // Not Implemented
        case 505:  // HTTP Version Not Supported
            return true;
        default:
            return false;
    }
}

bool TPC::CurlResultIsFinal(CURLcode result)
{
    switch (result) {
        // The request cannot be made on this server.
        case CURLE_FAILED_INIT:
        case CURLE_OUT_OF_MEMORY:
        case CURLE_NOT_BUILT_IN:
        case CURLE_BAD_FUNCTION_ARGUMENT:
        case CURLE_UNKNOWN_OPTION:
        case CURLE_URL_MALFORMAT:
        case CURLE_FILESIZE_EXCEEDED:
        // The source sends the request round in a circle.
        case CURLE_TOO_MANY_REDIRECTS:
        // The source does not serve ranges.
        case CURLE_RANGE_ERROR:
        // The certificate of the source is not accepted, or what this server
        // needs for TLS is missing or wrong.  A failure of the TLS handshake
        // itself (CURLE_SSL_CONNECT_ERROR) is not here: it is what a
        // connection cut during the handshake gives, and is retried.
        case CURLE_PEER_FAILED_VERIFICATION:
#if LIBCURL_VERSION_NUM < 0x073E00
        // Before libcurl 7.62 a certificate that no known authority signed
        // had a result of its own.
        case CURLE_SSL_CACERT:
#endif
        case CURLE_SSL_CERTPROBLEM:
        case CURLE_SSL_CIPHER:
        case CURLE_SSL_CACERT_BADFILE:
        case CURLE_SSL_CRL_BADFILE:
        case CURLE_SSL_ISSUER_ERROR:
        case CURLE_SSL_ENGINE_NOTFOUND:
        case CURLE_SSL_ENGINE_SETFAILED:
        case CURLE_SSL_ENGINE_INITFAILED:
        case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
        // The source refuses this server.
        case CURLE_LOGIN_DENIED:
        case CURLE_REMOTE_ACCESS_DENIED:
            return true;
        default:
            return false;
    }
}

NextStep TPC::AfterRequest(const RequestResult &result)
{
    // First what the request code found, which says more than the status or
    // the result of libcurl that came of it.
    switch (result.error_code) {
        case State::errNone:
            break;
        case State::errTimeout:
            // The stall check of the stream ended the request.
            return NextStep::Retry;
        case State::errRange:
            // A 200, or a 203 from a proxy, is the whole file: the source
            // does not serve ranges, and will not on the next request either.
            // Otherwise the range or the length of the body was not what was
            // asked for, which may be a connection that ended early or a
            // fault of the moment.  (A body longer than its range is ended
            // by the stream thread, which knows of it; see RunWorker().)
            return (result.status_code == 200 || result.status_code == 203) ? NextStep::Fail
                                                                            : NextStep::Retry;
        default:
            // The source file changed (errSource), a write to the storage
            // failed (errWrite, errFlush, errClose), an internal error
            // (BlockPull::errInternal), or a code this function does not
            // know: nothing the source can make good.
            return NextStep::Fail;
    }
    // A status of 400 or more fails the request; so does a final status that
    // brings no part of the file (204, 304, a redirection that was not
    // followed), for which libcurl reports no error.
    if (result.status_code >= 400
        || (result.curl_result == CURLE_OK && !State::CarriesContent(result.status_code))) {
        return StatusIsFinal(result.status_code) ? NextStep::Fail : NextStep::Retry;
    }
    if (result.curl_result != CURLE_OK) {
        return CurlResultIsFinal(result.curl_result) ? NextStep::Fail : NextStep::Retry;
    }
    return NextStep::Done;
}

bool RetryPolicy::Another(bool progress)
{
    m_tries++;
    m_without_progress = progress ? 0 : m_without_progress + 1;
    // One attempt is no retry at all, whatever the try brought.
    if (m_attempts <= 1) {return false;}
    return m_without_progress < m_attempts && m_tries < m_most;
}

long RetryPolicy::WaitMilliseconds(long retry_after, double chance) const
{
    // Doubled for each try in a row that brought nothing, from the second,
    // up to MaxWait; a configured wait that is longer is kept as it is.
    long seconds = m_wait;
    for (int idx = 1; idx < m_without_progress && seconds < MaxWait; idx++) {
        seconds = std::min<long>(seconds * 2, std::max(MaxWait, m_wait));
    }
    // The source knows better when it will be ready, within reason.
    seconds = std::max(seconds, std::min<long>(retry_after, MaxRetryAfter));
    if (seconds <= 0) {return 0;}
    chance = std::min(1.0, std::max(0.0, chance));
    return seconds * 1000 + static_cast<long>(seconds * 250 * chance);
}
