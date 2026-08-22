
/**
 * The "stream" interface is a simple abstraction of a file handle.
 *
 * The abstraction layer is necessary to do the necessary buffering
 * of multi-stream writes where the underlying filesystem only
 * supports single-stream writes.
 *
 * TPCR rework (WP-1) relative to the stock TPC Stream:
 *
 *  - Construction is seeded with an initial offset so that a resumed
 *    transfer can continue writing at the committed watermark W instead
 *    of byte 0 (FR-20).  All offsets remain absolute file offsets.
 *
 *  - Write() returns the number of bytes *fully accepted* (written to the
 *    file or safely buffered).  The stock code could return a short count
 *    while still buffering the remainder, making the caller re-send bytes
 *    that were already accepted -- a double-write (BUG-6).  Acceptance is
 *    now all-or-error: a remainder that cannot be accepted is a stream
 *    failure, never a short return.
 *
 *  - Buffer occupancy is no longer an admission signal.  The stock design
 *    coupled scheduling, memory and write ordering through a fixed pool of
 *    entries and the AvailableBuffers() count, which both stalled and
 *    hard-failed transfers when out-of-order arrival fragmented ranges
 *    across entries (BUG-7).  Entries are now created on demand and
 *    retired when empty; the scheduler instead governs admission with
 *    CommittedOffset() and ReorderSpan() (the byte distance covered by
 *    data buffered ahead of the committed offset).  Hard memory bounds
 *    arrive with the slab pool (WP-2) and the scheduler window (WP-4).
 *
 *  - WriteImpl() carries the in-order commit hook: every byte handed to
 *    the underlying file passes through it exactly once, at strictly
 *    ascending offsets, by construction.  The streaming digests (WP-10)
 *    attach here; until then the hook is an interface only.
 *
 * Unchanged invariants: data is handed to the file strictly in order
 * (required for HDFS/RADOS-class backends), and writes below the current
 * committed offset are rejected as a logic error.
 */

#ifndef __XRD_TPCR_STREAM_HH__
#define __XRD_TPCR_STREAM_HH__

#include "XrdSfs/XrdSfsInterface.hh"

#include "XrdHttpTpcRSlabPool.hh"

#include <functional>
#include <memory>
#include <vector>
#include <string>

#include <cassert>
#include <cstring>

struct stat;

class XrdSysError;

namespace TPCR {
class Stream {
public:
    // Invoked from WriteImpl for every byte range successfully handed to the
    // underlying file: strictly in-order, no gaps, no duplicates.  This is
    // the digest hook point (WP-10); it must not fail and must not block.
    using CommitHook =
        std::function<void(off_t offset, const char *buffer, size_t size)>;

    // initial_offset seeds the committed offset: a fresh transfer passes 0,
    // a resumed transfer passes the journal's watermark W.  buffer_size is
    // the capacity of each reorder entry and should equal the scheduler's
    // block size, so that one range fits one entry in the common case.
    Stream(std::unique_ptr<XrdSfsFile> fh, off_t initial_offset,
           size_t buffer_size, XrdSysError &log)
        : m_open_for_write(false),
          m_buffer_size(buffer_size),
          m_fh(std::move(fh)),
          m_offset(initial_offset),
          m_log(log)
    {
        m_open_for_write = true;
    }

    ~Stream();

    int Stat(struct stat *);

    int Read(off_t offset, char *buffer, size_t size);

    // Forwards to XrdSfsFile::sync(): the checkpoint's durability step
    // (SUB-1).  On POSIX OSS this is a real fsync (XRD-4).
    int Sync() {return m_fh->sync();}

    // Writes a buffer of a given size to an offset.
    // This will often keep the buffer in memory in to present the underlying
    // filesystem with a single stream of data (required for HDFS); further,
    // it will also buffer to align the writes on a 1MB boundary (required
    // for some RADOS configurations).  When force is set to true, it will
    // skip the buffering and always write (this should only be done at the
    // end of a stream!).
    //
    // Returns the number of bytes fully accepted -- always `size` on
    // success (BUG-6: never a short count with the remainder buffered).
    // On error, returns SFS_ERROR and sets the stream's error message.
    ssize_t Write(off_t offset, const char *buffer, size_t size, bool force);

