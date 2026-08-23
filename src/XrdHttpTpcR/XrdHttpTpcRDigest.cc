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

#include "XrdHttpTpcRDigest.hh"

#include "XrdOuc/XrdOucCRC.hh"

#include <cassert>
#include <cstdio>

using namespace TPCR;

namespace {

// Standard adler32 (zlib-derived): two running 16-bit sums modulo the
// largest prime below 2^16, with deferred modulo over NMAX-byte blocks
// (the largest n such that 255n(n+1)/2 + (n+1)(BASE-1) fits 32 bits).
const uint32_t kAdlerBase = 65521;
const size_t kAdlerNmax = 5552;

uint32_t AdlerUpdate(uint32_t adler, const unsigned char *data, size_t size)
{
    uint32_t s1 = adler & 0xffff;
    uint32_t s2 = (adler >> 16) & 0xffff;
    while (size > 0) {
        const size_t block = size < kAdlerNmax ? size : kAdlerNmax;
        for (size_t i = 0; i < block; i++) {
            s1 += data[i];
            s2 += s1;
        }
        s1 %= kAdlerBase;
        s2 %= kAdlerBase;
        data += block;
        size -= block;
    }
    return (s2 << 16) | s1;
}

// The serialized state: version, coverage, adler, open-epoch start + crc.
// Fixed 29-byte little-endian layout inside the journal's opaque blob.
const uint8_t kStateVersion = 1;
const size_t kStateBytes = 1 + 8 + 4 + 8 + 4;

void PutU32(std::string &out, uint32_t value) {
    for (int i = 0; i < 4; i++) {out.push_back(char((value >> (8 * i)) & 0xff));}
}
void PutU64(std::string &out, uint64_t value) {
    for (int i = 0; i < 8; i++) {out.push_back(char((value >> (8 * i)) & 0xff));}
}
uint32_t GetU32(const unsigned char *bytes) {
    uint32_t value = 0;
    for (int i = 3; i >= 0; i--) {value = (value << 8) | bytes[i];}
    return value;
}
uint64_t GetU64(const unsigned char *bytes) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; i--) {value = (value << 8) | bytes[i];}
    return value;
}

} // namespace

void TransferDigests::Update(off_t offset, const char *data, size_t size)
{
    // NFR-7: the commit hook guarantees in-order, exactly-once delivery; a
    // violation here means the digests no longer describe [0, covered), so
    // they are poisoned rather than silently wrong.  The completion gate
    // turns a poisoned digest into a transfer failure.
    if (offset != m_covered) {
        assert(offset == m_covered && "digest update out of order");
        m_poisoned = true;
        return;
    }
    if (m_poisoned || size == 0) {return;}
    const auto *bytes = reinterpret_cast<const unsigned char *>(data);
    m_adler = AdlerUpdate(m_adler, bytes, size);
    m_epoch_crc = XrdOucCRC::Calc32C(data, size, m_epoch_crc);
    m_covered += static_cast<off_t>(size);
}

JournalRecord::EpochDigest TransferDigests::CloseEpoch()
{
    JournalRecord::EpochDigest epoch;
    epoch.offset = static_cast<uint64_t>(m_epoch_start);
    epoch.length = static_cast<uint64_t>(m_covered - m_epoch_start);
    epoch.crc32c = m_epoch_crc;
    m_epoch_start = m_covered;
    m_epoch_crc = 0;
    return epoch;
}

std::string TransferDigests::Serialize() const
{
    std::string out;
    out.reserve(kStateBytes);
    out.push_back(char(kStateVersion));
    PutU64(out, uint64_t(m_covered));
    PutU32(out, m_adler);
    PutU64(out, uint64_t(m_epoch_start));
    PutU32(out, m_epoch_crc);
    return out;
}

bool TransferDigests::Restore(const std::string &blob, off_t expected_covered,
                              std::string &err)
{
    if (blob.size() != kStateBytes) {
        err = "digest state has unexpected size";
        return false;
    }
    const auto *bytes = reinterpret_cast<const unsigned char *>(blob.data());
    if (bytes[0] != kStateVersion) {
        err = "digest state version not supported";
        return false;
    }
    const uint64_t covered = GetU64(bytes + 1);
    const uint32_t adler = GetU32(bytes + 9);
    const uint64_t epoch_start = GetU64(bytes + 13);
    const uint32_t epoch_crc = GetU32(bytes + 21);
    // SUB-4: the state must describe exactly the journal's watermark; a
    // checkpoint always closes the open epoch, so epoch_start == covered.
    if (off_t(covered) != expected_covered || epoch_start != covered) {
        err = "digest state does not match the journal watermark";
        return false;
    }
    m_covered = off_t(covered);
    m_adler = adler;
    m_epoch_start = off_t(epoch_start);
    m_epoch_crc = epoch_crc;
    m_poisoned = false;
    return true;
}

std::string TransferDigests::AdlerHex() const
{
    char buffer[9];
    snprintf(buffer, sizeof(buffer), "%08x", m_adler);
    return buffer;
}

uint32_t TransferDigests::CombineAdler(uint32_t adler_a, uint32_t adler_b,
                                       uint64_t len_b)
{
    // Standard adler32_combine: shift A's s1 contribution into B's window.
    const uint32_t rem = uint32_t(len_b % kAdlerBase);
    uint32_t s1_a = adler_a & 0xffff;
    uint32_t s2_a = (adler_a >> 16) & 0xffff;
    uint32_t s1_b = adler_b & 0xffff;
    uint32_t s2_b = (adler_b >> 16) & 0xffff;

    // s1 of the concatenation: B's s1 already contains its own +1 seed, so
    // one seed must come back out; s2 gains A's s1 propagated over B's
    // length (rem = len_b mod BASE), minus the seed's rem contribution.
    uint32_t s1 = (s1_a + s1_b + kAdlerBase - 1) % kAdlerBase;
    uint32_t s2 = (uint32_t)(((uint64_t)rem * s1_a) % kAdlerBase);
    s2 = (s2 + s2_a + s2_b + 2 * kAdlerBase - rem) % kAdlerBase;
    return (s2 << 16) | s1;
}
