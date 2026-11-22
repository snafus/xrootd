#include "XrdHttpTpcPullMon.hh"
#include "XrdHttpTpcBlockPull.hh"

#include "Xrd/XrdMonRoll.hh"
#include "XrdOuc/XrdOucEnv.hh"
#include "XrdSys/XrdSysError.hh"

#include <mutex>
#include <vector>

using namespace TPC;

RAtomic_uint64_t PullMon::single{0};
RAtomic_uint64_t PullMon::sequential{0};
RAtomic_uint64_t PullMon::pulls{0};
RAtomic_uint64_t PullMon::succeeded{0};
RAtomic_uint64_t PullMon::failed{0};
RAtomic_uint64_t PullMon::blocks{0};
RAtomic_uint64_t PullMon::bytes{0};
RAtomic_uint64_t PullMon::limit_request{0};
RAtomic_uint64_t PullMon::limit_server{0};
RAtomic_uint64_t PullMon::sequential_marking{0};
RAtomic_uint64_t PullMon::sequential_ec{0};
RAtomic_uint64_t PullMon::stream_stalls{0};
RAtomic_uint64_t PullMon::pull_stalls{0};
RAtomic_uint64_t PullMon::retries{0};
RAtomic_uint64_t PullMon::retried_pulls{0};
RAtomic_uint64_t PullMon::retries_exhausted{0};
RAtomic_uint64_t PullMon::source_changed{0};
RAtomic_uint64_t PullMon::cut_backs{0};
RAtomic_uint64_t PullMon::cut_back_failures{0};
RAtomic_uint64_t PullMon::start_failures{0};

void PullMon::Register(XrdOucEnv *env, XrdSysError &log)
{
    // The names are the keys of the JSON and the tags of the XML that the
    // server reports.  The list must live as long as the server: XrdMonRoll
    // keeps a reference to it.
    //
    // pulls_single + pulls_sequential + pulls_in_blocks is every pull;
    // in_blocks_succeeded + in_blocks_failed is pulls_in_blocks less those
    // that are running.
    static std::vector<XrdMonRoll::Item> items = {
        {"pulls_single",             single},
        {"pulls_sequential",         sequential},
        {"pulls_in_blocks",          pulls},
        {"in_blocks_succeeded",      succeeded},
        {"in_blocks_failed",         failed},
        {"in_blocks_blocks_done",    blocks},
        {"in_blocks_bytes",          bytes},
        {"stream_threads",           StreamThreadBudget::Counter()},
        {"streams_cut_by_request_limit", limit_request},
        {"streams_cut_by_server_limit",  limit_server},
        {"sequential_for_marking",   sequential_marking},
        {"sequential_for_ec",        sequential_ec},
        {"stalled_requests",         stream_stalls},
        {"stalled_pulls",            pull_stalls},
        {"in_blocks_requests_retried",       retries},
        {"in_blocks_succeeded_after_retry",  retried_pulls},
        {"in_blocks_failed_retries_used_up", retries_exhausted},
        {"source_changed",           source_changed},
        {"cut_backs",                cut_backs},
        {"cut_back_failures",        cut_back_failures},
        {"thread_start_failures",    start_failures},
    };

    // Once for the process: a second handler would be refused the name.
    static std::once_flag once;
    std::call_once(once, [env, &log] {
        XrdMonRoll *roll = env ? static_cast<XrdMonRoll*>(env->GetPtr("XrdMonRoll*")) : nullptr;
        if (!roll) {
            // Not expected: the server puts it in the environment of every
            // plugin.  The counters are kept all the same and cost nothing.
            return;
        }
        if (!roll->Register(XrdMonRoll::Plugin, "http_tpc", items)) {
            log.Emsg("Config", "unable to register the counters of the multistream pull"
                               " with the summary statistics");
        }
    });
}