    // Force the data still held in the re-ordering buffers out to the underlying
    // file handle, even if it results in unaligned or short writes.  Typically
    // only done while shutting down the transfer.
    //
    // The flush is deliberately issued at the current offset of the stream: the
    // offset a given transfer state stopped at is not necessarily the offset the
    // stream has been written up to.  In the multistream case, all the states
    // share this stream, and all but the one that happened to serve the last
    // range end up before it -- flushing at their offset would be rejected as a
    // write to a prior offset.
    //
    // Returns 0 on success; SFS_ERROR on failure.
    ssize_t Flush() {return Write(m_offset, nullptr, 0, true);}

    // The number of bytes contiguously handed to the underlying file so far
    // (absolute offset).  This is the scheduler's commit candidate: the
    // checkpoint engine turns it into the durable watermark W by sync()ing
    // the file before persisting it (SUB-1).  Note the three-counter rule
    // (SUB-3): this is "committed", which is neither "scheduled" nor
    // "received".
    off_t CommittedOffset() const {return m_offset;}

    // The byte distance between the committed offset and the end of the
    // furthest buffered data.  The scheduler bounds this by the configured
    // reorder window when admitting new ranges (NFR-1); it replaces the
    // stock buffer-occupancy admission signal.
    size_t ReorderSpan() const;

    // Install (or clear, by passing nullptr) the in-order commit hook.
    // Must not be changed while writes are in flight.
    void SetCommitHook(CommitHook hook) {m_commit_hook = std::move(hook);}

    // Supplies pooled buffers for the reorder entries (NFR-1, BUG-11).  The
    // source may return an empty Slab ("none available"); the stream then
    // falls back to a private heap buffer rather than losing data -- the
    // scheduler makes that rare by reserving a slab per issued range, and
    // OverflowEntries() exposes how often the fallback fired.  Data already
    // handed to the stream must always find a home (the pool's never-fail
    // contract applies to *scheduling*, not to bytes in hand).
    using SlabSource = std::function<SlabPool::Slab()>;
    void SetSlabSource(SlabSource source) {m_slab_source = std::move(source);}
    size_t OverflowEntries() const {return m_overflow_entries;}

    void DumpBuffers() const;

    // Flush and finalize the stream.  If all data has been sent to the underlying
    // file handle, close() will be invoked on the file handle.
    //
    // Further write operations on this stream will result in an error.
    // If any memory buffers remain, an error occurs.
    //
    // Returns true on success; false otherwise.
    bool Finalize();

    std::string GetErrorMessage() const {return m_error_buf;}

private:

    class Entry {
    public:
        // Heap-backed entry (no pool configured, or pool exhausted).  The
        // buffer is deliberately uninitialized: the entry tracks its own
        // valid length and never reads bytes it did not write (BUG-11: the
        // stock code value-initialized 16 MiB per block cycle).
        explicit Entry(size_t capacity) :
            m_offset(-1),
            m_capacity(capacity),
            m_size(0),
            m_own(new char[capacity]),
            m_data(m_own.get())
        {}

        // Pool-backed entry: the buffer belongs to the server-global slab
        // pool and returns there when the entry is destroyed (NFR-1).
        explicit Entry(SlabPool::Slab slab) :
            m_offset(-1),
            m_capacity(slab.Capacity()),
            m_size(0),
            m_slab(std::move(slab)),
            m_data(m_slab.Data())
        {}

        bool Available() const {return m_offset == -1;}

        // Writes the contents of this buffer out to the stream, returning the
        // number of bytes written (0 if the buffer is not eligible for a write
        // yet) or SFS_ERROR.  On success the buffer is emptied and becomes
        // available again.
        //
        // Only full buffers are written unless force is set: a full buffer is
        // exactly one aligned block, which keeps the writes the underlying
        // filesystem sees block-sized in the common case.  (The stock code
        // had a second reason -- occupancy-based admission -- which is gone.)
        ssize_t Write(Stream &stream, bool force) {
            if (Available() || !CanWrite(stream)) {return 0;}
            if (!force && (m_size != m_capacity)) {
                return 0;
            }
            ssize_t retval = stream.WriteImpl(m_offset, m_data, m_size);
            // Currently the only valid negative value is SFS_ERROR (-1); checking for
            // all negative values to future-proof the code.
            if (retval < 0) {
                return -1;
            }
            if (static_cast<size_t>(retval) == m_size) {
                m_offset = -1;
                m_size = 0;
                return retval;
            }
            // Short write: legal SFS behavior (BUG-6 family).  The stream's
            // committed offset advanced by retval, so drop the written prefix
            // and keep the tail -- it stays contiguous with the committed
            // offset and drains on a later pass.  (The stock code returned an
            // error here, leaving the entry inconsistent with the offset the
            // stream had already advanced.)
            memmove(m_data, m_data + retval, m_size - retval);
            m_offset += retval;
            m_size -= static_cast<size_t>(retval);
            return retval;
        }

