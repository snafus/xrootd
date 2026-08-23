#include "XrdHttp/XrdHttpExtHandler.hh"
#include "XrdNet/XrdNetAddr.hh"
#include "XrdNet/XrdNetUtils.hh"
#include "XrdOuc/XrdOucEnv.hh"
#include "XrdSec/XrdSecEntity.hh"
#include "XrdSfs/XrdSfsInterface.hh"
#include "XrdSys/XrdSysAtomics.hh"
#include "XrdSys/XrdSysFD.hh"
#include "XrdVersion.hh"

#include "XrdXrootd/XrdXrootdRedirHelper.hh"
#include "XrdXrootd/XrdXrootdTpcMon.hh"
#include "XrdOuc/XrdOucPrivateUtils.hh"
#include "XrdOuc/XrdOucTUtils.hh"
#include "XrdHttpTpcR/XrdHttpTpcUtils.hh"
#include "XrdHttp/XrdHttpUtils.hh"

#include <curl/curl.h>

#include <dlfcn.h>
#include <fcntl.h>

#include <algorithm>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "XrdCks/XrdCksAssist.hh"
#include "XrdOuc/XrdOucCRC.hh"
#include "XrdSfs/XrdSfsFAttr.hh"

#include "XrdHttpTpcRDigest.hh"
#include "XrdHttpTpcRState.hh"
#include "XrdHttpTpcRStream.hh"
#include "XrdHttpTpcRTPC.hh"
#include <fstream>

using namespace TPCR;

CURLMcode TPCR::MultiWait(CURLM *multi_handle, int timeout_ms)
{
#if CURL_AT_LEAST_VERSION(7, 66, 0)
    // curl_multi_poll waits for the full timeout even with zero file
    // descriptors registered, which is exactly the semantic the transfer
    // loops need; no fallback logic required.
    return curl_multi_poll(multi_handle, NULL, 0, timeout_ms, NULL);
#else
    // Pre-7.66 emulation (see MultiWait declaration / BUG-9): if libcurl had
    // nothing to wait on, curl_multi_wait returns at once -- sleep a bounded
    // amount so the caller does not spin.  100 ms keeps the loop responsive
    // to newly-connectable sockets while capping the spin at ~10 Hz.
    int fd_count = 0;
    CURLMcode mres = curl_multi_wait(multi_handle, NULL, 0, timeout_ms, &fd_count);
    if (mres == CURLM_OK && fd_count == 0) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(std::min(timeout_ms, 100)));
    }
    return mres;
#endif
}

XrdXrootdTpcMon* TPCRHandler::TPCLogRecord::tpcMonitor = 0;

uint64_t TPCRHandler::m_monid{0};
int TPCRHandler::m_marker_period = 5;
size_t TPCRHandler::m_block_size = 16*1024*1024;
size_t TPCRHandler::m_small_block_size = 1*1024*1024;
XrdSysMutex TPCRHandler::m_monid_mutex;
bool TPCRHandler::allowMissingCRL = false;

XrdVERSIONINFO(XrdHttpGetExtHandler, HttpTPC);

/******************************************************************************/
/*   T P C H a n d l e r : : T P C L o g R e c o r d   D e s t r u c t o r    */
/******************************************************************************/
  
TPCRHandler::TPCLogRecord::~TPCLogRecord()
{
// Record monitoring data is enabled
//
   if (tpcMonitor)
      {XrdXrootdTpcMon::TpcInfo monInfo;

       monInfo.clID = clID.c_str();
       monInfo.begT = begT;
       gettimeofday(&monInfo.endT, 0);

       if (mTpcType == TpcType::Pull)
          {monInfo.dstURL = local.c_str();
           monInfo.srcURL = remote.c_str();
          } else {
           monInfo.dstURL = remote.c_str();
           monInfo.srcURL = local.c_str();
           monInfo.opts |= XrdXrootdTpcMon::TpcInfo::isaPush;
          }

       if (!status) monInfo.endRC = 0;
          else if (tpc_status > 0) monInfo.endRC = tpc_status;
                  else  monInfo.endRC = 1;
       monInfo.strm  = static_cast<unsigned char>(streams);
       monInfo.fSize = (bytes_transferred < 0 ? 0 : bytes_transferred);
       if (!isIPv6) monInfo.opts |= XrdXrootdTpcMon::TpcInfo::isIPv4;

       tpcMonitor->Report(monInfo);
      }
}
  
/******************************************************************************/
/*               C u r l D e l e t e r : : o p e r a t o r ( )                */
/******************************************************************************/
  
void CurlDeleter::operator()(CURL *curl)
{
    if (curl) curl_easy_cleanup(curl);
}

/******************************************************************************/
/*           s o c k o p t _ s e t c l o e x e c _ c a l l b a c k            */
/******************************************************************************/
  
/**
 * The callback that will be called by libcurl when the socket has been created
 * https://curl.se/libcurl/c/CURLOPT_SOCKOPTFUNCTION.html
 *
 * Note: that this callback has been replaced by the opensocket_callback as it
 *       was needed for monitoring to report what IP protocol was being used.
 *       It has been kept in case we will need this callback in the future.
 */
int TPCRHandler::sockopt_callback(void *clientp, curl_socket_t curlfd, curlsocktype purpose) {
  TPCLogRecord * rec = (TPCLogRecord *)clientp;
  if (purpose == CURLSOCKTYPE_IPCXN && rec && rec->pmarkManager.isEnabled()) {
      // We will not reach this callback if the corresponding socket could not have been connected
      // the socket is already connected only if the packet marking is enabled
      return CURL_SOCKOPT_ALREADY_CONNECTED;
  }
  return CURL_SOCKOPT_OK;
}

/******************************************************************************/
/*                   o p e n s o c k e t _ c a l l b a c k                    */
/******************************************************************************/
  
  
/**
 * The callback that will be called by libcurl when the socket is about to be
 * opened so we can capture the protocol that will be used.
 */
int TPCRHandler::opensocket_callback(void *clientp,
                                    curlsocktype purpose,
                                    struct curl_sockaddr *aInfo)
{
  /* CURLSOCKTYPE_IPCXN (for IP based connections) is the only type currently known by curl,
   * so let's make sure to reject other types if they appear in the furure */
  if (purpose != CURLSOCKTYPE_IPCXN)
    return CURL_SOCKET_BAD;

  if (!aInfo)
    return CURL_SOCKET_BAD;

  // Create the socket (note that O_CLOEXEC flag will be set)
  int fd = XrdSysFD_Socket(aInfo->family, aInfo->socktype, aInfo->protocol);

  if (fd < 0) {
    return CURL_SOCKET_BAD;
  }

  if (!clientp)
    return fd;

  XrdNetAddr thePeer(&(aInfo->addr));
  TPCLogRecord *rec = static_cast<TPCLogRecord*>(clientp);

  /* Reject attempts to connect to local/private addresses unless allowed by configuration */
  if ((!rec->allow_private && thePeer.isPrivate()) || (!rec->allow_local && thePeer.isLocal())) {
    rec->tpc_status = 403; // Forbidden
    rec->m_log->Emsg(rec->log_prefix.c_str(),
      "Connection to local/private address is forbidden");
    close(fd);
    return CURL_SOCKET_BAD;
  }

  rec->isIPv6 = (thePeer.isIPType(XrdNetAddrInfo::IPv6) && !thePeer.isMapped());

  std::stringstream connectErrMsg;
  if(!rec->pmarkManager.connect(fd, &(aInfo->addr), aInfo->addrlen, CONNECT_TIMEOUT, connectErrMsg)) {
    // at this point fd has already been closed
    rec->m_log->Emsg(rec->log_prefix.c_str(), "Unable to connect socket: ", connectErrMsg.str().c_str());
    return CURL_SOCKET_BAD;
  }

  return fd;
}

int TPCRHandler::closesocket_callback(void *clientp, curl_socket_t fd) {
  TPCLogRecord * rec = (TPCLogRecord *)clientp;

  // Destroy the PMark handle associated to the file descriptor before closing it.
  // Otherwise, we would lose the socket usage information if the socket is closed before
  // the PMark handle is closed.
  rec->pmarkManager.endPmark(fd);

  return close(fd);
}

/******************************************************************************/
/*           s s l _ c t x _ c a l l b a c k                                  */
/******************************************************************************/

/**
 * The callback that will be called by libcurl just before the initialization of an SSL connection
 * after having processed all other SSL related options to give a last chance to an application to
 * modify the behavior of the SSL initialization.
 * https://curl.se/libcurl/c/CURLOPT_SSL_CTX_FUNCTION.html
 */
int TPCRHandler::ssl_ctx_callback(CURL *curl, void *ssl_ctx, void *clientp) {
    TPCLogRecord * rec = (TPCLogRecord *)clientp;
    SSL_CTX* ctx = static_cast<SSL_CTX*>(ssl_ctx);

    if (rec && rec->ca_store) {
        // Bumps the store's reference count instead of re-parsing the CA and CRL
        // bundles for this connection.  libcurl runs this callback after it has
        // applied its own TLS options, so this replaces whatever store it built.
        SSL_CTX_set1_cert_store(ctx, rec->ca_store.get());
    }
    if (allowMissingCRL) {
        // verify_callback only excuses X509_V_ERR_UNABLE_TO_GET_CRL, i.e. a CA in
        // the chain for which no CRL could be found.  Every other verification rule
        // still applies, including revocation itself whenever a CRL is present.
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, verify_callback);
    }
    return CURLE_OK;
}

int TPCRHandler::verify_callback(int preverify_ok, X509_STORE_CTX* ctx) {
    if (preverify_ok == 1) return 1;

    int err = X509_STORE_CTX_get_error(ctx);

    if (err == X509_V_ERR_UNABLE_TO_GET_CRL) {
        X509_STORE_CTX_set_error(ctx, X509_V_OK);
        return 1;
    }

    return 0;
}

