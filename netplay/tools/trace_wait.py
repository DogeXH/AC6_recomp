#!/usr/bin/env -S python3 -I
"""Wait until a netplay JSONL trace contains a line with a given substring.

Usage: python3 -I trace_wait.py <trace> <substring> <timeout_seconds>
                                [--from-end] [--count N] [--quiet]

Prints each matching line. Exit status: 0 when found, 1 on timeout, 2 on bad usage.
The trace may not exist yet; it is followed while it grows (only complete lines count), and a
file that shrinks is read again from the start. --from-end ignores lines already present.
Example: python3 -I trace_wait.py run/instances/A/trace.jsonl '"ev":"xam.live"' 60
"""

import argparse
import os
import sys
import time

POLL_SECONDS = 0.1
MAX_READ = 1 << 20
MAX_LINE = 64 * 1024


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("trace")
    parser.add_argument("substring")
    parser.add_argument("timeout", type=float, help="seconds")
    parser.add_argument("--from-end", action="store_true",
                        help="ignore lines that are already in the file")
    parser.add_argument("--count", type=int, default=1, help="number of matching lines to wait for")
    parser.add_argument("--quiet", action="store_true", help="do not print matching lines")
    args = parser.parse_args()
    if args.count < 1 or args.timeout < 0 or not args.substring:
        parser.error("count must be >= 1, timeout >= 0 and the substring non-empty")

    needle = args.substring.encode("utf-8")
    deadline = time.monotonic() + args.timeout
    offset = 0
    if args.from_end:
        try:
            offset = os.path.getsize(args.trace)
        except OSError:
            offset = 0
    partial = b""
    found = 0

    while True:
        try:
            size = os.path.getsize(args.trace)
        except OSError:
            size = None
        if size is not None:
            if size < offset:  # truncated or replaced
                offset, partial = 0, b""
            if size > offset:
                try:
                    with open(args.trace, "rb") as f:
                        f.seek(offset)
                        chunk = f.read(min(size - offset, MAX_READ))
                except OSError:
                    chunk = b""
                offset += len(chunk)
                lines = (partial + chunk).split(b"\n")
                partial = lines.pop()[:MAX_LINE]
                for line in lines:
                    if needle in line:
                        found += 1
                        if not args.quiet:
                            print(line.decode("utf-8", "replace"), flush=True)
                        if found >= args.count:
                            return 0
                if size > offset:
                    continue  # more to read right away
        if time.monotonic() >= deadline:
            print(f"trace_wait: timed out after {args.timeout:g}s waiting for {args.substring!r} "
                  f"in {args.trace} ({found}/{args.count} found)", file=sys.stderr)
            return 1
        time.sleep(POLL_SECONDS)


if __name__ == "__main__":
    sys.exit(main())
