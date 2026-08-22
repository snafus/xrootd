//------------------------------------------------------------------------------
// This file is part of XrdHttpTpcR: the resumable HTTP-TPC handler.
//
// XRootD is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// XRootD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with XRootD.  If not, see <http://www.gnu.org/licenses/>.
//------------------------------------------------------------------------------

#include "XrdHttpTpcRConfig.hh"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <sstream>

using namespace TPCR;

namespace {

// Strict unsigned decimal parse of value[0, len); no sign, no whitespace.
bool ParseDigits(const std::string &value, size_t len, uint64_t &out)
{
    if (len == 0 || len > value.size()) {return false;}
    uint64_t result = 0;
    for (size_t i = 0; i < len; i++) {
        if (!isdigit(static_cast<unsigned char>(value[i]))) {return false;}
        uint64_t digit = static_cast<uint64_t>(value[i] - '0');
        if (result > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
            return false;  // overflow
        }
        result = result * 10 + digit;
    }
    out = result;
    return true;
}

} // namespace

bool Config::ParseUnsigned(const std::string &value, uint64_t &out)
{
    return ParseDigits(value, value.size(), out);
}

bool Config::ParseSize(const std::string &value, uint64_t &out)
{
    if (value.empty()) {return false;}
    uint64_t multiplier = 1;
    size_t digits = value.size();
    switch (value.back()) {
        case 'k': case 'K': multiplier = 1024ULL; digits--; break;
        case 'm': case 'M': multiplier = 1024ULL * 1024; digits--; break;
        case 'g': case 'G': multiplier = 1024ULL * 1024 * 1024; digits--; break;
        default: break;
    }
    uint64_t base;
    if (!ParseDigits(value, digits, base)) {return false;}
    if (base > std::numeric_limits<uint64_t>::max() / multiplier) {return false;}
    out = base * multiplier;
    return true;
}

bool Config::ParseDuration(const std::string &value, uint64_t &out_seconds)
{
    if (value.empty()) {return false;}
    uint64_t multiplier = 1;
    size_t digits = value.size();
    switch (value.back()) {
        case 's': digits--; break;
        case 'm': multiplier = 60; digits--; break;
        case 'h': multiplier = 3600; digits--; break;
        default: break;
    }
    uint64_t base;
    if (!ParseDigits(value, digits, base)) {return false;}
    if (base > std::numeric_limits<uint64_t>::max() / multiplier) {return false;}
    out_seconds = base * multiplier;
    return true;
}

bool Config::ParseBool(const std::string &value, bool &out)
{
    if (value == "1" || value == "yes" || value == "true" || value == "on") {
        out = true;
        return true;
    }
    if (value == "0" || value == "no" || value == "false" || value == "off") {
        out = false;
        return true;
    }
    return false;
}

size_t Config::ClampStreams(long requested, bool &clamped) const
{
    clamped = false;
    if (requested <= 0) {return 1;}  // 0 = client asks for the default.
    if (static_cast<size_t>(requested) > streams_max) {
        clamped = true;
        return streams_max;
    }
    return static_cast<size_t>(requested);
}

bool Config::Set(const std::string &directive, const std::string &value,
                 std::string &err)
{
    std::ostringstream bad;
    if (directive == "tpcr.blocksize") {
        uint64_t size;
        // Lower bound 1 MiB: the Stream's direct-write path is MB-aligned and
        // slabs below that would defeat it.  Upper bound 1 GiB as a sanity
        // stop against fat-fingered budgets.
        if (!ParseSize(value, size) || size < (1ULL << 20) || size > (1ULL << 30)) {
            bad << directive << " value '" << value
                << "' is not a valid size in [1m, 1g]";
            err = bad.str();
            return false;
        }
        block_size = static_cast<size_t>(size);
        return true;
    }
    if (directive == "tpcr.mempool.max") {
        uint64_t size;
        if (!ParseSize(value, size) || size == 0) {
            bad << directive << " value '" << value << "' is not a valid size";
            err = bad.str();
            return false;
        }
        mempool_max = size;
        return true;
    }
    if (directive == "tpcr.streams.max") {
        uint64_t count;
        if (!ParseUnsigned(value, count) || count == 0 || count > 1024) {
            bad << directive << " value '" << value
                << "' is not a valid stream count in [1, 1024]";
            err = bad.str();
            return false;
        }
        streams_max = static_cast<size_t>(count);
        return true;
    }
    // FR-30: unknown tpcr.* directives are fatal at init -- a typo silently
    // falling back to a default is exactly the failure mode this prevents.
    bad << "unknown tpcr directive '" << directive << "'";
    err = bad.str();
    return false;
}
