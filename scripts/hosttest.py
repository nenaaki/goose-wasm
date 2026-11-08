"""Tests export fns called from a host (--header, spec 7.11) against upstream.

Each test in tests/host/ is a Goose program NAME.goose and two hosts making
the same calls: NAME.c, a C host for upstream's build (`goose --standalone
--header NAME.h -o NAME.c`, compiled with llvm-mingw's clang), and NAME.mjs,
a JS host for goose-wasm's module (`goose-wasm --header`, run by Node as a
WASI reactor). The module must validate, and both hosts must print the same
and exit alike; when upstream's exits nonzero, the module's stderr must hold
upstream's last stderr line, its runtime error.

    python scripts/hosttest.py [-O0] [--filter TEXT] [-v]
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from difftest import ROOT, decode, find_tool  # noqa: E402

HOST = ROOT / "tests" / "host"
OUT = ROOT / "build" / "hosttest"


def upstream(f, tools, work):
    """Upstream's build of f with its C host: (exit code, stdout, stderr), or
    a string saying what failed."""
    c, h, exe = work / f"{f.stem}.c", work / f"{f.stem}.h", work / f"{f.stem}.exe"
    r = subprocess.run([tools["goose"], "--standalone", str(f), "-o", str(c), "--header", str(h)],
                       capture_output=True, timeout=300)
    if r.returncode != 0:
        return f"upstream compile: {decode(r.stderr).strip()[-300:]}"
    env = dict(os.environ)
    env["PATH"] = str(tools["clang_dir"]) + os.pathsep + env.get("PATH", "")
    r = subprocess.run([str(Path(tools["clang_dir"]) / "clang"), "-O1", "-w", f"-I{HOST}", f"-I{work}", str(c), str(HOST / f"{f.stem}.c"),
                        "-o", str(exe)], capture_output=True, timeout=300, env=env)
    if r.returncode != 0:
        return f"host compile: {decode(r.stderr).strip()[-300:]}"
    r = subprocess.run([str(exe)], capture_output=True, timeout=120, cwd=work, stdin=subprocess.DEVNULL)
    return r.returncode, decode(r.stdout), decode(r.stderr)


def wasm(f, tools, work, opt):
    w, h = work / f"{f.stem}.wasm", work / f"{f.stem}.wasm.h"
    r = subprocess.run([tools["goose_wasm"], str(f), "-o", str(w), opt, "--header", str(h)],
                       capture_output=True, timeout=300)
    if r.returncode != 0:
        return f"compile: {decode(r.stdout + r.stderr).strip()[-300:]}"
    r = subprocess.run([tools["wasm_tools"], "validate", "--features", "all", str(w)], capture_output=True)
    if r.returncode != 0:
        return f"invalid module: {decode(r.stderr).strip()[:300]}"
    r = subprocess.run([tools["node"], "--no-warnings", "--stack-size=65500", str(HOST / f"{f.stem}.mjs"), str(w)],
                       capture_output=True, timeout=120, cwd=work, stdin=subprocess.DEVNULL)
    return r.returncode, decode(r.stdout), decode(r.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-O0", dest="opt", action="store_const", const="-O0", default="-O2")
    ap.add_argument("-O2", dest="opt", action="store_const", const="-O2")
    ap.add_argument("--filter", default="")
    ap.add_argument("-v", action="store_true", help="print the outputs of a failing test")
    args = ap.parse_args()

    exe = ".exe" if os.name == "nt" else ""
    tools = {
        "goose_wasm": find_tool(ROOT / "build" / "mingw" / f"goose-wasm{exe}", ROOT / "build" / f"goose-wasm{exe}"),
        "wasm_tools": find_tool(*sorted(ROOT.glob(f"tools/wasm-tools-*/wasm-tools{exe}"))),
        "goose": find_tool(ROOT / "upstream" / "goose" / "build-mingw" / f"goose{exe}"),
        "clang_dir": Path(find_tool(*sorted(ROOT.glob(f"tools/llvm-mingw-*/bin/clang{exe}")))).parent,
        "node": find_tool(shutil.which("node")),
    }
    failed = 0
    tests = [f for f in sorted(HOST.glob("*.goose")) if args.filter in f.stem]
    for f in tests:
        work = OUT / f.stem
        # A directory Windows will not remove (a process still in it) is
        # reused: every file a test makes is written anew.
        shutil.rmtree(work, ignore_errors=True)
        work.mkdir(parents=True, exist_ok=True)
        want, got = upstream(f, tools, work), wasm(f, tools, work, args.opt)
        status = "pass"
        if isinstance(want, str):
            status = want
        elif isinstance(got, str):
            status = got
        elif (want[0] != 0) != (got[0] != 0):
            status = f"exit {got[0]}, upstream {want[0]}: {got[2].strip()[-200:]}"
        elif want[1].rstrip("\n") != got[1].rstrip("\n"):
            status = "wrong output"
        elif want[0] != 0:
            last = want[2].strip().splitlines()[-1] if want[2].strip() else ""
            if last not in got[2]:
                status = f"wrong stderr: missing {last!r}"
        print(f"{'pass' if status == 'pass' else 'FAIL':5} {f.stem}" + ("" if status == "pass" else f"  {status}"))
        if status != "pass":
            failed += 1
            if args.v and not isinstance(want, str) and not isinstance(got, str):
                print(f"--- upstream (exit {want[0]})\n{want[1]}{want[2]}--- goose-wasm (exit {got[0]})\n{got[1]}{got[2]}")
    print(f"{len(tests) - failed} of {len(tests)} pass ({args.opt})")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
