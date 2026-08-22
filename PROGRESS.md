# TPCR Progress

**Current focus:** WP-4 — range scheduler (FSM, retry classification, backoff, ConfigureHandle, slab wiring).

| WP | Status | Commit(s) | Tests passing | Notes |
|----|--------|-----------|---------------|-------|
| WP-0 skeleton fork + build | done | a285371dc, 8806c8e08, 70f78f39e, 50b30f07c | 15/15 (xrdhttptpcr-*) | Fork is rename-only vs stock (verified by reverse-sed diff) + BUG-4/5/9. T-U1 full parity deferred to harness (Q-2). |
| WP-1 Stream rework | done | 8bba3eeb8 | 22/22 TPCR, 32/32 incl. stock | CommittedOffset/ReorderSpan surface; BUG-6/7 dispositions in; commit hook ready for WP-10. |
| WP-2 Slab pool | done | (this commit) | 35/35 TPCR+stock; ASan+TSan clean | Pool + tpcr.{blocksize,mempool.max,streams.max} + BUG-10 clamp. Entry wiring at WP-4 (see DECISIONS). |
| WP-3 Response validation | done | fdaac1ffc | 55/55 | Body-start + completion validation; expected/reported split; permanent-class error codes ready for FR-12. |
| WP-4 Range scheduler | todo | | | |
| WP-5 Degraded state | todo | | | |
| WP-6 Backend semantics matrix | todo | | | |
| WP-7 Journal + checkpointing | todo | | | |
| WP-8 Resume open path | todo | | | |
| WP-9 Capability + client-visible | todo | | | |
| WP-10 Digests | todo | | | |
| WP-11 Tail verify + cksum injection | todo | | | |
| WP-12 Scale and soak | todo | | | |
| WP-13 Docs and handback | todo | | | |
