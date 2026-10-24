#!/usr/bin/env python3
"""Re-applies the Deck tree's hand patch to freshly generated code (marketplace null-record guard)."""
import glob, re, sys
root = sys.argv[1]
anchor = "\tctx.lr = 0x821C0E28;\n\trex_sub_82234D40(ctx, base);\n"
guard = ("\t// Treat an unavailable marketplace color record as an empty color table.\n"
         "\t// The retail path otherwise dereferences the null record at guest +2.\n"
         "\tif (ctx.r3.u32 == 0) goto loc_821C0EB0;\n")
hits = 0
for p in sorted(glob.glob(f"{root}/generated/ac6recomp_recomp.*.cpp")):
    s = open(p).read()
    if anchor not in s:
        continue
    if anchor + guard in s:
        print("already patched", p); hits += 1; continue
    i = s.index(anchor) + len(anchor)
    if "loc_821C0EB0:" not in s[i:i + 20000]:
        sys.exit(f"label loc_821C0EB0 not found after anchor in {p}")
    s = s[:i] + guard + s[i:]
    open(p, "w").write(s); print("patched", p); hits += 1
if hits != 1:
    sys.exit(f"expected exactly one anchor, found {hits}")
