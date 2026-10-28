#!/usr/bin/env python3
"""Build the Linux (Steam Deck) release archive of AC6 Recomp Online.

  netplay/release/package_linux.py VERSION [--bin PATH] [--image IMAGE] [--out DIR]

VERSION is the release name used in file names, for example "beta.1", giving
out/release/AC6-Recomp-Online-beta.1-linux-x86_64.tar.gz with one top folder of the same name.
The game defaults to out/build/linux-amd64-relwithdebinfo/ac6recomp; its debug info is stripped
into the archive (the unstripped binary stays in the build dir). glibc's loader and libraries are
copied from the build image (default ac6-build:amd64), the same ones the game was linked
against; see linux/NOTICES-LINUX-RUNTIME.txt. Entries have a fixed owner and timestamp, so the
same inputs give the same archive. Also writes <archive>.sha256 and prints the hashes.
"""
import argparse
import gzip
import hashlib
import io
import os
import subprocess
import sys
import tarfile
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
MTIME = 1767225600  # 2026-01-01 00:00 UTC
GLIBC = ["libc.so.6", "libm.so.6", "libmvec.so.1", "libdl.so.2", "libpthread.so.0", "librt.so.1",
         "libresolv.so.2", "libutil.so.1", "libanl.so.1"]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def text(path, crlf=False):
    with open(path, "rb") as f:
        b = f.read().replace(b"\r\n", b"\n")
    return b.replace(b"\n", b"\r\n") if crlf else b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("version")
    ap.add_argument("--bin", default=os.path.join(ROOT, "out/build/linux-amd64-relwithdebinfo/ac6recomp"))
    ap.add_argument("--image", default="ac6-build:amd64")
    ap.add_argument("--out", default=os.path.join(ROOT, "out/release"))
    a = ap.parse_args()

    name = f"AC6-Recomp-Online-{a.version}-linux-x86_64"
    rel = os.path.join(ROOT, "netplay/release")
    lin = os.path.join(rel, "linux")

    with tempfile.TemporaryDirectory() as tmp:
        game = os.path.join(tmp, "ac6recomp")
        subprocess.run(["llvm-strip", "--strip-debug", "-o", game, a.bin], check=True)
        runtime = os.path.join(tmp, "runtime")
        os.makedirs(runtime)
        libs = " ".join(f"/usr/lib/{l}" for l in ["ld-linux-x86-64.so.2"] + GLIBC)
        subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64",
                        "--user", f"{os.getuid()}:{os.getgid()}", "-v", f"{runtime}:/out",
                        a.image, "bash", "-c", f"cp -L {libs} /out/"], check=True)

        files = [  # (name in the archive, data, mode)
            ("ac6recomp.sh", text(os.path.join(lin, "ac6recomp.sh")), 0o755),
            ("ac6recomp", open(game, "rb").read(), 0o755),
            ("ld-linux-x86-64.so.2", open(os.path.join(runtime, "ld-linux-x86-64.so.2"), "rb").read(), 0o755),
        ]
        files += [(f"lib/{l}", open(os.path.join(runtime, l), "rb").read(), 0o755) for l in GLIBC]
        files += [
            ("ac6recomp-online-example.toml", text(os.path.join(lin, "ac6recomp-online-example.toml")), 0o644),
            ("README-ONLINE-LINUX.txt", text(os.path.join(lin, "README-ONLINE-LINUX.txt")), 0o644),
            ("PLAYING_ONLINE.md", text(os.path.join(ROOT, "netplay/docs/PLAYING_ONLINE.md")), 0o644),
            ("LICENSE.txt", text(os.path.join(ROOT, "LICENSE")), 0o644),
            ("THIRD_PARTY_NOTICES.txt", text(os.path.join(rel, "THIRD_PARTY_NOTICES.txt")), 0o644),
            ("NOTICES-LINUX-RUNTIME.txt", text(os.path.join(lin, "NOTICES-LINUX-RUNTIME.txt")), 0o644),
        ]
        for d in (os.path.join(rel, "licenses"), os.path.join(lin, "licenses")):
            for lic in sorted(os.listdir(d)):
                files.append((f"licenses/{lic}", text(os.path.join(d, lic)), 0o644))

        os.makedirs(a.out, exist_ok=True)
        tpath = os.path.join(a.out, name + ".tar.gz")
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as t:
            def entry(arc, mode, typ=tarfile.REGTYPE, size=0):
                info = tarfile.TarInfo(f"{name}/{arc}" if arc else name)
                info.type, info.mode, info.size, info.mtime = typ, mode, size, MTIME
                info.uid = info.gid = 0
                info.uname = info.gname = ""
                return info
            t.addfile(entry("", 0o755, tarfile.DIRTYPE))
            for d in ("lib", "licenses"):
                t.addfile(entry(d, 0o755, tarfile.DIRTYPE))
            for arc, data, mode in files:
                t.addfile(entry(arc, mode, size=len(data)), io.BytesIO(data))
        with open(tpath, "wb") as f, gzip.GzipFile(fileobj=f, mode="wb", compresslevel=9, mtime=MTIME, filename="") as g:
            g.write(buf.getvalue())

        game_sum, arc_sum = sha256(game), sha256(tpath)
    with open(tpath + ".sha256", "w") as f:
        f.write(f"{arc_sum}  {name}.tar.gz\n")
    print(f"game {game_sum}  (stripped ac6recomp)")
    print(f"tar  {arc_sum}  {tpath}  ({os.path.getsize(tpath)} bytes)")


if __name__ == "__main__":
    sys.exit(main())
