# Adversarial review: is TPCR bullet-proof for very large transfers?

> **STATUS 2026-08-24: dispositions accepted (Q-7) and implemented as
> WP-14.** C1 (sync poison), C2 (lease-first + acquire-before-verify with
> read-back), C3 (probe-loop lease renewal), C4 (epoch pruning, serialize
> guard, reconciled caps) plus H3/H4/H5/H6/H7 are fixed with tests; see
> DECISIONS.md. H1's residual (atomic lockfile, NTP skew margin) remains a
> deployment note in ADMIN.md. The findings below are kept as written for
> the record.

Four independent hostile-reading audits of `src/XrdHttpTpcR/` against the
10 TB / multi-day / multi-resume design point (integer safety; journal &
digest scale; long-run scheduler behavior; boundary and fault edges).
**Findings only — no fixes applied.** Line numbers refer to the tree at
tag `tpcr-handback` + CI-hardening commits.

## Verdict up front

The steady-state data path is genuinely solid: 64-bit offsets are forced
build-wide and verified overflow-free at 2^44 scale; response validation
provably prevents any byte landing at a wrong offset; coverage arithmetic
is exact at every size boundary including zero; per-transfer memory is
window-bounded, not file-bounded; marker accounting is monotonic through
every retry/shrink/teardown; and the success gate cannot emit a success
verdict past a failed sync, close, digest, or coverage check.

**It is not yet bullet-proof.** Four correctness-class defects survive in
the fault/resume machinery — precisely the new code — each reachable on
the exact workload this handler exists for. None is architectural; all
four have small, local dispositions. Until they are fixed, the honest
claim is "far safer than stock, with known sharp edges under compound
faults", not "bullet-proof".

## Critical findings (correctness / attested-false-success class)

### C1. Data-sync failure ride-through is a real fsyncgate
`XrdHttpTpcRJournal.cc:486-491` (`Checkpointer::Take`). A failed
`stream.Sync()` correctly refuses to advance W — but the transfer then
*continues* on the same fd. On Linux (errseq_t semantics), the failed
fsync consumes the writeback error and the lost dirty pages are dropped;
the **next** checkpoint's sync succeeds and W advances over bytes the disk
never kept. No path ever re-writes the affected range; epoch CRCs and the
adler are computed from memory, not disk, so they attest the lost bytes.
End states: a resumed transfer with a permanent hole below W (tail verify
only catches it if the hole lands in the last `verify.tailbytes`), or —
with no crash at all — `SuccessCleanup`'s final sync returning success and
a **false `success: Created`** over a holed file, with the (memory-computed,
matching) adler injected into the checksum store so nothing downstream
re-reads the bytes. This violates priority 1 directly. Disposition
implied by SUB-1's own "resume must be disabled, not silent best-effort"
clause: any data-sync failure must be terminal for the session (fail the
transfer; the old journal's W remains the only trusted state), or at
minimum poison the handle so no later sync can advance W.

### C2. Destructive resume rejections run before the lease check
`XrdHttpTpcRTPC.cc:1564-1596`. The decision tree evaluates the validator
ladder (1568) and the `dest_size < W` check (1576) — both of whose
failures `reject()` **deletes the journal** and fall through to a fresh
`SFS_O_TRUNC` open — *before* consulting `LeaseLive` (1580). A second
gateway (orchestrator duplicate/retry) whose HEAD sees changed validators,
or whose NFS attribute cache serves a stale `st_size`, destroys the
live-leased journal and truncates the partial underneath a still-writing
session. Both sessions then write the same file; each digests its own
bytes in memory, so **both can attest success** over an interleaved file.
This defeats 02-ARCHITECTURE §7's own contract ("an arriving transfer
that finds a live lease returns 409"; XRD-6: the lease is the *only*
mutual exclusion). Disposition: the lease check must precede every branch
that removes a journal or opens with O_TRUNC. (The lazy-GC branch at 1551
also precedes the lease check; safe at default gc.age ≫ lease, but
nothing enforces `gc.age > lease duration` — config floor is 10 s.)

### C3. Lease renewal starves inside the degraded probe loop
`XrdHttpTpcRScheduler.cc:505-579`. The lease (expiry = 2× checkpoint.secs
= 120 s) is renewed only via `checkpoint_tick`, which the degraded state
calls in its between-probe pause loop but **not** in the probe's inner
`while (!settled)` loop. A probe trickling between the 10 KiB/s low-speed
floor and ~137 KiB/s (16 MiB block) keeps that loop alive indefinitely —
tens of minutes — with the lease expired throughout. Any concurrent COPY
then legitimately *steals* the lease (`TPC.cc:1618-1634`) and becomes a
second writer: same two-attested-writers end state as C2. One-line
disposition (a `checkpoint_tick()` in the probe loop), which also
restores W checkpointing during long probes.

### C4. Unbounded epoch growth crosses the journal's own parse caps
`XrdHttpTpcRTPC.cc:1753-1758` appends one CRC epoch per progress
checkpoint; nothing ever prunes. The caps are mutually inconsistent:
`kMaxEpochs` = 65,536 × 20 B/epoch = 1.31 MiB > `kMaxRecordBytes` = 1 MiB
(`Journal.hh:76-77`), and `Serialize` checks only the count while `Parse`
rejects on total size. Consequences in sequence on a legitimately slow
large transfer (timed 60 s cadence closes ~1 epoch/minute; 10 TB at
~4 MB/s ≈ 29 days ≈ 42k epochs; the 1 MiB `checkpoint.bytes` floor gets
there in ~51 GiB): at ~52,400 epochs the journal commits fine but every
subsequent `Load` rejects it as "implausible" — **weeks of resume equity
silently forfeited**; at 65,536 `Serialize` fails, checkpoints stop, W
freezes, and — the nasty tail — **lease renewal stops while the session
is still writing**, opening C2/C3's two-writer window to any retry.
Disposition: coalesce/prune epochs wholly below `W − verify.tailbytes`
(provably never read again) and/or add a serialize-side byte guard;
reconcile the two caps. (Independently found by three of the four
audits.)

