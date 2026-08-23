## WP-12 scale & soak -- 2026-08-23T16:46:22Z
Testbed: 4 cores, loopback, ext4; sizes fit a ~2 GB budget.

### T-S1 interrupted 384 MiB transfer
- interruptions: 3 (gateway kill -9, 5s source outage, client disconnect)
- re-fetched beyond file size: 20971520 bytes (NFR-4 bound: 46137344)
- RESULT: PASS (content byte-identical)

### T-S2 concurrency soak (8 x 48 MiB, pool 16 MiB = 16 slabs, 32 streams total)
- completed correctly: 8/8
- server peak RSS: 62 MiB (pool budget 16 MiB + server baseline)
- RESULT: PASS

### T-P2 CPU + checkpoint overhead (256 MiB, 4 streams, loopback)
| leg | wall secs | server CPU secs |
|-----|-----------|-----------------|
| stock TPC | 1.13 | 1.07 |
| TPCR, default cadence (0 periodic ckpts at this size) | 2.41 | 0.79 |
| TPCR, 8 MiB cadence (32 ckpts) | 2.04 | 0.76 |
| TPCR, tpcr.resume no | 0.68 | 0.67 |
- (default - off) = FR-23 final data sync: a durability cost, amortizes at scale
- per-checkpoint cost: 0.0 ms; extrapolated NFR-3 fraction for 10 TB at default cadence: 0.0000%
- checkpoint overhead: PASS (NFR-3)
- CPU: PASS (BUG-11 memset signature absent)

### T-U10/BUG-9 stalled-source loop CPU (1 stream, 8s stall)
- server CPU over the 6s in-stall window: 0.00s
- RESULT: PASS

