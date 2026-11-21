/**
 * What the threads of a multistream pull in blocks share, and the count of
 * stream threads in the server.  See XrdHttpTpcBlockPull.hh for how a pull in
 * blocks works and for the rules on which thread may touch what.
 *
 * Nothing here depends on the server or on libcurl beyond its result codes,
 * so that it is tested on its own; the threads themselves are in
 * XrdHttpTpcPullInBlocks.cc.
 */

#include "XrdHttpTpcBlockPull.hh"

#include <algorithm>
#include <sstream>

using namespace TPC;

/******************************************************************************/
/*                    S t r e a m T h r e a d B u d g e t                     */
/******************************************************************************/

RAtomic_int32_t StreamThreadBudget::s_in_use{0};

StreamThreadBudget::Share StreamThreadBudget::Take(int wanted, int limit)
{
    int32_t in_use = s_in_use;
    while (true) {
        const int count = std::min(wanted, limit - static_cast<int>(in_use));
        if (count <= 0) {return Share();}
        // On failure in_use is reloaded and the share is worked out again.
        if (s_in_use.compare_exchange_weak(in_use, in_use + count)) {return Share(count);}
    }
}

void StreamThreadBudget::Share::ShrinkTo(int count)
{
    if (count < 0) {count = 0;}
    if (count >= m_count) {return;}
    s_in_use -= (m_count - count);
    m_count = count;
}

StreamThreadBudget::Share &StreamThreadBudget::Share::operator=(Share &&other) noexcept
{
    if (this != &other) {
        ShrinkTo(0);
        m_count = other.m_count;
        other.m_count = 0;
    }
    return *this;
}

/******************************************************************************/
/*                             B l o c k P u l l                              */
/******************************************************************************/

BlockPull::BlockPull(off_t file_size, off_t block_size, size_t workers) :
    m_file_size(file_size),
    m_block_size(block_size),
    m_blocks(static_cast<size_t>(file_size / block_size + (file_size % block_size ? 1 : 0))),
    m_window(2 * static_cast<off_t>(workers) * block_size)
{}

off_t BlockPull::OldestUnfinished() const
{
    return m_in_flight.empty() ? m_next_offset : *m_in_flight.begin();
}

bool BlockPull::NextBlock(off_t &offset, off_t &size)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    // The wait ends when a block is done, which moves the oldest unfinished
    // block, or when the pull stops.  If it waits, a block is in flight, and
    // the worker that holds it will finish it or fail, so the wait ends.
    m_changed.wait(lock, [this] {
        return stop.load() || m_next_offset >= m_file_size
            || m_next_offset - OldestUnfinished() < m_window;
    });
    if (stop.load() || m_next_offset >= m_file_size) {return false;}
    offset = m_next_offset;
    size = std::min(m_block_size, m_file_size - m_next_offset);
    m_in_flight.insert(offset);
    m_next_offset += size;
    return true;
}

void BlockPull::BlockDone(size_t stream, off_t offset, off_t size, double seconds)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_in_flight.erase(offset);
        m_blocks_done++;
        m_reports.push_back(BlockReport{stream, offset, offset + size, seconds});
    }
    m_changed.notify_all();
}

void BlockPull::Fail(const BlockFailure &failure) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_failed) {
            m_failed = true;
            m_failure.error_code = failure.error_code;
            m_failure.status_code = failure.status_code;
            m_failure.curl_result = failure.curl_result;
            m_failure.stream = failure.stream;
            m_failure.block_start = failure.block_start;
            m_failure.block_end = failure.block_end;
            m_failure.block_received = failure.block_received;
            m_failure.stall_limit = failure.stall_limit;
            m_failure.stall_idle = failure.stall_idle;
            // Last: the copy of the text is the one thing here that can
            // throw, and a failure without its text is still a failure.
            m_failure.message = failure.message;
        }
    } catch (...) {
        // Nothing can be done about it here; the pull is stopped below, and
        // the coordinator does not report a success for a stopped pull.
    }
    Stop();
}

void BlockPull::Stop() noexcept
{
    try {
        // Under the mutex, so that a worker that has just tested the flag in
        // NextBlock() is either not yet waiting, and sees it, or is waiting,
        // and gets the notification.
        std::lock_guard<std::mutex> lock(m_mutex);
        stop.store(true);
    } catch (...) {
        stop.store(true);
    }
    m_changed.notify_all();
}

void BlockPull::WorkerStarting()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_running++;
}

void BlockPull::WorkerNotStarted()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_running--;
}

void BlockPull::WorkerEnded() noexcept
{
    try {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running--;
    } catch (...) {
        // The coordinator would then wait for ever; a mutex that cannot be
        // locked is not something this code can recover from.
    }
    m_changed.notify_all();
}

