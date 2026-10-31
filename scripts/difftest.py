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

    python scripts/difftest.py [-j N] [-O0|-O2] [--filter text] [-v]

Results go to build/difftest/results.txt, one line per fixture.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TESTS = ROOT / "upstream" / "goose" / "test"
EXPECTED = TESTS / "expected"
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


def fixtures(filter_text):
    for f in sorted(TESTS.glob("*/*.goose")):
        if f.parent.name in SKIP_DIRS or f.name == "lexer_tokens.goose":
            continue
        if not (EXPECTED / f"{f.stem}.out").exists():
            continue
        if filter_text and filter_text not in str(f.relative_to(TESTS)):
            continue
        yield f


def decode(data):
    return data.decode("utf-8", errors="replace").replace("\r\n", "\n")


def run_one(f, tools, opt, workdir):
    name = f.stem
    wasm = Path(workdir) / f"{name}.wasm"
    r = subprocess.run([tools["goose_wasm"], str(f), "-o", str(wasm), opt],
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
    try:
        r = subprocess.run([tools["wasmtime"], "run", "-W", "multi-memory=y", str(wasm)],
                           capture_output=True, timeout=120, cwd=f.parent)
    except subprocess.TimeoutExpired:
        return "timeout", ""
    out, err = decode(r.stdout), decode(r.stderr)
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-j", type=int, default=os.cpu_count())
    ap.add_argument("-O0", dest="opt", action="store_const", const="-O0", default="-O2")
    ap.add_argument("-O2", dest="opt", action="store_const", const="-O2")
    ap.add_argument("--filter", default="")
    ap.add_argument("-v", action="store_true", help="print every fixture, not only a summary")
    args = ap.parse_args()

    exe = ".exe" if os.name == "nt" else ""
    tools = {
        "goose_wasm": find_tool(ROOT / "build" / "mingw" / f"goose-wasm{exe}",
                                ROOT / "build" / f"goose-wasm{exe}"),
        "wasm_tools": find_tool(*sorted(ROOT.glob(f"tools/wasm-tools-*/wasm-tools{exe}"))),
        "wasmtime": find_tool(os.environ.get("WASMTIME"),
                              r"C:\Program Files\Wasmtime\bin\wasmtime.exe",
                              Path.home() / ".wasmtime" / "bin" / f"wasmtime{exe}"),
    }
    files = list(fixtures(args.filter))
    OUT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=OUT) as workdir, ThreadPoolExecutor(args.j) as pool:
        results = list(pool.map(lambda f: (f, *run_one(f, tools, args.opt, workdir)), files))

    counts = {}
    lines = []
    for f, status, detail in results:
        counts[status] = counts.get(status, 0) + 1
        line = f"{status:14} {f.relative_to(TESTS)}" + (f"  {detail}" if detail else "")
        lines.append(line)
        if args.v or status in ("invalid", "compile-error", "timeout"):
            print(line)
    (OUT / "results.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    summary = ", ".join(f"{k} {v}" for k, v in sorted(counts.items(), key=lambda kv: -kv[1]))
    print(f"{len(results)} fixtures ({args.opt}): {summary}")
    # Unsupported is expected while the backend grows; anything else is a failure.
    return 0 if all(s in ("pass", "unsupported") for _, s, _ in results) else 1


if __name__ == "__main__":
    sys.exit(main())
