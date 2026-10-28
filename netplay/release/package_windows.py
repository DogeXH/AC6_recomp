#!/usr/bin/env python3
"""Build the Windows release zip of AC6 Recomp Online.

  netplay/release/package_windows.py VERSION [--exe PATH] [--out DIR]

VERSION is the release name used in file names, for example "beta.1", giving
out/release/AC6-Recomp-Online-beta.1-win-amd64.zip with one top folder of the same name. The exe
defaults to the path-free stage build (out/stage/win-amd64/ac6recomp.exe). Text files (.txt)
get Windows line endings; entries have a fixed timestamp, so the same inputs give the same zip.
Also writes <zip>.sha256 and prints the exe and zip hashes.
"""
import argparse
import hashlib
import os
import sys
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
STAMP = (2026, 1, 1, 0, 0, 0)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def text(path, crlf):
    with open(path, "rb") as f:
        b = f.read().replace(b"\r\n", b"\n")
    return b.replace(b"\n", b"\r\n") if crlf else b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("version")
    ap.add_argument("--exe", default=os.path.join(ROOT, "out/stage/win-amd64/ac6recomp.exe"))
    ap.add_argument("--out", default=os.path.join(ROOT, "out/release"))
    a = ap.parse_args()

    name = f"AC6-Recomp-Online-{a.version}-win-amd64"
    rel = os.path.join(ROOT, "netplay/release")
    files = [  # (name in the zip, source, Windows line endings)
        ("ac6recomp-online-example.toml", os.path.join(rel, "ac6recomp-online-example.toml"), False),
        ("README-ONLINE.txt", os.path.join(rel, "README-ONLINE.txt"), True),
        ("PLAYING_ONLINE.md", os.path.join(ROOT, "netplay/docs/PLAYING_ONLINE.md"), False),
        ("LICENSE.txt", os.path.join(ROOT, "LICENSE"), True),
        ("THIRD_PARTY_NOTICES.txt", os.path.join(rel, "THIRD_PARTY_NOTICES.txt"), True),
    ]
    for lic in sorted(os.listdir(os.path.join(rel, "licenses"))):
        files.append((f"licenses/{lic}", os.path.join(rel, "licenses", lic), lic.endswith(".txt")))

    os.makedirs(a.out, exist_ok=True)
    zpath = os.path.join(a.out, name + ".zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        def add(arc, data, mode=0o644):
            info = zipfile.ZipInfo(f"{name}/{arc}", STAMP)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o100000 | mode) << 16
            z.writestr(info, data)
        with open(a.exe, "rb") as f:
            add("ac6recomp.exe", f.read(), 0o755)
        for arc, src, crlf in files:
            add(arc, text(src, crlf))

    exe_sum, zip_sum = sha256(a.exe), sha256(zpath)
    with open(zpath + ".sha256", "w") as f:
        f.write(f"{zip_sum}  {name}.zip\n")
    print(f"exe  {exe_sum}  {a.exe}")
    print(f"zip  {zip_sum}  {zpath}  ({os.path.getsize(zpath)} bytes)")


if __name__ == "__main__":
    sys.exit(main())
