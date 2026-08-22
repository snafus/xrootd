
#include <sstream>

#include "XrdHttpTpcRStream.hh"

#include "XrdSfs/XrdSfsInterface.hh"
#include "XrdSys/XrdSysError.hh"

using namespace TPCR;

Stream::~Stream()
{
    m_fh->close();
}


bool
Stream::Finalize()
{
    // Do not close twice
    if (!m_open_for_write) {
        return false;
    }
    m_open_for_write = false;

    // If there are outstanding buffers to reorder, finalization failed; the
    // check has to happen before the buffers are released.
    bool all_buffers_returned = !AnyBufferedData();
    m_buffers.clear();

    if (m_fh->close() == SFS_ERROR) {
        std::stringstream ss;
        const char *msg = m_fh->error.getErrText();
        if (!msg || (*msg == '\0')) {msg = "(no error message provided)";}
        ss << "Failure when closing file handle: " << msg << " (code=" << m_fh->error.getErrInfo() << ")";
        m_error_buf = ss.str();
        return false;
    }

    return all_buffers_returned;
}


int
Stream::Stat(struct stat* buf)
{
    return m_fh->stat(buf);
}

ssize_t
Stream::Write(off_t offset, const char *buf, size_t size, bool force)
{
    if (!m_open_for_write) {
        if (!m_error_buf.size()) {m_error_buf = "Logic error: writing to a buffer not opened for write";}
        return SFS_ERROR;
    }
    if (offset < m_offset) {
        if (!m_error_buf.size()) {m_error_buf = "Logic error: writing to a prior offset";}
        return SFS_ERROR;
    }
    size_t bytes_accepted = 0;
    // If this write is appending to the stream and MB-aligned, then we write
    // it to disk; otherwise, the data will be buffered.
    if (offset == m_offset && (force || (size && !(size % (1024*1024))))) {
        ssize_t retval = WriteImpl(offset, buf, size);
            // On failure, we don't care about flushing buffers from memory --
            // the stream is now invalid.
        if (retval < 0) {
            return retval;
        }
        bytes_accepted = retval;
        // BUG-6 fix: a short write here is legal SFS behavior.  The remainder
        // is now contiguous at the advanced committed offset and falls
        // through to the buffering path below; the caller sees either the
        // full size accepted or an error, never a short count with bytes
        // secretly buffered (which caused double-writes on retry).
        if (bytes_accepted == size && !AnyBufferedData()) {
            return size;
        }
    }
    // Even if we already accepted the current data, always iterate through the
    // buffers and try to write as much out to disk as possible.
    //
    // Accepting data can complete a buffer, and flushing a buffer advances
    // m_offset, which can in turn let another buffer accept more data or become
    // writable.  Alternate between the two until neither makes progress.  When
    // size == 0 we force a flush even if things are not MB-aligned.
    ssize_t buffers_flushed;
    do {
        bytes_accepted += AcceptIntoBuffers(offset + bytes_accepted,
                                            buf + bytes_accepted,
                                            size - bytes_accepted);
        buffers_flushed = FlushBuffers(size == 0);
        if (buffers_flushed == SFS_ERROR) {return SFS_ERROR;}
    } while ((buffers_flushed > 0) && (bytes_accepted != size));

    // Whatever could not extend an existing buffer goes into empty entries,
    // created on demand.  A single Write chunk can straddle entry capacity
    // boundaries, hence the loop.  (The stock code treated "no empty entry"
    // as a hard error -- BUG-7's production failure mode; entries are now
    // elastic and fragmentation is harmless.)
    while (bytes_accepted != size && size) {
        Entry *avail_entry = EmptyEntry();
        if (!avail_entry) {  // Allocation failure; cannot happen short of OOM.
            m_error_buf = "Unable to allocate a re-ordering buffer entry.";
            return SFS_ERROR;
        }
        size_t accepted = avail_entry->Accept(offset + bytes_accepted,
                                              buf + bytes_accepted,
                                              size - bytes_accepted);
        if (accepted == 0) {  // Empty buffer cannot accept?!?
            m_error_buf = "Empty re-ordering buffer was unable to accept data; internal logic error.";
            return SFS_ERROR;
        }
        bytes_accepted += accepted;
        // The buffer we just filled may already be complete and contiguous with
        // m_offset; flush it now instead of waiting for a later callback to
        // notice, as every curl handle may be idle by then.
        if (FlushBuffers(false) == SFS_ERROR) {return SFS_ERROR;}
    }

    // If buffered pressure has dropped, release the empty entries' memory.
    TrimEmptyEntries();

    // BUG-6: every byte is now on disk or buffered; report full acceptance.
    return size;
}


size_t
Stream::AcceptIntoBuffers(off_t offset, const char *buf, size_t size)
{
    size_t bytes_accepted = 0;
    if (!size) {return 0;}
    for (auto &entry : m_buffers) {
        // Empty buffers are deliberately skipped here: they are handed out
        // explicitly by Write() so that new placements stay deliberate.
        if (entry->Available()) {continue;}
        bytes_accepted += entry->Accept(offset + bytes_accepted,
                                        buf + bytes_accepted,
                                        size - bytes_accepted);
        if (bytes_accepted == size) {break;}
    }
    return bytes_accepted;
}


