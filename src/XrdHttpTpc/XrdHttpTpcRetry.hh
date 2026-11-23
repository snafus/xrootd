/**
 * Retry of a block of a multistream pull in blocks: which failures of a
 * request are worth another try, and how many tries a block gets.
 *
 * The rule
 * --------
 * A request that fails is tried again, unless the failure is one that asking
 * again is known not to cure.  The reasoning is the cost of being wrong.  A
 * retry of a failure that is in fact permanent costs the tries of one block
 * before the pull fails as it would have: seconds for a source that answers
 * or refuses at once, minutes for one that has gone silent, each try then
 * waiting out the stall time.  No retry of a failure that was passing costs
 * the whole copy.  So the list below is of what is NOT retried, and it is
 * kept short:
 *
 * - what this server did wrong or could not do: a write to the storage, an
 *   internal error;
 * - the source file changed: the parts would be of two versions;
 * - the source answered a range request with the whole file: it does not
 *   serve ranges;
 * - the source sent more than the range it announced: it does not keep to
 *   what it says, and a block that is full has nothing left to ask for;
 * - a status that says the request itself is refused or cannot be served:
 *   see StatusIsFinal();
 * - a result of libcurl that says the request cannot be made, or that the
 *   source is not to be trusted: see CurlResultIsFinal().
 *
 * Everything else is retried: a connection that is refused, reset or closed,
 * a name that does not resolve, a body that is too short, a range that is
 * not the one asked for, a request ended by the stall check of its stream,
 * and every other status.
 *
 * How a retry is made is in XrdHttpTpcPullInBlocks.cc: the stream asks for
 * the rest of its block only, and keeps what it has.
 *
 * Nothing here touches the server, a thread or a clock, so that it is tested
 * on its own.
 */

#ifndef __XRD_TPC_RETRY_HH__
#define __XRD_TPC_RETRY_HH__

#include <curl/curl.h>

namespace TPC {

/**
 * How a request for a block, or for the rest of one, ended.
 */
struct RequestResult {
    CURLcode curl_result{CURLE_OK};  // what libcurl returned for the request
    // The status of the last response of the request, as the source sent it;
    // -1 if there was none.
    int status_code{-1};
    // What the request code recorded: a State::ErrorCode, BlockPull::errInternal,
    // or 0.  A request ended by the stall check of its stream has errTimeout.
    int error_code{0};
};

enum class NextStep {
    Done,   // the request brought what was asked for
    Retry,  // it failed, and may be tried again
    Fail    // it failed in a way that another try is known not to cure
};

// What to do after a request, by the rule at the head of this file.
NextStep AfterRequest(const RequestResult &result);

/**
 * Whether a status of the source is known not to be cured by asking again
 * with the same request: the request is malformed or not allowed (400, 401,
 * 403, 405, 407), what it asks for is not there or not servable in this form
 * (404, 406, 410, 411 to 417, 426, 431, 451), or the source does not have the
 * means at all (501, 505).
 *
 * All others are retried, among them 408, 409, 421, 423, 425, 429, every
 * other 5xx, and codes this list does not know.
 */
bool StatusIsFinal(int status_code);

/**
 * Whether a result of libcurl is known not to be cured by another try: the
 * request cannot be made on this server (no memory, an option or a feature
 * that is missing, a URL that cannot be parsed), the source sends in a loop
 * of redirections, or its certificate or the local one is not accepted.
 *
 * A pull has made one request to the source before it starts its streams, so
 * most of these have failed it earlier; they are listed for completeness.
 */
bool CurlResultIsFinal(CURLcode result);

/**
 * The tries of one block.
 *
 * A block gets `attempts` tries in a row without progress.  A try that made
 * progress, although it failed, sets that count back: a long copy over a
 * path that is cut again and again goes on, while a source that is gone
 * fails the pull after `attempts` tries.  What counts as progress is for the
 * caller to say; the stream thread asks for one write of new data, so that
 * a source which lets a little through and fails is not taken to be moving.
 * A second number, `most`, caps all the tries of the block.
 *
 * The wait before a try is `wait` seconds, doubled for each try in a row
 * without progress up to MaxWait (a configured wait that is longer stays as
 * it is).  The source can ask for longer with Retry-After, up to
 * MaxRetryAfter.  A random part of up to a quarter is added, so that the
 * streams of many pulls do not come back at one instant to a source that is
 * in trouble.
 *
 * Used by one stream thread; not shared.
 */
class RetryPolicy {
public:
    // Seconds, before the random part: where the doubling of the wait
    // stops, and the longest wait a source may ask for with Retry-After.
    static constexpr int MaxWait = 30;
    static constexpr int MaxRetryAfter = 120;

    /**
     * @param attempts  tries in a row without progress; 1 means no retry
     * @param most      tries of the block in all
     * @param wait      seconds before the second try; 0 for none
     */
    RetryPolicy(int attempts, int most, int wait)
        : m_attempts(attempts), m_most(most), m_wait(wait) {}

    /**
     * Counts a try that failed.
     *
     * @param progress  whether it brought enough new data to count as moving
     * @return whether the block may be tried again
     */
    bool Another(bool progress);

    // Tries that failed so far.
    int Tries() const {return m_tries;}

    /**
     * The wait before the next try, in milliseconds.  To be called after
     * Another() has returned true.
     *
     * @param retry_after  seconds the source asked for; negative if it did not
     * @param chance       a number from 0 to 1, for the random part
     */
    long WaitMilliseconds(long retry_after, double chance) const;

private:
    const int m_attempts;
    const int m_most;
    const int m_wait;
    int m_tries{0};             // tries that failed
    int m_without_progress{0};  // of those, the last ones in a row that brought nothing
};

}

#endif // __XRD_TPC_RETRY_HH__
