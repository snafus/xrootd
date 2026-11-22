
#include "XrdHttpTpcTPC.hh"

#include <climits>
#include <dlfcn.h>
#include <fcntl.h>

#include "XrdOuc/XrdOuca2x.hh"
#include "XrdOuc/XrdOucEnv.hh"
#include "XrdOuc/XrdOucStream.hh"
#include "XrdOuc/XrdOucPinPath.hh"
#include "XrdSfs/XrdSfsInterface.hh"
#include "XrdHttp/XrdHttpProtocol.hh"
#include "XrdOuc/XrdOucTUtils.hh"

using namespace TPC;

namespace {
// The directives of the multistream pull have a prefix of their own, so that
// they are found in one place and are not taken for settings of the tpc.
// family or of ofs.tpc.  It is spelled here and nowhere else.
const char MultistreamPrefix[] = "http_multi.";
}


bool TPCHandler::Configure(const char *configfn, XrdOucEnv *myEnv)
{
    XrdOucEnv cfgEnv;
    XrdOucStream Config(&m_log, getenv("XRDINSTANCE"), &cfgEnv, "=====> ");

    m_log.setMsgMask(LogMask::Warning | LogMask::Error);

    // test if XrdEC is used
    usingEC = getenv("XRDCL_EC")? true : false;
    // Test if the CRL checking is enabled
    allowMissingCRL = (bool) myEnv->GetInt("http.allowmissingcrl");
    std::string authLib;
    std::string authLibParms;
    int cfgFD = open(configfn, O_RDONLY, 0);
    if (cfgFD < 0) {
        m_log.Emsg("Config", errno, "open config file", configfn);
        return false;
    }
    Config.Attach(cfgFD);
    static const char *cvec[] = { "*** http tpc plugin config:", 0 };
    Config.Capture(cvec);
    const char *val;
    while ((val = Config.GetMyFirstWord())) {
        if (!strcmp("http.desthttps", val)) {
            if (!(val = Config.GetWord())) {
                Config.Close();
                m_log.Emsg("Config", "http.desthttps value not specified");
                return false;
            }
            if (!strcmp("1", val) || !strcasecmp("yes", val) || !strcasecmp("true", val)) {
                m_desthttps = true;
            } else if (!strcmp("0", val) || !strcasecmp("no", val) || !strcasecmp("false", val)) {
                m_desthttps = false;
            } else {
                Config.Close();
                m_log.Emsg("Config", "https.desthttps value is invalid", val);
                return false;
            }
        } else if (!strcmp("tpc.allow", val)) {
            if (!(val = Config.GetWord())) {
                Config.Close();
                m_log.Emsg("Config", "tpc.allow value not specified");
                return false;
            }
            if (strcmp(val, "local") == 0) {
              m_allow_local = true;
            } else if (strcmp(val, "private") == 0) {
              m_allow_private = true;
            } else {
                Config.Close();
                m_log.Emsg("Config", "tpc.allow value is invalid", val);
                return false;
            }
        } else if (!strcmp("tpc.deny", val)) {
            if (!(val = Config.GetWord())) {
                Config.Close();
                m_log.Emsg("Config", "tpc.deny value not specified");
                return false;
            }
            if (strcmp(val, "local") == 0) {
              m_allow_local = false;
            } else if (strcmp(val, "private") == 0) {
              m_allow_private = false;
            } else {
                Config.Close();
                m_log.Emsg("Config", "tpc.deny value is invalid", val);
                return false;
            }
        } else if (!strcmp("tpc.trace", val)) {
            if (!ConfigureLogger(Config)) {
                Config.Close();
                return false;
            }
        } else if (!strcmp("tpc.fixed_route", val)) {
            if (!(val = Config.GetWord())) {
                Config.Close();
                m_log.Emsg("Config", "tpc.fixed_route value not specified");
                return false;
            }
            if (!strcmp("1", val) || !strcasecmp("yes", val) || !strcasecmp("true", val)) {
                m_fixed_route= true;
            } else if (!strcmp("0", val) || !strcasecmp("no", val) || !strcasecmp("false", val)) {
                m_fixed_route= false;
            } else {
                Config.Close();
                m_log.Emsg("Config", "tpc.fixed_route value is invalid", val);
                return false;
            }
        } else if (!strcmp("tpc.header2cgi",val)) {
            // header2cgi parsing
            if(XrdHttpProtocol::parseHeader2CGI(Config,m_log,hdr2cgimap)){
              Config.Close();
              return false;
            }
            // remove authorization header2cgi parsing as it will anyway be added to the CGI before the file open
            // by the HTTP/TPC logic
            auto authHdr = XrdOucTUtils::caseInsensitiveFind(hdr2cgimap,"authorization");
            if(authHdr != hdr2cgimap.end()) {
              hdr2cgimap.erase(authHdr);
            }
        } else if (!strcmp("tpc.low_speed", val)) {
            if (!(val = Config.GetWord())) {
                Config.Close();
                m_log.Emsg("Config", "tpc.low_speed rate not specified.");
                return false;
            }

            long long low_speed_limit;
            if (XrdOuca2x::a2sz(m_log, "low speed rate", val, &low_speed_limit, 0, LONG_MAX)) {
                return false;
            }
            m_low_speed_limit = static_cast<long>(low_speed_limit);

            if ((val = Config.GetWord())) {
                int low_speed_time;
                if (XrdOuca2x::a2tm(m_log, "low speed time", val, &low_speed_time, 1)) {
                    return false;
                }
                m_low_speed_time = low_speed_time;
            }
        } else if (!strcmp("tpc.timeout", val)) {
            if (!(val = Config.GetWord())) {
                Config.Close();
                m_log.Emsg("Config","tpc.timeout value not specified.");  return false;
            }
            if (XrdOuca2x::a2tm(m_log, "timeout value", val, &m_timeout, 0)) return false;
                // First byte timeout can be set separately from the continuous timeout.
            if ((val = Config.GetWord())) {
                if (XrdOuca2x::a2tm(m_log, "first byte timeout value", val, &m_first_timeout, 0)) return false;
            } else {
                m_first_timeout = 2*m_timeout;
            }
        } else if (!strncmp(MultistreamPrefix, val, sizeof(MultistreamPrefix) - 1)) {
            if (!ConfigureMultistream(val + sizeof(MultistreamPrefix) - 1, Config)) {
                Config.Close();
                return false;
            }
        }
    }
    Config.Close();

    // The settings of the multistream pull are checked together, once all of
    // them are known: a minimum block above the maximum, for example.
    {
        std::string error;
        if (!m_pull_options.Validate(error)) {
            m_log.Emsg("Config", error.c_str());
            return false;
        }
        if (m_pull_options.mode == PullOptions::Mode::Blocks) {
            std::stringstream ss;
            ss << "multistream pulls are carried out in blocks: at most "
               << m_pull_options.max_streams_request << " streams a request and "
               << m_pull_options.max_stream_threads << " stream threads in the server; blocks of "
               << m_pull_options.block_min << " to " << m_pull_options.block_max
               << " bytes; writes of " << m_pull_options.write_size << " bytes (up to "
               << (static_cast<long long>(m_pull_options.max_stream_threads) * m_pull_options.write_size / PullOptions::MiB)
               << " MiB of write buffers); a stream may wait " << m_first_timeout
               << " seconds for the first byte of a request and " << m_timeout
               << " for more; the source file is checked for "
               << (m_pull_options.source_validators ? "its size and its ETag or Last-Modified"
                                                    : "its size only");
            if (m_pull_options.retry_attempts > 1) {
                ss << "; a block that fails gets " << m_pull_options.retry_attempts
                   << " tries without progress and " << m_pull_options.retry_most
                   << " in all, the second after " << m_pull_options.retry_wait << " seconds";
            } else {
                ss << "; a block that fails is not tried again";
            }
            m_log.Say("Config ", ss.str().c_str());
        }
    }

    // Internal override: allow xrdtpc to use a different ca dir from the one prepared by the xrootd
    // framework.  meant for exceptional situations where the site might need a specially-prepared set
    // of cas only for tpc (such as trying out various workarounds for libnss).  Explicitly disables
    // the NSS hack below.
    auto env_cadir = getenv("XRDTPC_CADIR");
    if (env_cadir) m_cadir = env_cadir;

    // Sharing a single pre-parsed CA/CRL store between transfers relies on
    // CURLOPT_SSL_CTX_FUNCTION, which only libcurl's OpenSSL, mbedTLS and wolfSSL
    // backends implement; the others reject it with CURLE_NOT_BUILT_IN.  libcurl
    // checks this against the backend selected at run time rather than at build
    // time, so probe an actual handle, and do it here so that an unsupported build
    // is reported at startup instead of silently costing memory per transfer.
    {
        ManagedCurlHandle probe(curl_easy_init());
        m_sslctx_supported = probe &&
            curl_easy_setopt(probe.get(), CURLOPT_SSL_CTX_FUNCTION,
                             ssl_ctx_callback) == CURLE_OK;
        if (!m_sslctx_supported) {
            m_log.Emsg("Config", "libcurl does not support CURLOPT_SSL_CTX_FUNCTION; "
                       "each transfer will parse the CA and CRL bundles for itself, "
                       "which costs significant memory per concurrent transfer.");
        }
    }

    const char *cadir = nullptr, *cafile = nullptr;
    if ((cadir = env_cadir ? env_cadir : myEnv->Get("http.cadir"))) {
        m_cadir = cadir;
        if (!env_cadir) {
            // Only ask for the pre-parsed store when we can actually install it;
            // maintaining one costs tens of MB that the fallback path never reads.
            m_ca_file.reset(new XrdTlsTempCA(&m_log, m_cadir, m_sslctx_supported));
            if (!m_ca_file->IsValid()) {
                m_log.Emsg("Config", "CAs / CRL generation for libcurl failed.");
                return false;
            }
        }
    }
    if ((cafile = myEnv->Get("http.cafile"))) {
        m_cafile = cafile;
    }

    if (!cadir && !cafile) {
        // We do not necessary need TLS to perform HTTP TPC transfers, just log that these values were not specified
        m_log.Emsg("Config", "neither xrd.tls cadir nor certfile value specified; is TLS enabled?");
    }

    void *sfs_raw_ptr;
    if ((sfs_raw_ptr = myEnv->GetPtr("XrdSfsFileSystem*"))) {
        m_sfs = static_cast<XrdSfsFileSystem*>(sfs_raw_ptr);
        m_log.Emsg("Config", "Using filesystem object from the framework.");
        return true;
    } else {
        m_log.Emsg("Config", "No filesystem object available to HTTP-TPC subsystem.  Internal error.");
        return false;
    }
    return true;
}

