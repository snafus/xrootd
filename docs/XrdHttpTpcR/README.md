# XrdHttpTpcR — resumable HTTP third-party copy

**BLUF: TPCR is a drop-in alternative to XRootD's HTTP-TPC handler that
makes large pull-mode transfers survive faults. A 10 TB copy that dies at
9.9 TB resumes from the last durable byte instead of restarting, verified
bit-identical by checksums — with zero changes to clients, sources, or the
wire protocol. Enable it with one config line; roll back the same way.**

## TL;DR

- **Same wire protocol.** Existing FTS/gfal2/davix clients and any
  HTTP/WebDAV source work unchanged (grammar verified against gfal/davix
  parsers in CI). Everything new is additive headers.
- **Two tiers of fault tolerance.** In-session: per-range retries with
  failure classification, then a degraded mode that rides out a total
  source outage for a configurable budget. Cross-session: progress is
  checkpointed (data fsync first, then an atomic sidecar journal), so a
  crashed/killed/redriven transfer resumes from the watermark.
- **Never lies about success.** Success requires full delivery + streaming
  checksum agreement (adler32, gated against the source's `Repr-Digest`
  when offered) + a final durable sync. Resume re-verifies the partial's
  tail against journaled CRC32C epochs before trusting it.
- **Bounded memory.** Server-global buffer pool with per-transfer fair
  share; 8 concurrent 4-stream transfers run inside ~66 MiB RSS in the
  soak tests. Parallel ranged reads give ~3x stock throughput at 4 streams
  on the loopback baseline, at lower CPU than stock.
- **Safe by default, easy to say no to.** Resume needs a shared filesystem
  across gateways and a POSIX backend — anywhere else set `tpcr.resume no`
  and you still keep all the in-session resilience. POSC deployments get a
  startup warning (POSC deletes the partials resume needs).
- **Ships in the normal packages.** Built with the standard xrootd build;
  RPM/deb put `libXrdHttpTPCR-6.so` and the `tpcr-journal-dump` ops tool
  in the existing server packages.

## 60-second start

```
# xrootd config on the HTTP gateway (add +notls if the server has no TLS):
http.exthandler xrdtpcr /usr/lib64/libXrdHttpTPCR-6.so

# optional tuning — defaults are sane; see ADMIN.md for all 14 directives
tpcr.resume true            # off unless gateways share one filesystem
tpcr.recovery.maxsecs 120   # keep below your orchestrator's stall timeout
```

Restart xrootd. Watch for `event=CHECKPOINT` / `event=RESUME_START` lines
in the log. Rollback = revert the `http.exthandler` line (the stock handler
is in the same package).

## Document map

| Read this | When you want |
|---|---|
| [ADMIN.md](ADMIN.md) | Build/deploy, full `tpcr.*` config reference, resume prerequisites, backend support matrix, sizing, multi-tenant warning |
| [RUNBOOK.md](RUNBOOK.md) | Operating it: the structured log-event vocabulary, `tpcr-journal-dump`, 409/lease behavior, common situations |
| [CHANGES.md](CHANGES.md) | What differs from the stock handler (guarantees kept, architecture, bug dispositions) |
| [UPSTREAM.md](UPSTREAM.md) | Five stock XrdHttpTpc bugs found during this work, written up as standalone upstreamable patches |

Source: `src/XrdHttpTpcR/`. Tests: `tests/XrdHttpTpcR/` (unit suites,
fault-injecting mock source, integration/crash/resume/soak matrices, and a
requirement-traceability gate).

## Status

Feature-complete (tag `tpcr-handback`, branched from v6.1.1). All planned
tests pass: 82 unit test entries, ~34 integration scenarios, crash matrix
3/3, resume matrix 12/12, soak/perf 7/7; requirement traceability clean.

**Known limits, stated plainly:** resume is qualified on POSIX backends
only — EC/CephFS/proxy deployments are *unsupported until tested* (a
qualification harness ships in `tests/XrdHttpTpcR/integration/`); the
10 TB overhead numbers are measured-then-extrapolated (testbed disk was
small); crash durability is validated at the API-ordering level, not
against real power loss; and a handful of deliberate spec deviations await
reviewer sign-off in the working tree's `QUESTIONS.md`.
