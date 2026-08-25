# TPCR Decisions Log

Append-only. Each entry: date, WP, decision, reason, requirement(s) touched.

---

**2026-08-22 / WP-0 — Library naming follows the in-tree plugin-version convention.**
CON-2 names the library `libXrdHttpTPCR.so`. Every module in this tree is built as
`lib<Name>-${PLUGIN_VERSION}.so` (stock TPC: `libXrdHttpTPC-6.so`) and XRootD's plugin
path resolution (`XrdOucPinPath`) resolves the unversioned name administrators write in
the config to the versioned file. The fork therefore builds target `XrdHttpTPCR-6` →
`libXrdHttpTPCR-6.so`, loaded via `http.exthandler xrdtpcr libXrdHttpTPCR.so` exactly as
CON-2 specifies. Touches: CON-2. No weakening — admin-visible name is unchanged.

**2026-08-22 / WP-0 — Multistream forked as a transitional file.**
The 03 file map lists `XrdHttpTpcRScheduler` as *replacing* `XrdHttpTpcMultistream.cc`,
but WP-0's acceptance requires working, byte-identical multi-stream transfers before the
scheduler exists (WP-4). The fork therefore carries `XrdHttpTpcRMultistream.cc` until
WP-4, which must delete it (standing directive 5: replaced code is deleted, not left
dead). Touches: none (sequencing only).

**2026-08-22 / WP-0 — Entry-point symbol kept as `XrdHttpGetExtHandler`.**
XrdHttp locates ext handlers by this fixed dlsym name; it is per-library, and the
version script keeps every other symbol local, so the stock and fork libraries cannot
interfere even if misconfigured together. Touches: CON-2, FR-1.

**2026-08-22 / WP-2 — Config parsing lives in a dedicated TPCR::Config module.**
The 03 file map routes config work through `XrdHttpTpcRConfigure.cc`; the `tpcr.*`
directive table is factored into `XrdHttpTpcRConfig.{hh,cc}` (a struct with a pure
string-based Set()) so T-U3 can unit-test every directive and default without a server
environment.  Configure.cc remains the single place that reads the config file.
Directives are added in the WP that consumes them, so "parsed" always equals
"implemented".  Touches: FR-30 (strengthens testability).

**2026-08-22 / WP-2 — Stream-entry slab wiring lands with WP-4, not WP-2.**
03 says "Stream entries take slabs from the pool" under WP-2, but safe integration
requires the reservation semantics only the scheduler has: a slab must be reserved when
a range is *issued* (deferring issue when none is available), because once curl delivers
bytes they must land somewhere -- an acquire-on-arrival failure would have to fail the
transfer, exactly what NFR-1 forbids.  WP-2 therefore delivers the pool itself (budget,
fair share, non-zeroed recycle, T-U6 + ASan/TSan clean) plus the streams clamp; WP-4
wires entries to reserved slabs when it introduces range issue admission.  Interim cost:
Entry keeps its heap vector (BUG-11's zero-fill persists until WP-4 -- a performance
matter only).  Touches: NFR-1 sequencing; no requirement weakened at any commit.

**2026-08-22 / WP-2 — TSan runs need ASLR reduced on this testbed.**
gcc 11's ThreadSanitizer runtime is incompatible with the high ASLR entropy of the
kernel 6.8 testbed ("unexpected memory mapping"); sanitizer runs use
`setarch $(uname -m) -R <binary>`.  ASan needs no workaround.  Touches: T-U6 procedure.

**2026-08-22 / WP-4 — Retry re-fetches only the undelivered remainder.**
02 §6 does not specify what happens to bytes a failed range already delivered.  Decision:
they are kept.  They are validated 206 data accepted in order by the Stream, so the retry
shrinks the range to the remainder rather than purging and re-fetching.  This removes any
need for a purge API, guarantees no byte is delivered twice (FR-10's exactly-once over
issue history), and keeps the Stream's prior-offset guard as a hard invariant.  Trust
model equals stock: data received through the TLS/TCP channel before a failure is as
trustworthy as data received before a success.  Touches: FR-10, FR-13, FR-25 groundwork.

