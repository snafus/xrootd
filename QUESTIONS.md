# TPCR Open Questions for Human Review

Blocked WPs reference entries here. Answered questions move to the bottom with the
answer recorded.

---

## Q-1 (WP-0, informational — no block)
**Single-stream block size under FR-7.** Stock uses `m_small_block_size` (default
smaller than 16 MiB) for `streams=1` and `m_block_size` for multi-stream. FR-7 routes
`streams=1` through the range scheduler whose range size is `tpcr.blocksize` (FR-10,
default 16 MiB). This changes single-stream request granularity relative to stock —
wire-compatible (they are ordinary ranged GETs) but visible in source-side logs and
request counts. Proceeding per FR-10 at WP-4; flag here in case operators care.

## Q-4 (WP-8 — review requested, not blocking)
**Resume reopen authorization scope.** 02 §4 accepts create-scoped tokens for the resume
reopen by passing SFS_O_CREAT; this tree maps SFS_O_CREAT to O_CREAT|O_EXCL, which
refuses existing files, so the implementation opens with plain SFS_O_WRONLY (see
DECISIONS.md).  Under authorization plugins that distinguish create from update scope, a
create-only token cannot resume (the handler falls back to a fresh transfer, which the
same token authorizes — safe but wasteful).  If create-token resume matters in
production, the upstreamable fix is an OFS-level "create-or-open-existing" mode (no
EXCL); flagging rather than deciding, per the operating rules.

## Q-5 (WP-12 scope audit — review requested, not blocking)
**FR-3 advertisement when resume is disabled.** FR-3 says OPTIONS responses MUST
advertise `X-Transfer-Capabilities: resume/1`, unconditionally as written. The
implementation advertises it only when `tpcr.resume` is on, because advertising
resume against a config that will never write a journal misleads orchestrators
into planning retries around a capability that is not there. Conditional
advertisement looks like the spec's *intent* (FR-30 makes resume switchable),
but it deviates from the letter of a MUST, so it is flagged here rather than
silently decided. If the unconditional reading is the intended one, the fix is
a one-line change in `TPCRHandler::ProcessOptionsReq`.

## Q-6 (WP-12 scope audit — review requested, not blocking)
**`tpcr.retry.budget` (02 §12) is not implemented.** No FR references it: FR-13
names only per-range `tpcr.retry.max`, and the wall-clock ceiling on riding out
faults is FR-16's `tpcr.recovery.maxsecs`. The directive was therefore judged
subsumed and never parsed — today it would fail startup as an unknown directive
(FR-30 fail-fast). If a separate cumulative-retry budget distinct from the
recovery budget is wanted, it needs a definition of what it bounds that 02 does
not give. See DECISIONS.md entry of 2026-08-23.

## Q-7 — ANSWERED 2026-08-24 (user: "ok Accept proposals")
**Large-transfer review dispositions accepted; implemented as WP-14.** C1-C4
critical fixes plus H3-H7 ordinary fixes landed with unit and integration
tests; see the two WP-14 entries in DECISIONS.md and the status note at the
top of `LARGE-FILE-REVIEW.md`. H1's remaining hardening (an O_EXCL/link()
lockfile for cryptographic-grade lease acquisition, and the ~120 s NTP skew
margin) stays a documented deployment note rather than code.

## Q-3 (WP-6, informational — no block on M2 for POSIX)
**Backend matrix coverage limited to POSIX.** This testbed offers no EC, CephFS, or
proxy/PSS deployment, so those legs of the WP-6 matrix could not run; per the plan they
are listed unsupported-for-resume in the admin doc until a site runs
`backend_matrix.sh` (and a POSC leg) against them. The kill-9 durability leg validates
API ordering, not power loss — a hardware test remains for a production validation pass.

## Q-2 (WP-0, informational — no block)
**T-U1 "smoke parity" scope at WP-0.** True pull/push parity against a live server
needs the integration harness (mock HTTP source + running xrootd), which lands with
WP-4/WP-5 per 05. At WP-0 the closest achievable variant is: the fork builds, loads its
config, and the ported unit tests (prepareOpenURL, Stream) pass identically to stock,
plus the stock-vs-fork diff review. Full T-U1/T-I1 parity runs are executed when the
harness exists. Recorded per 00 ground rule on test variants.
