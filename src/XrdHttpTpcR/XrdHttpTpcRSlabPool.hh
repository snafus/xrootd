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

#ifndef __XRD_TPCR_SLABPOOL_HH__
#define __XRD_TPCR_SLABPOOL_HH__

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace TPCR {

// Server-global, byte-budgeted pool of fixed-size buffers ("slabs") that back
// the Stream reorder entries (NFR-1; dispositions of BUG-10 and BUG-11).
//
// Design rules, from 02-ARCHITECTURE §7:
//
//  - Slabs are all the same size (the configured block size), so recycling is
//    trivial and fragmentation impossible.
//
//  - The pool never exceeds its byte budget (`tpcr.mempool.max`), globally
//    across all transfers on the server.
//
//  - Acquire() NEVER blocks and NEVER fails the transfer: when the budget is
//    exhausted (or the caller is over its fair share while the pool is
//    saturated), it returns an empty Slab and the caller defers scheduling
//    instead -- backpressure, not failure.
//
//  - Recycled slabs are NOT zeroed (BUG-11: the stock code memset 16 MiB per
//    block cycle).  Consumers track their own valid-byte count and must never
//    read beyond what they wrote.
//
//  - Fair share: with the pool saturated, a transfer already holding at least
//    (budget / slab_size) / active_clients slabs is denied so that released
//    slabs reach the transfers below their share.  This bounds starvation:
//    every client is always entitled to at least one slab's worth of
//    eventual service.
//
// Thread model: transfers run on their own worker threads (NFR-6); all pool
// state is guarded by one mutex.  Acquire/release rates are per-range, not
// per-byte, so contention is negligible.
class SlabPool {
public:
    SlabPool(size_t slab_size, uint64_t max_bytes);
    ~SlabPool() = default;

    SlabPool(const SlabPool &) = delete;
    SlabPool &operator=(const SlabPool &) = delete;

    // Registration handle for fair-share accounting: one per transfer, kept
    // for the transfer's lifetime.  Created via RegisterClient(); slabs hold
    // a reference, so a client outlives its last slab automatically.
    class Client {
    private:
        friend class SlabPool;
        size_t m_held = 0;  // slabs currently held; guarded by the pool mutex
    };

    // A leased slab.  Movable, empty-constructible; returns its buffer to the
    // pool's free list on destruction.  Contents are whatever the previous
    // holder wrote (deliberately not zeroed -- BUG-11).
    class Slab {
    public:
        Slab() = default;
        ~Slab() {Release();}

        Slab(Slab &&other) noexcept {*this = std::move(other);}
        Slab &operator=(Slab &&other) noexcept {
            if (this != &other) {
                Release();
                m_pool = other.m_pool;
                m_client = std::move(other.m_client);
                m_data = std::move(other.m_data);
                m_capacity = other.m_capacity;
                other.m_pool = nullptr;
                other.m_capacity = 0;
            }
            return *this;
        }

        Slab(const Slab &) = delete;
        Slab &operator=(const Slab &) = delete;

        explicit operator bool() const {return m_data != nullptr;}
        char *Data() {return m_data.get();}
        size_t Capacity() const {return m_capacity;}

        // Explicit early return to the pool (destructor does the same).
        void Release();

    private:
        friend class SlabPool;
        Slab(SlabPool *pool, std::shared_ptr<Client> client,
             std::unique_ptr<char[]> data, size_t capacity)
            : m_pool(pool), m_client(std::move(client)),
              m_data(std::move(data)), m_capacity(capacity) {}

        SlabPool *m_pool = nullptr;
        std::shared_ptr<Client> m_client;
        std::unique_ptr<char[]> m_data;
        size_t m_capacity = 0;
    };

    // Registers a transfer with the pool.  The returned handle participates
    // in the fair-share computation until it (and every slab it acquired)
    // is destroyed.
    std::shared_ptr<Client> RegisterClient();

    // Tries to lease a slab to `client`.  Returns an empty Slab when the
    // caller should defer (budget exhausted, or caller over fair share while
    // the pool is saturated).  Never blocks.
    Slab Acquire(const std::shared_ptr<Client> &client);

    // Observability (tests and debug logging).
    uint64_t BytesAllocated() const;
    size_t FreeSlabs() const;
    size_t ActiveClients() const;

private:
    friend class Slab;

    // Called by Slab on release: recycle the buffer, credit the client.
    void ReturnSlab(std::unique_ptr<char[]> data, const std::shared_ptr<Client> &client);

    // Called when a Client's registration drops (via shared_ptr deleter).
    void UnregisterClient();

    const size_t m_slab_size;
    const uint64_t m_max_bytes;

    mutable std::mutex m_mutex;
    std::vector<std::unique_ptr<char[]>> m_free;  // recycled, never zeroed
    uint64_t m_allocated_bytes = 0;  // total live slab bytes (free + leased)
    size_t m_clients = 0;
};

} // namespace TPCR

#endif // __XRD_TPCR_SLABPOOL_HH__