/******************************************************************************/
/*                            p r e p a r e U R L                             */
/******************************************************************************/

// See XrdHttpTpcUtils::prepareOpenURL() documentation
std::string TPCRHandler::prepareURL(XrdHttpExtReq &req) {
  XrdHttpTpcUtils::PrepareOpenURLParams parms {req.resource, req.headers, hdr2cgimap,req.mReprDigest};
  return XrdHttpTpcUtils::prepareOpenURL(parms);
}

bool TPCRHandler::mismatchReprDigest(const std::map<std::string, std::string> & passiveSrvReprDigest, XrdHttpExtReq &req,
                                    TPCLogRecord &rec) {
    if(passiveSrvReprDigest.size()) {
        for (const auto & [digestName, digestValue]: passiveSrvReprDigest) {
            auto clientDigestMatch = req.mReprDigest.find(digestName);
            if (clientDigestMatch != req.mReprDigest.end()) {
                // We found a checksum type match between the client-provided one and the source server-provided one
                if (clientDigestMatch->second != digestValue) {
                    // The checksum value does not match, return an error to the client 412 PRECONDITION_FAILED
                    std::stringstream errMsg;
                    errMsg << "Mismatch between client-provided and remote server checksums:"
                           << " client = (" << clientDigestMatch->first << "=" <<  clientDigestMatch->second << ")"
                           << " server = (" << digestName << "=" << digestValue << ")";
                    logTransferEvent(LogMask::Error, rec, "REPRDIGEST_VERIFY_FAIL", errMsg.str());
                    rec.status=412;
                    req.SendSimpleResp(rec.status, NULL, NULL, generateClientErr(errMsg, rec, CURLcode::CURLE_OK).c_str(), 0);
                    return true;
                }
            }
        }
    }
    return false;
}

/******************************************************************************/
/*           e n c o d e _ x r o o t d _ o p a q u e _ t o _ u r i            */
/******************************************************************************/
  
// When processing a redirection from the filesystem layer, it is permitted to return
// some xrootd opaque data.  The quoting rules for xrootd opaque data are significantly
// more permissive than a URI (basically, only '&' and '=' are disallowed while some
// URI parsers may dislike characters like '"').  This function takes an opaque string
// (e.g., foo=1&bar=2&baz=") and makes it safe for all URI parsers.
std::string encode_xrootd_opaque_to_uri(CURL *curl, const std::string &opaque)
{
    std::stringstream parser(opaque);
    std::string sequence;
    std::stringstream output;
    bool first = true;
    while (getline(parser, sequence, '&')) {
        if (sequence.empty()) {continue;}
        size_t equal_pos = sequence.find('=');
        char *val = NULL;
        if (equal_pos != std::string::npos)
            val = curl_easy_escape(curl, sequence.c_str() + equal_pos + 1, sequence.size()  - equal_pos - 1);
        // Do not emit parameter if value exists and escaping failed.
        if (!val && equal_pos != std::string::npos) {continue;}

        if (!first) output << "&";
        first = false;
        output << sequence.substr(0, equal_pos);
        if (val) {
            output << "=" << val;
            curl_free(val);
        }
    }
    return output.str();
}

/******************************************************************************/
/*           T P C H a n d l e r : : C o n f i g u r e C u r l C A            */
/******************************************************************************/
  
bool
TPCRHandler::ConfigureCurlCA(CURL *curl, TPCLogRecord &rec)
{
    // Preferred path: hand libcurl the CA/CRL store that XrdTlsTempCA already
    // parsed, rather than the bundle filenames.  Passing filenames makes libcurl
    // build a private X509_STORE per connection, which costs tens of MB for a grid
    // CA directory and is held for the whole transfer; sharing one store makes that
    // a reference count.  See https://github.com/xrootd/xrootd/issues/2873
    //
    // Skipped when m_cafile is set, so that the http.cafile precedence established
    // at the bottom of this function is preserved.
    if (m_ca_file && m_sslctx_supported && m_cafile.empty()) {
        rec.ca_store = m_ca_file->CAStore();
        if (!rec.ca_store) {
            m_log.Log(Error, "TpcHandler", "No CA store is available; refusing to "
                      "fall back to libcurl's default CA bundle");
            return false;
        }
        // Stop libcurl loading its build-time default bundle, which the callback
        // below would only discard; the callback supplies the trust anchors.
        curl_easy_setopt(curl, CURLOPT_CAINFO, static_cast<char *>(nullptr));
        curl_easy_setopt(curl, CURLOPT_CAPATH, static_cast<char *>(nullptr));
        curl_easy_setopt(curl, CURLOPT_SSL_CTX_FUNCTION, ssl_ctx_callback);
        curl_easy_setopt(curl, CURLOPT_SSL_CTX_DATA, &rec);
        return true;
    }

    auto ca_filename = m_ca_file ? m_ca_file->CAFilename() : "";
    auto crl_filename = m_ca_file ? m_ca_file->CRLFilename() : "";
    if (!ca_filename.empty() && !crl_filename.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca_filename.c_str());
        //Check that the CRL file contains at least one entry before setting this option to curl
        //Indeed, an empty CRL file will make curl unhappy and therefore will fail
        //all HTTP TPC transfers (https://github.com/xrootd/xrootd/issues/1543)
        std::ifstream in(crl_filename, std::ifstream::ate | std::ifstream::binary);
        if(in.tellg() > 0 && m_ca_file->atLeastOneValidCRLFound()){
            curl_easy_setopt(curl, CURLOPT_CRLFILE, crl_filename.c_str());
            if (allowMissingCRL) {
                // No need to set the callback if there is no need to do it
                curl_easy_setopt(curl, CURLOPT_SSL_CTX_FUNCTION, ssl_ctx_callback);
            }
        } else {
            std::ostringstream oss;
            oss << "No valid CRL file has been found in the file " << crl_filename << ". Disabling CRL checking.";
            m_log.Log(Warning,"TpcHandler",oss.str().c_str());
        }
    }
    else if (!m_cadir.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAPATH, m_cadir.c_str());
    }
    if (!m_cafile.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, m_cafile.c_str());
    }
    return true;
}

void
TPCRHandler::ConfigureCurlLowSpeed(CURL *curl)
{
    // Older versions have poor transfer performance when low-speed limits are
    // enabled; this was corrected in curl commit cacdc27f for version 7.38.0.
    curl_version_info_data *curl_ver = curl_version_info(CURLVERSION_NOW);
    if (m_low_speed_limit > 0 && curl_ver && curl_ver->age > 0 &&
        curl_ver->version_num >= 0x072600) {
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, m_low_speed_time);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, m_low_speed_limit);
    }
}


bool TPCRHandler::MatchesPath(const char *verb, const char *path) {
    return !strcmp(verb, "COPY") || !strcmp(verb, "OPTIONS");
}

/******************************************************************************/
/*                            P r e p a r e U R L                             */
/******************************************************************************/
  
static std::string PrepareURL(const std::string &url)
{
  const std::string replace_schemes[] = { "davs://", "s3://", "s3s://" };

  for (const auto& s : replace_schemes)
    if (url.compare(0, s.size(), s) == 0)
      return "https://" + url.substr(s.size());

  return url;
}

static bool IsAllowedScheme(const std::string& url)
{
  const std::string allowed_schemes[] = { "https://", "http://" };

  for (const auto& s : allowed_schemes)
    if (url.compare(0, s.size(), s) == 0)
      return true;

  return false;
}

/******************************************************************************/
/*                T P C H a n d l e r : : P r o c e s s R e q                 */
/******************************************************************************/
  
int TPCRHandler::ProcessReq(XrdHttpExtReq &req) {
    if (req.verb == "OPTIONS") {
        return ProcessOptionsReq(req);
    }
    auto header = XrdOucTUtils::caseInsensitiveFind(req.headers,"credential");
    if (header != req.headers.end()) {
        if (header->second != "none") {
            m_log.Emsg("ProcessReq", "COPY requested an unsupported credential type: ", header->second.c_str());
            return req.SendSimpleResp(400, NULL, NULL, "COPY requestd an unsupported Credential type", 0);
        }
    }
    header = XrdOucTUtils::caseInsensitiveFind(req.headers,"source");
    if (header != req.headers.end()) {
        std::string src = PrepareURL(header->second);
        if (!IsAllowedScheme(src)) {
            const char *error_src = "COPY rejected: disallowed scheme in source URL";
            m_log.Emsg("ProcessReq", error_src, src.c_str());
            return req.SendSimpleResp(400, NULL, NULL, error_src, 0);
        }
        return ProcessPullReq(src, req);
    }
    header = XrdOucTUtils::caseInsensitiveFind(req.headers,"destination");
    if (header != req.headers.end()) {
        const std::string& dst = header->second;
        if (!IsAllowedScheme(dst)) {
            const char *error_dst = "COPY rejected: disallowed scheme in destination URL";
            m_log.Emsg("ProcessReq", error_dst, dst.c_str());
            return req.SendSimpleResp(400, NULL, NULL, error_dst, 0);
        }
        return ProcessPushReq(header->second, req);
    }
    m_log.Emsg("ProcessReq", "COPY verb requested but no source or destination specified.");
    return req.SendSimpleResp(400, NULL, NULL, "No Source or Destination specified", 0);
}

/******************************************************************************/
/*                 T P C H a n d l e r   D e s t r u c t o r                  */
/******************************************************************************/
  
TPCRHandler::~TPCRHandler() {
    m_sfs = NULL;
}

/******************************************************************************/
/*                T P C H a n d l e r   C o n s t r u c t o r                 */
/******************************************************************************/
  
