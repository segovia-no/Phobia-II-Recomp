#!/usr/bin/env python3
"""Assemble a release kit for one platform (docs/notes/07-builder.md).

    package.py --platform macos-arm64|macos-x86_64|linux-x86_64|linux-arm64 --host PATH --tools DIR
               [--sdl LIB] [--out DIR] [--version V]

The kit contains no game code. On the player's machine the launcher (`phobia2.sh`) builds the game-code
module from their own Game.exe with the bundled Python + zig (first start and after updates, ~15 s), then
starts the program:

    PhobiaIIRecomp-<version>-<platform>/
      phobia2.sh            launcher: build gamecode if stale, run
      phobia2               host program (runtime + SDL2 loader, built with PHOBIA_GAME_MODULE=ON)
      lib/                  SDL2 shared library
      recompiler/           lift.py, patches.py, extra_entries.txt, build_gamecode.py
      include/              runtime headers the generated C includes (cpu.h, runtime.h)
      tools/                python/, zig/, licenses/ (scripts/release/fetch_tools.py)
      game/                 (empty) put Phobia II here: Game.exe, pics/, sounds/, dat/
      README.txt, LICENSE
Output: DIR/PhobiaIIRecomp-<version>-<platform>.tar.gz
"""
import argparse
import os
import shutil
import subprocess
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

LAUNCHER = r'''#!/bin/sh
# Phobia II Recomp launcher: builds the game code from your Game.exe when needed, then starts the game.
#   ./phobia2.sh [game folder]        (default: ./game next to this script, or $PHOBIA_DATA)
KIT="$(cd "$(dirname "$0")" && pwd)"
GAME="${1:-${PHOBIA_DATA:-$KIT/game}}"
if [ ! -f "$GAME/Game.exe" ]; then
  echo "Phobia II not found in: $GAME"
  echo "Copy your Phobia II folder (Game.exe, pics/, sounds/, dat/) into $KIT/game, or pass its path."
  exit 1
fi
GAME="$(cd "$GAME" && pwd)"
case "$(uname -s)" in Darwin) LIB=gamecode.dylib ;; *) LIB=gamecode.so ;; esac
"$KIT/tools/python/bin/python3" -I "$KIT/recompiler/build_gamecode.py" \
    --exe "$GAME/Game.exe" --out "$KIT/gamecode" --if-stale || exit 1
export PHOBIA_DATA="$GAME"
export PHOBIA_GAME_MODULE="$KIT/gamecode/$LIB"
exec "$KIT/phobia2" "$@"
'''

README = """Phobia II Recomp {version} ({platform})
https://github.com/segovia-no/Phobia-II-Recomp

Native {osname} port of Phobia II (WAH-Software, 1998) by static recompilation.
This download contains no game code or data: you need your own copy of Phobia II v2.0.

1. Copy your Phobia II folder (Game.exe, pics/, sounds/, dat/) into the game/ folder here.
2. Run ./phobia2.sh
   The first start translates your Game.exe to C and compiles it with the bundled tools
   (about 15 seconds, once; again only after an update). Then the game starts.

Keys: the game's own (Enter fires by default; scripts in the source repository rebind them).
Alt+Enter toggles fullscreen.
{osnote}
The compiled game code (gamecode/) is made from your Game.exe: do not redistribute it.
Licences of the bundled tools: tools/licenses/. The port itself: LICENSE (MIT).
"""

OSNOTE = {
    "macos": "macOS: the first time, right-click phobia2.sh > Open (or run it from Terminal) because the\n"
             "download is not notarised. If macOS blocks the program, run:\n"
             "    xattr -dr com.apple.quarantine <this folder>\n",
    "linux": "Linux: needs a desktop with X11 or Wayland and glibc 2.31 or newer (Steam Deck: desktop mode,\n"
             "or add phobia2.sh to Steam as a non-Steam game).\n",
}


