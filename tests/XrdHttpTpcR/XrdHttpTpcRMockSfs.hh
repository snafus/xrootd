//------------------------------------------------------------------------------
// Shared test mock: an in-memory XrdSfsFileSystem, just enough surface for
// the JournalStore (stat / newFile / rename / rem) so the Checkpointer can
// be unit-tested end to end (WP-14: sync-poison, epoch pruning).
//------------------------------------------------------------------------------
#ifndef __XRD_TPCR_MOCK_SFS_HH__
#define __XRD_TPCR_MOCK_SFS_HH__

#include "XrdSfs/XrdSfsInterface.hh"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>

class MemorySfs;

// A file handle bound to one path inside a MemorySfs.
class MemorySfsStoreFile : public XrdSfsFile {
public:
  explicit MemorySfsStoreFile(std::map<std::string, std::string> &files)
      : XrdSfsFile("test", 0), m_files(files) {}

  int open(const char *path, XrdSfsFileOpenMode mode, mode_t,
           const XrdSecEntity * = 0, const char * = 0) override {
    std::string name(path ? path : "");
    const size_t query = name.find('?');
    if (query != std::string::npos) {name.resize(query);}
    const bool exists = m_files.count(name) != 0;
    if ((mode & SFS_O_CREAT) || (mode & SFS_O_TRUNC)) {
      m_files[name].clear();
    } else if (!exists) {
      return SFS_ERROR;
    }
    m_path = name;
    return SFS_OK;
  }

  int close() override {m_path.clear(); return SFS_OK;}
  int fctl(const int, const char *, XrdOucErrInfo &) override {return SFS_OK;}
  const char *FName() override {return m_path.c_str();}
  int getMmap(void **addr, off_t &size) override {
    *addr = nullptr; size = 0; return SFS_ERROR;
  }

  XrdSfsXferSize read(XrdSfsFileOffset, XrdSfsXferSize) override {return 0;}
  XrdSfsXferSize read(XrdSfsFileOffset offset, char *buffer,
                      XrdSfsXferSize size) override {
    if (m_path.empty() || offset < 0) {return SFS_ERROR;}
    const std::string &data = m_files[m_path];
    if (size_t(offset) > data.size()) {return SFS_ERROR;}
    const size_t to_copy =
        std::min(size_t(size), data.size() - size_t(offset));
    memcpy(buffer, data.data() + size_t(offset), to_copy);
    return XrdSfsXferSize(to_copy);
  }
  int read(XrdSfsAio *) override {return SFS_ERROR;}

  XrdSfsXferSize write(XrdSfsFileOffset offset, const char *buffer,
                       XrdSfsXferSize size) override {
    if (m_path.empty() || offset < 0) {return SFS_ERROR;}
    std::string &data = m_files[m_path];
    const size_t end = size_t(offset) + size_t(size);
    if (data.size() < end) {data.resize(end);}
    memcpy(&data[size_t(offset)], buffer, size_t(size));
    return size;
  }
  int write(XrdSfsAio *) override {return SFS_ERROR;}

  int stat(struct stat *buf) override {
    if (m_path.empty()) {return SFS_ERROR;}
    memset(buf, 0, sizeof(*buf));
    buf->st_size = off_t(m_files[m_path].size());
    return SFS_OK;
  }
  int sync() override {return SFS_OK;}
  int sync(XrdSfsAio *) override {return SFS_OK;}
  int truncate(XrdSfsFileOffset) override {return SFS_ERROR;}
  int getCXinfo(char cxtype[4], int &cxrsz) override {
    memset(cxtype, 0, 4); cxrsz = 0; return SFS_OK;
  }

private:
  std::map<std::string, std::string> &m_files;
  std::string m_path;
};

class MemorySfs : public XrdSfsFileSystem {
public:
  std::map<std::string, std::string> files;

  XrdSfsDirectory *newDir(char * = 0, int = 0) override {return nullptr;}
  XrdSfsFile *newFile(char * = 0, int = 0) override {
    return new MemorySfsStoreFile(files);
  }

  int chmod(const char *, XrdSfsMode, XrdOucErrInfo &,
            const XrdSecEntity * = 0, const char * = 0) override {
    return SFS_ERROR;
  }
  int exists(const char *, XrdSfsFileExistence &, XrdOucErrInfo &,
             const XrdSecEntity * = 0, const char * = 0) override {
    return SFS_ERROR;
  }
  int fsctl(const int, const char *, XrdOucErrInfo &,
            const XrdSecEntity * = 0) override {
    return SFS_ERROR;
  }
  int getStats(char *, int) override {return SFS_ERROR;}
  const char *getVersion() override {return "memory-sfs";}
  int mkdir(const char *, XrdSfsMode, XrdOucErrInfo &,
            const XrdSecEntity * = 0, const char * = 0) override {
    return SFS_ERROR;
  }
  int prepare(XrdSfsPrep &, XrdOucErrInfo &,
              const XrdSecEntity * = 0) override {
    return SFS_ERROR;
  }
  int rem(const char *path, XrdOucErrInfo &,
          const XrdSecEntity * = 0, const char * = 0) override {
    return files.erase(path ? path : "") ? SFS_OK : SFS_ERROR;
  }
  int remdir(const char *, XrdOucErrInfo &,
             const XrdSecEntity * = 0, const char * = 0) override {
    return SFS_ERROR;
  }
  int rename(const char *oPath, const char *nPath, XrdOucErrInfo &,
             const XrdSecEntity * = 0, const char * = 0,
             const char * = 0) override {
    auto it = files.find(oPath ? oPath : "");
    if (it == files.end()) {return SFS_ERROR;}
    files[nPath ? nPath : ""] = std::move(it->second);
    files.erase(it);
    return SFS_OK;
  }
  int stat(const char *name, struct stat *buf, XrdOucErrInfo &,
           const XrdSecEntity * = 0, const char * = 0) override {
    auto it = files.find(name ? name : "");
    if (it == files.end()) {return SFS_ERROR;}
    memset(buf, 0, sizeof(*buf));
    buf->st_size = off_t(it->second.size());
    return SFS_OK;
  }
  int stat(const char *, mode_t &, XrdOucErrInfo &,
           const XrdSecEntity * = 0, const char * = 0) override {
    return SFS_ERROR;
  }
  int truncate(const char *, XrdSfsFileOffset, XrdOucErrInfo &,
               const XrdSecEntity * = 0, const char * = 0) override {
    return SFS_ERROR;
  }
};

#endif // __XRD_TPCR_MOCK_SFS_HH__
