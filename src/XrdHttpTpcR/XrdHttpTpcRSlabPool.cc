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

#include "XrdHttpTpcRSlabPool.hh"

#include <algorithm>

using namespace TPCR;

SlabPool::SlabPool(size_t slab_size, uint64_t max_bytes)
    : m_slab_size(slab_size), m_max_bytes(max_bytes)
{
}

std::shared_ptr<SlabPool::Client> SlabPool::RegisterClient()
{
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_clients++;
    }
    // The custom deleter unregisters the client when the last reference --
    // whether the transfer's own or one held by an outstanding slab -- drops.
    return std::shared_ptr<Client>(new Client(),
                                   [this](Client *client) {
                                       UnregisterClient();
                                       delete client;
                                   });
}

void SlabPool::UnregisterClient()
{
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_clients > 0) {m_clients--;}
}

SlabPool::Slab SlabPool::Acquire(const std::shared_ptr<Client> &client)
{
    if (!client) {return Slab();}

    std::unique_ptr<char[]> data;
    {
        std::lock_guard<std::mutex> guard(m_mutex);

        // Saturated = the budget does not admit another allocation.  (The
        // budget counts live slabs, free and leased alike: freeing a slab
        // does not shrink the footprint, recycling does not grow it.)
        const bool saturated = m_allocated_bytes + m_slab_size > m_max_bytes;

        // Fair share in slabs, per active client, at least one.  Only
        // enforced when the pool is saturated: with headroom available a
        // busy transfer may run ahead, since others can still allocate.
        const size_t budget_slabs =
            static_cast<size_t>(m_max_bytes / m_slab_size);
        const size_t fair_share =
            std::max<size_t>(1, budget_slabs / std::max<size_t>(1, m_clients));

        if (saturated && client->m_held >= fair_share) {
            // Over-share while saturated: released slabs must reach the
            // transfers below their share, so this one defers.
            return Slab();
        }

        if (!m_free.empty()) {
            data = std::move(m_free.back());
            m_free.pop_back();
            // Deliberately NOT zeroed (BUG-11): the consumer tracks its own
            // valid length and never reads bytes it did not write.
        } else if (!saturated) {
            m_allocated_bytes += m_slab_size;
            // Allocation happens outside the lock would be nicer, but the
            // bookkeeping must be atomic with the budget check; new[] of a
            // slab is rare (steady state recycles) and does not touch pages.
            data.reset(new char[m_slab_size]);
        } else {
            // Saturated and nothing recycled: defer.
            return Slab();
        }

        client->m_held++;
    }
    return Slab(this, client, std::move(data), m_slab_size);
}

void SlabPool::ReturnSlab(std::unique_ptr<char[]> data,
                          const std::shared_ptr<Client> &client)
{
    std::lock_guard<std::mutex> guard(m_mutex);
    m_free.push_back(std::move(data));
    if (client && client->m_held > 0) {client->m_held--;}
}

void SlabPool::Slab::Release()
{
    if (!m_pool || !m_data) {
        m_pool = nullptr;
        m_client.reset();
        return;
    }
    m_pool->ReturnSlab(std::move(m_data), m_client);
    m_pool = nullptr;
    m_client.reset();
    m_capacity = 0;
}

uint64_t SlabPool::BytesAllocated() const
{
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_allocated_bytes;
}

size_t SlabPool::FreeSlabs() const
{
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_free.size();
}

size_t SlabPool::ActiveClients() const
{
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_clients;
}
