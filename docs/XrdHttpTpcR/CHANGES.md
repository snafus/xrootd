# XrdHttpTpcR: changes versus the stock XrdHttpTpc handler

Forked from `src/XrdHttpTpc/` at v6.1.1 into `src/XrdHttpTpcR/`, producing
`libXrdHttpTPCR-6.so` (entry point `XrdHttpGetExtHandler`, config prefix
`tpcr.`). The stock handler is untouched and both can be packaged side by
side; a server loads one or the other.

## Behavior-preserving guarantees

- **Wire compatibility (CON-4):** COPY semantics, chunked-response and
  perf-marker grammar are byte-compatible with stock (verified against a
  strict gfal/davix-grammar parser in CI). Markers report absolute bytes even
  on resumed transfers, so client progress/stall heuristics are unaffected.
- **Push mode (CON-6):** stock code ported verbatim (plus the bug fixes
  below); no resumption features.
- **Monitoring (FR-32):** `XrdXrootdTpcMon` records emitted as stock.
- All stock `tpc.*` directives keep stock semantics.

## Additive client-visible surface

- OPTIONS: `X-Transfer-Capabilities: resume/1` (when resume is enabled).
- Failure final chunks append informational `resumable-from: <W>`.
- New request header `X-Resume: F` forces a fresh transfer.
- `409 Conflict + Retry-After` when a retried COPY hits a live resume lease.

## Architectural changes (pull mode)

- **Range scheduler replaces Multistream.** A windowed FSM
  (PENDING→ISSUED→RECEIVING→DONE) issues block-sized ranged GETs over reusable
  curl handles; per-range failures are classified (retryable / permanent /
  auth-retry) and retried with jittered exponential backoff; the delivered
  prefix of a failed range is never re-fetched. `streams=1` goes through the
  same scheduler (no separate legacy path).
- **Degraded state (Tier-1 recovery).** When per-range patience is exhausted,
  the handler drops to a single probing connection, revalidates the source by
  re-HEAD, and rides out up to `tpcr.recovery.maxsecs` of zero commit
  progress before admitting failure. Markers keep flowing throughout.
- **Bounded memory.** A server-global slab pool (`tpcr.mempool.max`,
  fair-shared per transfer) replaces per-transfer `streams x 16 x 16 MiB`
  allocation; per-transfer buffering is bounded by `tpcr.window.bytes`.
  Requested stream counts clamp at `tpcr.streams.max`.
- **Cross-session resume (Tier-2, optional).** Committed watermark `W`
  (contiguous bytes written *and* synced) checkpointed to an atomic sidecar
  journal with strict ordering: data sync first, then journal rename. Resume
  revalidates the source (digest > strong ETag > Last-Modified; length
  always), re-verifies the partial's tail against journaled CRC32C epochs,
  and is guarded by a short lease against concurrent retries.
- **End-to-end integrity.** Streaming adler32 (+ CRC32C epochs) computed
  in-order over every committed byte, survives resume via the journal,
  gated against the source's `Repr-Digest` when advertised, and injected
  into the checksum xattr store on success so a later `xrdfs query checksum`
  does not re-read the file.
- **Structured events** for every lifecycle transition
  (`CHECKPOINT`, `RESUME_START/REJECTED`, `DEGRADED_ENTER/EXIT`,
  `RANGE_RETRY`, `LEASE_CONFLICT`, `GC_DISCARD`, ...) — see RUNBOOK.md.

## Stock bugs fixed in the fork

See UPSTREAM.md for the fixes that apply to stock as well: BUG-4 (Move loses
Repr-Digests), BUG-5 (stale error state across handle reuse), BUG-6 (short
direct write double-buffers), BUG-9 (`curl_multi_wait` busy-spin), and the
sticky `tpc_status` HEAD veto. Additionally fixed/superseded within the new
design: BUG-7 (buffer-occupancy admission deadlock, replaced by the window
model), BUG-8 (dead `CURLMOPT_PIPELINING`, handle-count sizing), BUG-10
(unbounded stream memory), BUG-11 (16 MiB zero-fill per block, replaced by
non-zeroed slab recycling).

## New files

Handler: `src/XrdHttpTpcR/` (State, Stream, Scheduler+Core, SlabPool,
Config, Journal, Digest, TPC handler, verbatim PMark/Utils ports), ops tool
`tpcr-journal-dump` (installed to bin/). Tests: `tests/XrdHttpTpcR/` — six
unit suites, a fault-injectable mock HTTP source, five integration/soak
matrices, and `trace_check` (requirement-id traceability gate).