ssize_t
Stream::FlushBuffers(bool force)
{
    ssize_t buffers_flushed = 0;
    bool buffer_was_written;
    do {
        buffer_was_written = false;
        for (auto &entry : m_buffers) {
            ssize_t retval = entry->Write(*this, force);
            if (retval == SFS_ERROR) {
                if (!m_error_buf.size()) {m_error_buf = "Unknown filesystem write failure.";}
                return SFS_ERROR;
            }
            if (retval > 0) {
                buffer_was_written = true;
                buffers_flushed ++;
            }
        }
        // Writing a buffer advances m_offset, which may have made a buffer we
        // already walked past contiguous with the stream; go around again.
    } while (buffer_was_written && AnyBufferedData());
    return buffers_flushed;
}


Stream::Entry *
Stream::EmptyEntry()
{
    for (auto &entry : m_buffers) {
        if (entry->Available()) {return entry.get();}
    }
    // Prefer a pooled slab (NFR-1); fall back to a private heap buffer when
    // the pool has nothing -- bytes already in hand must never be dropped.
    // The scheduler keeps the fallback rare by reserving a slab per issued
    // range; OverflowEntries() makes any residual pressure observable.
    if (m_slab_source) {
        auto slab = m_slab_source();
        if (slab) {
            m_buffers.push_back(std::make_unique<Entry>(std::move(slab)));
            return m_buffers.back().get();
        }
        m_overflow_entries++;
    }
    m_buffers.push_back(std::make_unique<Entry>(m_buffer_size));
    return m_buffers.back().get();
}


bool
Stream::AnyBufferedData() const
{
    for (const auto &entry : m_buffers) {
        if (!entry->Available()) {return true;}
    }
    return false;
}


void
Stream::TrimEmptyEntries()
{
    // Keep a couple of empty entries around as working set; drop the rest so
    // a burst of out-of-order arrival does not pin memory for the whole
    // transfer.  (Entry buffer memory itself is released on flush; this trims
    // the vector's entry objects.)
    static const size_t keep = 2;
    size_t empties = 0;
    auto it = m_buffers.begin();
    while (it != m_buffers.end()) {
        if ((*it)->Available() && ++empties > keep) {
            it = m_buffers.erase(it);
        } else {
            ++it;
        }
    }
}


size_t
Stream::ReorderSpan() const
{
    off_t furthest = m_offset;
    for (const auto &entry : m_buffers) {
        if (entry->Available()) {continue;}
        off_t end = entry->GetOffset() + static_cast<off_t>(entry->GetSize());
        if (end > furthest) {furthest = end;}
    }
    return static_cast<size_t>(furthest - m_offset);
}


ssize_t Stream::WriteImpl(off_t offset, const char *buf, size_t size)
{
    ssize_t retval;
    if (size == 0) {return 0;}
    // In-order invariant (NFR-7): callers only ever hand bytes to the file at
    // the committed offset.  Anything else is an internal logic error: abort
    // in debug builds, fail the transfer in release -- never write the bytes,
    // as an out-of-order write would corrupt the committed prefix that the
    // journal's watermark (and later the digests) vouch for.
    if (offset != m_offset) {
        assert(offset == m_offset && "Stream::WriteImpl called out of order");
        m_error_buf = "Internal invariant violation: out-of-order commit write.";
        return SFS_ERROR;
    }
    retval = m_fh->write(offset, buf, size);
    if (retval != SFS_ERROR) {
        // The commit hook sees every byte exactly once, in order: this is the
        // digest attachment point (WP-10).  Invoke before advancing so the
        // hook observes (offset == committed offset at time of write).
        if (m_commit_hook && retval > 0) {
            m_commit_hook(offset, buf, static_cast<size_t>(retval));
        }
        m_offset += retval;
    } else {
        std::stringstream ss;
        const char *msg = m_fh->error.getErrText();
        if (!msg || (*msg == '\0')) {msg = "(no error message provided)";}
        ss << msg << " (code=" << m_fh->error.getErrInfo() << ")";
        m_error_buf = ss.str();
    }
    return retval;
}


void
Stream::DumpBuffers() const
{
    m_log.Emsg("Stream::DumpBuffers", "Beginning dump of stream buffers.");
    {
        std::stringstream ss;
        ss << "Committed offset: " << m_offset
           << ", reorder span: " << ReorderSpan();
        m_log.Emsg("Stream::DumpBuffers", ss.str().c_str());
    }
    size_t idx = 0;
    for (const auto &entry : m_buffers) {
        std::stringstream ss;
        ss << "Buffer " << idx << ": Offset=" << entry->GetOffset() << ", Size="
           << entry->GetSize() << ", Capacity=" << entry->GetCapacity();
        m_log.Emsg("Stream::DumpBuffers", ss.str().c_str());
        idx ++;
    }
    m_log.Emsg("Stream::DumpBuffers", "Finish dump of stream buffers.");
}


int
Stream::Read(off_t offset, char *buf, size_t size)
{
    return m_fh->read(offset, buf, size);
}
