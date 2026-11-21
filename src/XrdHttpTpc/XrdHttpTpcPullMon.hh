/**
 * Counters of the multistream pull, reported with the summary statistics of
 * the server.
 *
 * The log tells what one pull did.  These counters tell what the server as a
 * whole is doing with multistream pulls: how many run in blocks, whether its
 * limits are being hit, how often pulls stall or have to be cut back.  They
 * are registered once, as the set "http_tpc", through XrdMonRoll, and appear
 * wherever the summary statistics do (the xrd.report stream, a query for
 * statistics).
 *
 * They are static, as XrdMonRoll asks, and counted for the process: a server
 * with two instances of the handler reports their sum.  Each is one atomic
 * number; any thread may count.
 *
 * The stream threads in use are reported from the count the server works
 * with (StreamThreadBudget), not from a copy kept here.
 */

#ifndef __XRD_TPC_PULL_MON_HH__
#define __XRD_TPC_PULL_MON_HH__

#include "XrdSys/XrdSysRAtomic.hh"

class XrdOucEnv;
class XrdSysError;

namespace TPC {

struct PullMon {
    // Every pull is counted in one of three: carried out with a single
    // request (asked for so, or reduced to it), with several streams in the
    // sequential mode, or in blocks.
    static RAtomic_uint64_t single;
    static RAtomic_uint64_t sequential;
    static RAtomic_uint64_t pulls;
    // How the pulls in blocks ended.  A pull whose client went away is
    // counted as failed.
    static RAtomic_uint64_t succeeded;
    static RAtomic_uint64_t failed;
    // Blocks completed and bytes received by the pulls in blocks, counted
    // while they run.
    static RAtomic_uint64_t blocks;
    static RAtomic_uint64_t bytes;
    // Times a request was given fewer streams than it asked for, because of
    // the limit on one request or because the server was out of threads.
    static RAtomic_uint64_t limit_request;
    static RAtomic_uint64_t limit_server;
    // Pulls with several streams that were carried out in the sequential
    // mode although the server is in blocks mode: packet marking, or the
    // erasure coding writer.
    static RAtomic_uint64_t sequential_marking;
    static RAtomic_uint64_t sequential_ec;
    // Requests of a stream that were ended because nothing came from the
    // source, and pulls that failed because nothing moved at all.
    static RAtomic_uint64_t stream_stalls;
    static RAtomic_uint64_t pull_stalls;
    // Pulls that failed because the source file changed under them.
    static RAtomic_uint64_t source_changed;
    // Failed pulls whose file was cut back to its complete leading blocks,
    // and failed pulls where the storage refused the cut.
    static RAtomic_uint64_t cut_backs;
    static RAtomic_uint64_t cut_back_failures;
    // Stream threads that could not be started.
    static RAtomic_uint64_t start_failures;

    /**
     * Registers the counters with the server, the first time it is called.
     *
     * @param env  the environment the handler was given
     * @param log  where to say whether the counters are registered
     */
    static void Register(XrdOucEnv *env, XrdSysError &log);
};

}

#endif // __XRD_TPC_PULL_MON_HH__
