"""Keeps the code copied verbatim from upstream Goose identical to upstream.

    python scripts/upstream_copy.py check     exit 1 if a copy differs from upstream
    python scripts/upstream_copy.py update    rewrite each copy from upstream

Each copy is a file under src/upstream_copy/ whose upstream text sits between
the BEGIN and END marker lines, untouched. Everything outside the markers is
ours: the provenance header and any wrapper (such as `namespace goose {`).
A block is located in upstream by its first and last lines, not by line
numbers, so it is found again after upstream edits elsewhere in the file.
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UPSTREAM = ROOT / "upstream" / "goose"
BEGIN = "// ---- BEGIN UPSTREAM COPY (verbatim; edit upstream, then run scripts/upstream_copy.py update) ----"
END = "// ---- END UPSTREAM COPY ----"

# dest: (upstream file, first line of the block, last line of the block, the
# line that must follow the block, which tells the right closing brace apart)
COPIES = {
    "src/upstream_copy/main_parse.h": (
        "src/main.cpp",
        "// In version.cpp: the compiler's version, as the build found it.",
        "}",
        "// Parses a program's dump as a program of its own and requires that dumping",
    ),
}


def upstream_commit():
    return subprocess.run(["git", "-C", str(UPSTREAM), "rev-parse", "HEAD"],
                          capture_output=True, text=True, check=True).stdout.strip()


def extract(src, first, last, after):
    lines = (UPSTREAM / src).read_text(encoding="utf-8").splitlines()
    try:
        start = lines.index(first)
    except ValueError:
        sys.exit(f"{src}: block start not found: {first!r}")
    for i in range(start, len(lines) - 2):
        # The block ends at `last`, followed by one blank line and `after`.
        if lines[i] == last and lines[i + 1] == "" and lines[i + 2] == after:
            return start + 1, i + 1, lines[start:i + 1]
    sys.exit(f"{src}: block end not found after line {start + 1}")


def split_copy(path):
    lines = path.read_text(encoding="utf-8").splitlines()
    b, e = lines.index(BEGIN), lines.index(END)
    return lines[:b], lines[b + 1:e], lines[e + 1:]


def check():
    ok = True
    for dest, (src, first, last, after) in COPIES.items():
        _, _, want = extract(src, first, last, after)
        _, have, _ = split_copy(ROOT / dest)
        if have != want:
            ok = False
            print(f"DIFFERS: {dest} from upstream {src}")
        else:
            print(f"ok: {dest} ({len(have)} lines of upstream {src})")
    return 0 if ok else 1


def update():
    commit = upstream_commit()
    for dest, (src, first, last, after) in COPIES.items():
        lo, hi, block = extract(src, first, last, after)
        path = ROOT / dest
        head, _, tail = split_copy(path)
        # The provenance line names the commit and lines the copy was taken from.
        head = [f"// Copied from: upstream/goose/{src}, lines {lo}-{hi}, commit {commit}."
                if l.startswith("// Copied from: ") else l for l in head]
        path.write_text("\n".join(head + [BEGIN] + block + [END] + tail) + "\n", encoding="utf-8")
        print(f"updated: {dest} <- {src}:{lo}-{hi} @ {commit[:9]}")
    return 0


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "check":
        sys.exit(check())
    if cmd == "update":
        sys.exit(update())
    sys.exit(__doc__)
