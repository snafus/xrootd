#!/usr/bin/env python3
"""T-I8: strict wire-grammar checker for TPCR chunked COPY responses.

Models what gfal2/davix-style clients parse (CON-4): a sequence of perf
marker blocks followed by exactly one final verdict line.  TPCR's additions
(absolute byte counts, the FR-6 ``resumable-from`` suffix inside the failure
line) must be invisible to this grammar -- if this parser accepts the
response, a stock client's parser does too.

    marker      := "Perf Marker" NL
                   "Timestamp: " INT NL
                   "Stripe Index: 0" NL
                   "Stripe Bytes Transferred: " INT NL
                   "Total Stripe Count: 1" NL
                   [ "RemoteConnections: " ... NL ]
                   "End" NL
    response    := marker* verdict
    verdict     := ("success: " ...) | ("failure: " ...)

Also enforces BUG-13's frozen stripe fields and that marker byte counts are
monotonically non-decreasing (FR-5's absolute counts guarantee this even
across a resume).

Usage: check_grammar.py <response-file> [expected-verdict: success|failure]
Exit 0 = grammar OK; 1 = violation (printed).
"""

import re
import sys


def check(text, expected_verdict):
    lines = text.split("\n")
    index = 0
    marker_count = 0
    last_bytes = -1
    verdict = None

    while index < len(lines):
        line = lines[index]
        if line == "":
            index += 1
            continue
        if line == "Perf Marker":
            block = lines[index:index + 6]
            if len(block) < 5:
                return "truncated marker block at line %d" % (index + 1)
            if not re.fullmatch(r"Timestamp: \d+", block[1]):
                return "bad Timestamp line: %r" % block[1]
            # BUG-13: these two fields are frozen wire shape.
            if block[2] != "Stripe Index: 0":
                return "bad Stripe Index line: %r" % block[2]
            match = re.fullmatch(r"Stripe Bytes Transferred: (\d+)", block[3])
            if not match:
                return "bad Stripe Bytes Transferred line: %r" % block[3]
            transferred = int(match.group(1))
            if transferred < last_bytes:
                return "marker bytes went backwards (%d -> %d)" % (
                    last_bytes, transferred)
            last_bytes = transferred
            if block[4] != "Total Stripe Count: 1":
                return "bad Total Stripe Count line: %r" % block[4]
            index += 5
            if index < len(lines) and lines[index].startswith("RemoteConnections: "):
                index += 1
            if index >= len(lines) or lines[index] != "End":
                return "marker block missing End at line %d" % (index + 1)
            index += 1
            marker_count += 1
            continue
        if line.startswith("success: ") or line.startswith("failure: "):
            if verdict is not None:
                return "second verdict line at line %d" % (index + 1)
            verdict = "success" if line.startswith("success") else "failure"
            index += 1
            continue
        return "unrecognized line %d: %r" % (index + 1, line)

    if verdict is None:
        return "no final verdict line"
    if expected_verdict and verdict != expected_verdict:
        return "expected %s verdict, found %s" % (expected_verdict, verdict)
    return None


def main():
    if len(sys.argv) < 2:
        print("usage: check_grammar.py <response-file> [success|failure]")
        return 2
    expected = sys.argv[2] if len(sys.argv) > 2 else None
    with open(sys.argv[1]) as handle:
        text = handle.read()
    problem = check(text, expected)
    if problem:
        print("GRAMMAR VIOLATION: %s" % problem)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