## High findings (reliability at the design point)

- **H1. Lease steal is check-then-act, not compare-and-swap** — and the
  window between `LeaseLive` (TPC.cc:1580) and the stealing `Commit`
  (1629) now contains the synchronous tail verify, i.e. seconds-to-
  minutes. Two gateways racing on a dead lease can both steal and both
  write. Compounding: writer exclusion rests on absolute-timestamp
  comparison across gateways with a ~120 s skew margin — one broken NTP
  node voids it.
- **H2. Tail-verify cost is `verify.tailbytes + checkpoint.bytes`**, read
  synchronously on the worker thread before any response byte
  (TPC.cc:1251-1287 reads whole overlapping epochs — correct, but at
  raised `checkpoint.bytes` it is minutes, unbounded server-side, gated
  only by the client's patience). Needs at least a documented warning.
- **H3. Tail verify passes vacuously when the journal carries no epochs
  overlapping the tail** — coverage of `[W−tail, W)` is never asserted
  (TPC.cc:1288-1295). The journal CRC is a checksum, not a MAC, so this
  undercuts SUB-9's forged-journal claim (low practical severity: the
  attacker postulated can already write the destination).
- **H4. Batched 401s burn the FR-12 one-shot re-probe before it runs**
  (Scheduler.cc:714-726 with the `aborted` break at :786): a token expiry
  hitting several in-flight ranges in one harvest batch races the
  degraded entry and turns a recoverable auth flap into a permanent (but
  safe, resumable-at-W) failure on multi-stream transfers.
- **H5. HEAD without Content-Length** becomes `uint64(-1)` →
  `oss.asize=18446744073709551615` on the open and a misleading
  "Internal completion gate failure ... of -1" verdict (TPC.cc:729, 1490).
  A stock streams=1 transfer would have worked; TPCR should reject the
  source cleanly at HEAD time.
- **H6. In-place source modification with no transport fault is caught
  only if the source advertises a digest** (or a fault forces a re-HEAD).
  The ETag captured at HEAD is never sent as `If-Match`/`If-Range` on
  range GETs, so a torn old/new mixture can be attested successful with a
  self-consistent wrong-file adler. Inherited from stock — but TPCR holds
  the validator and is one header away from closing it.
- **H7. Success-path resumability cliff**: journal removal precedes
  close and verdict delivery, so a close failure after removal, or a
  client disconnect in the sync→verdict window, leaves a complete file
  whose retry does a full 10 TB re-pull (spec-conformant per FR-23's
  stated order; the order itself is worth revisiting).

## Notable low/cosmetic (grouped; full details in the audit transcripts)

Config upper bounds absent on `checkpoint.bytes`/`mempool.max`/
`verify.tailbytes` (a >2^63 value makes `bytes_due` permanently true);
HTTP status parsed via `stol` truncates into `int` (defeats one gate for
a hostile source; range echo still protects the bytes); per-re-probe
`curl_slist` leak in `SetupHeadersForHEAD`; `running_handles`
double-decrement (bounded extra sleep); unchecked curl return codes in
the probe path (bounded misdiagnosis); stale `Content-Length` from a
redirect response can spuriously *permanently* fail a range;
`LEASE_STEAL` logged on every resume; auth-outage failures reported as
"source unavailable"; `monInfo.strm` truncated to 8 bits; wall-clock
timers vs NTP steps (self-healing); PMark queue growth when `Begin`
persistently fails (inherited); `CURLOPT_*, 1` int-vs-long literals
(inherited); journal `Remove` not directory-durable (harmless orphan);
fixed temp-file name turns interleaved double-commits into a parse
reject (safe, given C2/C3 are fixed).

## What was verified sound (no action)

64-bit `off_t` forced; all offset/length arithmetic, adler32 (incl.
NMAX loop and combine), CRC epochs, and the defensive journal parser
correct at 2^44; CURLOPT_RANGE construction; Content-Range parsing incl.
>2^32 and malformed forms; wrong-offset/short/over-delivering responses
rejected before any write; multipart responses rejected; empty-file and
every block-boundary case; ENOSPC/EIO classified permanent with no futile
retries and safe checkpoint behavior; SUB-1 ordering honored at each
checkpoint; rename atomicity trade (documented, regress-to-older-W safe);
zero-progress checkpoints close no zero-length epochs; window-bounded
memory (buffers, range table, slab stash) with verified eviction; attempt
counters cannot bleed between ranges; connection recycling burns at most
one attempt on one range; stall clocks all refreshed/reset across
degraded episodes; marker monotonicity through every teardown path;
success-gate ordering (sync → remove → close → verdict) admits no false
success on its own; credential expiry ends in a safe, resumable-at-W
failure by design.

## Recommended disposition order

C1 (sync-failure = terminal) and C2 (lease check first) are small,
priority-1-mandated fixes; C3 is one line; C4 needs the epoch-pruning
decision (mechanically simple, journal-format-neutral). H1's CAS gap and
skew margin deserve a design note in QUESTIONS.md; H4-H7 are ordinary
fixes; the rest is polish. All four audits agree nothing here requires
architectural change.
