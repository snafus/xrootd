//------------------------------------------------------------------------------
// Shared test mock for the TPCR unit tests.
//------------------------------------------------------------------------------
#ifndef __XRD_TPCR_MOCK_SFSFILE_HH__
#define __XRD_TPCR_MOCK_SFSFILE_HH__

#include "XrdSfs/XrdSfsInterface.hh"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

// Minimal XrdSfsFile implementation used to observe which writes the Stream
// reordering layer sends to the backing filesystem.
class MemorySfsFile : public XrdSfsFile {
public:
  MemorySfsFile() : XrdSfsFile("test", 0) {}

  int open(const char *, XrdSfsFileOpenMode, mode_t,
           const XrdSecEntity * = 0, const char * = 0) override {
    return SFS_OK;
  }

  int close() override {
    return SFS_OK;
  }

  int fctl(const int, const char *, XrdOucErrInfo &) override {
    return SFS_OK;
  }

  const char *FName() override {
    return "memory";
  }

  int getMmap(void **addr, off_t &size) override {
    *addr = nullptr;
    size = 0;
    return SFS_ERROR;
  }

  XrdSfsXferSize read(XrdSfsFileOffset, XrdSfsXferSize) override {
    return 0;
  }

  XrdSfsXferSize read(XrdSfsFileOffset offset, char *buffer,
                      XrdSfsXferSize size) override {
    if (offset < 0 || static_cast<size_t>(offset) > m_data.size()) {
      return SFS_ERROR;
    }
    size_t available = m_data.size() - static_cast<size_t>(offset);
    size_t to_copy = std::min(static_cast<size_t>(size), available);
    if (!to_copy) {
      return 0;
    }
    memcpy(buffer, &m_data[static_cast<size_t>(offset)], to_copy);
    return static_cast<XrdSfsXferSize>(to_copy);
  }

  int read(XrdSfsAio *) override {
    return SFS_ERROR;
  }

  XrdSfsXferSize write(XrdSfsFileOffset offset, const char *buffer,
                       XrdSfsXferSize size) override {
    if (offset < 0 || m_fail_writes) {
      return SFS_ERROR;
    }
    if (!size) {
      return 0;
    }
    // Emulate a short-writing backend when configured: accepting fewer bytes
    // than offered is legal XrdSfsFile behavior (BUG-6 coverage).
    size_t to_accept = static_cast<size_t>(size);
    if (m_max_write && to_accept > m_max_write) {
      to_accept = m_max_write;
    }
    size_t begin = static_cast<size_t>(offset);
    size_t end = begin + to_accept;
    if (m_data.size() < end) {
      m_data.resize(end);
    }
    memcpy(&m_data[begin], buffer, to_accept);
    m_writes.emplace_back(offset, static_cast<XrdSfsXferSize>(to_accept));
    return static_cast<XrdSfsXferSize>(to_accept);
  }

  int write(XrdSfsAio *) override {
    return SFS_ERROR;
  }

  int stat(struct stat *buf) override {
    memset(buf, 0, sizeof(*buf));
    buf->st_size = m_data.size();
    return SFS_OK;
  }

  int sync() override {
    if (m_fail_syncs > 0) {
      m_fail_syncs--;
      return SFS_ERROR;
    }
    return SFS_OK;
  }

  int sync(XrdSfsAio *) override {
    return sync();
  }

  int truncate(XrdSfsFileOffset size) override {
    if (size < 0) {
      return SFS_ERROR;
    }
    m_data.resize(static_cast<size_t>(size));
    return SFS_OK;
  }

  int getCXinfo(char cxtype[4], int &cxrsz) override {
    memset(cxtype, 0, 4);
    cxrsz = 0;
    return SFS_OK;
  }

  const std::vector<char> &Data() const {
    return m_data;
  }

  const std::vector<std::pair<XrdSfsFileOffset, XrdSfsXferSize>> &Writes() const {
    return m_writes;
  }

  // Total bytes the backing file has accepted; if this ever exceeds the file
  // size for gap-free content, something was written twice.
  size_t BytesWritten() const {
    size_t total = 0;
    for (const auto &w : m_writes) {
      total += static_cast<size_t>(w.second);
    }
    return total;
  }

  // Makes every subsequent write() fail, to check error propagation.
  void FailWrites() {
    m_fail_writes = true;
  }

  // Makes the next `count` sync() calls fail -- the WP-14/C1 fsyncgate
  // scenario: one transient failure, then "success" that proves nothing.
  void FailSyncs(int count) {
    m_fail_syncs = count;
  }

  // Caps the number of bytes a single write() call accepts (0 = no cap).
  void SetMaxWriteSize(size_t max_write) {
    m_max_write = max_write;
  }

  // Pre-populates the file, emulating a partial left by a previous session.
  void Preload(const std::vector<char> &data) {
    m_data = data;
  }

private:
  std::vector<char> m_data;
  std::vector<std::pair<XrdSfsFileOffset, XrdSfsXferSize>> m_writes;
  bool m_fail_writes = false;
  int m_fail_syncs = 0;
  size_t m_max_write = 0;
};


#endif // __XRD_TPCR_MOCK_SFSFILE_HH__
