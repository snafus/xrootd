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

**2026-08-22 / WP-0 — Verbatim ports keep their identifiers.**
`XrdHttpTpcPMarkManager.*` and `XrdHttpTpcUtils.*` are copied with their original file
names, class names and namespaces (`XrdHttpTpc::PMarkManager`, `XrdHttpTpcUtils`), per
the 03 file map ("verbatim + include-path fixes"). Only the references to the renamed
`TPC` namespace (forward declaration of `TpcType`, `TPC::TPCHandler::OSS_TASK_OPAQUE`)
are adjusted — without them the files do not compile. All symbols are local to the
module, so no runtime clash with the stock library is possible. Touches: CON-6, FR-1.
