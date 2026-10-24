#!/usr/bin/env python3
# Maps guest return addresses (for example the "stack" arrays of net.close traces) to call sites
# in the recompiled code: the containing function, the generated file and line, and the callee.
#
#   guest_sym.py [--gen DIR] 0x821C0E28 0x8216D2D8 ...
#   guest_sym.py [--gen DIR] --trace trace.jsonl [--ev net.close]
#
# A return address R is found as "ctx.lr = 0xR;" just before the call it returns from.
import glob
import json
import os
import re
import sys

FUNC_RE = re.compile(r"(?:DEFINE_REX_FUNC\(|PPC_FUNC_IMPL\(__imp__)rex_sub_([0-9A-Fa-f]{8})\)")


def index(gen):
    """{return address: (file, line, function, callee line)} for every call site."""
    sites = {}
    for path in sorted(glob.glob(os.path.join(gen, "ac6recomp_recomp.*.cpp"))):
        func = None
        with open(path, errors="replace") as f:
            lines = f.readlines()
        for i, line in enumerate(lines):
            m = FUNC_RE.search(line)
            if m:
                func = "sub_" + m.group(1).upper()
                continue
            s = line.strip()
            if s.startswith("ctx.lr = 0x") and s.endswith(";"):
                addr = int(s[len("ctx.lr = "):-1], 16)
                callee = lines[i + 1].strip() if i + 1 < len(lines) else ""
                sites[addr] = (os.path.basename(path), i + 1, func, callee)
    return sites


def main():
    args = sys.argv[1:]
    gen = "/Users/cole/AC6_recomp_netplay/generated"
    trace, ev, addrs = None, "net.close", []
    while args:
        a = args.pop(0)
        if a == "--gen":
            gen = args.pop(0)
        elif a == "--trace":
            trace = args.pop(0)
        elif a == "--ev":
            ev = args.pop(0)
        else:
            addrs.append(int(a, 16))
    sites = index(gen)
    groups = []
    if trace:
        with open(trace) as f:
            for line in f:
                try:
                    e = json.loads(line)
                except ValueError:
                    continue
                if e.get("ev", "").startswith(ev) and e.get("stack"):
                    groups.append((json.dumps({k: v for k, v in e.items() if k != "stack"}),
                                   [int(x, 16) for x in e["stack"]]))
    if addrs:
        groups.append(("addresses", addrs))
    seen = set()
    for title, stack in groups:
        key = tuple(stack)
        if key in seen:
            print(title, "(same stack as above)")
            continue
        seen.add(key)
        print(title)
        for a in stack:
            s = sites.get(a)
            if s:
                print(f"  {a:08X}  in {s[2]}  ({s[0]}:{s[1]})  -> {s[3][:80]}")
            else:
                print(f"  {a:08X}  (no call site; not a return address in this codegen)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
