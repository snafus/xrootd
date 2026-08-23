# TPCR Progress

**Current focus:** M3 COMPLETE (tag tpcr-m3). Next: WP-12 — scale and soak (T-S1/T-S2, T-P1/T-P2 finals).

| WP | Status | Commit(s) | Tests passing | Notes |
|----|--------|-----------|---------------|-------|
| WP-0 skeleton fork + build | done | a285371dc, 8806c8e08, 70f78f39e, 50b30f07c | 15/15 (xrdhttptpcr-*) | Fork is rename-only vs stock (verified by reverse-sed diff) + BUG-4/5/9. T-U1 full parity deferred to harness (Q-2). |
| WP-1 Stream rework | done | 8bba3eeb8 | 22/22 TPCR, 32/32 incl. stock | CommittedOffset/ReorderSpan surface; BUG-6/7 dispositions in; commit hook ready for WP-10. |
| WP-2 Slab pool | done | (this commit) | 35/35 TPCR+stock; ASan+TSan clean | Pool + tpcr.{blocksize,mempool.max,streams.max} + BUG-10 clamp. Entry wiring at WP-4 (see DECISIONS). |
| WP-3 Response validation | done | fdaac1ffc | 55/55 | Body-start + completion validation; expected/reported split; permanent-class error codes ready for FR-12. |
| WP-4 Range scheduler | done | 2f8bf506d, b8c05c884, 224ecbd27, ad79aba00 | 73/73 unit + 18/18 integration | Pure FSM + curl loop; Multistream deleted; slab wiring done; T-P1 baseline: no regression, ~3x at 4 streams (loopback). |
| WP-5 Degraded state | done | e936dcc1b | 76/76 unit + 29/29 integration; trace_check m1 clean | Full Tier-1 ladder; sticky tpc_status stock bug found+fixed; auth one-shot re-probe; push smoke added. |
| WP-6 Backend semantics matrix | done | (this commit) | backend_matrix.sh 6/6 | POSIX leg green incl. empirical XRD-1 POSC unlink + hasPOSC warning. EC/Ceph/PSS unsupported-until-tested (Q-3). |
| WP-7 Journal + checkpointing | done | (this commit) | 85/85 unit; crash matrix 3/3 | Journal format+CRC, SFS store, Checkpointer with SUB-1 ordering; FR-17/23 on all exit paths; tpcr-journal-dump shipped. |
| WP-8 Resume open path | done | (this commit) | 86/86 unit; resume matrix 10/10 | Full a–e decision tree; ladder; lease 409/steal; GC; SUB-2 table; FR-5 absolute markers; O_EXCL deviation in DECISIONS/Q-4. |
| WP-9 Capability + client-visible | done | 2ebd24ee1 | 33/33 integration; trace_check m2 clean | FR-3 OPTIONS header; FR-6 in-line resumable-from; T-I8 strict grammar checker. |
| WP-10 Digests | done | (this commit) | 92/92 unit; resume matrix 11/11 incl. digest-across-resume | Local seedable adler32 + XrdOucCRC crc32c; SUB-4 checkpoint-atomic; FR-29 gate live end-to-end. |
| WP-11 Tail verify + cksum injection | done | (this commit) | 92/92 unit; resume matrix 12/12; T-I10 3/3 | FR-28 epoch tail verify (tamper caught); FR-27 xattr via FAttr, XRD-2 ordering incl. mtime-binding negative test. |
| WP-12 Scale and soak | todo | | | |
| WP-13 Docs and handback | todo | | | |
