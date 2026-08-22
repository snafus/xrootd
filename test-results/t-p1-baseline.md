## T-P1 loopback baseline -- 2026-08-22T22:44:39Z
Payload: 256 MiB via python mock source on loopback.
(Relative handler comparison only; netem runs land with WP-12.)

| streams | stock secs | tpcr secs |
|---------|------------|-----------|
| 1 | 0.65 | 0.63 |
| 4 | 2.12 | 0.73 |
| 8 | 2.11 | 1.37 |