bool TPCHandler::ConfigureLogger(XrdOucStream &config_obj)
{
    char *val = config_obj.GetWord();
    if (!val || !val[0])
    {   
        m_log.Emsg("Config", "tpc.trace requires at least one directive [all | error | warning | info | debug | none]");
        return false;
    }
    // If the config option is given, reset the log mask.
    m_log.setMsgMask(0);
    
    do {
        if (!strcasecmp(val, "all"))
        {   
            m_log.setMsgMask(m_log.getMsgMask() | LogMask::All);
        }
        else if (!strcasecmp(val, "error"))
        {   
            m_log.setMsgMask(m_log.getMsgMask() | LogMask::Error);
        }
        else if (!strcasecmp(val, "warning"))
        {   
            m_log.setMsgMask(m_log.getMsgMask() | LogMask::Warning);
        }
        else if (!strcasecmp(val, "info"))
        {   
            m_log.setMsgMask(m_log.getMsgMask() | LogMask::Info);
        }
        else if (!strcasecmp(val, "debug"))
        {   
            m_log.setMsgMask(m_log.getMsgMask() | LogMask::Debug);
        }
        else if (!strcasecmp(val, "none"))
        {   
            m_log.setMsgMask(0);
        }
        else
        {   
            m_log.Emsg("Config", "tpc.trace encountered an unknown directive (valid values: [all | error | warning | info | debug | none]):", val);
            return false;
        }
        val = config_obj.GetWord();
    } while (val);
    
    return true;
}

