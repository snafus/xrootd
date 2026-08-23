//------------------------------------------------------------------------------
// tpcr-journal-dump: inspect a TPCR journal sidecar (ops runbook tool,
// shipped per WP-13).  Reads the file directly from the local filesystem
// (operators run it on the storage node) and prints every field, or the
// precise reason the record is invalid -- the same defensive parser the
// server uses (SUB-9), so "this tool rejects it" == "the server rejects it".
//
// Usage: tpcr-journal-dump <journal-file> [...]
// Exit:  0 all records valid; 1 any invalid/unreadable; 2 usage.
//------------------------------------------------------------------------------

#include "XrdHttpTpcR/XrdHttpTpcRJournal.hh"

#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>

using TPCR::JournalRecord;

namespace {

std::string FormatTime(int64_t seconds)
{
    if (seconds <= 0) {return "(unset)";}
    char buffer[64];
    time_t when = time_t(seconds);
    struct tm parts;
    gmtime_r(&when, &parts);
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%SZ", &parts);
    return buffer;
}

std::string FormatOwner(const std::array<unsigned char, 16> &owner)
{
    std::ostringstream out;
    for (unsigned char byte : owner) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", byte);
        out << hex;
    }
    return out.str();
}

int DumpOne(const char *path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        fprintf(stderr, "%s: cannot open\n", path);
        return 1;
    }
    // Plain size-then-read (istreambuf_iterator trips a GCC 13
    // -Wnull-dereference false positive in <streambuf> at -O2).
    in.seekg(0, std::ios::end);
    const std::streamoff len = in.tellg();
    in.seekg(0, std::ios::beg);
    std::string raw;
    if (len > 0) {
        raw.resize(static_cast<size_t>(len));
        in.read(&raw[0], static_cast<std::streamsize>(len));
    }
    if (!in) {
        fprintf(stderr, "%s: read failed\n", path);
        return 1;
    }
    JournalRecord record;
    std::string err;
    if (!JournalRecord::Parse(raw.data(), raw.size(), record, err)) {
        printf("%s: INVALID (%s)\n", path, err.c_str());
        return 1;
    }
    const time_t now = time(NULL);
    printf("%s: valid TPCR journal (format v%u, %zu bytes)\n", path,
           JournalRecord::kFormatVersion, raw.size());
    printf("  committed watermark W : %lld\n", (long long)record.committed);
    printf("  content length        : %lld\n",
           (long long)record.validators.content_length);
    printf("  source URL            : %s\n", record.source_url.c_str());
    printf("  ETag                  : %s\n",
           record.validators.etag.empty() ? "(none)"
                                          : record.validators.etag.c_str());
    printf("  Last-Modified         : %s\n",
           record.validators.last_modified.empty()
               ? "(none)" : record.validators.last_modified.c_str());
    for (const auto &[name, value] : record.validators.repr_digests) {
        printf("  Repr-Digest           : %s=%s\n", name.c_str(), value.c_str());
    }
    printf("  lease owner           : %s\n",
           FormatOwner(record.lease_owner).c_str());
    printf("  lease expiry          : %s (%s)\n",
           FormatTime(record.lease_expiry).c_str(),
           record.LeaseLive(now) ? "LIVE" : "expired");
    printf("  created / updated     : %s / %s (age %llds)\n",
           FormatTime(record.created).c_str(),
           FormatTime(record.updated).c_str(),
           (long long)record.Age(now));
    printf("  digest state          : %zu bytes%s\n",
           record.digest_state.size(),
           record.digest_state.empty() ? " (none)" : "");
    printf("  epoch digests         : %zu\n", record.epochs.size());
    printf("  block size / streams  : %llu / %u (informational)\n",
           (unsigned long long)record.block_size, record.streams);
    printf("  attempts              : %u\n", record.attempts);
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <journal-file> [...]\n", argv[0]);
        return 2;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (DumpOne(argv[i]) != 0) {status = 1;}
        if (i + 1 < argc) {printf("\n");}
    }
    return status;
}