TPCRHandler::TPCRHandler(XrdSysError *log, const char *config, XrdOucEnv *myEnv) :
        m_allow_local(false),
        m_allow_private(true),
        m_desthttps(false),
        m_fixed_route(false),
        m_low_speed_limit(10*1024),
        m_low_speed_time(2*60),
        m_timeout(60),
        m_first_timeout(120),
        m_log(log->logger(), "TPC_"),
        m_sfs(NULL)
{
    if (!Configure(config, myEnv)) {
        throw std::runtime_error("Failed to configure the HTTP third-party-copy handler.");
    }

// Extract out the TPC monitoring object (we share it with xrootd).
//
   XrdXrootdGStream *gs = (XrdXrootdGStream*)myEnv->GetPtr("Tpc.gStream*");
   if (gs)
      TPCLogRecord::tpcMonitor = new XrdXrootdTpcMon("http",log->logger(),*gs);
}

/******************************************************************************/
/*         T P C H a n d l e r : : P r o c e s s O p t i o n s R e q          */
/******************************************************************************/
  
/**
 * Handle the OPTIONS verb as we have added a new one...
 */
int TPCRHandler::ProcessOptionsReq(XrdHttpExtReq &req) {
    // FR-3: advertise the resume capability so orchestrators that care can
    // reason about it; nothing on the wire depends on them doing so.  The
    // header is only offered when resume is actually enabled.
    if (m_tpcr.resume) {
        return req.SendSimpleResp(200, NULL, (char *)
            "DAV: 1\r\nDAV: <http://apache.org/dav/propset/fs/1>\r\n"
            "Allow: HEAD,GET,PUT,PROPFIND,DELETE,OPTIONS,COPY\r\n"
            "X-Transfer-Capabilities: resume/1", NULL, 0);
    }
    return req.SendSimpleResp(200, NULL, (char *) "DAV: 1\r\nDAV: <http://apache.org/dav/propset/fs/1>\r\nAllow: HEAD,GET,PUT,PROPFIND,DELETE,OPTIONS,COPY", NULL, 0);
}

/******************************************************************************/
/*                  T P C H a n d l e r : : G e t A u t h z                   */
/******************************************************************************/
  
std::string TPCRHandler::GetAuthz(XrdHttpExtReq &req) {
    std::string authz;
    auto authz_header = XrdOucTUtils::caseInsensitiveFind(req.headers,"authorization");
    if (authz_header != req.headers.end()) {
        std::stringstream ss;
        ss << "authz=" << encode_str(authz_header->second);
        authz += ss.str();
    }
    return authz;
}

/******************************************************************************/
/*          T P C H a n d l e r : : R e d i r e c t T r a n s f e r           */
/******************************************************************************/
  
int TPCRHandler::RedirectTransfer(CURL *curl, const std::string &redirect_resource,
    XrdHttpExtReq &req, XrdOucErrInfo &error, TPCLogRecord &rec)
{
    int port;
    const char *ptr = error.getErrText(port);
    if ((ptr == NULL) || (*ptr == '\0') || (port == 0)) {
        rec.status = 500;
        std::stringstream ss;
        ss << "Internal error: redirect without hostname";
        logTransferEvent(LogMask::Error, rec, "REDIRECT_INTERNAL_ERROR", ss.str());
        return req.SendSimpleResp(rec.status, NULL, NULL, generateClientErr(ss, rec).c_str(), 0);
    }

    // The XrdSfs layer hands back the redirect target in host[?cgi] form; the
    // port arrived separately via getErrText() above.  Default to that target
    // and let the redirect plugin block below override it if it rewrites it.
    std::string finalTarget = ptr;

    // When a redirect plugin (XrdXrootdRedirPI) is configured, run the COPY
    // redirect target through it so the same plugin-driven routing applies
    // as in the XRootD protocol's fsRedirPI().  The plugin may rewrite host,
    // port, and CGI; on a fatal plugin error surface a 500 with the plugin's
    // message rather than emit a redirect we know is wrong.  See issue #2767.
    if (XrdNetAddrInfo *clientAddr = req.GetSecEntity().addrInfo;
        XrdXrootdRedirHelper::IsActive() && clientAddr) {
        // Redirect() takes the host[?cgi] target as a single string and splits
        // it itself; the non-negative port selects its host+port form.
        int         newPort = port;
        std::string newTarget;
        std::string errMsg;
        auto outcome = XrdXrootdRedirHelper::Redirect(ptr, newPort, *clientAddr,
                                                      newTarget, errMsg);
        if (outcome == XrdXrootdRedirHelper::Outcome::Replaced) {
            finalTarget = std::move(newTarget);
            port        = newPort;
            logTransferEvent(LogMask::Info, rec, "REDIRECT_PLUGIN_REWRITE",
                             finalTarget);
        } else if (outcome == XrdXrootdRedirHelper::Outcome::Error) {
            rec.status = 500;
            std::stringstream ess;
            ess << "Redirect plugin error: " << errMsg;
            logTransferEvent(LogMask::Error, rec, "REDIRECT_PLUGIN_ERROR",
                             ess.str());
            return req.SendSimpleResp(rec.status, nullptr, nullptr,
                                      generateClientErr(ess, rec).c_str(), 0);
        }
        // Outcome::Unchanged: keep the original target.
    }

    // Split the (possibly plugin-rewritten) host[?cgi] target: the host goes
    // into the Location authority, the cgi into its query string.  splitHostCgi
    // keeps the leading '?' on cgi; the Location builder below wants the bare
    // opaque body, so drop that '?' here.
    std::string host;
    std::string cgi;
    splitHostCgi(finalTarget, host, cgi);
    std::string opaque = cgi.empty() ? std::string() : cgi.substr(1);

    std::stringstream ss;
    ss << "Location: http" << (m_desthttps ? "s" : "") << "://" << host << ":" << port << "/" << redirect_resource;

    if (!opaque.empty()) {
      // redirect_resource (sourced from xrd-http-fullresource) may already
      // carry the client's query string, so pick the separator accordingly
      // to avoid emitting a malformed URL with two '?'.
      char sep = (redirect_resource.find('?') == std::string::npos) ? '?' : '&';
      ss << sep << encode_xrootd_opaque_to_uri(curl, opaque);
    }

    rec.status = 307;
    logTransferEvent(LogMask::Info, rec, "REDIRECT", ss.str());
    return req.SendSimpleResp(rec.status, NULL, const_cast<char *>(ss.str().c_str()),
        NULL, 0);
}

/******************************************************************************/
/*             T P C H a n d l e r : : O p e n W a i t S t a l l              */
/******************************************************************************/
  
int TPCRHandler::OpenWaitStall(XrdSfsFile &fh, const std::string &resource,
                      int mode, int openMode, const XrdSecEntity &sec,
                      const std::string &authz)
{
    int open_result;
    while (1) {
        int orig_ucap = fh.error.getUCap();
        fh.error.setUCap(orig_ucap | XrdOucEI::uIPv64);
        std::string opaque;
        size_t pos = resource.find('?');
        // Extract the path and opaque info from the resource
        std::string path = resource.substr(0, pos);

        if (pos != std::string::npos) {
          opaque = resource.substr(pos + 1);
        }

        // Append the authz information if there are some
        if(!authz.empty()) {
            opaque += (opaque.empty() ? "" : "&");
            opaque += authz;
        }
        open_result = fh.open(path.c_str(), mode, openMode, &sec, opaque.c_str());

        if ((open_result == SFS_STALL) || (open_result == SFS_STARTED)) {
            int secs_to_stall = fh.error.getErrInfo();
            if (open_result == SFS_STARTED) {secs_to_stall = secs_to_stall/2 + 5;}
            std::this_thread::sleep_for (std::chrono::seconds(secs_to_stall));
        }
        break;
    }
    return open_result;
}

/******************************************************************************/
/*         T P C H a n d l e r : : D e t e r m i n e X f e r S i z e          */
/******************************************************************************/



/**
 * Determine size at remote end.
 */
int TPCRHandler::PerformHEADRequest(CURL *curl, XrdHttpExtReq &req, State &state,
                                   bool &success, TPCLogRecord &rec, bool shouldReturnErrorToClient) {
    success = false;
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1);
    // Set a custom timeout of 60 seconds (= CONNECT_TIMEOUT for convenience) for the HEAD request
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, CONNECT_TIMEOUT);
    CURLcode res;
    res = curl_easy_perform(curl);
    //Immediately set the CURLOPT_NOBODY flag to 0 as we anyway
    //don't want the next curl call to do be a HEAD request
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0);
    // Reset the CURLOPT_TIMEOUT to no timeout (default)
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, true);

    std::stringstream ss;

    if (state.GetStatusCode() >= 400)
      res = CURLE_HTTP_RETURNED_ERROR;

    if (res != CURLE_OK) { /* curl failed */
      ss << curl_easy_strerror(res);
      switch (res) {
      case CURLE_HTTP_RETURNED_ERROR: /* remote side may have returned an error */
        rec.tpc_status = state.GetStatusCode(); /* relay status received from remote side to the client */
        ss << ": remote host returned '" << rec.tpc_status << " "
           << httpStatusToString(rec.tpc_status) << "' while fetching file size";
        break;
      case CURLE_COULDNT_CONNECT: /* socket callback may have failed */
        switch (rec.tpc_status) {
        case 403:
          ss << ": connection to local/private addresses is forbidden";
          break;
        default:
          ss << ": internal server failure";
          rec.tpc_status = 500;
        }
        break;
      default:
        rec.tpc_status = 500;
        state.SetErrorCode(500);
      }
    }

    if (rec.tpc_status >= 400) {
      logTransferEvent(LogMask::Error, rec, "HEAD_FAIL", ss.str());
      return shouldReturnErrorToClient ? req.SendSimpleResp(rec.tpc_status, NULL, NULL, generateClientErr(ss, rec, res).c_str(), 0) : -1;
    }

    success = true;
    // The size a HEAD reports is response data: read the reported length
    // (the "expected" length field does not exist for a bodyless probe).
    ss << "Successfully determined remote file information for pull request: "
       << "size=" << state.GetReportedLength();
    if(state.GetReprDigest().size()) {
      unsigned int cksumIndex = 1;
      for(const auto & [cksumType,cksumValue]: state.GetReprDigest()) {
        ss << " chksum" << cksumIndex << "=(" << cksumType << "," << cksumValue << ")";
        cksumIndex++;
      }
    }
    logTransferEvent(LogMask::Debug, rec, "HEAD_SUCCESS", ss.str());
    return 0;
}

