# Finds code pointers in the image's data sections that target addresses the recompiler did not
# register as functions (and that are not jump-table labels inside a function).
import re, struct, sys, glob
img = open(sys.argv[1], 'rb').read()   # decrypted image, loaded at 0x82000000
gen = sys.argv[2]                       # generated/ dir
BASE = 0x82000000; CODE_LO = 0x82090000; CODE_HI = 0x82090000 + 0x3515F8
init = open(f'{gen}/ac6recomp_init.cpp').read()
funcs = set(int(a, 16) for a in re.findall(r'\{ 0x([0-9A-F]{8}), ', init))
labels = set()
for f in glob.glob(f'{gen}/ac6recomp_recomp.*.cpp'):
    for m in re.finditer(r'^loc_([0-9A-F]{8}):', open(f).read(), re.M):
        labels.add(int(m.group(1), 16))
# PE sections
e_lfanew = struct.unpack_from('<I', img, 0x3C)[0]
nsec = struct.unpack_from('<H', img, e_lfanew + 6)[0]
opt_size = struct.unpack_from('<H', img, e_lfanew + 20)[0]
sec_off = e_lfanew + 24 + opt_size
secs = []
for i in range(nsec):
    name, vsize, va, rsize, rptr = struct.unpack_from('<8sIIII', img, sec_off + 40 * i)
    ch = struct.unpack_from('<I', img, sec_off + 40 * i + 36)[0]
    secs.append((name.rstrip(b'\0').decode(), va, vsize, ch))
print('sections:', [(n, hex(BASE + va), hex(vs), hex(ch)) for n, va, vs, ch in secs])
cands = {}
for n, va, vs, ch in secs:
    if ch & 0x20000000:  # executable
        continue
    lo = va; hi = min(va + vs, len(img))
    for off in range(lo, hi - 3, 4):
        v = struct.unpack_from('>I', img, off)[0]
        if CODE_LO <= v < CODE_HI and v % 4 == 0 and v not in funcs and v not in labels:
            cands.setdefault(v, []).append((n, BASE + off))
def insn(a):
    return struct.unpack_from('>I', img, a - BASE)[0]
# keep targets whose first word decodes as a plausible instruction (not 0, not data-ish)
good = {}
for v, refs in cands.items():
    w = insn(v)
    if w == 0 or (w >> 26) == 0:
        continue
    good[v] = refs
print('raw candidates', len(cands), 'plausible', len(good))
for v in sorted(good)[:400]:
    print(hex(v), hex(insn(v)), len(good[v]), [(n, hex(a)) for n, a in good[v][:3]])

def is_terminator(w):
    if w in (0x4E800020, 0x4E800420, 0x00000000):  # blr, bctr, padding
        return True
    if (w >> 26) == 18 and (w & 3) == 0:  # b (unconditional, no link)
        return True
    return False
starts = []
for v in sorted(good):
    prev = insn(v - 4)
    if is_terminator(prev):
        starts.append(v)
print('function-start candidates', len(starts))
for v in starts:
    print('START', hex(v), hex(insn(v)), [(n, hex(a)) for n, a in good[v][:2]])
