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

**2026-08-22 / WP-0 — Verbatim ports keep their identifiers.**
`XrdHttpTpcPMarkManager.*` and `XrdHttpTpcUtils.*` are copied with their original file
names, class names and namespaces (`XrdHttpTpc::PMarkManager`, `XrdHttpTpcUtils`), per
the 03 file map ("verbatim + include-path fixes"). Only the references to the renamed
`TPC` namespace (forward declaration of `TpcType`, `TPC::TPCHandler::OSS_TASK_OPAQUE`)
are adjusted — without them the files do not compile. All symbols are local to the
module, so no runtime clash with the stock library is possible. Touches: CON-6, FR-1.