int TPCRHandler::GetRemoteFileInfoTPCPull(CURL *curl, XrdHttpExtReq &req, uint64_t &contentLength, std::map<std::string,std::string> & reprDigest, bool & success, TPCLogRecord &rec, TPCR::SourceValidators *validators) {
    State state(curl,req.tpcForwardCreds);
    //Don't forget to copy the headers of the client's request before doing the HEAD call. Otherwise, if there is a need for authentication,
    //it will fail
    state.SetupHeadersForHEAD(req);
    int result;
    //In case we cannot get the file HEAD request, we return the error to the client
    if ((result = PerformHEADRequest(curl, req, state, success, rec)) || !success) {
        return result;
    }
    contentLength = state.GetReportedLength();
    reprDigest = state.GetReprDigest();
    if (validators) {
        // SUB-7: the session baseline the degraded-state re-probe (FR-14)
        // revalidates against, and the journal persists (FR-21, WP-8).
        validators->content_length = state.GetReportedLength();
        validators->etag = state.GetETag();
        validators->last_modified = state.GetLastModified();
        validators->repr_digests = state.GetReprDigest();
    }
    return result;
}

// Mid-session re-probe (FR-14/SUB-7): a dedicated easy handle so the transfer
// states and the multi handle are untouched.  Any transport or HTTP failure
// simply returns false -- the degraded loop keeps riding out the outage.
bool TPCRHandler::ProbeSourceValidators(XrdHttpExtReq &req, TPCLogRecord &rec,
                                        const std::string &resource_url,
                                        const std::string &interface_ip,
                                        TPCR::SourceValidators &fresh)
{
    ManagedCurlHandle probe(curl_easy_init());
    if (!probe) {return false;}
    State head_state(probe.get(), req.tpcForwardCreds);
    if (!ConfigureHandle(probe.get(), head_state, rec, resource_url,
                         interface_ip)) {
        return false;
    }
    head_state.SetupHeadersForHEAD(req);
    bool success = false;
    // PerformHEADRequest treats rec.tpc_status >= 400 as "this HEAD failed",
    // but the field is sticky on the log record: a previous failed probe's
    // status would veto a now-successful one.  (Latent in stock, where the
    // HEAD ran once per transfer; fatal for re-probing.)  Clear it for the
    // duration of the probe; a genuine failure sets it again.
    const int prior_tpc_status = rec.tpc_status;
    rec.tpc_status = -1;
    // shouldReturnErrorToClient=false: the client already holds the 202
    // chunked stream; errors here are recovery-internal.
    PerformHEADRequest(probe.get(), req, head_state, success, rec,
                       /*shouldReturnErrorToClient=*/false);
    if (!success) {
        // Keep the freshest meaningful status for monitoring.
        if (rec.tpc_status < 400 && prior_tpc_status >= 400) {
            rec.tpc_status = prior_tpc_status;
        }
        return false;
    }
    fresh.content_length = head_state.GetReportedLength();
    fresh.etag = head_state.GetETag();
    fresh.last_modified = head_state.GetLastModified();
    fresh.repr_digests = head_state.GetReprDigest();
    return true;
}
  
/******************************************************************************/
/*            T P C H a n d l e r : : S e n d P e r f M a r k e r             */
/******************************************************************************/
  
int TPCRHandler::SendPerfMarker(XrdHttpExtReq &req, TPCLogRecord &rec, TPCR::State &state) {
    std::stringstream ss;
    const std::string crlf = "\n";
    ss << "Perf Marker" << crlf;
    ss << "Timestamp: " << time(NULL) << crlf;
    ss << "Stripe Index: 0" << crlf;
    ss << "Stripe Bytes Transferred: " << state.BytesTransferred() << crlf;
    ss << "Total Stripe Count: 1" << crlf;
    // Include the TCP connection associated with this transfer; used by
    // the TPC client for monitoring purposes.
    std::string desc = state.GetConnectionDescription();
    if (!desc.empty())
        ss << "RemoteConnections: " << desc << crlf;
    ss << "End" << crlf;
    rec.bytes_transferred = state.BytesTransferred();
    logTransferEvent(LogMask::Debug, rec, "PERF_MARKER");

    return req.ChunkResp(ss.str().c_str(), 0);
}

/******************************************************************************/
/*            T P C H a n d l e r : : S e n d P e r f M a r k e r             */
/******************************************************************************/
  
int TPCRHandler::SendPerfMarker(XrdHttpExtReq &req, TPCLogRecord &rec, std::vector<State*> &state,
    off_t bytes_transferred)
{
    // The 'performance marker' format is largely derived from how GridFTP works
    // (e.g., the concept of `Stripe` is not quite so relevant here).  See:
    //    https://twiki.cern.ch/twiki/bin/view/LCG/HttpTpcTechnical
    // Example marker:
    //    Perf Marker\n
    //    Timestamp: 1537788010\n
    //    Stripe Index: 0\n
    //    Stripe Bytes Transferred: 238745\n
    //    Total Stripe Count: 1\n
    //    RemoteConnections: tcp:129.93.3.4:1234,tcp:[2600:900:6:1301:268a:7ff:fef6:a590]:2345\n
    //    End\n
    //
    std::stringstream ss;
    const std::string crlf = "\n";
    ss << "Perf Marker" << crlf;
    ss << "Timestamp: " << time(NULL) << crlf;
    ss << "Stripe Index: 0" << crlf;
    ss << "Stripe Bytes Transferred: " << bytes_transferred << crlf;
    ss << "Total Stripe Count: 1" << crlf;
    // Build a list of TCP connections associated with this transfer; used by
    // the TPC client for monitoring purposes.
    bool first = true;
    std::stringstream ss2;
    for (std::vector<State*>::const_iterator iter = state.begin();
        iter != state.end(); iter++)
    {
        std::string desc = (*iter)->GetConnectionDescription();
        if (!desc.empty()) {
            ss2 << (first ? "" : ",") << desc;
            first = false;
        }
    }
    if (!first)
        ss << "RemoteConnections: " << ss2.str() << crlf;
    ss << "End" << crlf;
    rec.bytes_transferred = bytes_transferred;
    logTransferEvent(LogMask::Debug, rec, "PERF_MARKER");

    return req.ChunkResp(ss.str().c_str(), 0);
}

/******************************************************************************/
/*        T P C H a n d l e r : : R u n C u r l W i t h U p d a t e s         */
/******************************************************************************/
  