**2026-08-22 / WP-4 — Handles are NOT reset after clean completions.**
SUB-5 requires full reconfiguration after every reset; it does not require a reset per
request.  A clean completion keeps the handle (and its live connection + TLS session);
per-request state is rebuilt by ResetAfterRequest and every issue sets CURLOPT_RANGE
afresh.  Only cancelled/failed handles are reset+reconfigured.  Resetting per range would
force a TLS handshake per 16 MiB block — a needless NFR-2 regression.  Touches: SUB-5,
NFR-2.

**2026-08-22 / WP-4 — Two scheduler translation units.**
The 03 file map lists XrdHttpTpcRScheduler.{cc,hh}; the implementation splits the pure
FSM (XrdHttpTpcRSchedulerCore.cc, curl-free, exhaustively unit-tested) from the curl
loop (XrdHttpTpcRScheduler.cc).  Same surface, better testability.  Touches: file map
only.

**2026-08-22 / WP-4 — `+notls` required on non-TLS deployments.**
XrdHttp only loads ext handlers lacking the `+notls` tag from its HTTPS initialization
path; on a server without TLS they are silently never loaded.  The integration harness
uses `http.exthandler xrdtpcr +notls …`; the WP-13 admin guide must document this for
plain-HTTP deployments (production WLCG deployments use TLS and are unaffected).
Touches: CON-2 documentation.

**2026-08-23 / WP-6 — Backend semantics matrix results (M2 gate).**
Testbed: Ubuntu 22.04, ext4, POSIX OSS via `oss.localroot`.  Results
(tests/XrdHttpTpcR/integration/backend_matrix.sh, all passing):
- (a) fsync-then-kill-9: data present after process death — the API ordering the
  checkpoint (SUB-1) relies on holds.  True power-loss durability is a hardware test;
  recorded as out of scope for this environment (the ordering argument, not the medium,
  is what WP-7 depends on).
- (b) reopen `O_CREAT|O_WRONLY` without TRUNC + write at W: prefix intact, write lands —
  the FR-20 resume open works on POSIX.
- (c) temp→write→fsync→close→rename journal update: 300 randomized kill-9 interruptions,
  zero torn journals observed — XRD-5's sequence holds on ext4.
- (d) POSC (XRD-1) empirically confirmed: under `ofs.persist auto hold 3` a crashed
  transfer's partial is unlinked at restart after the hold window, and the TPCR hasPOSC
  startup warning fires; under default persistence the partial survives.
**Supported-for-resume backend list as of M2: POSIX OSS.**  EC (XrdEC), CephFS, and
proxy/PSS deployments are *unsupported until tested* (XRD-4) — see QUESTIONS.md Q-3.
Touches: FR-18, FR-20, CON-3, XRD-1, XRD-4, XRD-5.