        size_t Accept(off_t offset, const char *buf, size_t size) {
            // Validate acceptance criteria: an empty entry accepts data at any
            // offset; a non-empty entry only accepts an exactly-contiguous
            // extension of what it already holds.
            if ((m_offset != -1) && (offset != m_offset + static_cast<ssize_t>(m_size))) {
                return 0;
            }
            size_t to_accept = m_capacity - m_size;
            if (to_accept == 0) {return 0;}
            if (size > to_accept) {
                size = to_accept;
            }

            // Finally, do the copy (the backing buffer is fixed-size
            // and never zero-filled -- BUG-11).
            memcpy(m_data + m_size, buf, size);
            m_size += size;
            if (m_offset == -1) {
                m_offset = offset;
            }
            return size;
        }

        off_t GetOffset() const {return m_offset;}
        size_t GetCapacity() const {return m_capacity;}
        size_t GetSize() const {return m_size;}

    private:

        Entry(const Entry&) = delete;

        bool CanWrite(Stream &stream) const {
            return (m_size > 0) && (m_offset == stream.m_offset);
        }

        off_t m_offset;  // Offset within file that m_data[0] represents.
        size_t m_capacity;
        size_t m_size;  // Number of bytes held in buffer.
        // Exactly one of the two owns the storage; m_data points at it.
        SlabPool::Slab m_slab;           // pool-backed (empty if heap-backed)
        std::unique_ptr<char[]> m_own;   // heap-backed (null if pool-backed)
        char *m_data;
    };

    // Hands bytes to the underlying file at the committed offset, advancing
    // it and driving the commit hook.  Enforces the in-order invariant
    // (NFR-7): a call at any offset other than the committed offset is an
    // internal logic error that fails the stream.
    ssize_t WriteImpl(off_t offset, const char *buffer, size_t size);

    // Copies as much of [buffer, buffer+size) as possible into the buffers that
    // are already holding data and can be extended contiguously.  This is pure
    // bookkeeping: it never touches the underlying filesystem.  Empty entries
    // are skipped: they are handed out explicitly by Write() so that data
    // placement stays deliberate.
    //
    // Returns the number of bytes consumed.
    size_t AcceptIntoBuffers(off_t offset, const char *buffer, size_t size);

    // Writes out every buffer that is contiguous with m_offset, repeating until
    // no further progress is made: flushing one buffer advances m_offset, which
    // can in turn make another buffer writable.  Only completely full buffers
    // are written unless force is set (see Entry::Write).
    //
    // Returns the number of buffers written out, or SFS_ERROR.
    ssize_t FlushBuffers(bool force);

    // Returns an empty entry, creating one if none exists.  Entries are
    // created on demand and trimmed when empty (see Write), so the entry
    // count tracks the actual reorder pressure instead of a fixed pool size.
    Entry *EmptyEntry();

    // True if any entry currently holds data awaiting reordering.
    bool AnyBufferedData() const;

    // Erase empty entries, keeping the vector's footprint proportional to
    // the data actually buffered.  Called when buffered pressure drops.
    void TrimEmptyEntries();

    bool m_open_for_write;
    size_t m_buffer_size;  // Capacity of each reorder entry.
    std::unique_ptr<XrdSfsFile> m_fh;
    off_t m_offset;  // Committed offset: bytes contiguously handed to m_fh.
    std::vector<std::unique_ptr<Entry>> m_buffers;
    XrdSysError &m_log;
    std::string m_error_buf;
    CommitHook m_commit_hook;
    SlabSource m_slab_source;
    size_t m_overflow_entries = 0;  // heap fallbacks; should stay ~0 (NFR-1)
};
}

#endif // __XRD_TPCR_STREAM_HH__
