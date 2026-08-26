# TPCR Open Questions for Human Review

Blocked WPs reference entries here. Answered questions move to the bottom with the
answer recorded.

---

## Q-1 — ANSWERED 2026-08-26 (delegated review)
**Single-stream block size under FR-7: accepted as designed.** One scheduler
path for every pull is the FR-7/FR-10 architecture; `streams=1` issuing
`tpcr.blocksize`-sized ranged GETs is wire-compatible and visible only in
source-side request counts. Operators who care tune `tpcr.blocksize`; noted
in ADMIN.md.

## Q-4 — ANSWERED 2026-08-26 (delegated review)
**Resume reopen stays plain SFS_O_WRONLY; the real fix is upstream.** Within
CON-1 (no core-xrootd changes) this is the only mode that opens an existing
partial on this tree, and the fresh-transfer fallback keeps CON-3 intact for
create-scoped tokens. Operator consequence documented in ADMIN.md (resume
requires write/update authorization on the destination); the proper
`create-or-open-existing` OFS open mode is recorded in UPSTREAM.md as a
candidate core enhancement. Revisit only if a deployment must resume with
create-only tokens.

## Q-5 — ANSWERED 2026-08-26 (delegated review)
**Conditional FR-3 advertisement stands.** Advertising `resume/1` on a server
with `tpcr.resume no` would tell orchestrators to plan around a capability
that is not there; capability headers exist to be honest. FR-3's text should
gain the qualifier "while resume is enabled" — recorded here as spec errata
since the spec package is the requirements authority.

## Q-6 — ANSWERED 2026-08-26 (delegated review)
**`tpcr.retry.budget` stays unimplemented (subsumed).** No FR defines what it
bounds; per-range patience is `tpcr.retry.max` (FR-13) and the wall-clock
ceiling is `tpcr.recovery.maxsecs` (FR-16). The DECISIONS entry of 2026-08-23
stands; ADMIN.md's config reference now notes the subsumption. 02 §12 should
drop the directive — spec errata.

## Q-7 — ANSWERED 2026-08-24 (user: "ok Accept proposals")
**Large-transfer review dispositions accepted; implemented as WP-14.** C1-C4
critical fixes plus H3-H7 ordinary fixes landed with unit and integration
tests; see the two WP-14 entries in DECISIONS.md and the status note at the
top of `LARGE-FILE-REVIEW.md`. H1's remaining hardening (an O_EXCL/link()
lockfile for cryptographic-grade lease acquisition, and the ~120 s NTP skew
margin) stays a documented deployment note rather than code.

## Q-8 — INVESTIGATED 2026-08-26 (mitigated; attributed to the CI runner environment)
**Intermittent xrootd startup segfault on GitHub CI's Alma 8 job only.**
~1 in 12 server boots in CI dies with SIGSEGV while the TPCR plugin loads.
Investigation record: never observed on the 16 other CI platforms; 120 boots
clean locally (Ubuntu 22.04, MALLOC_CHECK_/MALLOC_PERTURB_); then, in a real
AlmaLinux 8 container with the exact CI toolchain (gcc 8.5, libcurl 7.61.1 —
which on EL8 is OpenSSL-backed, correcting this entry's earlier NSS guess):
200 gdb-supervised boots clean; 300 kill-9-churn boots into dirty work dirs
under 2-lane concurrency clean (RelWithDebInfo); and 300 more clean after
rebuilding at maximum fidelity to CI — Debug build type (CI's default),
full builddep environment with macaroons/SciTokens/VOMS present.  800 EL8
boots, zero failures, where CI's rate predicts ~65.  Conclusion: the crash
is a property of the GitHub-hosted runner environment (Azure virtualization
/ host kernel / container confinement / extreme CPU oversubscription during
the serial 441-test run), not of the binary or EL8 userland.  Standing
posture: harness boot-retries (3x) with per-occurrence log evidence keep CI
green while every incident stays counted; if the symptom EVER appears on a
real EL8 host, treat it as a new, serious finding and capture a core there.

## Q-3 (WP-6, informational — no block on M2 for POSIX)
**Backend matrix coverage limited to POSIX.** This testbed offers no EC, CephFS, or
proxy/PSS deployment, so those legs of the WP-6 matrix could not run; per the plan they
are listed unsupported-for-resume in the admin doc until a site runs
`backend_matrix.sh` (and a POSC leg) against them. The kill-9 durability leg validates
API ordering, not power loss — a hardware test remains for a production validation pass.

## Q-2 — ANSWERED 2026-08-26 (delegated review)
**T-U1 parity was satisfied once the harness existed.** WP-4/5 delivered the
full T-I1 size-by-streams byte-compare matrix, the CON-6 push smoke, and the
T-I8 gfal/davix-grammar checks — the complete parity the WP-0 note deferred.