int TPCRHandler::RunCurlWithUpdates(CURL *curl, XrdHttpExtReq &req, State &state,
    TPCLogRecord &rec)
{
    // Create the multi-handle and add in the current transfer to it.
    CURLM *multi_handle = curl_multi_init();
    if (!multi_handle) {
        rec.status = 500;
        logTransferEvent(LogMask::Error, rec, "CURL_INIT_FAIL",
            "Failed to initialize a libcurl multi-handle");
        std::stringstream ss;
        ss << "Failed to initialize internal server memory";
        return req.SendSimpleResp(rec.status, NULL, NULL, generateClientErr(ss, rec).c_str(), 0);
    }

    //curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 128*1024);

    CURLMcode mres;
    mres = curl_multi_add_handle(multi_handle, curl);
    if (mres) {
        rec.status = 500;
        std::stringstream ss;
        ss << "Failed to add transfer to libcurl multi-handle: HTTP library failure=" << curl_multi_strerror(mres);
        logTransferEvent(LogMask::Error, rec, "CURL_INIT_FAIL", ss.str());
        curl_multi_cleanup(multi_handle);
        return req.SendSimpleResp(rec.status, NULL, NULL, generateClientErr(ss, rec).c_str(), 0);
    }

    // Start response to client prior to the first call to curl_multi_perform
    int retval = req.StartChunkedResp(202, NULL, "Content-Type: text/plain");
    if (retval) {
        curl_multi_cleanup(multi_handle);
        logTransferEvent(LogMask::Error, rec, "RESPONSE_FAIL",
            "Failed to send the initial response to the TPC client");
        return retval;
    } else {
        logTransferEvent(LogMask::Debug, rec, "RESPONSE_START",
            "Initial transfer response sent to the TPC client");
    }

    // Transfer loop: use curl to actually run the transfer, but periodically
    // interrupt things to send back performance updates to the client.
    int running_handles = 1;
    time_t last_marker = 0;
    // Track how long it's been since the last time we recorded more bytes being transferred.
    off_t last_advance_bytes = 0;
    time_t last_advance_time = time(NULL);
    time_t transfer_start = last_advance_time;
    CURLcode res = static_cast<CURLcode>(-1);
    do {
        time_t now = time(NULL);
        time_t next_marker = last_marker + m_marker_period;
        if (now >= next_marker) {
            off_t bytes_xfer = state.BytesTransferred();
            if (bytes_xfer > last_advance_bytes) {
                last_advance_bytes = bytes_xfer;
                last_advance_time = now;
            }
            if (SendPerfMarker(req, rec, state)) {
                curl_multi_remove_handle(multi_handle, curl);
                curl_multi_cleanup(multi_handle);
                logTransferEvent(LogMask::Error, rec, "PERFMARKER_FAIL",
                    "Failed to send a perf marker to the TPC client");
                return -1;
            }
            int timeout = (transfer_start == last_advance_time) ? m_first_timeout : m_timeout;
            if (now > last_advance_time + timeout) {
                const char *log_prefix = rec.log_prefix.c_str();
                bool tpc_pull = strncmp("Pull", log_prefix, 4) == 0;

                state.SetErrorCode(State::errTimeout);
                std::stringstream ss;
                ss << "Transfer failed because no bytes have been "
                   << (tpc_pull ? "received from the source (pull mode) in "
                                : "transmitted to the destination (push mode) in ") << timeout << " seconds.";
                state.SetErrorMessage(ss.str());
                curl_multi_remove_handle(multi_handle, curl);
                curl_multi_cleanup(multi_handle);
                break;
            }
            last_marker = now;
        }
        // The transfer will start after this point, notify the packet marking manager
        rec.pmarkManager.startTransfer();
        mres = curl_multi_perform(multi_handle, &running_handles);
        if (mres == CURLM_CALL_MULTI_PERFORM) {
            // curl_multi_perform should be called again immediately.  On newer
            // versions of curl, this is no longer used.
            continue;
        } else if (mres != CURLM_OK) {
            break;
        } else if (running_handles == 0) {
            break;
        }

        rec.pmarkManager.beginPMarks();
        //printf("There are %d running handles\n", running_handles);

        // Harvest any messages, looking for CURLMSG_DONE.
        CURLMsg *msg;
        do {
            int msgq = 0;
            msg = curl_multi_info_read(multi_handle, &msgq);
            if (msg && (msg->msg == CURLMSG_DONE)) {
                CURL *easy_handle = msg->easy_handle;
                res = msg->data.result;
                curl_multi_remove_handle(multi_handle, easy_handle);
            }
        } while (msg);

        int64_t max_sleep_time = next_marker - time(NULL);
        if (max_sleep_time <= 0) {
            continue;
        }
        mres = MultiWait(multi_handle, max_sleep_time*1000);
        if (mres != CURLM_OK) {
            break;
        }
    } while (running_handles);

    if (mres != CURLM_OK) {
        std::stringstream ss;
        ss << "Internal libcurl multi-handle error: HTTP library failure=" << curl_multi_strerror(mres);
        logTransferEvent(LogMask::Error, rec, "TRANSFER_CURL_ERROR", ss.str());

        curl_multi_remove_handle(multi_handle, curl);
        curl_multi_cleanup(multi_handle);

        if ((retval = req.ChunkResp(generateClientErr(ss, rec).c_str(), 0))) {
            logTransferEvent(LogMask::Error, rec, "RESPONSE_FAIL",
                "Failed to send error message to the TPC client");
            return retval;
        }
        return req.ChunkResp(NULL, 0);
    }

    // Harvest any messages, looking for CURLMSG_DONE.
    CURLMsg *msg;
    do {
        int msgq = 0;
        msg = curl_multi_info_read(multi_handle, &msgq);
        if (msg && (msg->msg == CURLMSG_DONE)) {
            CURL *easy_handle = msg->easy_handle;
            res = msg->data.result;
            curl_multi_remove_handle(multi_handle, easy_handle);
        }
    } while (msg);

    if (!state.GetErrorCode() && res == static_cast<CURLcode>(-1)) { // No transfers returned?!?
        curl_multi_remove_handle(multi_handle, curl);
        curl_multi_cleanup(multi_handle);
        std::stringstream ss;
        ss << "Internal state error in libcurl";
        logTransferEvent(LogMask::Error, rec, "TRANSFER_CURL_ERROR", ss.str());

        if ((retval = req.ChunkResp(generateClientErr(ss, rec).c_str(), 0))) {
            logTransferEvent(LogMask::Error, rec, "RESPONSE_FAIL",
                "Failed to send error message to the TPC client");
            return retval;
        }
        return req.ChunkResp(NULL, 0);
    }
    curl_multi_cleanup(multi_handle);

    // The transfer is over at this point: any error recorded so far - a failed
    // write to the local filesystem or the stall detector having fired - is the
    // reason why the transfer failed.  Flushing and closing the destination file
    // below may fail as well but, as such a failure is usually a consequence of
    // the transfer failure, it must not be reported instead of it.
    const int transferErrorCode = state.GetErrorCode();
    std::string transferErrorMsg = state.GetErrorMessage();

    state.Flush();

    rec.bytes_transferred = state.BytesTransferred();
    rec.tpc_status = state.GetStatusCode();

    // Explicitly finalize the stream (which will close the underlying file
    // handle) before the response is sent.  In some cases, subsequent HTTP
    // requests can occur before the filesystem is done closing the handle -
    // and those requests may occur against partial data.
    state.Finalize();

    // A failure to flush or to close the destination file is always logged and is
    // appended to the error reported to the client, but it never replaces the
    // transfer failure itself: it is usually a consequence of it.
    std::string finalizeErrorMsg, finalizeErrorSuffix;
    if (state.GetFinalizeErrorCode()) {
        std::stringstream ss2;
        ss2 << (state.GetFinalizeErrorCode() == State::errFlush
                    ? "Failed to flush the file to the local filesystem."
                    : "Failed to finalize and close file handle.");
        std::string err = state.GetFinalizeErrorMessage();
        if (!err.empty()) {
            std::replace(err.begin(), err.end(), '\n', ' ');
            ss2 << " " << err;
        }
        finalizeErrorMsg = ss2.str();
        logTransferEvent(LogMask::Error, rec, "CLOSE_FAIL", finalizeErrorMsg);
        finalizeErrorSuffix = "; " + finalizeErrorMsg;
    }

    // Generate the final response back to the client.
    std::stringstream ss;
    bool success = false;
    if (state.GetStatusCode() >= 400) {
        std::string err = state.GetErrorMessage();
        std::stringstream ss2;
        ss2 << "Remote side failed with status code " << state.GetStatusCode();
        if (!err.empty()) {
            std::replace(err.begin(), err.end(), '\n', ' ');
            ss2 << "; error message: \"" << err << "\"";
        }
        logTransferEvent(LogMask::Error, rec, "TRANSFER_FAIL", ss2.str());
        ss2 << finalizeErrorSuffix;
        ss << generateClientErr(ss2, rec);
    } else if (transferErrorCode == State::errTimeout) {
        // The stall detector fired; its message already describes precisely
        // what happened, report it as-is.
        std::stringstream ss2;
        ss2 << transferErrorMsg;
        logTransferEvent(LogMask::Error, rec, "TRANSFER_FAIL", ss2.str());
        ss2 << finalizeErrorSuffix;
        ss << generateClientErr(ss2, rec);
    } else if (transferErrorCode) {
        if (transferErrorMsg.empty()) {transferErrorMsg = "(no error message provided)";}
        else {std::replace(transferErrorMsg.begin(), transferErrorMsg.end(), '\n', ' ');}
        std::stringstream ss2;
        ss2 << "Error when interacting with local filesystem: " << transferErrorMsg;
        logTransferEvent(LogMask::Error, rec, "TRANSFER_FAIL", ss2.str());
        ss2 << finalizeErrorSuffix;
        ss << generateClientErr(ss2, rec);
    } else if (res != CURLE_OK) {
        std::stringstream ss2;
        ss2 << "Internal transfer failure";
        std::stringstream ss3;
        ss3 << ss2.str() << ": " << curl_easy_strerror(res);
        logTransferEvent(LogMask::Error, rec, "TRANSFER_FAIL", ss3.str());
        ss2 << finalizeErrorSuffix;
        ss << generateClientErr(ss2, rec, res);
    } else if (!finalizeErrorMsg.empty()) {
        // Nothing else went wrong: the flush/close failure is the reason of the failure.
        std::stringstream ss2;
        ss2 << finalizeErrorMsg;
        ss << generateClientErr(ss2, rec);
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

/******************************************************************************/
/*            T P C H a n d l e r : : P r o c e s s P u s h R e q             */
/******************************************************************************/
  
int TPCRHandler::ProcessPushReq(const std::string & resource, XrdHttpExtReq &req) {
    TPCLogRecord rec(req, TpcType::Push);
    rec.allow_local = m_allow_local;
    rec.allow_private = m_allow_private;
    rec.log_prefix = "PushRequest";
    rec.local = req.resource;
    rec.remote = resource;
    rec.m_log = &m_log;
    char *name = req.GetSecEntity().name;
    req.GetClientID(rec.clID);
    if (name) rec.name = name;
    logTransferEvent(LogMask::Info, rec, "PUSH_START", "Starting a push request");

    ManagedCurlHandle curlPtr(curl_easy_init());
    auto curl = curlPtr.get();
    if (!curl) {
        std::stringstream ss;
        ss << "Failed to initialize internal transfer resources";
        rec.status = 500;
        logTransferEvent(LogMask::Error, rec, "PUSH_FAIL", ss.str());
        return req.SendSimpleResp(rec.status, NULL, NULL, generateClientErr(ss, rec).c_str(), 0);
    }
    ConfigureCurlLowSpeed(curl);
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
    curl_easy_setopt(curl, CURLOPT_CLOSESOCKETFUNCTION, closesocket_callback);
    curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, sockopt_callback);
    curl_easy_setopt(curl, CURLOPT_CLOSESOCKETDATA, &rec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT);

    auto query_header = XrdOucTUtils::caseInsensitiveFind(req.headers,"xrd-http-fullresource");
    std::string redirect_resource = req.resource;
    if (query_header != req.headers.end()) {
        redirect_resource = query_header->second;
    }

    AtomicBeg(m_monid_mutex);
    uint64_t file_monid = AtomicInc(m_monid);
    AtomicEnd(m_monid_mutex);
    std::unique_ptr<XrdSfsFile> fh(m_sfs->newFile(name, file_monid));
    if (!fh.get()) {
        rec.status = 500;
        std::stringstream ss;
        ss <<  "Failed to initialize internal transfer file handle";
        logTransferEvent(LogMask::Error, rec, "OPEN_FAIL",
                         ss.str());
        return req.SendSimpleResp(rec.status, NULL, NULL, generateClientErr(ss, rec).c_str(), 0);
    }
    std::string full_url = prepareURL(req);

    std::string authz = GetAuthz(req);

    int open_results = OpenWaitStall(*fh, full_url, SFS_O_RDONLY, 0644,
                                     req.GetSecEntity(), authz);
    if (SFS_REDIRECT == open_results) {
        int result = RedirectTransfer(curl, redirect_resource, req, fh->error, rec);
        return result;
    } else if (SFS_OK != open_results) {
        int code;
        std::stringstream ss;
        const char *msg = fh->error.getErrText(code);
        if (msg == NULL) ss << "Failed to open local resource";
        else ss << msg;
        rec.status = mapErrNoToHttp(code);
        logTransferEvent(LogMask::Error, rec, "OPEN_FAIL", msg);
        int resp_result = req.SendSimpleResp(rec.status, NULL, NULL, generateClientErr(ss, rec).c_str(), 0);
        fh->close();
        return resp_result;
    }
    if (!ConfigureCurlCA(curl, rec)) {
        std::stringstream ss;
        ss << "Failed to configure the certificate authorities for the transfer";
        rec.status = 500;
        logTransferEvent(LogMask::Error, rec, "PUSH_FAIL", ss.str());
        int resp_result = req.SendSimpleResp(rec.status, NULL, NULL, generateClientErr(ss, rec).c_str(), 0);
        fh->close();
        return resp_result;
    }
    curl_easy_setopt(curl, CURLOPT_URL, resource.c_str());

    // Push mode reads from the local file; the reorder buffering is unused
    // (buffer_size 0) and push never resumes (initial offset 0, CON-6).
    Stream stream(std::move(fh), 0, 0, m_log);
    State state(0, stream, curl, true, req.tpcForwardCreds);
    state.SetupHeaders(req);

    return RunCurlWithUpdates(curl, req, state, rec);
}

/******************************************************************************/
/*            T P C H a n d l e r : : P r o c e s s P u l l R e q             */
/******************************************************************************/
  
bool TPCRHandler::VerifyResumeTail(const std::string &dest_path,
                                   const TPCR::JournalRecord &journal,
                                   const XrdSecEntity *client,
                                   uint64_t tail_bytes, std::string &reason)
{
    // Epochs overlapping [W - tail, W), each verified over its FULL interval
    // (a partial interval cannot be checked against its digest).  Cost is
    // bounded by tail + one checkpoint interval.
    const off_t watermark = journal.committed;
    const off_t tail_start =
        watermark > off_t(tail_bytes) ? watermark - off_t(tail_bytes) : 0;

    // A dedicated RDONLY handle: the resume write-open happens later and
    // carries side effects; this read has none (XRD-6 stays intact).
    std::unique_ptr<XrdSfsFile> file(m_sfs->newFile());
    if (!file || file->open(dest_path.c_str(), SFS_O_RDONLY, 0, client, "") !=
                     SFS_OK) {
        reason = "could not open the partial for tail verification";
        return false;
    }

    std::vector<char> buffer(4 * 1024 * 1024);
    size_t epochs_checked = 0;
    for (const auto &epoch : journal.epochs) {
        const off_t epoch_end = off_t(epoch.offset) + off_t(epoch.length);
        if (epoch_end <= tail_start) {continue;}       // below the tail span
        if (epoch_end > watermark || epoch.length == 0) {
            // Defensive (SUB-9): an epoch past W can only come from a
            // forged or corrupt record the CRC happened to bless.
            reason = "journal epoch extends past the watermark";
            file->close();
            return false;
        }
        uint32_t crc = 0;
        off_t cursor = off_t(epoch.offset);
        off_t remaining = off_t(epoch.length);
        while (remaining > 0) {
            const XrdSfsXferSize want = XrdSfsXferSize(
                std::min<off_t>(remaining, off_t(buffer.size())));
            const XrdSfsXferSize got = file->read(cursor, buffer.data(), want);
            if (got != want) {
                reason = "short read during tail verification";
                file->close();
                return false;
            }
            crc = XrdOucCRC::Calc32C(buffer.data(), size_t(got), crc);
            cursor += got;
            remaining -= got;
        }
        if (crc != epoch.crc32c) {
            std::stringstream ss;
            ss << "epoch [" << epoch.offset << ", " << epoch_end
               << ") CRC mismatch: destination data does not match what the "
               << "journal attested";
            reason = ss.str();
            file->close();
            return false;
        }
        epochs_checked++;
    }
    file->close();
    if (m_log.getMsgMask() & LogMask::Debug) {
        std::stringstream ss;
        ss << "tail verification passed: " << epochs_checked
           << " epoch(s) over [" << tail_start << ", " << watermark << ")";
        m_log.Emsg("TailVerify", ss.str().c_str());
    }
    return true;
}

void TPCRHandler::InjectChecksum(const std::string &dest_path,
                                 const std::string &adler_hex,
                                 const XrdSecEntity *client, TPCLogRecord &rec)
{
    // XRD-2 mandatory ordering, enforced by the call site: the destination
    // is CLOSED before we stat, so the mtime we bind into the attribute is
    // the settled one.  A stale-mtime attribute is a SILENT performance
    // failure (the checksum manager quietly recalculates terabytes), which
    // is why the negative test in T-I10 exists.
    struct stat settled;
    XrdOucErrInfo error;
    if (m_sfs->stat(dest_path.c_str(), &settled, error, client) != SFS_OK) {
        logTransferEvent(LogMask::Info, rec, "CKSUM_INJECT_SKIP",
                         "destination stat failed; checksum not injected");
        return;
    }
    std::vector<char> attr_data =
        XrdCksAttrData("adler32", adler_hex.c_str(), settled.st_mtime);
    if (attr_data.empty()) {
        logTransferEvent(LogMask::Info, rec, "CKSUM_INJECT_SKIP",
                         "could not build checksum attribute data");
        return;
    }
    std::string attr_name = XrdCksAttrName("adler32");

    XrdSfsFACtl ctl(dest_path.c_str(), nullptr, 1);
    ctl.rqst = XrdSfsFACtl::faSet;
    ctl.info = new XrdSfsFAInfo[1];   // freed by ~XrdSfsFACtl
    ctl.info[0].Name = const_cast<char *>(attr_name.c_str());
    ctl.info[0].NLen = short(attr_name.size());
    ctl.info[0].Value = attr_data.data();
    ctl.info[0].VLen = int(attr_data.size());

    if (m_sfs->FAttr(&ctl, error, client) != SFS_OK ||
        ctl.info[0].faRC != 0) {
        // FR-27: no store / unsupported backend => skip silently (Info-level
        // trace only); a subsequent checksum query just recalculates.
        std::stringstream ss;
        ss << "checksum attribute set failed (rc="
           << ctl.info[0].faRC << "); store not updated";
        logTransferEvent(LogMask::Info, rec, "CKSUM_INJECT_SKIP", ss.str());
        return;
    }
    logTransferEvent(LogMask::Debug, rec, "CKSUM_INJECTED",
                     "adler32 " + adler_hex + " -> " + attr_name);
}

int TPCRHandler::ProcessPullReq(const std::string &resource, XrdHttpExtReq &req) {
    TPCLogRecord rec(req,TpcType::Pull);
    rec.allow_local = m_allow_local;
    rec.allow_private = m_allow_private;
    rec.log_prefix = "PullRequest";
    rec.local = req.resource;
    rec.remote = resource;
    rec.m_log = &m_log;
    char *name = req.GetSecEntity().name;
    req.GetClientID(rec.clID);
    if (name) rec.name = name;
    logTransferEvent(LogMask::Info, rec, "PULL_START", "Starting a pull request");

    ManagedCurlHandle curlPtr(curl_easy_init());
    auto curl = curlPtr.get();
    if (!curl) {
        std::stringstream ss;
        ss << "Failed to initialize internal transfer resources";
        rec.status = 500;
        logTransferEvent(LogMask::Error, rec, "PULL_FAIL", ss.str());
        return req.SendSimpleResp(rec.status, NULL, NULL, generateClientErr(ss, rec).c_str(), 0);
    }
    ConfigureCurlLowSpeed(curl);

    // ddavila 2023-01-05:
    // The following change was required by the Rucio/SENSE project where
    // multiple IP addresses, each from a different subnet, are assigned to a
    // single server and routed differently by SENSE.
    // The above requires the server to utilize the same IP, that was used to
    // start the TPC, for the resolution of the given TPC instead of
    // using any of the IPs available.
    // Kept as a named variable: the scheduler loop applies the same
    // interface pin to every extra transfer handle it creates (SUB-5).
    std::string iface_ip;
    if (m_fixed_route) {
        char ip[64];
        char ipType = 0;

        XrdNetAddrInfo *addrInfo = req.GetSecEntity().addrInfo;
        int sockFD = addrInfo ? addrInfo->SockFD() : -1;

        if (sockFD < 0 || XrdNetUtils::GetSokInfo(-sockFD, ip, sizeof(ip), ipType) < 0) {
            // The socket information could not be fetched for some reason, treat this tpc.fixed_route as "best-effort" instead
            // of failing the transfer
            logTransferEvent(LogMask::Error, rec, "FIXED_ROUTE_ERR", "Failed to determine local address of incoming fixed route request");
        } else {
            logTransferEvent(LogMask::Info, rec, "LOCAL IP", ip);
            curl_easy_setopt(curl, CURLOPT_INTERFACE, ip);
            iface_ip = ip;
        }
    }
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
    curl_easy_setopt(curl, CURLOPT_SOCKOPTDATA , &rec);
    curl_easy_setopt(curl, CURLOPT_CLOSESOCKETFUNCTION, closesocket_callback);
    curl_easy_setopt(curl, CURLOPT_CLOSESOCKETDATA, &rec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT);
    std::unique_ptr<XrdSfsFile> fh(m_sfs->newFile(name, m_monid++));
    if (!fh.get()) {
        std::stringstream ss;
        ss << "Failed to initialize internal transfer file handle";
        rec.status = 500;
        logTransferEvent(LogMask::Error, rec, "PULL_FAIL", ss.str());
        return req.SendSimpleResp(rec.status, NULL, NULL, generateClientErr(ss, rec).c_str(), 0);
    }
    auto query_header = XrdOucTUtils::caseInsensitiveFind(req.headers,"xrd-http-fullresource");
    std::string redirect_resource = req.resource;
    if (query_header != req.headers.end()) {
        redirect_resource = query_header->second;
    }
    // SUB-2: the Overwrite decision participates in the resume decision
    // tree below; the actual open mode is chosen there.
    bool overwrite_allowed = true;
    {
        auto overwrite_header = XrdOucTUtils::caseInsensitiveFind(req.headers,"overwrite");
        if (overwrite_header != req.headers.end() && overwrite_header->second != "T") {
            overwrite_allowed = false;
        }
    }
    int streams = 1;
    {
        auto streams_header = XrdOucTUtils::caseInsensitiveFind(req.headers,"x-number-of-streams");
        if (streams_header != req.headers.end()) {
            long stream_req = -1;
            try {
                stream_req = std::stol(streams_header->second);
            } catch (...) { // Handled below
            }
            // Negative or unparseable stays a client error, as stock.
            if (stream_req < 0) {
                std::stringstream ss;
                ss << "Invalid request for number of streams";
                rec.status = 400;
                logTransferEvent(LogMask::Info, rec, "INVALID_REQUEST", ss.str());
                return req.SendSimpleResp(rec.status, NULL, NULL, generateClientErr(ss, rec).c_str(), 0);
            }
            // BUG-10 / NFR-1: values above the configured cap are clamped,
            // not rejected -- existing orchestrator configs keep working,
            // and per-transfer memory stays bounded by the pool budget.
            bool clamped = false;
            streams = static_cast<int>(m_tpcr.ClampStreams(stream_req, clamped));
            if (clamped) {
                std::stringstream ss;
                ss << "X-Number-Of-Streams " << stream_req
                   << " clamped to configured maximum " << m_tpcr.streams_max;
                logTransferEvent(LogMask::Info, rec, "STREAMS_CLAMPED", ss.str());
            }
        }
    }
    rec.streams = streams;
    std::string full_url = prepareURL(req);
    std::string authz = GetAuthz(req);
    curl_easy_setopt(curl, CURLOPT_URL, resource.c_str());
    if (!ConfigureCurlCA(curl, rec)) {
        std::stringstream ss;
        ss << "Failed to configure the certificate authorities for the transfer";
        rec.status = 500;
        logTransferEvent(LogMask::Error, rec, "PULL_FAIL", ss.str());
        return req.SendSimpleResp(rec.status, NULL, NULL, generateClientErr(ss, rec).c_str(), 0);
    }
    uint64_t sourceFileContentLength = 0;
    // Session-start validator baseline (SUB-7), captured from the HEAD.
    TPCR::SourceValidators sourceValidators;
    {
        //Get the content-length of the source file and pass it to the OSS layer
        //during the open
        bool success = false;
        bool mismatchDigests = false;
        std::map<std::string,std::string> sourceFileReprDigest;
        GetRemoteFileInfoTPCPull(curl, req, sourceFileContentLength, sourceFileReprDigest, success, rec, &sourceValidators);
        if(success) {
            //In the case we cannot get the information from the source server (offline or other error)
            //we just don't add the file information to the opaque of the local file to open
            full_url += "&oss.asize=" + std::to_string(sourceFileContentLength);
            mismatchDigests = mismatchReprDigest(sourceFileReprDigest,req,rec);
        }
        if(!success || mismatchDigests) {
            // We could not get remote file information, or the checksum provided by the client
            // does not match the source file one, we already sent the error to the client so we
            // just exit here
            return 0;
        }
    }
    // =====================================================================
    //          Resume decision tree (WP-8; 02-ARCHITECTURE §4 cases a-e)
    // =====================================================================
    const std::string dest_path = full_url.substr(0, full_url.find('?'));
    off_t resume_offset = 0;
    bool resuming = false;
    // Streaming integrity digests (WP-10, FR-26): fresh here; a resume
    // restores them from the journal below (SUB-4).
    TPCR::TransferDigests digests;
    std::unique_ptr<TPCR::JournalStore> journal_store;
    TPCR::JournalRecord record;          // the record this session will own
    bool have_record = false;            // record already committed on disk

    if (m_tpcr.resume) {
        journal_store.reset(new TPCR::JournalStore(
            m_sfs, dest_path, m_tpcr.journal_suffix, &req.GetSecEntity()));

        // FR-4: X-Resume: F forces a fresh transfer; absence permits resume.
        bool force_fresh = false;
        {
            auto resume_header =
                XrdOucTUtils::caseInsensitiveFind(req.headers, "x-resume");
            if (resume_header != req.headers.end() &&
                (resume_header->second == "F" || resume_header->second == "f")) {
                force_fresh = true;
            }
        }

        TPCR::JournalRecord journal;
        std::string load_err;
        const bool journal_present = journal_store->Exists();
        const bool journal_valid =
            journal_present && journal_store->Load(journal, load_err);
        const time_t now = time(NULL);

        // Rejecting a resume logs the coded reason (FR-31) and drops the
        // journal; the fresh path below then takes over per Overwrite.
        auto reject = [&](const std::string &reason) {
            logTransferEvent(LogMask::Info, rec, "RESUME_REJECTED", reason);
            std::string remove_err;
            journal_store->Remove(remove_err);   // best effort
        };

        if (force_fresh) {
            if (journal_present) {reject("reason=client-forced (X-Resume: F)");}
        } else if (!journal_present) {
            // Cases (a) and (d): no journal.  A journal-less partial is
            // indistinguishable from a stale foreign file (FR-25) and gets
            // stock Overwrite treatment below.
        } else if (!journal_valid) {
            reject("reason=journal-invalid (" + load_err + ")");
        } else if (journal.Age(now) > (int64_t)m_tpcr.gc_age_secs) {
            // FR-24 lazy GC: past the orchestrator retry horizon.
            logTransferEvent(LogMask::Info, rec, "GC_DISCARD",
                "journal older than tpcr.gc.age; discarding");
            std::string remove_err;
            journal_store->Remove(remove_err);
        } else {
            struct stat dest_stat;
            XrdOucErrInfo stat_error;
            const bool have_partial =
                m_sfs->stat(dest_path.c_str(), &dest_stat, stat_error,
                            &req.GetSecEntity()) == SFS_OK;
            std::string reason;
            if (!have_partial) {
                // Case (e): journal without a partial -- common under POSC,
                // which unlinks crashed creates (XRD-1).
                reject("reason=no-partial (journal without destination; POSC?)");
            } else if (!TPCR::SourceValidators::ResumeAccepts(
                           journal.validators, sourceValidators,
                           m_tpcr.validators_length_only
                               ? TPCR::SourceValidators::Policy::LengthOnly
                               : TPCR::SourceValidators::Policy::Strong,
                           reason)) {
                // FR-21 ladder; FR-20 case (c).
                reject("reason=validator (" + reason + ")");
            } else if (dest_stat.st_size < journal.committed) {
                // FR-20/FR-25: the partial was truncated or replaced; W no
                // longer describes it.
                reject("reason=dest-truncated (size below watermark)");
            } else if (journal.LeaseLive(now)) {
                // FR-22: live foreign lease -- another gateway is writing.
                // 409 + Retry-After; touch nothing.
                const long retry_after =
                    (long)(journal.lease_expiry - (int64_t)now) + 1;
                std::stringstream retry_hdr;
                retry_hdr << "Retry-After: " << retry_after;
                std::stringstream body;
                body << "Transfer already in progress by another gateway; "
                     << "retry after " << retry_after << "s";
                rec.status = 409;
                logTransferEvent(LogMask::Info, rec, "LEASE_CONFLICT",
                                 body.str());
                fh->close();
                return req.SendSimpleResp(409, NULL,
                    const_cast<char *>(retry_hdr.str().c_str()),
                    const_cast<char *>(body.str().c_str()), 0);
            } else if (m_tpcr.verify_tailbytes > 0 && journal.committed > 0 &&
                       !VerifyResumeTail(dest_path, journal,
                                         &req.GetSecEntity(),
                                         m_tpcr.verify_tailbytes, reason)) {
                // FR-28: the tail of the partial no longer matches what the
                // journal attested (torn write, external tampering, or a
                // lying backend) -- resume would build on bad bytes.
                reject("reason=tail-verify (" + reason + ")");
            } else if (journal.committed > 0 &&
                       !digests.Restore(journal.digest_state,
                                        journal.committed, reason)) {
                // SUB-4: without a digest state matching W, the resumed
                // prefix could never be attested (adler32 over [0, W) is not
                // recomputable without re-reading) -- reject rather than
                // resume into an unverifiable transfer (FR-26).
                reject("reason=digest-state (" + reason + ")");
            } else {
                // Resume -- case (b).  XRD-6 strict sequencing: acquire (or
                // steal) the lease via atomic journal rewrite BEFORE opening
                // the data file; opening first would create case-(a) side
                // effects on a path another session may own.
                if (journal.lease_expiry != 0) {
                    logTransferEvent(LogMask::Info, rec, "LEASE_STEAL",
                        "expired lease taken over");
                }
                journal.lease_owner = TPCR::JournalRecord::NewLeaseOwner();
                journal.lease_expiry =
                    (int64_t)now + 2 * (int64_t)m_tpcr.checkpoint_secs;
                journal.attempts += 1;
                journal.updated = (int64_t)now;
                journal.streams = (uint32_t)streams;
                std::string commit_err;
                if (!journal_store->Commit(journal, commit_err)) {
                    reject("reason=lease-write-failed (" + commit_err + ")");
                } else {
                    resuming = true;
                    resume_offset = journal.committed;
                    record = journal;
                    have_record = true;
                    std::stringstream ss;
                    ss << "resuming from W=" << resume_offset
                       << " (attempt " << journal.attempts << ")";
                    logTransferEvent(LogMask::Info, rec, "RESUME_START",
                                     ss.str());
                }
            }
        }
    }

    // Open mode (SUB-2 table); never SFS_O_POSC (XRD-1):
    //  - resume: plain SFS_O_WRONLY.  No TRUNC ever; and no SFS_O_CREAT,
    //    because this tree's OFS maps SFS_O_CREAT to O_CREAT|O_EXCL, which
    //    would refuse the existing partial -- FR-20 explicitly demands "no
    //    O_EXCL semantics" (create-token authz consequence in DECISIONS.md).
    //  - fresh + Overwrite T/absent: stock behavior -- TRUNC (which OFS
    //    maps to O_CREAT|O_TRUNC) unless EC, which cannot truncate.
    //  - fresh + Overwrite F: SFS_O_CREAT alone; its O_EXCL makes an
    //    existing journal-less partial fail exactly as stock does (SUB-2).
    XrdSfsFileOpenMode mode;
    if (resuming) {
        mode = SFS_O_WRONLY;
    } else if (overwrite_allowed) {
        mode = usingEC ? SFS_O_CREAT : SFS_O_TRUNC;
    } else {
        mode = SFS_O_CREAT;
    }

    int open_result = OpenWaitStall(*fh, full_url, mode|SFS_O_WRONLY,
                                    0644 | SFS_O_MKPTH,
                                    req.GetSecEntity(), authz);
    if (resuming && SFS_OK != open_result && SFS_REDIRECT != open_result) {
        // Resume open failed (plain-write authz, or the partial vanished in
        // the stat/open window).  Degrade to a fresh transfer (CON-3):
        // resume must never fail a transfer stock would have carried.
        logTransferEvent(LogMask::Info, rec, "RESUME_REJECTED",
            "reason=open-failed (falling back to a fresh transfer)");
        std::string remove_err;
        journal_store->Remove(remove_err);
        resuming = false;
        have_record = false;
        resume_offset = 0;
        // The digests may hold restored resume state; a fresh transfer
        // starts hashing from byte 0 again.
        digests = TPCR::TransferDigests();
        mode = overwrite_allowed ? (usingEC ? SFS_O_CREAT : SFS_O_TRUNC)
                                 : SFS_O_CREAT;
        open_result = OpenWaitStall(*fh, full_url, mode|SFS_O_WRONLY,
                                    0644 | SFS_O_MKPTH,
                                    req.GetSecEntity(), authz);
    }
    if (SFS_REDIRECT == open_result) {
        int result = RedirectTransfer(curl, redirect_resource, req, fh->error, rec);
        return result;
    } else if (SFS_OK != open_result) {
        int code;
        std::stringstream ss;
        const char *msg = fh->error.getErrText(code);
        if ((msg == NULL) || (*msg == '\0')) ss << "Failed to open local resource";
        else ss << msg;
        rec.status = mapErrNoToHttp(code);
        logTransferEvent(LogMask::Error, rec, "OPEN_FAIL", ss.str());
        int resp_result = req.SendSimpleResp(rec.status, NULL, NULL,
                                             generateClientErr(ss, rec).c_str(), 0);
        fh->close();
        return resp_result;
    }

    // The Stream's committed offset seeds at W (0 fresh); all offsets stay
    // absolute.  Entry capacity equals the scheduler's range size.
    Stream stream(std::move(fh), resume_offset, m_tpcr.block_size, m_log);
    // FR-26: every committed byte flows through the digests, in order,
    // exactly once -- the Stream's commit hook is that guarantee.
    stream.SetCommitHook([&digests](off_t offset, const char *data,
                                    size_t size) {
        digests.Update(offset, data, size);
    });
    State state(0, stream, curl, false, req.tpcForwardCreds);
    state.SetupHeaders(req);
    state.SetContentLength(sourceFileContentLength);

    // Journal for FRESH transfers -- created at open time (FR-18; case (a)):
    // this bounds the never-resumable window to transfers that die before
    // the open completes.  Creation failure only disables resume for this
    // transfer (CON-3: degrade safely).  Resumed transfers already committed
    // their (lease-carrying) record above.
    std::unique_ptr<TPCR::Checkpointer> checkpointer;
    if (m_tpcr.resume && journal_store) {
        if (!have_record) {
            record = TPCR::JournalRecord();
            record.committed = 0;
            record.source_url = TPCR::NormalizeSourceUrl(resource);
            record.validators = sourceValidators;
            record.lease_owner = TPCR::JournalRecord::NewLeaseOwner();
            record.created = record.updated = time(NULL);
            record.lease_expiry =
                record.created + 2 * (int64_t)m_tpcr.checkpoint_secs;
            record.block_size = m_tpcr.block_size;
            record.streams = (uint32_t)streams;
            std::string journal_err;
            if (journal_store->Commit(record, journal_err)) {
                have_record = true;
                logTransferEvent(LogMask::Debug, rec, "JOURNAL_CREATED",
                                 journal_store->JournalPath());
            } else {
                logTransferEvent(LogMask::Warning, rec, "JOURNAL_DISABLED",
                    "journal creation failed; resume disabled for this transfer: "
                    + journal_err);
            }
        }
        if (have_record) {
            // SUB-4: the snapshot closes the open CRC32C epoch into the
            // record and refreshes the serialized digest state under the
            // very commit that persists W.
            checkpointer.reset(new TPCR::Checkpointer(
                *journal_store, record, m_tpcr.checkpoint_bytes,
                m_tpcr.checkpoint_secs, m_log,
                [&digests](TPCR::JournalRecord &snapshot) {
                    auto epoch = digests.CloseEpoch();
                    if (epoch.length > 0) {
                        snapshot.epochs.push_back(epoch);
                    }
                    snapshot.digest_state = digests.Serialize();
                }));
        }
    }

    // FR-7: every pull -- streams=1 included -- runs through the range
    // scheduler.  There is exactly one pull code path.
    return RunPullScheduler(req, state, stream, streams, resource, iface_ip,
                            dest_path, sourceValidators, checkpointer.get(),
                            &digests, rec);
}

/******************************************************************************/
/*          T P C H a n d l e r : : l o g T r a n s f e r E v e n t           */
/******************************************************************************/
  
void TPCRHandler::logTransferEvent(LogMask mask, const TPCLogRecord &rec,
        const std::string &event, const std::string &message)
{
    if (!(m_log.getMsgMask() & mask)) {return;}

    std::stringstream ss;
    ss << "event=" << event << ", local=" << rec.local << ", remote=" << rec.remote;
    if (rec.name.empty())
       ss << ", user=(anonymous)";
    else
       ss << ", user=" << rec.name;
    if (rec.streams != 1)
       ss << ", streams=" << rec.streams;
    if (rec.bytes_transferred >= 0)
       ss << ", bytes_transferred=" << rec.bytes_transferred;
    if (rec.status >= 0)
       ss << ", status=" << rec.status;
    if (rec.tpc_status >= 0)
       ss << ", tpc_status=" << rec.tpc_status;
    if (!message.empty())
       ss << "; " << message;
    m_log.Log(mask, rec.log_prefix.c_str(), ss.str().c_str());
}

std::string TPCRHandler::generateClientErr(std::stringstream &err_ss, const TPCLogRecord &rec, CURLcode cCode) {
  std::stringstream ssret;
  ssret << "failure: " << err_ss.str() << ", local=" << rec.local <<", remote=" << rec.remote;
  if(cCode != CURLcode::CURLE_OK) {
    ssret << ", HTTP library failure=" << curl_easy_strerror(cCode);
  }
  return ssret.str();
}
/******************************************************************************/
/*                  X r d H t t p G e t E x t H a n d l e r                   */
/******************************************************************************/
  
extern "C" {

XrdHttpExtHandler *XrdHttpGetExtHandler(XrdSysError *log, const char * config, const char * /*parms*/, XrdOucEnv *myEnv) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT)) {
        log->Emsg("TPCInitialize", "libcurl failed to initialize");
        return NULL;
    }

    TPCRHandler *retval{NULL};
    if (!config) {
        log->Emsg("TPCInitialize", "TPC handler requires a config filename in order to load");
        return NULL;
    }
    try {
        log->Emsg("TPCInitialize", "Will load configuration for the TPC handler from", config);
        retval = new TPCRHandler(log, config, myEnv);
    } catch (std::runtime_error &re) {
        log->Emsg("TPCInitialize", "Encountered a runtime failure when loading ", re.what());
        //printf("Provided env vars: %p, XrdInet*: %p\n", myEnv, myEnv->GetPtr("XrdInet*"));
    }
    return retval;
}

}
