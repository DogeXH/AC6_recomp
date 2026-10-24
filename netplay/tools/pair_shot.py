# Combines A_<name>.png and B_<name>.png side by side at half size -> pair_<name>.png (run root).
import sys
from PIL import Image
root = "/Users/cole/ACE6-Recomp/ac6-online-20261019/run/instances"
name = sys.argv[1]
a = Image.open(f"{root}/A/shots/repl/A_{name}.png"); b = Image.open(f"{root}/B/shots/repl/B_{name}.png")
w, h = a.width // 2, a.height // 2
out = Image.new("RGB", (w * 2 + 8, h), (255, 255, 255))
out.paste(a.resize((w, h)), (0, 0)); out.paste(b.resize((w, h)), (w + 8, 0))
p = f"{root}/pair_{name}.png"; out.save(p); print(p)
