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

**2026-08-22 / WP-0 — Verbatim ports keep their identifiers.**
`XrdHttpTpcPMarkManager.*` and `XrdHttpTpcUtils.*` are copied with their original file
names, class names and namespaces (`XrdHttpTpc::PMarkManager`, `XrdHttpTpcUtils`), per
the 03 file map ("verbatim + include-path fixes"). Only the references to the renamed
`TPC` namespace (forward declaration of `TpcType`, `TPC::TPCHandler::OSS_TASK_OPAQUE`)
are adjusted — without them the files do not compile. All symbols are local to the
module, so no runtime clash with the stock library is possible. Touches: CON-6, FR-1.