def run(cmd):
    subprocess.run(cmd, check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--platform", required=True, choices=["macos-arm64", "macos-x86_64", "linux-x86_64", "linux-arm64"])
    ap.add_argument("--host", required=True, help="the host program (built with -DPHOBIA_GAME_MODULE=ON)")
    ap.add_argument("--tools", required=True, help="tools folder from fetch_tools.py for this platform")
    ap.add_argument("--sdl", help="SDL2 shared library to bundle (default: the one next to --host, as the "
                                  "build_*_host.sh scripts leave it)")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "release"))
    ap.add_argument("--version", default=subprocess.run(["git", "-C", ROOT, "describe", "--always", "--dirty"],
                                                        capture_output=True, text=True).stdout.strip() or "dev")
    args = ap.parse_args()
    osname = args.platform.split("-")[0]
    name = f"PhobiaIIRecomp-{args.version}-{args.platform}"
    kit = os.path.join(args.out, name)
    if os.path.exists(kit):
        shutil.rmtree(kit)
    os.makedirs(os.path.join(kit, "lib"))

    # host program + SDL2
    host = os.path.join(kit, "phobia2")
    shutil.copy2(args.host, host)
    if osname == "linux":
        sdl = args.sdl or os.path.join(os.path.dirname(args.host), "libSDL2-2.0.so.0")
        shutil.copy2(sdl, os.path.join(kit, "lib", "libSDL2-2.0.so.0"))   # found through RPATH $ORIGIN/lib
    else:
        sdl = args.sdl or os.path.join(os.path.dirname(args.host), "libSDL2-2.0.0.dylib")
        dst = os.path.join(kit, "lib", "libSDL2-2.0.0.dylib")
        shutil.copy2(sdl, dst)
        os.chmod(dst, 0o755)
        old = subprocess.run(["otool", "-L", host], capture_output=True, text=True).stdout
        old = next(l.split()[0] for l in old.splitlines()[1:] if "libSDL2" in l)
        run(["install_name_tool", "-id", "@rpath/libSDL2-2.0.0.dylib", dst])
        run(["install_name_tool", "-change", old, "@executable_path/lib/libSDL2-2.0.0.dylib", host])
        for f in (dst, host):   # changed binaries need a fresh (ad-hoc) signature on Apple silicon
            run(["codesign", "-s", "-", "-f", f])

    # recompiler + headers + tools
    rc = os.path.join(kit, "recompiler")
    os.makedirs(rc)
    for f in ("lift.py", "patches.py", "extra_entries.txt", "build_gamecode.py", "requirements.txt"):
        shutil.copy2(os.path.join(ROOT, "recompiler", f), rc)
    inc = os.path.join(kit, "include")
    os.makedirs(inc)
    for f in ("cpu.h", "runtime.h"):
        shutil.copy2(os.path.join(ROOT, "runtime", f), inc)
    shutil.copytree(args.tools, os.path.join(kit, "tools"), symlinks=True)

    # launcher, docs, empty game folder
    with open(os.path.join(kit, "phobia2.sh"), "w") as f:
        f.write(LAUNCHER)
    os.chmod(os.path.join(kit, "phobia2.sh"), 0o755)
    with open(os.path.join(kit, "README.txt"), "w") as f:
        f.write(README.format(version=args.version, platform=args.platform,
                              osname="macOS" if osname == "macos" else "Linux", osnote=OSNOTE[osname]))
    shutil.copy2(os.path.join(ROOT, "LICENSE"), kit)
    os.makedirs(os.path.join(kit, "game"))
    open(os.path.join(kit, "game", "PUT_PHOBIA_II_HERE.txt"), "w").write(
        "Copy Game.exe, pics/, sounds/ and dat/ from your Phobia II v2.0 folder here.\n")

    archive = kit + ".tar.gz"
    with tarfile.open(archive, "w:gz") as t:
        t.add(kit, arcname=name)
    print(f"{archive} ({os.path.getsize(archive) // (1 << 20)} MB)")


if __name__ == "__main__":
    main()
