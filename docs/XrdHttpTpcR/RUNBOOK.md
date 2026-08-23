# XrdHttpTpcR operations runbook

## Structured log events

Every significant transfer event is a single greppable line in the standard
`logTransferEvent` style: `event=<NAME>`, with the transfer's local/remote
identifiers on the same line. Watch these (FR-31 minimum set in **bold**):

| Event | Meaning / operator action |
|---|---|
| `PULL_START` / `PUSH_START` | Transfer admitted; `PUSH_*` is the stock push path. |
| `TRANSFER_SUCCESS` / `TRANSFER_FAIL` | Final verdict, after (success) the data sync + checksum gate. |
| **`CHECKPOINT`** | A checkpoint was taken; `W=<bytes>` is the durable watermark (`final W=` on exit paths). Absence over minutes on a long transfer means no commit progress. |
| **`RANGE_RETRY`** | A range failed and was re-queued (classification and attempt count in the message). Occasional retries are normal; storms indicate a sick source. |
| **`DEGRADED_ENTER` / `DEGRADED_EXIT`** | Per-range patience exhausted; the handler dropped to single-connection probing (`DEGRADED_PROBE` per attempt). EXIT means full parallelism restored. A transfer failing *from* degraded reports "no commit progress within the recovery budget". |
| `SOURCE_CHANGED` | Mid-session or resume revalidation saw different validators (size/ETag/digest) — permanent failure, by design. The source file really changed; re-drive the transfer. |
| **`RESUME_START`** | Cross-session resume accepted; message carries the seeded watermark. |
| **`RESUME_REJECTED`** | Journal found but resume refused (reason in message: validator mismatch, policy, dest shorter than W, tail-verify failure...). The transfer proceeds fresh — this is safe degradation, not an error. |
| **`LEASE_CONFLICT`** | Retry arrived while a prior session's lease is live → client got `409 + Retry-After`. Self-heals when the lease lapses (2x `tpcr.checkpoint.secs`). Persistent 409s beyond that suggest two *live* writers on one destination. |
| `LEASE_STEAL` | An expired lease was taken over by a new session (normal after a crash). |
| **`GC_DISCARD`** | A journal older than `tpcr.gc.age` was discarded on encounter. |
| `CLIENT_DISCONNECT` | TPC client dropped mid-transfer; progress checkpointed, **no verdict sent** — the orchestrator's retry can resume. |
| `STREAMS_CLAMPED` | Client asked for more than `tpcr.streams.max`. |
| `SLAB_DEFER` / `SLAB_OVERFLOW` | Buffer-pool backpressure: extra parallelism deferred / private buffer used. Frequent occurrences mean `tpcr.mempool.max` is undersized for the concurrency level — transfers still complete correctly. |
| `REPRDIGEST_VERIFY_FAIL` | Full-file digest disagreed with the source's advertised Repr-Digest → transfer failed rather than storing corrupt data. Investigate the source. |
| `CKSUM_INJECTED` / `CKSUM_INJECT_SKIP` | Final adler32 stored into (or skipped for) the checksum xattr store. |

Failure/diagnostic events: `OPEN_FAIL`, `CLOSE_FAIL`, `FLUSH_FAIL`,
`HEAD_FAIL`, `RESPONSE_FAIL`, `PERFMARKER_FAIL`, `SCHEDULER_ERROR`,
`SCHEDULER_FAIL`, `TRANSFER_CURL_ERROR`, `TRANSFER_ERROR`, `CURL_INIT_FAIL`,
`INVALID_REQUEST`, plus the stock redirect events. Every error string in the
handler is unique — grep for the exact text to find the single code site.

## Journal inspection: `tpcr-journal-dump`

Ships with the handler (installed to `bin/`). Read-only; uses the same
defensive parser as the server, so it is safe on corrupt/hostile files.

```
tpcr-journal-dump /data/path/file.root.xrdtpcr
```

Prints the watermark `W`, source URL, validators (length/ETag/Last-Modified/
Repr-Digest), digest state and CRC epochs, lease owner/expiry, attempt count
and timestamps. A file the parser rejects prints the precise reason — a
rejected journal is exactly what the server would GC/ignore.

Typical uses:
- *"Will this partial resume?"* — dump the journal, compare `W` with the
  partial's size (`W` ≤ partial size is required) and check the lease expiry.
- *"Why did the resume fall back to fresh?"* — the `RESUME_REJECTED` log line
  has the reason; the dump shows the recorded validators to compare with a
  `curl -I` of the source.

## Common situations

**Orphan `.xrdtpcr` files.** Normal after failed transfers whose retries
never came back. Lazy GC removes one the next time a COPY targets that
destination and it is older than `tpcr.gc.age`. They are small (a few KB);
bulk-clean with `find <namespace> -name '*.xrdtpcr' -mtime +N -delete` only
while no transfers target those paths.

**Client gets 409 Conflict.** A live lease (see `LEASE_CONFLICT` above).
`Retry-After` in the response says when to come back. No action needed unless
persistent, in which case check for a duplicate submission writing the same
destination.

**Transfer fails with "no commit progress within the recovery budget".**
The source was unreachable for more than `tpcr.recovery.maxsecs`. The partial
was checkpointed first (`CHECKPOINT final W=...`, plus `resumable-from:` in
the failure chunk); an orchestrator retry against the same namespace resumes
from there.

**Server killed / crashed mid-transfer.** Nothing to do: the journal's
watermark is behind by at most one checkpoint interval + reorder window. The
next COPY for that destination resumes (watch for `RESUME_START` and, on the
wire, perf markers that start at the resumed offset).

**Assertion failure in logs.** Internal-invariant assertions fail the
transfer in release builds and abort in debug builds (NFR-7). A transfer
failed by an assertion is a bug report: capture the unique error string and
the transfer's event lines.
