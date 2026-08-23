# Upstreamable fixes for stock XrdHttpTpc

These defects exist in stock `src/XrdHttpTpc/` at v6.1.1, were found during
the TPCR close reading, and are fixed independently of TPCR's new
architecture — each is a small patch upstream xrootd would want on its own.
File/line references are to the stock sources; the corresponding TPCR fix
(with unit test) can be cherry-picked as the model.

## 1. `State::Move` loses parsed Repr-Digests (BUG-4)

`XrdHttpTpcState.cc`, `State::Move`: the assignment runs backwards —
`other.m_repr_digests = m_repr_digests;` copies the fresh (empty) map *onto*
the source instead of taking from it. Latent in stock today (the multistream
pull validates digests via the separate HEAD before Move) but a landmine for
any caller that relies on digests surviving the move.
Fix: `m_repr_digests = std::move(other.m_repr_digests);`.
TPCR test: `XrdHttpTpcRStateTests.MovePreservesReprDigests`.

## 2. `State::ResetAfterRequest` leaves stale error state (BUG-5)

Resets offsets/status/lengths but not `m_error_buf` / `m_error_code`, so a
reused curl handle carries the previous request's error text into the next
one and failures get misattributed. Fix: clear all transient per-request
fields; write the reset as a checklist comment enumerating them so future
fields are not missed. TPCR test: `XrdHttpTpcRStateTests` reset coverage.

## 3. `Stream::Write` short direct write double-buffers the remainder (BUG-6)

`XrdHttpTpcStream.cc`: on the aligned direct path, a legal short return from
the SFS write is passed back to the caller *and* the remainder is accepted
into the reorder buffers. `State::Write` advances its offset by the short
count, curl sees a short consume and aborts, yet the "unconsumed" bytes are
buffered — a retry then writes them twice. Fix: return bytes *fully
accepted* (disk + buffers); an unacceptable remainder is an error, never a
short return. TPCR test: `XrdHttpTpcRStreamTests` short-write cases against
a mock SfsFile.

## 4. `curl_multi_wait` busy-spin when libcurl holds no fds (BUG-9)

Both stock transfer loops: with zero internal fds, `curl_multi_wait` returns
immediately, producing a hot spin until the next marker period (a full CPU
core per stalled transfer). Fix: use `curl_multi_poll` where available
(`CURL_AT_LEAST_VERSION(7,66,0)`), else sleep a bounded amount when
`fd_count == 0`. Applies to push mode too. TPCR test: live stalled-source
CPU probe in `tests/XrdHttpTpcR/integration/soak_matrix.sh` (T-U10 leg:
0.00 s CPU across a 6 s stall).

## 5. Sticky `tpc_status` vetoes a successful re-HEAD

`PerformHEADRequest`-equivalent path: once a probe records
`rec.tpc_status >= 400`, a *later, successful* HEAD of the same source is
rejected because the stale status is consulted instead of the fresh
response's. Any retry logic that re-probes a recovered source trips over
this. Fix: clear (and restore on failure) the recorded status around a
re-probe, or key the decision on the fresh response only. Found while
implementing degraded-state recovery; fixed in TPCR's
`ProbeSourceValidators`.

## Not proposed upstream

BUG-7 (occupancy-based admission deadlock), BUG-8 (dead
`CURLMOPT_PIPELINING` and handle-count sizing) and BUG-10/11 (memory bounds
and zero-fill) are superseded by TPCR's scheduler/pool architecture rather
than patched in place; porting them to stock would mean back-porting the
architecture. They are documented in the spec package's
`04-BUGS-AND-SUBTLETIES.md`.
