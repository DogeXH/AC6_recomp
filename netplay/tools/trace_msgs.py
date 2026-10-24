#!/usr/bin/env python3
# Summarises the game-channel messages (vnet.msg) of one or more netplay traces around a match.
#
#   trace_msgs.py <trace.jsonl>[=NAME] ... [--from EVENT] [--before S] [--after S] [--ids]
#
# Times are seconds relative to the first EVENT (default xlive.session.start) in the first
# trace. Traces are aligned by the wall clock in their netplay_start line (unix_ms); without it
# each trace is aligned on its own EVENT. Prints, per second, the message counts by direction
# (stx/srx = stream, dtx/drx = datagram) and engine message id, then the socket and session
# events (vnet.bind, net.close with guest stacks, xlive.session.*).
import collections
import json
import sys


def load(path):
    evs = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                evs.append(json.loads(line))
            except ValueError:
                pass
    return evs


def main():
    args = sys.argv[1:]
    anchor, before, after, show_ids = "xlive.session.start", 30.0, 60.0, False
    files = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--from":
            anchor = args[i + 1]; i += 2; continue
        if a == "--before":
            before = float(args[i + 1]); i += 2; continue
        if a == "--after":
            after = float(args[i + 1]); i += 2; continue
        if a == "--ids":
            show_ids = True; i += 1; continue
        path, _, name = a.partition("=")
        files.append((path, name or chr(ord("A") + len(files))))
        i += 1
    traces = [(name, load(path)) for path, name in files]

    # Wall-clock offset (ms) of each trace's t_ms = 0, if known.
    def wall0(evs):
        for e in evs:
            if e.get("ev") == "netplay_start" and "unix_ms" in e:
                return e["unix_ms"] - e["t_ms"]
        return None

    def first(evs, ev):
        for e in evs:
            if e.get("ev") == ev:
                return e["t_ms"]
        return None

    w0 = [wall0(evs) for _, evs in traces]
    base_name, base_evs = traces[0]
    t_anchor = first(base_evs, anchor)
    if t_anchor is None:
        sys.exit(f"no {anchor} in {base_name}")
    origin = []  # per trace: t_ms value that corresponds to relative time 0
    for k, (name, evs) in enumerate(traces):
        if w0[0] is not None and w0[k] is not None:
            origin.append(t_anchor + w0[0] - w0[k])
        else:
            t = first(evs, anchor)
            origin.append(t if t is not None else t_anchor)
            if k:
                print(f"note: {name} aligned on its own {anchor} (no unix_ms)")

    rows = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    events = []
    for k, (name, evs) in enumerate(traces):
        for e in evs:
            rel = (e["t_ms"] - origin[k]) / 1000.0
            if rel < -before or rel > after:
                continue
            ev = e.get("ev")
            if ev == "vnet.msg":
                key = f'{e["d"]}{e["lp"]}:{e["id"]:04x}' if e["id"] >= 0 else f'{e["d"]}{e["lp"]}:-'
                rows[int(rel // 1)][name][key] += 1
            elif ev in ("vnet.bind", "net.close", "net.shutdown", "vnet.reset", "vnet.drop",
                        "net.err", "vnet.established") or ev.startswith("xlive.session"):
                f = {x: y for x, y in e.items() if x not in ("t_ms", "ev")}
                events.append((rel, name, ev, json.dumps(f)[:240]))

    for sec in sorted(rows):
        parts = []
        for name, _ in traces:
            c = rows[sec].get(name)
            if not c:
                continue
            tot = sum(c.values())
            detail = " ".join(f"{k}x{n}" for k, n in sorted(c.items())) if show_ids else ""
            parts.append(f"{name}:{tot} {detail}".rstrip())
        print(f"{sec:+5d}s  " + " | ".join(parts))
    print()
    for rel, name, ev, f in sorted(events):
        print(f"{rel:+8.2f} {name} {ev} {f}")


if __name__ == "__main__":
    main()
