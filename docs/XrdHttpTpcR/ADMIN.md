# XrdHttpTpcR administrator guide

TPCR (`libXrdHttpTPCR-6.so`) is a drop-in alternative to the stock HTTP
third-party-copy handler that makes large pull-mode transfers survivable: it
streams the file as parallel ranged reads through a memory-bounded scheduler,
rides out transient source faults in-session, and (optionally) checkpoints
durable progress to a sidecar journal so a crashed or killed transfer resumes
from the last fsync'd byte instead of restarting. Streaming checksums carried
across the resume guarantee the finished file is bit-identical; the handler
never reports success unless that is proven.

Wire compatibility is absolute: existing FTS/gfal2/davix clients and any
HTTP/WebDAV source work unchanged. Everything TPCR adds is additive (an
OPTIONS capability header, an informational `resumable-from:` field on failure
chunks, an optional `X-Resume: F` client header). Push mode is the stock code,
unchanged and without resumption.

## Building and deploying

TPCR builds as part of the normal xrootd build — no extra switches; it is
compiled whenever HTTP support is (libcurl present):

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
```

Artifacts: `build/lib/libXrdHttpTPCR-6.so` and `build/bin/tpcr-journal-dump`.
`cmake --install build` places them in the standard libdir/bindir.

**RPM:** the shipped `xrootd.spec` packages `libXrdHttpTPCR-6.so` in
`xrootd-server-libs` and `tpcr-journal-dump` in `xrootd-server` — the same
subpackages that carry the stock TPC handler and server tools, so a plain

```
./gen-tarball.sh && rpmbuild -tb xrootd-*.tar.gz
```

on an EL build host produces RPMs that include TPCR; deploying them on a
gateway that already runs xrootd-server adds the handler with no new
package to track. **Debian:** the `debian/` packaging includes the same two
artifacts in `xrootd-server-plugins` and `xrootd-server`.

Rolling out: install the updated server packages on the HTTP gateways, add
the `http.exthandler` line (below) plus any `tpcr.*` tuning, and restart
xrootd. Rollback is reverting the config line — the stock handler is still
in the same package.

## Loading the handler

```
http.exthandler xrdtpcr /usr/lib64/libXrdHttpTPCR-6.so
```

- On servers **without TLS configured**, the `+notls` marker is required or
  XrdHttp silently skips the handler:
  `http.exthandler xrdtpcr +notls /usr/lib64/libXrdHttpTPCR-6.so`
- Load either TPCR **or** the stock `libXrdHttpTPC` handler, not both: they
  claim the same COPY/OPTIONS verbs. Rollback is a one-line config change.
- All stock `tpc.*` directives (`tpc.allow`, `tpc.trace`, ...) are honored
  with stock semantics. For loopback/lab testing remember
  `tpc.allow local` and `tpc.allow private`.

## Configuration reference (`tpcr.*`)

All directives are optional; defaults hold a correctness-first posture. An
unknown `tpcr.` directive or invalid value **fails server startup** (fail
fast, FR-30). Sizes accept `k`/`m`/`g` (powers of 1024); durations accept
`s` (default), `m`, `h`.

| Directive | Default | Valid range | Meaning |
|---|---|---|---|
| `tpcr.blocksize` | `16m` | `[1m, 1g]` | Range/block size for pull transfers and the slab size of the global buffer pool. |
| `tpcr.mempool.max` | `4g` | any size | Server-global byte budget of the slab pool. Transfers beyond the budget fall back to window-bounded private buffers, so this bounds *pooled* memory, not correctness. |
| `tpcr.streams.max` | `16` | `[1, 1024]` | Cap on the client's `X-Number-Of-Streams`. Higher requests are clamped (logged as `STREAMS_CLAMPED`), never rejected. |
| `tpcr.window.bytes` | `256m` | `[1m, 64g]` | Reorder/admission window: ranges are only scheduled within `[committed, committed + window]`, bounding per-transfer buffering and re-fetch after a crash. |
| `tpcr.retry.max` | `5` | `[0, 100]` | Per-range attempt cap before the transfer escalates to the degraded state. |
| `tpcr.range.timeout` | `60` | `[5s, 1h]` | Seconds without delivery progress before an in-flight range is cancelled and re-queued. |
| `tpcr.resume` | `true` | boolean | Cross-session resume master switch. **Turn off on deployments without a shared filesystem** (see below). |
| `tpcr.journal.suffix` | `.xrdtpcr` | starts with `.`, no `/`, ≤64 chars | Sidecar journal suffix (journal lives next to the destination file). |
| `tpcr.checkpoint.bytes` | `4g` | `>= 1m` | Checkpoint every this many committed bytes... |
| `tpcr.checkpoint.secs` | `60` | `[5s, 1h]` | ...or this many seconds, whichever comes first. Also sets the lease term (2x this value). |
| `tpcr.validators.require` | `strong` | `strong` \| `length-only` | Resume validator policy. `strong` refuses to resume when the source offers neither a digest, strong ETag, nor Last-Modified. `length-only` is for known-immutable sources only. |
| `tpcr.gc.age` | `24h` | `>= 10s` | Lazy GC: a journal older than this is discarded by the next COPY that encounters it. Tune to your orchestrator's retry horizon. |
| `tpcr.verify.tailbytes` | `1g` | any size | On resume, re-read and CRC-verify this many bytes below the watermark against the journal's epoch digests. `0` disables — see the multi-tenant warning. |
| `tpcr.recovery.maxsecs` | `120` | `[5s, 24h]` | Degraded-state budget: seconds of zero commit progress the handler may ride through a total source outage before admitting failure. Keep **below** your orchestrator's marker-stall timeout. |

## Cross-session resume: deployment prerequisites

Resume state is the sidecar journal (`<dest><suffix>`), *never* the partial
file's length. For a resume to be possible the retried COPY must land where
both the partial file and its journal are visible:

- **Shared-filesystem deployments only** (all gateways export the same
  namespace over POSIX). On anything else, set `tpcr.resume no`; the handler
  then behaves like a hardened stock TPC (in-session recovery still works).
- **Supported backends:** POSIX OSS is tested and supported. **EC (XrdEC),
  CephFS, and proxy/PSS deployments are unsupported for resume until tested**
  — the `tests/XrdHttpTpcR/integration/backend_matrix.sh` harness exists to
  qualify them; run it (and a POSC leg) against a candidate deployment first.
- **POSC (`ofs.persist`)**: persist-on-successful-close *unlinks* partials
  when a session dies, which deletes exactly the file resume needs. TPCR logs
  a startup warning when POSC is active; resume will always take the
  fresh-transfer path there. Choose one: POSC or resume.
- A dead session's journal holds a **lease** for 2x `tpcr.checkpoint.secs`.
  A retry inside that window gets `409 + Retry-After` and self-heals on the
  next attempt; orchestrator retry backoffs longer than ~2 minutes (at
  defaults) never see it. The lease is the writer exclusion, checked before
  anything destructive (including `X-Resume: F`), and startup enforces
  `tpcr.gc.age` > the lease term. **Keep gateway clocks NTP-disciplined**:
  lease expiry compares absolute timestamps across gateways, so a node
  whose clock runs more than the lease term (~120 s at defaults) ahead of
  its peers could steal a live lease.
- **Storage sync failures are deliberately fatal.** If a checkpoint's data
  fsync fails even once, the transfer fails with a unique error
  ("checkpoint data sync failed; durability ... unprovable") rather than
  riding through — a later fsync returning success proves nothing on Linux
  once a writeback error was consumed, and continuing could attest a file
  with a hole in it. The failure chunk advertises the last provably
  durable watermark; the orchestrator's retry resumes from there, so the
  cost of a transient storage hiccup is one retry round-trip.
- **If-Range guard**: when the source's HEAD offers a strong ETag, every
  range request carries `If-Range`, so a source file replaced in place
  mid-transfer fails immediately as source-changed instead of producing a
  torn copy. Sources that ignore `If-Range` behave exactly as before.

### Interoperability with standard remote storage

All improvements are unilateral — they run on this (destination) gateway,
and the remote source sees an ordinary HTTP client. TPCR requires of a
pull source only what stock multistream TPC already requires: HEAD plus
ranged GETs (206) over multiple connections, with credentials forwarded
exactly as stock. Any dCache/EOS/StoRM/xrootd endpoint that serves WLCG
HTTP-TPC today qualifies, and the parallel-range throughput gains need
nothing further from the far end.

Optional source capabilities enable optional features, degrading safely
when absent: no strong validators (ETag/Last-Modified) means cross-session
resume under the default `strong` policy refuses and runs fresh
(in-session recovery still works — length is always revalidated); no
`Repr-Digest` means the end-to-end digest gate does not engage (the local
checksum is still computed and stored). A source that ignores `Range`
outright gets a clean permanent failure rather than corruption. Note that
parallelism follows the client's `X-Number-Of-Streams` (FTS requests this
by default); a single-stream request keeps the resilience and resume but
little of the throughput gain. Push mode is stock-verbatim by design:
unchanged performance, no resume — the remote destination owns its writes.

### Mixed clusters and partial rollout

Running TPCR on some gateways and stock TPC on others is **safe**: the wire
surface is identical, so clients cannot tell nodes apart and every transfer
completes correctly wherever it lands. What changes is that resume becomes
best-effort — it happens only when the *retry* lands on a TPCR node:

- Retry lands on a **stock** node: the file is simply rewritten from
  scratch (stock behavior). The leftover journal is orphaned until a TPCR
  node next touches that destination (stock nodes never clean journals).
- A stock node rewrites a partial that a TPCR journal describes, and a
  later retry lands on TPCR: the handler never trusts the partial's length
  (only the journal watermark) and re-verifies the tail below the watermark
  against the journaled CRCs before resuming — consistent bytes resume,
  anything else is `RESUME_REJECTED` and the transfer runs fresh. The
  worst case of mixing is lost resume opportunity, never corruption.

Use mixed mode for canary/rolling rollout; for deterministic resume, run
TPCR (with the same configuration) on every gateway serving a namespace.

### Multi-tenant warning (do not disable tail verification)

`tpcr.verify.tailbytes 0` disables the resume-time re-read of the partial's
tail against the journal's CRC epochs. In a namespace where users can write
each other's paths, that check is also the defense against a *forged journal*
steering a resume to accept attacker-controlled partial content. Keep it
non-zero (the default re-verifies up to 1 GiB) in any multi-tenant namespace.

## Sizing notes

- **Threads:** one XrdHttp worker thread is held for the duration of each
  active TPC transfer (stock behavior, unchanged). Size `xrd.sched` so that
  expected concurrent transfers plus normal HTTP traffic fit.
- **Memory:** steady-state pooled memory is bounded by `tpcr.mempool.max`;
  per-transfer overflow is bounded by `tpcr.window.bytes`. The soak tests
  hold 8 concurrent 4-stream transfers over a deliberately starved 16 MiB
  pool inside 66 MiB of total server RSS.
- **Checkpoint overhead** at defaults measures ~0 ms per checkpoint
  (sync of already-written data + one small atomic file rename); the NFR-3
  budget of 1% of wall time holds with orders of magnitude to spare.
- **Client-visible surface:** OPTIONS advertises
  `X-Transfer-Capabilities: resume/1` when resume is enabled; clients may
  force a fresh transfer with `X-Resume: F`.
