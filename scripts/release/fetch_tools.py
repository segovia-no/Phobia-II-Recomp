#!/usr/bin/env python3
"""Assemble the tools a release kit carries for one platform (docs/notes/07-builder.md).

    fetch_tools.py --platform macos-arm64|macos-x86_64|linux-x86_64|linux-arm64 --out DIR [--cache DIR]

Downloads the pinned files of scripts/release/toolchain.json (SHA-256 checked, cached) and lays out:
    DIR/python/    a private CPython (python-build-standalone) with the recompiler's dependencies
                   (pefile, capstone) in its site-packages
    DIR/zig/       the C compiler for the game-code module (recompiler/build_gamecode.py), with only
                   this platform's C headers / libc sources
    DIR/licenses/  the bundled tools' licences
Works for any platform from any machine: nothing downloaded is run here. Needs Python 3.12+.
Adapted from MotoRacerRecomp's scripts/release/fetch_tools.py.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
PLATFORMS = ("macos-arm64", "macos-x86_64", "linux-x86_64", "linux-arm64")


def fetch(url, sha256, cache):
    path = os.path.join(cache, url.rsplit("/", 1)[1])
    if not os.path.exists(path) or digest(path) != sha256:
        print(f"downloading {url}", flush=True)
        if shutil.which("curl"):   # the system's certificates (some Python installs have none)
            subprocess.run(["curl", "-fsSL", "-o", path + ".part", url], check=True)
        else:
            with urllib.request.urlopen(url) as r, open(path + ".part", "wb") as f:
                shutil.copyfileobj(r, f)
        os.replace(path + ".part", path)
    if digest(path) != sha256:
        raise SystemExit(f"checksum mismatch: {url}")
    return path


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def unpack(archive, dest, strip_top):
    """Unpacks a .tar.* or .zip into dest, dropping the archive's top folder if strip_top."""
    os.makedirs(dest, exist_ok=True)
    if archive.endswith((".zip", ".whl")):
        with zipfile.ZipFile(archive) as z:
            for info in z.infolist():
                name = info.filename.split("/", 1)[1] if strip_top and "/" in info.filename else info.filename
                if not name or name.startswith("/") or ".." in name.split("/"):
                    continue
                target = os.path.join(dest, name)
                if info.is_dir():
                    os.makedirs(target, exist_ok=True)
                    continue
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with z.open(info) as src, open(target, "wb") as out:
                    shutil.copyfileobj(src, out)
                mode = info.external_attr >> 16
                if mode & 0o111:
                    os.chmod(target, 0o755)
        return
    with tarfile.open(archive) as t:
        members = []
        for m in t.getmembers():
            # terminal descriptions (Linux Python): unused, and their case-only name variants
            # collide on case-insensitive disks
            if "/share/terminfo/" in m.name:
                continue
            if strip_top:
                parts = m.name.split("/", 1)
                if len(parts) < 2 or not parts[1]:
                    continue
                m.name = parts[1]
            members.append(m)
        t.extractall(dest, members=members, filter="data")


# zig carries the C headers and libc sources of every target it supports (~420 MB unpacked); a
# release builds the game code for one (recompiler/build_gamecode.py: C only, no C++): keep that one's
ZIG_KEEP = {
    "macos-arm64": {"include": {"any-darwin-any"}, "libc": {"darwin"}},
    "macos-x86_64": {"include": {"any-darwin-any"}, "libc": {"darwin"}},
    "linux-x86_64": {"include": {"x86-linux-gnu", "x86-linux-any", "any-linux-any", "generic-glibc"}, "libc": {"glibc"}},
    "linux-arm64": {"include": {"aarch64-linux-gnu", "aarch64-linux-any", "any-linux-any", "generic-glibc"}, "libc": {"glibc"}},
}
ZIG_DROP = ["doc", "lib/docs", "lib/libcxx", "lib/libcxxabi", "lib/libtsan"]


def prune_zig(zig, plat):
    keep = ZIG_KEEP[plat]
    for d in ZIG_DROP:
        shutil.rmtree(os.path.join(zig, d), ignore_errors=True)
    libc = os.path.join(zig, "lib", "libc")
    for name in os.listdir(libc):
        path = os.path.join(libc, name)
        if os.path.isdir(path) and name != "include" and name not in keep["libc"]:
            shutil.rmtree(path)
    inc = os.path.join(libc, "include")
    for name in os.listdir(inc):
        if name not in keep["include"]:
            shutil.rmtree(os.path.join(inc, name))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--platform", choices=PLATFORMS, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--cache", default=os.path.join(os.path.dirname(os.path.dirname(HERE)), "build", "tools", "cache"))
    args = ap.parse_args()
    pins = json.load(open(os.path.join(HERE, "toolchain.json")))
    plat, out = args.platform, os.path.abspath(args.out)
    os.makedirs(args.cache, exist_ok=True)
    if os.path.exists(out):
        shutil.rmtree(out)
    lic = os.path.join(out, "licenses")
    os.makedirs(lic)

    # Python + the translator's dependencies
    name, sha = pins["python"]["files"][plat]
    unpack(fetch(pins["python"]["url"].format(file=name), sha, args.cache), out, strip_top=False)   # -> out/python
    site = os.path.join(out, "python", "lib/python3.12", "site-packages")
    for url, sha in pins["wheels"]["all"] + pins["wheels"][plat]:
        unpack(fetch(url, sha, args.cache), site, strip_top=False)   # a wheel is a zip of site-packages
    py_lic = os.path.join(out, "python", "lib/python3.12/LICENSE.txt")
    if os.path.exists(py_lic):
        shutil.copy(py_lic, os.path.join(lic, "python.txt"))

    # zig
    name, sha = pins["zig"]["files"][plat]
    unpack(fetch(pins["zig"]["url"].format(file=name), sha, args.cache), os.path.join(out, "zig"), strip_top=True)
    prune_zig(os.path.join(out, "zig"), plat)
    shutil.copy(os.path.join(out, "zig", "LICENSE"), os.path.join(lic, "zig.txt"))

    print(f"tools for {plat} in {out}")


if __name__ == "__main__":
    if sys.version_info < (3, 12):
        raise SystemExit("needs Python 3.12+ (tarfile extraction filters)")
    main()
