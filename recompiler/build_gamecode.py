#!/usr/bin/env python3
"""Build the game-code module from Game.exe for a target platform (docs/notes/07-builder.md).

    build_gamecode.py --exe Game.exe --out DIR [--cc auto|zig|cc|PATH] [--zig ZIG]
                      [--target macos|linux] [--arch arm64|x86_64] [--jobs N] [--if-stale]

Translates Game.exe to C (lift.py --module), compiles the files in parallel and links
DIR/gamecode.dylib (macOS) or DIR/gamecode.so (Linux), which the program built with
PHOBIA_GAME_MODULE=ON loads. Only a C compiler is needed: the module's undefined symbols (M, dispatch,
rt_*, ...) are the program's, resolved when the program loads it.

Compiler: a bundled `zig cc` (cross-compiles to every target, no SDK needed) or the system `cc` for the
host platform. --if-stale skips the build when DIR/gamecode.stamp matches Game.exe and this recompiler.
"""
import argparse
import concurrent.futures
import hashlib
import os
import platform
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FILES = 16
# zig targets: glibc floor 2.28 (Debian 10 / RHEL 8 era; Steam Deck has 2.35+), macOS 11+
ZIG_TARGETS = {
    "macos": {"arm64": "aarch64-macos.11.0", "x86_64": "x86_64-macos.11.0"},
    "linux": {"x86_64": "x86_64-linux-gnu.2.28", "arm64": "aarch64-linux-gnu.2.28"},
}
SUFFIX = {"macos": ".dylib", "linux": ".so"}
CFLAGS = ["-O2", "-g0", "-w", "-std=gnu11", "-fwrapv", "-fno-strict-aliasing", "-fPIC", "-fvisibility=hidden"]


def host_os():
    return {"Darwin": "macos", "Linux": "linux"}.get(platform.system(), "unknown")


def host_arch():
    m = platform.machine().lower()
    return "arm64" if m in ("arm64", "aarch64") else "x86_64"


def find_include_dir():
    # source tree: ../runtime; release kit: ../include (copied runtime headers)
    for d in (os.path.join(HERE, "..", "runtime"), os.path.join(HERE, "..", "include")):
        if os.path.exists(os.path.join(d, "runtime.h")):
            return os.path.abspath(d)
    raise SystemExit("runtime headers (runtime.h, cpu.h) not found next to the recompiler")


def find_zig(explicit):
    cands = [explicit] if explicit else []
    cands += [os.path.join(HERE, "..", "tools", "zig", "zig"), shutil.which("zig")]
    for c in cands:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return os.path.abspath(c)
    return None


def stamp_of(exe):
    h = hashlib.sha256(open(exe, "rb").read()).hexdigest()
    t = hashlib.sha256()
    for f in ("lift.py", "patches.py", "extra_entries.txt", "build_gamecode.py"):
        p = os.path.join(HERE, f)
        if os.path.exists(p):
            t.update(open(p, "rb").read())
    return f"exe={h}\nrecompiler={t.hexdigest()}\n"


def run(cmd, env=None):
    p = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if p.returncode != 0:
        sys.stderr.write(" ".join(cmd) + "\n" + p.stdout + p.stderr)
        raise SystemExit(f"failed: {os.path.basename(cmd[0])} (exit {p.returncode})")
    return p


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exe", required=True, help="Game.exe (Phobia II v2.0)")
    ap.add_argument("--out", required=True, help="output folder (gamecode library, generated C, objects)")
    ap.add_argument("--cc", default="auto", help="auto (bundled zig, else system cc), zig, cc, or a compiler path")
    ap.add_argument("--zig", help="path to zig (default: tools/zig/zig next to the recompiler, then PATH)")
    ap.add_argument("--zig-lib", help="zig's lib/ folder to use (a kit's zig keeps only its own platform's C "
                                      "headers: cross builds point this at the target kit's tools/zig/lib)")
    ap.add_argument("--target", choices=sorted(ZIG_TARGETS), default=host_os() if host_os() in ZIG_TARGETS else "linux")
    ap.add_argument("--arch", choices=["arm64", "x86_64"], default=host_arch())
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--if-stale", action="store_true", help="do nothing if the module is up to date")
    args = ap.parse_args()

    out = os.path.abspath(args.out)
    lib = os.path.join(out, "gamecode" + SUFFIX[args.target])
    stamp_path = os.path.join(out, "gamecode.stamp")
    stamp = stamp_of(args.exe) + f"target={args.target}-{args.arch}\n"
    if args.if_stale and os.path.exists(lib) and os.path.exists(stamp_path) and open(stamp_path).read() == stamp:
        print(f"up to date: {lib}")
        return

    # compiler selection
    native = args.target == host_os() and args.arch == host_arch()
    zig = None
    if args.cc in ("auto", "zig"):
        zig = find_zig(args.zig)
        if not zig and (args.cc == "zig" or not native):
            raise SystemExit("zig not found (needed for --cc zig or cross builds): pass --zig or put it in tools/zig/")
    if zig:
        triple = ZIG_TARGETS[args.target][args.arch]
        cc = [zig, "cc", "-target", triple]
        how = f"zig cc ({triple})"
    else:
        if not native:
            raise SystemExit(f"cross-building for {args.target}-{args.arch} needs zig")
        compiler = args.cc if args.cc not in ("auto", "cc") else (os.environ.get("CC") or shutil.which("cc") or "cc")
        cc = [compiler]
        if args.target == "macos" and args.arch == "arm64" and host_arch() != "arm64":
            cc += ["-arch", "arm64"]
        how = os.path.basename(compiler)

    gen, obj = os.path.join(out, "gen"), os.path.join(out, "obj")
    os.makedirs(obj, exist_ok=True)
    env = dict(os.environ)
    env["ZIG_GLOBAL_CACHE_DIR"] = os.path.join(out, "zig-cache")
    env["ZIG_LOCAL_CACHE_DIR"] = os.path.join(out, "zig-cache")
    if args.zig_lib:
        env["ZIG_LIB_DIR"] = os.path.abspath(args.zig_lib)

    t0 = time.time()
    print(f"translating {args.exe} ...", flush=True)
    run([sys.executable, "-I", os.path.join(HERE, "lift.py"), args.exe, gen, "--files", str(FILES), "--module",
         "--extra", os.path.join(HERE, "extra_entries.txt")])
    t1 = time.time()

    srcs = sorted(os.path.join(gen, f) for f in os.listdir(gen) if f.endswith(".c"))
    inc = ["-I", find_include_dir(), "-I", gen]
    defs = ["-D_GNU_SOURCE"] if args.target == "linux" else []

    def compile_one(src):
        o = os.path.join(obj, os.path.basename(src)[:-2] + ".o")
        run(cc + CFLAGS + defs + inc + ["-c", src, "-o", o], env)
        return o

    print(f"compiling {len(srcs)} files with {how}, {args.jobs} jobs ...", flush=True)
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        objs = list(pool.map(compile_one, srcs))
    t2 = time.time()

    link = cc + ["-shared", "-o", lib] + objs
    if args.target == "macos":
        link += ["-Wl,-undefined,dynamic_lookup"]
    else:
        link += ["-s"]
    run(link, env)
    open(stamp_path, "w").write(stamp)
    print(f"built {lib} ({os.path.getsize(lib) // 1024} KB): translate {t1 - t0:.1f}s, "
          f"compile {t2 - t1:.1f}s, link {time.time() - t2:.1f}s")


if __name__ == "__main__":
    main()