**2026-08-23 / WP-8 — Resume open uses plain SFS_O_WRONLY, not SFS_O_CREAT.**
FR-20 prescribes `SFS_O_CREAT|SFS_O_WRONLY` "(no TRUNC, no O_EXCL semantics)" for the
resume reopen; this tree's XrdOfs maps SFS_O_CREAT unconditionally to `O_CREAT|O_EXCL`
(XrdOfs.cc:557), which would refuse the existing partial outright.  The two halves of
FR-20 conflict on this tree; "no O_EXCL semantics" is the load-bearing half (the reopen
must succeed on the partial), so the resume open passes plain `SFS_O_WRONLY`.
Consequence: authorization for the reopen is write/update, not create — a client whose
token carries *only* create scope may fail the resume open; the handler then falls back
to a fresh transfer (safe: CON-3 degrade, transfer still succeeds via the stock path).
Flagged for human review in QUESTIONS.md Q-4 since it weakens 02 §4's create-scope
rationale.  A stat/open race (partial vanishing between the tree's stat and the open)
lands in the same fresh-transfer fallback.  Touches: FR-20, CON-3, SUB-2.

**2026-08-23 / WP-8 — tpcr.gc.age floor lowered to 10s.**
Purely a validation bound (default stays 24h); a 60s floor made the lazy-GC integration
scenario needlessly slow, and no correctness property depends on the floor.  Touches:
FR-24, FR-30.

**2026-08-22 / WP-0 — Verbatim ports keep their identifiers.**
`XrdHttpTpcPMarkManager.*` and `XrdHttpTpcUtils.*` are copied with their original file
names, class names and namespaces (`XrdHttpTpc::PMarkManager`, `XrdHttpTpcUtils`), per
the 03 file map ("verbatim + include-path fixes"). Only the references to the renamed
`TPC` namespace (forward declaration of `TpcType`, `TPC::TPCHandler::OSS_TASK_OPAQUE`)
are adjusted — without them the files do not compile. All symbols are local to the
module, so no runtime clash with the stock library is possible. Touches: CON-6, FR-1.

**2026-08-23 / WP-12 — Slab reservation is a valve, never a gate on the only in-flight range.**
T-S2 (8 transfers over a 16-slab pool) exposed a pool-wide starvation: Stream entries
that once buffered out-of-order bytes retain their slab for the transfer's lifetime, so
under saturation the whole budget migrates into entries, every client sits at its
fair-share cap, `issue_ready` refuses to issue, in-flight drops to zero, and the global
stall timeout kills every transfer.  Fix: (a) an unused reservation already in the
stash satisfies the next issue (reservations are not stacked), and (b) the reservation
is skipped entirely when the transfer has nothing in flight — delivery never needs the
pool (Stream's window-bounded heap fallback), so the pool gates only *extra*
parallelism.  Verified: 3 consecutive T-S2 runs at 8/8.  Touches: NFR-1, FR-13, SUB-3.

**2026-08-23 / WP-12 scope audit — `tpcr.retry.budget` (02 §12) subsumed, not parsed.**
No FR defines what it bounds: FR-13 names per-range `tpcr.retry.max`, FR-16 the
wall-clock `tpcr.recovery.maxsecs`.  Implementing a third knob with invented semantics
would be scope creep; today the directive fails startup as unknown (FR-30 fail-fast).
Flagged in QUESTIONS.md Q-6 for the docs-vs-implementation divergence.  Touches:
FR-13, FR-16, FR-30.

**2026-08-23 / WP-12 scope audit — unknown CURLcodes classify as Retryable.**
FR-12's table is exhaustive for the codes it names but silent on codes it does not
(new libcurl versions add them).  Retryable is the conservative default under the
priority order: a wrongly-permanent classification forfeits a recoverable transfer,
while a wrongly-retryable one is bounded by `tpcr.retry.max` and the FR-16 recovery
budget.  Touches: FR-12, FR-13, FR-16.

**2026-08-23 / WP-12 scope audit — degraded-state exit resets the global stall clock.**
Outage time ridden out in degraded state is charged to FR-16's recovery budget, not to
the stock global stall timer (SUB-11 precedence): without the reset, a recovered
transfer could be killed by the stall timer for an outage the recovery budget just
authorized it to survive.  Wire-visible only as "transfers survive what FR-16 says
they survive".  Touches: FR-16, SUB-11.

**2026-08-23 / WP-12 scope audit — dead stock-port surface removed.**
An audit of the implementation against 01/02/04 found no unjustified subsystems, but
~90 lines of orphaned surface, mostly left behind when WP-4 deleted Multistream:
`State::Duplicate`, `State::BodyTransferInProgress`, `State::RangeRequested` (test now
uses the StateTestPeer seam), `SlabPool::SlabSize`, `Checkpointer::Record/MutableRecord`,
`TransferDigests::EpochStart`, the `m_block_size`/`m_small_block_size` statics
(replaced by `tpcr.blocksize`), and dead mock-source bookkeeping.  `Stream::DumpBuffers`
was re-wired to the Finalize failure path (stock behavior) instead of deleted, which
also re-justifies `ReorderSpan`/`GetCapacity` as its observability; the stale claim
that ReorderSpan drives admission was corrected (the scheduler window does).  Touches:
03 standing directive 5 ("delete rather than leave dead code").

**2026-08-24 / WP-14 — Q-7 accepted: the four critical review findings are fixed.**
User approved the dispositions in LARGE-FILE-REVIEW.md (Q-7).  C1: a failed data
sync now permanently poisons the Checkpointer -- no later sync can advance W or
attest success (Linux consumes writeback errors at the first fsync, so a
retry-then-succeed sync proves nothing); the transfer fails with a unique error
and advertises the last PERSISTED watermark as resumable-from.  This strengthens
SUB-1's "failed sync must not advance W" to "failed sync ends the session".
C2: the resume decision tree now gates on the lease FIRST -- a live lease gets
409 unconditionally (including X-Resume: F and GC), and a dead lease is
ACQUIRED (with read-back confirmation against rename races) before the validator
ladder, dest-size check, and tail verify run, so every destructive rejection
executes while holding the exclusion.  C3: the degraded probe inner loop now
runs checkpoint_tick, keeping the lease renewed (and W flowing) during long
trickling probes.  C4: the Checkpointer prunes epochs wholly below
W - verify.tailbytes at each commit (tail verification never reads them),
Serialize refuses records over kMaxRecordBytes, and kMaxEpochs dropped to
32768 so the two caps are mutually consistent (32768 x 20 B < 1 MiB).
Touches: SUB-1, FR-22, FR-4, FR-24, FR-28, NFR-5, XRD-6.

**2026-08-24 / WP-14 — accepted H-grade fixes.**
H3: VerifyResumeTail now requires the verified epochs to tile the tail span
contiguously up to exactly W (a forged epoch-free journal previously passed
vacuously, hollowing out SUB-9's forged-watermark defense).  H4: multiple 401s
in one harvest batch fold into the single pending degraded entry instead of
consuming the FR-12 one-shot re-probe before it runs.  H5: a source HEAD
without Content-Length is rejected at HEAD time with the real reason (it
previously rode a uint64(-1) into oss.asize and failed at the completion gate
as a misleading internal error).  H6: when the session baseline holds a strong
ETag, every range GET carries If-Range -- an in-place source change now fails
immediately as source-changed (errSourceChanged, permanent) instead of
depending on a fault-triggered re-HEAD or an optional Repr-Digest; sources
ignoring If-Range behave as before (additive header, CON-4).  H7: FR-23's
destructive order is now sync -> close -> checksum-inject -> journal-remove ->
verdict, so a close failure after a provable sync leaves a resumable journal;
on FinalCheckpoint failure the failure chunk falls back to advertising the
persisted watermark.  New FR-30 cross-directive invariant: tpcr.gc.age must
exceed the lease term (2 x tpcr.checkpoint.secs).
Touches: FR-4, FR-12, FR-21, FR-23, FR-28, FR-30, SUB-9, CON-4.

**2026-08-25 / WP-15 — TLS and two-gateway integration matrices.**
Closes the two locally-closable gaps from the release-readiness assessment.
tls_matrix.sh: throwaway CA + IP-SAN cert per run; HTTPS client->gateway with
the handler loaded WITHOUT +notls; an HTTPS *source* pulled through the
CA-verified curl path (http.cadir hashed dir -- note xrootd refuses CA
material with group-write permissions); resume over TLS after kill -9.
multi_gateway.sh: two live xrootd instances sharing one data directory --
cross-process 409 while the lease is live (WP-14/C2 across real processes),
X-Resume: F blocked the same way, and a genuine cross-gateway resume of the
dead instance's watermark.  Both registered in ctest (RUN_SERIAL, SKIP 127).
Honest scope: single-host ext4 stands in for the shared filesystem, and
token/macaroon flows remain untested (libraries absent here) -- both stay
site-validation items (Q-3).  Touches: FR-22, XRD-6, CON-3, NFR-2.