/******************************************************************************/
/*      T P C H a n d l e r : : C o n f i g u r e M u l t i s t r e a m       */
/******************************************************************************/

// The http_multi.* directives.  `name` is what follows the prefix.
//
//   http_multi.mode        sequential | blocks
//   http_multi.max_streams <streams of one request> [<stream threads of the server>]
//   http_multi.block_size  <minimum> [<maximum>]         sizes take k, m, g, t
//   http_multi.write_size  <size>
//   http_multi.source_check full | size
//   http_multi.retry       <attempts> [<wait> [<most>]]   tries of a block without progress,
//                                                          seconds before the second, tries in all
//
// Each value is checked against its limits here, so that the message names
// the value that is wrong; PullOptions::Validate() checks them against each
// other afterwards.  An unknown name is an error: a misspelt directive that
// is ignored leaves the server in a mode its operator did not choose.
bool TPCHandler::ConfigureMultistream(const char *name, XrdOucStream &Config)
{
    const std::string directive = std::string(MultistreamPrefix) + name;
    char *val = Config.GetWord();
    if (!val || !*val) {
        m_log.Emsg("Config", directive.c_str(), "value not specified.");
        return false;
    }

    if (!strcmp(name, "mode")) {
        if (!strcmp(val, "sequential")) {
            m_pull_options.mode = PullOptions::Mode::Sequential;
        } else if (!strcmp(val, "blocks")) {
            m_pull_options.mode = PullOptions::Mode::Blocks;
        } else {
            m_log.Emsg("Config", "http_multi.mode must be 'sequential' or 'blocks', not", val);
            return false;
        }
    } else if (!strcmp(name, "max_streams")) {
        if (XrdOuca2x::a2i(m_log, "http_multi.max_streams streams of a request", val,
                           &m_pull_options.max_streams_request, 1, PullOptions::MaxStreamsPerRequest)) {
            return false;
        }
        if ((val = Config.GetWord()) && *val) {
            if (XrdOuca2x::a2i(m_log, "http_multi.max_streams stream threads of the server", val,
                               &m_pull_options.max_stream_threads, 2, PullOptions::MaxStreamThreads)) {
                return false;
            }
        }
    } else if (!strcmp(name, "block_size")) {
        long long size;
        if (XrdOuca2x::a2sz(m_log, "http_multi.block_size minimum", val, &size,
                            PullOptions::MinWriteSize, PullOptions::MaxBlockSize)) {
            return false;
        }
        m_pull_options.block_min = static_cast<off_t>(size);
        if ((val = Config.GetWord()) && *val) {
            if (XrdOuca2x::a2sz(m_log, "http_multi.block_size maximum", val, &size,
                                PullOptions::MinWriteSize, PullOptions::MaxBlockSize)) {
                return false;
            }
            m_pull_options.block_max = static_cast<off_t>(size);
        } else if (m_pull_options.block_max < m_pull_options.block_min) {
            // Only a minimum was given, above the default maximum.
            m_pull_options.block_max = m_pull_options.block_min;
        }
    } else if (!strcmp(name, "write_size")) {
        long long size;
        if (XrdOuca2x::a2sz(m_log, "http_multi.write_size", val, &size,
                            PullOptions::MinWriteSize, PullOptions::MaxWriteSize)) {
            return false;
        }
        m_pull_options.write_size = static_cast<off_t>(size);
    } else if (!strcmp(name, "source_check")) {
        if (!strcmp(val, "full")) {
            m_pull_options.source_validators = true;
        } else if (!strcmp(val, "size")) {
            m_pull_options.source_validators = false;
        } else {
            m_log.Emsg("Config", "http_multi.source_check must be 'full' or 'size', not", val);
            return false;
        }
    } else if (!strcmp(name, "retry")) {
        bool most_given = false;
        if (XrdOuca2x::a2i(m_log, "http_multi.retry attempts", val,
                           &m_pull_options.retry_attempts, 1, PullOptions::MaxRetryAttempts)) {
            return false;
        }
        if ((val = Config.GetWord()) && *val) {
            if (XrdOuca2x::a2tm(m_log, "http_multi.retry wait", val,
                                &m_pull_options.retry_wait, 0, PullOptions::MaxRetryWait)) {
                return false;
            }
            if ((val = Config.GetWord()) && *val) {
                if (XrdOuca2x::a2i(m_log, "http_multi.retry most tries of a block", val,
                                   &m_pull_options.retry_most, 1, PullOptions::MaxRetryAttempts)) {
                    return false;
                }
                most_given = true;
            }
        }
        // More attempts than the default cap, and no cap given: the cap
        // follows, so that the attempts asked for are all made.
        if (!most_given && m_pull_options.retry_most < m_pull_options.retry_attempts) {
            m_pull_options.retry_most = m_pull_options.retry_attempts;
        }
    } else {
        m_log.Emsg("Config", "unknown directive", directive.c_str());
        return false;
    }
    // A word too many is as likely a mistake as a word that is wrong.
    if ((val = Config.GetWord()) && *val) {
        m_log.Emsg("Config", directive.c_str(), "has a value too many:", val);
        return false;
    }
    return true;
}