bool BlockPull::WaitForWorkers(std::chrono::seconds patience)
{
    std::unique_lock<std::mutex> lock(m_mutex);
#if defined(__SANITIZE_THREAD__)
    // wait_for() ends in pthread_cond_clockwait(), which the thread sanitizer
    // of GCC 11 does not know, so that it takes this lock for held twice.
    // Under the sanitizer only, wait on the system clock instead.
    return m_changed.wait_until(lock, std::chrono::system_clock::now() + patience,
                                [this] {return m_running == 0;});
#else
    // On the steady clock: a step of the system clock must not hold up the
    // markers and the stall check of the coordinator.
    return m_changed.wait_for(lock, patience, [this] {return m_running == 0;});
#endif
}

int BlockPull::Running()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_running;
}

std::vector<BlockReport> BlockPull::TakeReports()
{
    std::vector<BlockReport> reports;
    std::lock_guard<std::mutex> lock(m_mutex);
    reports.swap(m_reports);
    return reports;
}

bool BlockPull::Failed()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_failed;
}

BlockFailure BlockPull::Failure()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_failure;
}

size_t BlockPull::BlocksDone()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_blocks_done;
}

bool BlockPull::AllHandedOut()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_next_offset >= m_file_size;
}

off_t BlockPull::CompleteUpTo()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return OldestUnfinished();
}

/******************************************************************************/
/*                            S t a l l W a t c h                             */
/******************************************************************************/

long long StallWatch::Stalled(long long now, off_t received, off_t request_start, int phase,
                              unsigned request, int first_timeout, int timeout)
{
    if (!m_seen || request != m_request) {
        // A request not seen before: its time starts now.
        m_seen = true;
        m_request = request;
        m_received = received;
        m_since = now;
    }
    if (received != m_received) {
        m_received = received;
        m_since = now;
    }
    if (phase != WorkerStatus::Receiving) {
        // Writing, or between requests: not waiting for the source.
        m_since = now;
        return 0;
    }
    const long long idle = now - m_since;
    // A request that has not had a byte yet is connecting, or waiting for
    // the first one, and is given longer.
    return idle > Allowed(received, request_start, first_timeout, timeout) ? idle : 0;
}

/******************************************************************************/
/*                           S o u r c e C h e c k                            */
/******************************************************************************/

namespace {

// A strong ETag is a quoted string; a weak one starts with W/ and says only
// that two versions are equivalent, which is not enough to put parts together.
bool IsStrong(const std::string &etag)
{
    return !etag.empty() && etag[0] == '"';
}

// A header value of the source, made fit for a message: the source chooses
// it, so it may be very long or hold control characters.
std::string Printable(const std::string &value)
{
    static const size_t most = 128;
    std::string text = value.substr(0, most);
    for (char &c : text) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {c = '?';}
    }
    if (value.size() > most) {text += "...";}
    return text;
}

// " (server tcp:1.2.3.4:1094)", or nothing when the server is not known.
std::string ServerText(const std::string &host)
{
    return host.empty() ? std::string() : " (server " + host + ")";
}

}

bool SourceCheck::Agrees(const Seen &seen, std::string &error)
{
    std::stringstream ss;
    if (seen.total >= 0 && seen.total != m_file_size) {
        ss << "The source file changed during the transfer: its size was " << m_file_size
           << " bytes and is now " << seen.total << ServerText(seen.host);
        error = ss.str();
        return false;
    }
    if (!m_validators) {return true;}

    std::lock_guard<std::mutex> lock(m_mutex);
    Version &version = m_hosts[seen.host];
    // Whether the ETag of this response was compared with an earlier one of
    // the server and found the same.  Only then does it decide, and
    // Last-Modified is not looked at.
    bool same_etag = false;
    if (IsStrong(seen.etag)) {
        if (version.etag.empty()) {
            version.etag = seen.etag;
        } else if (version.etag != seen.etag) {
            ss << "The source file changed during the transfer: its ETag was "
               << Printable(version.etag) << " and is now " << Printable(seen.etag)
               << ServerText(seen.host);
            error = ss.str();
            return false;
        } else {
            same_etag = true;
        }
    }
    if (!seen.last_modified.empty()) {
        if (version.last_modified.empty()) {
            version.last_modified = seen.last_modified;
        } else if (version.last_modified != seen.last_modified && !same_etag) {
            ss << "The source file changed during the transfer: its Last-Modified was '"
               << Printable(version.last_modified) << "' and is now '"
               << Printable(seen.last_modified) << "'" << ServerText(seen.host);
            error = ss.str();
            return false;
        }
    }
    return true;
}
