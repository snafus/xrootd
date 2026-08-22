#undef NDEBUG

// T-U6 (05-TEST-PLAN.md): the server-global slab pool.
//   - The byte budget is honored, including under concurrent acquire/release
//     from many threads (NFR-1, BUG-10).
//   - Recycled slabs are NOT zero-filled (BUG-11): a poison pattern written
//     before release must still be there on re-acquire.
//   - Fair share bounds starvation: with the pool saturated by one client,
//     a second client obtains slabs as soon as any are released.
//   - Acquire never blocks: exhaustion yields an empty slab, not a wait.

#include "XrdHttpTpcR/XrdHttpTpcRSlabPool.hh"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

using TPCR::SlabPool;

TEST(XrdHttpTpcRSlabPoolTests, BudgetIsHonored) {
  // 4 slabs of 1 KiB.
  SlabPool pool(1024, 4 * 1024);
  auto client = pool.RegisterClient();

  std::vector<SlabPool::Slab> held;
  for (int i = 0; i < 4; i++) {
    auto slab = pool.Acquire(client);
    ASSERT_TRUE(static_cast<bool>(slab)) << "slab " << i << " within budget";
    EXPECT_EQ(1024u, slab.Capacity());
    held.push_back(std::move(slab));
  }
  // Budget exhausted: acquire must fail fast, never block or overshoot.
  auto denied = pool.Acquire(client);
  EXPECT_FALSE(static_cast<bool>(denied));
  EXPECT_EQ(4 * 1024u, pool.BytesAllocated());

  // Releasing one slab makes exactly one acquirable again.
  held.pop_back();
  auto again = pool.Acquire(client);
  EXPECT_TRUE(static_cast<bool>(again));
  EXPECT_EQ(4 * 1024u, pool.BytesAllocated()) << "recycle must not grow the footprint";
}

TEST(XrdHttpTpcRSlabPoolTests, RecycledSlabsAreNotZeroed) {
  SlabPool pool(64, 64);  // exactly one slab
  auto client = pool.RegisterClient();

  {
    auto slab = pool.Acquire(client);
    ASSERT_TRUE(static_cast<bool>(slab));
    memset(slab.Data(), 0x5A, slab.Capacity());  // poison
  }  // released back to the free list

  auto slab = pool.Acquire(client);
  ASSERT_TRUE(static_cast<bool>(slab));
  // BUG-11: recycling must not memset the buffer.  (The consumer tracks its
  // own valid length; zeroing 16 MiB per block cycle was pure waste.)
  for (size_t i = 0; i < slab.Capacity(); i++) {
    ASSERT_EQ(0x5A, static_cast<unsigned char>(slab.Data()[i]))
        << "slab was zero-filled on recycle at byte " << i;
  }
}

TEST(XrdHttpTpcRSlabPoolTests, FairShareBoundsStarvation) {
  // 4 slabs; client A grabs everything while alone, then client B arrives.
  SlabPool pool(1024, 4 * 1024);
  auto client_a = pool.RegisterClient();

  std::vector<SlabPool::Slab> held_a;
  for (int i = 0; i < 4; i++) {
    auto slab = pool.Acquire(client_a);
    ASSERT_TRUE(static_cast<bool>(slab));
    held_a.push_back(std::move(slab));
  }

  auto client_b = pool.RegisterClient();
  // Pool saturated and nothing free: B cannot get a slab yet...
  EXPECT_FALSE(static_cast<bool>(pool.Acquire(client_b)));

  // ...but the moment A releases one, it must go to B, not back to A:
  // A holds 3 >= fair share (4 slabs / 2 clients = 2) while saturated.
  held_a.pop_back();
  EXPECT_FALSE(static_cast<bool>(pool.Acquire(client_a)))
      << "over-share client must be denied while saturated";
  auto slab_b = pool.Acquire(client_b);
  EXPECT_TRUE(static_cast<bool>(slab_b))
      << "released slab must be available to the under-share client";
}

TEST(XrdHttpTpcRSlabPoolTests, ClientUnregistrationRestoresShares) {
  SlabPool pool(1024, 4 * 1024);
  auto client_a = pool.RegisterClient();
  auto client_b = pool.RegisterClient();
  EXPECT_EQ(2u, pool.ActiveClients());

  // With B gone, A's fair share covers the whole pool again.
  client_b.reset();
  EXPECT_EQ(1u, pool.ActiveClients());
  std::vector<SlabPool::Slab> held;
  for (int i = 0; i < 4; i++) {
    auto slab = pool.Acquire(client_a);
    EXPECT_TRUE(static_cast<bool>(slab)) << "slab " << i;
    held.push_back(std::move(slab));
  }
}

TEST(XrdHttpTpcRSlabPoolTests, ConcurrentAcquireReleaseHonorsBudget) {
  // Hammer the pool from several threads; the budget must never be exceeded
  // and no acquire may block.  Run under ASan/TSan in the sanitizer builds.
  const size_t slab_size = 256;
  const size_t budget_slabs = 8;
  SlabPool pool(slab_size, slab_size * budget_slabs);

  std::atomic<bool> over_budget{false};
  std::atomic<size_t> total_acquired{0};

  auto worker = [&](unsigned int seed) {
    auto client = pool.RegisterClient();
    std::vector<SlabPool::Slab> held;
    // Deterministic per-thread pseudo-random hold/release pattern.
    unsigned int state = seed * 2654435761u + 1;
    for (int iter = 0; iter < 5000; iter++) {
      state = state * 1664525u + 1013904223u;
      if ((state & 1) && !held.empty()) {
        held.pop_back();  // release
      } else {
        auto slab = pool.Acquire(client);
        if (slab) {
          // Touch the memory: sanitizers catch use-after-free / races.
          slab.Data()[0] = static_cast<char>(iter);
          held.push_back(std::move(slab));
          total_acquired++;
        }
      }
      if (pool.BytesAllocated() > slab_size * budget_slabs) {
        over_budget = true;
      }
    }
  };

  std::vector<std::thread> threads;
  for (unsigned int t = 0; t < 4; t++) {
    threads.emplace_back(worker, t + 1);
  }
  for (auto &thread : threads) {
    thread.join();
  }

  EXPECT_FALSE(over_budget.load()) << "pool exceeded its byte budget";
  EXPECT_GT(total_acquired.load(), 0u);
  // All slabs returned: the whole budget must be acquirable again.
  auto client = pool.RegisterClient();
  std::vector<SlabPool::Slab> drain;
  for (size_t i = 0; i < budget_slabs; i++) {
    auto slab = pool.Acquire(client);
    EXPECT_TRUE(static_cast<bool>(slab)) << "slab " << i << " after drain";
    drain.push_back(std::move(slab));
  }
}
