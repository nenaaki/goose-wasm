"""Runs upstream Goose's test fixtures through goose-wasm and wasmtime and
checks them against upstream's expected output, by the rules of upstream's
test/run_tests.py:

- stdout, with CRLF folded to LF and trailing newlines dropped, equals
  test/expected/<name>.out;
- a fixture with expected/<name>.aborts must exit nonzero, one without it
  must exit 0;
- every nonblank line of expected/<name>.stderr must occur in stderr.

Every module is also validated with `wasm-tools validate` before it runs, so
a module that breaks the Wasm spec fails even if its output would be right.

This project's own programs in tests/ have no expected files: upstream's
compiler is the oracle. Each runs under `goose run` (the build that
scripts/upstream.ps1 makes), or, for a program with workers, which upstream
does not JIT, as the executable `goose build` makes with llvm-mingw's clang;
goose-wasm must match its stdout, its exit status and, for a nonzero exit,
its stderr.

A module of a program with workers or queues is a wasi-threads one and runs
on wasmtime 46 (tools/dl46, or $WASMTIME46), the last with wasi-threads.
With `--host node`, every module runs on Node.js instead, under
scripts/wasihost.mjs, wasi-threads ones included.

    python scripts/difftest.py [-j N] [-O0|-O2] [--host wasmtime|node] [--filter text] [-v]

Results go to build/difftest/results.txt, one line per fixture.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TESTS = ROOT / "upstream" / "goose" / "test"
EXPECTED = TESTS / "expected"
OWN = ROOT / "tests"
OUT = ROOT / "build" / "difftest"

# Categories that are not runnable programs (upstream's runner leaves out the
# first four), or that need native packages this backend does not provide.
SKIP_DIRS = {"errors", "errors_tc", "goose_in_goose", "lsp",
             "audio", "gfx", "physics", "ui", "packages"}

# The backend reports what it cannot translate yet with this prefix.
UNSUPPORTED = "goose-wasm: unsupported:"


def find_tool(*candidates):
    for c in candidates:
        if c and Path(c).exists():
            return str(c)
    sys.exit(f"tool not found: {candidates}")


def find_optional(*candidates):
    """A tool only some fixtures need: its path, or None, which the
    fixtures that need it report (no-tool) instead of stopping the run."""
    for c in candidates:
        if c and Path(c).exists():
            return str(c)
    return None


def fixtures(filter_text):
    for f in sorted(TESTS.glob("*/*.goose")):
        if f.parent.name in SKIP_DIRS or f.name == "lexer_tokens.goose":
            continue
        if not (EXPECTED / f"{f.stem}.out").exists():
            continue
        if filter_text and filter_text not in label(f):
            continue
        yield f
    for f in sorted(OWN.glob("*.goose")):
        if not filter_text or filter_text in label(f):
            yield f


def label(f):
    return str(f.relative_to(TESTS)) if f.is_relative_to(TESTS) else str(f.relative_to(ROOT))


def runtime_defines(f):
    """A fixture's `// runtime-define: NAME=VALUE` first line, which upstream's
    runner passes to every compile as -D (run_tests.py, runtime_defines)."""
    with open(f, encoding="utf-8", errors="replace") as fh:
        first = fh.readline().rstrip("\r\n")
    return re.findall(r"^// runtime-define: (\w+=\w+)$", first)


def decode(data):
    return data.decode("utf-8", errors="replace").replace("\r\n", "\n")


# On Node, a wasi-threads module's threads are Workers whose engine stacks
# Windows commits whole (scripts/wasihost.mjs): such modules run one at a
# time, so that together they do not run out of memory.
NODE_THREADED = threading.Lock()


def run_one(f, tools, opt, workdir, host):
    name = f.stem
    wasm = Path(workdir) / f"{name}.wasm"
    defines = [a for d in runtime_defines(f) for a in ("-D", d)]
    r = subprocess.run([tools["goose_wasm"], str(f), "-o", str(wasm), opt, *defines],
                       capture_output=True, timeout=300)
    if r.returncode != 0:
        err = decode(r.stderr)
        if UNSUPPORTED in err:
            line = next(l for l in err.splitlines() if UNSUPPORTED in l)
            return "unsupported", line[line.index(UNSUPPORTED) + len(UNSUPPORTED):].strip()
        return "compile-error", err.strip().splitlines()[-1] if err.strip() else f"exit {r.returncode}"
    r = subprocess.run([tools["wasm_tools"], "validate", str(wasm)], capture_output=True)
    if r.returncode != 0:
        return "invalid", decode(r.stderr).strip()
    # A module of a program with workers or queues is a wasi-threads one,
    # which wasmtime 47 and later no longer run (README: running the
    # modules): it runs on wasmtime 46, with threads, shared memories and
    # exceptions on.
    threaded = b"thread-spawn" in wasm.read_bytes()
    engine = [tools["wasmtime"], "run"]
    if host == "node":
        # The Node host takes no engine options: it preopens "." itself and
        # gives the program's Worker the same engine stack.
        engine = [tools["node"], "--no-warnings", str(ROOT / "scripts" / "wasihost.mjs"), str(wasm)]
    elif threaded:
        if not tools["wasmtime46"]:
            return "no-tool", "wasmtime 46 not found (tools/dl46, or $WASMTIME46); try --host node"
        engine = [tools["wasmtime46"], "run", "-S", "threads=y", "-W",
                  "threads=y,shared-memory=y,exceptions=y"]
    if host != "node":
        # A stack for the engine as deep as recursion can go before the
        # module itself reports the overflow (README: hosts).
        engine += ["-W", "multi-memory=y", "-W", "max-wasm-stack=268435456", "--dir", ".", str(wasm)]
    # Each run gets a directory of its own, preopened as "." (os's working
    # directory), so that files a fixture writes stay out of upstream's tree.
    rundir = Path(workdir) / f"{name}.run"
    rundir.mkdir(exist_ok=True)
    try:
        if host == "node" and threaded:
            with NODE_THREADED:
                r = subprocess.run(engine, capture_output=True, timeout=120, cwd=rundir, stdin=subprocess.DEVNULL)
        else:
            r = subprocess.run(engine, capture_output=True, timeout=120, cwd=rundir, stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return "timeout", ""
    out, err = decode(r.stdout), decode(r.stderr)
    if threaded and host != "node":
        # wasmtime 46's notice that -S threads is going away.
        err = "\n".join(l for l in err.splitlines() if "-Sthreads" not in l)
    if f.is_relative_to(OWN):
        return compare_with_upstream(f, tools, r.returncode, out, err, workdir)
    aborts = (EXPECTED / f"{name}.aborts").exists()
    if aborts != (r.returncode != 0):
        return "wrong-exit", f"exit {r.returncode}: {err.strip()[:200]}"
    stderr_file = EXPECTED / f"{name}.stderr"
    if stderr_file.exists():
        markers = [l.strip() for l in decode(stderr_file.read_bytes()).splitlines() if l.strip()]
        missing = [m for m in markers if m not in err]
        if missing:
            return "wrong-stderr", f"missing {missing[0]!r}"
    want = decode((EXPECTED / f"{name}.out").read_bytes()).rstrip("\n")
    if out.rstrip("\n") != want:
        return "wrong-output", f"got {out.rstrip()[:80]!r}"
    return "pass", ""


def compare_with_upstream(f, tools, code, out, err, workdir):
    """An own test: upstream's `goose run` of the same file is the oracle.
    Both get the file by the same absolute path, so locations in runtime
    errors agree."""
    defines = [a for d in runtime_defines(f) for a in ("-D", d)]
    # Upstream's run in a directory of its own too, for the files it writes.
    updir = Path(workdir) / f"{f.stem}.upstream"
    updir.mkdir(exist_ok=True)
    r = subprocess.run([tools["goose"], "run", *defines, str(f)], capture_output=True, timeout=300,
                       cwd=updir, stdin=subprocess.DEVNULL)
    if b"JIT mode does not support threads" in r.stderr:
        # Upstream runs a program with workers only as an executable, which
        # it builds with llvm-mingw's clang.
        if not tools["clang_dir"]:
            return "no-tool", "llvm-mingw's clang not found (tools/llvm-mingw-*)"
        exe = Path(workdir) / f"{f.stem}-upstream.exe"
        env = dict(os.environ)
        env["PATH"] = str(tools["clang_dir"]) + os.pathsep + env.get("PATH", "")
        b = subprocess.run([tools["goose"], "build", "--cc", "clang", *defines, "-o", str(exe), str(f)],
                           capture_output=True, timeout=300, env=env)
        if b.returncode != 0:
            return "compile-error", f"upstream build: {decode(b.stderr).strip()[-200:]}"
        r = subprocess.run([str(exe)], capture_output=True, timeout=120, cwd=updir, stdin=subprocess.DEVNULL)
    want_out, want_err = decode(r.stdout), decode(r.stderr)
    if (code != 0) != (r.returncode != 0):
        return "wrong-exit", f"exit {code}, upstream {r.returncode}: {err.strip()[:200]}"
    if r.returncode != 0:
        # Upstream's stderr also holds its compile warnings; its runtime
        # error is the last line.
        last = want_err.strip().splitlines()[-1] if want_err.strip() else ""
        if last not in err:
            return "wrong-stderr", f"missing {last!r}"
    if out.rstrip("\n") != want_out.rstrip("\n"):
        return "wrong-output", f"got {out.rstrip()[:80]!r}, upstream {want_out.rstrip()[:80]!r}"
    return "pass", ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-j", type=int, default=os.cpu_count())
    ap.add_argument("-O0", dest="opt", action="store_const", const="-O0", default="-O2")
    ap.add_argument("-O2", dest="opt", action="store_const", const="-O2")
    ap.add_argument("--host", choices=("wasmtime", "node"), default="wasmtime",
                    help="what runs the modules: wasmtime (46 for wasi-threads ones), or Node.js")
    ap.add_argument("--filter", default="")
    ap.add_argument("-v", action="store_true", help="print every fixture, not only a summary")
    args = ap.parse_args()

    exe = ".exe" if os.name == "nt" else ""
    clang = find_optional(*sorted(ROOT.glob(f"tools/llvm-mingw-*/bin/clang{exe}")))
    tools = {
        "goose_wasm": find_tool(ROOT / "build" / "mingw" / f"goose-wasm{exe}",
                                ROOT / "build" / f"goose-wasm{exe}"),
        "wasm_tools": find_tool(*sorted(ROOT.glob(f"tools/wasm-tools-*/wasm-tools{exe}"))),
        "wasmtime": find_tool(os.environ.get("WASMTIME"),
                              r"C:\Program Files\Wasmtime\bin\wasmtime.exe",
                              Path.home() / ".wasmtime" / "bin" / f"wasmtime{exe}"),
        # The last wasmtime with wasi-threads, for modules with workers.
        "wasmtime46": find_optional(os.environ.get("WASMTIME46"),
                                    ROOT / "tools" / "dl46" / "wasmtime-v46.0.1-x86_64-windows" / f"wasmtime{exe}"),
        # Upstream's compiler, the oracle for tests/ (scripts/upstream.ps1 builds it).
        "goose": find_tool(ROOT / "upstream" / "goose" / "build-mingw" / f"goose{exe}"),
        # The C compiler upstream builds executables with (scripts/upstream.ps1).
        "clang_dir": clang and Path(clang).parent,
        "node": find_optional(shutil.which("node")) if args.host == "node" else None,
    }
    if args.host == "node" and not tools["node"]:
        sys.exit("node not found")
    files = list(fixtures(args.filter))
    OUT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=OUT) as workdir, ThreadPoolExecutor(args.j) as pool:
        results = list(pool.map(lambda f: (f, *run_one(f, tools, args.opt, workdir, args.host)), files))

    counts = {}
    lines = []
    for f, status, detail in results:
        counts[status] = counts.get(status, 0) + 1
        line = f"{status:14} {label(f)}" + (f"  {detail}" if detail else "")
        lines.append(line)
        if args.v or status in ("invalid", "compile-error", "timeout"):
            print(line)
    (OUT / "results.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    summary = ", ".join(f"{k} {v}" for k, v in sorted(counts.items(), key=lambda kv: -kv[1]))
    print(f"{len(results)} fixtures ({args.opt}, {args.host}): {summary}")
    # Unsupported is expected while the backend grows; anything else is a failure.
    return 0 if all(s in ("pass", "unsupported") for _, s, _ in results) else 1


if __name__ == "__main__":
    sys.exit(main())
