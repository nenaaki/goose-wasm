"""Checks lib/math.goose against musl's libm, bit for bit.

lib/math.goose ports musl's libm to Goose. This script builds musl's own C
for those functions natively (clang, musl's headers, the public names
prefixed musl_) and runs the same inputs through both: musl's results and
the Wasm module's, at -O0 and -O2, are compared as bits (any NaN as NaN, as
WebAssembly leaves a NaN's sign open). It can also regenerate the data
tables lib/math.goose holds (--tables), from musl's.

    python scripts/libm_check.py [--tables]

Needs clang (CC, or llvm-mingw's under tools/), wasmtime and a built
goose-wasm (as scripts/difftest.py finds them). musl's release tarball is
downloaded into build/ext/musl once and checked against its SHA-256.
"""

import hashlib
import os
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MUSL = "musl-1.2.5"
MUSL_URL = f"https://musl.libc.org/releases/{MUSL}.tar.gz"
MUSL_SHA256 = "a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4"
EXT = ROOT / "build" / "ext" / "musl"
WORK = ROOT / "build" / "libm"

UNARY = ["sin", "cos", "tan", "asin", "acos", "atan", "exp", "log", "log2", "log10"]
BINARY = ["atan2", "pow"]
PUBLIC = UNARY + BINARY + ["scalbn", "floor", "sqrt", "fabs"]
SOURCES = ["sin", "cos", "tan", "__sin", "__cos", "__tan", "__rem_pio2", "__rem_pio2_large",
           "asin", "acos", "atan", "atan2", "exp", "exp_data", "log", "log_data", "log2",
           "log2_data", "log10", "pow", "pow_data", "sinf", "cosf", "tanf", "__sindf", "__cosdf",
           "__tandf", "__rem_pio2f", "asinf", "acosf", "atanf", "atan2f", "expf", "exp2f_data",
           "logf", "logf_data", "log2f", "log2f_data", "log10f", "powf", "powf_data",
           "__math_oflow", "__math_uflow", "__math_xflow", "__math_divzero", "__math_invalid",
           "__math_oflowf", "__math_uflowf", "__math_xflowf", "__math_divzerof",
           "__math_invalidf", "scalbn", "floor", "fabs", "fabsf"]
# The data structs, their sizes in 8-byte fields and the Goose arrays
# lib/math.goose keeps them in.
TABLES = [("__exp_data", 270, "EXP_DATA", "exp_data.c"),
          ("__log_data", 530, "LOG_DATA", "log_data.c"),
          ("__log2_data", 274, "LOG2_DATA", "log2_data.c"),
          ("__pow_log_data", 521, "POW_LOG_DATA", "pow_data.c"),
          ("__exp2f_data", 41, "EXP2F_DATA", "exp2f_data.c"),
          ("__logf_data", 36, "LOGF_DATA", "logf_data.c"),
          ("__log2f_data", 36, "LOG2F_DATA", "log2f_data.c"),
          ("__powf_log2_data", 37, "POWF_LOG2_DATA", "powf_data.c")]


def tool(env, *paths):
    for p in [os.environ.get(env), *paths]:
        if p and Path(p).exists():
            return str(p)
    sys.exit(f"libm_check: no {env} found")


def run(args, **kw):
    r = subprocess.run(args, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.exit(f"libm_check: {args[0]} failed:\n{r.stdout}{r.stderr}")
    return r.stdout


def fetch_musl():
    src = EXT / MUSL
    if (src / "src" / "math").exists():
        return src
    EXT.mkdir(parents=True, exist_ok=True)
    tgz = EXT / f"{MUSL}.tar.gz"
    if not tgz.exists():
        urllib.request.urlretrieve(MUSL_URL, tgz)
    if hashlib.sha256(tgz.read_bytes()).hexdigest() != MUSL_SHA256:
        sys.exit(f"libm_check: {tgz} does not have musl's SHA-256")
    with tarfile.open(tgz) as t:
        t.extractall(EXT, filter="data")
    return src


def build_reference(cc, src):
    """musl's sources, built for the host with musl's headers. x86_64's
    alltypes.h assumes LP64; on an LLP64 host its 64-bit types become long
    long. The public names are prefixed musl_, sqrt is the instruction (as
    musl's x86_64 sqrt is, and Wasm's)."""
    lib = WORK / "libmuslm.a"
    if lib.exists():
        return lib
    inc = WORK / "include" / "bits"
    inc.mkdir(parents=True, exist_ok=True)
    sed_in = (src / "arch/x86_64/bits/alltypes.h.in").read_text() + (src / "include/alltypes.h.in").read_text()
    alltypes = []
    for line in sed_in.splitlines():
        # tools/mkalltypes.sed: TYPEDEF/STRUCT/UNION lines become guarded typedefs.
        parts = line.split()
        if line.startswith("TYPEDEF "):
            name = parts[-1].rstrip(";")
            body = line[len("TYPEDEF "):]
            alltypes += [f"#if defined(__NEED_{name}) && !defined(__DEFINED_{name})",
                         f"typedef {body}", f"#define __DEFINED_{name}", "#endif", ""]
        elif line.startswith("STRUCT ") or line.startswith("UNION "):
            kind = "struct" if line.startswith("STRUCT ") else "union"
            name = parts[1]
            body = line.split(None, 2)[2]
            alltypes += [f"#if defined(__NEED_{kind}_{name}) && !defined(__DEFINED_{kind}_{name})",
                         f"{kind} {name} {body}", f"#define __DEFINED_{kind}_{name}", "#endif", ""]
        else:
            alltypes.append(line)
    text = "\n".join(alltypes) + "\n"
    for t in ("_Addr", "_Int64", "_Reg"):
        text = text.replace(f"#define {t} long\n", f"#define {t} long long\n")
    (inc / "alltypes.h").write_text(text)
    names = PUBLIC + [p + "f" for p in PUBLIC]
    defs = [f"-D{n}=musl_{n}" for n in names]
    objs = []
    for s in SOURCES:
        o = WORK / f"ref_{s}.o"
        run([cc, "--target=x86_64-w64-mingw32" if os.name == "nt" else "-O2", "-O2", "-ffp-contract=off",
             "-ffreestanding", "-nostdinc", "-fno-builtin", "-std=c99", "-D_XOPEN_SOURCE=700", "-w",
             *defs, f"-I{src}/arch/x86_64", f"-I{src}/arch/generic", f"-I{WORK}/include",
             f"-I{src}/src/include", f"-I{src}/src/internal", f"-I{src}/include",
             "-c", str(src / "src/math" / f"{s}.c"), "-o", str(o)])
        objs.append(o)
    hw = WORK / "hwsqrt.c"
    hw.write_text("double musl_sqrt(double x) { return __builtin_sqrt(x); }\n"
                  "float musl_sqrtf(float x) { return __builtin_sqrtf(x); }\n")
    run([cc, "-O2", "-c", str(hw), "-o", str(WORK / "hwsqrt.o")])
    objs.append(WORK / "hwsqrt.o")
    ar = str(Path(cc).with_name("llvm-ar" + (".exe" if os.name == "nt" else "")))
    run([ar, "rcs", str(lib), *map(str, objs)])
    return lib


def specials(width):
    return ["0.0", "-0.0", "1.0", "-1.0", "0.5", "-0.5", "2.0", "1e-300" if width == 64 else "1e-40",
            "1e300" if width == 64 else "1e38", "3.0", "10.0", "100.0", "1000.0", "0.1", "-2.5"]


def harness(width):
    """The same inputs in Goose and in C: an LCG's bit patterns, two thirds
    of them with exponents near 1 or within 2^±40 (2^±30), then special
    values. Each result is printed as hex bits, a NaN as nan."""
    t, u = ("f64", "u64") if width == 64 else ("f32", "u32")
    ct, cu, sfx = ("double", "uint64_t", "") if width == 64 else ("float", "uint32_t", "f")
    mask, bias, wide, shift = ((0x800fffffffffffff, 1023, 40, 52) if width == 64 else
                               (0x807fffff, 127, 30, 23))
    g = ["import math;", "import std;", "",
         f"fn hex(x: {t}) -> u8[>..] {{",
         "    var s: u8[>..] = [];",
         "    if x != x { s.append(\"nan\"); return s; }",
         f"    let b = libm::bits(x);",
         f"    var i = {width // 4};",
         "    while i > 0 {",
         "        i--;",
         f"        let d = ((b >> ((i * 4) as {u})) & 15) as! u8;",
         "        s.push(if d < 10 { '0' + d } else { 'a' + d - 10 });",
         "    }",
         "    return s;",
         "}",
         "fn main() {",
         "    var seed: u64 = 12345;",
         f"    var xs: {t}[>..] = [];",
         "    for i in 3000 {",
         "        seed = seed * 6364136223846793005 + 1442695040888963407;"]
    if width == 64:
        g.append("        var b = seed;")
    else:
        g.append("        var b = (seed >> 32) as! u32;")
    g += [f"        if i % 3 == 0 {{ b = (b & {mask:#x}) | ((({bias} + (((seed >> 20) % {2 * wide}) as! i64) - {wide}) as! {u}) << {shift}); }}",
          f"        if i % 3 == 1 {{ b = (b & {mask:#x}) | ((({bias} + (((seed >> 20) % 8) as! i64) - 4) as! {u}) << {shift}); }}",
          "        xs.push(libm::from_bits(b));",
          "    }"]
    g += [f"    xs.push({s});" for s in specials(width)]
    g.append("    xs.push(1.0 / 0.0); xs.push(-1.0 / 0.0); xs.push(0.0 / 0.0);")
    g += [f"    for x in xs {{ print(\"{f} \", hex({f}(x))); }}" for f in UNARY]
    g += [f"    for x, i in xs {{ print(\"{f} \", hex({f}(x, xs[(i * 7 + 3) % xs.len]))); }}" for f in BINARY]
    g.append("}")

    nl = "\\n"
    c = ["#include <stdio.h>", "#include <stdint.h>", "#include <string.h>"]
    c += [f"{ct} musl_{f}{sfx}({ct});" for f in UNARY]
    c += [f"{ct} musl_{f}{sfx}({ct}, {ct});" for f in BINARY]
    c += [f"static void H({ct} x, const char *f) {{",
          f"  {cu} b; memcpy(&b, &x, sizeof b);",
          f"  if (x != x) printf(\"%s nan{nl}\", f); else printf(\"%s %0{width // 4}llx{nl}\", f, (unsigned long long)b);",
          "}",
          f"static {ct} F({cu} b) {{ {ct} x; memcpy(&x, &b, sizeof x); return x; }}",
          f"static {ct} xs[4000]; static int n;",
          "int main(void) {",
          "  uint64_t seed = 12345;",
          "  for (int i = 0; i < 3000; i++) {",
          "    seed = seed * 6364136223846793005ull + 1442695040888963407ull;",
          f"    {cu} b = {'seed' if width == 64 else '(uint32_t)(seed >> 32)'};",
          f"    if (i % 3 == 0) b = (b & {mask:#x}ull) | (({cu})({bias} + (int64_t)((seed >> 20) % {2 * wide}) - {wide}) << {shift});",
          f"    if (i % 3 == 1) b = (b & {mask:#x}ull) | (({cu})({bias} + (int64_t)((seed >> 20) % 8) - 4) << {shift});",
          "    xs[n++] = F(b);",
          "  }"]
    c += [f"  xs[n++] = ({ct}){s};" for s in specials(width)]
    c.append(f"  {{ volatile {ct} z = 0; xs[n++] = 1 / z; xs[n++] = -1 / z; xs[n++] = z / z; }}")
    c += [f"  for (int i = 0; i < n; i++) H(musl_{f}{sfx}(xs[i]), \"{f}\");" for f in UNARY]
    c += [f"  for (int i = 0; i < n; i++) H(musl_{f}{sfx}(xs[i], xs[(i * 7 + 3) % n]), \"{f}\");" for f in BINARY]
    c += ["  return 0;", "}"]
    return "\n".join(g) + "\n", "\n".join(c) + "\n"


def dump_tables(cc, lib):
    """musl's data structs, every field 8 bytes, as Goose arrays of bits."""
    c = ["#include <stdio.h>", "#include <stdint.h>"]
    c += [f"extern const uint64_t {sym}[];" for sym, _, _, _ in TABLES]
    c.append("int main(void) {")
    for sym, n, _, _ in TABLES:
        c.append(f"  for (int i = 0; i < {n}; i++) printf(\"%016llx\\n\", (unsigned long long){sym}[i]);")
    c += ["  return 0;", "}"]
    src = WORK / "dump.c"
    src.write_text("\n".join(c) + "\n")
    exe = WORK / "dump.exe"
    run([cc, "-O1", str(src), str(lib), "-o", str(exe)])
    words = run([str(exe)]).split()
    out, at = [], 0
    for _, n, name, file in TABLES:
        v = words[at:at + n]
        at += n
        out += ["", f"// {file}: the struct's fields as bits, in order.", f"let libm::{name}: u64[{n}] = ["]
        out += ["    " + ", ".join("0x" + x for x in v[j:j + 4]) + "," for j in range(0, n, 4)]
        out.append("];")
    return "\n".join(out) + "\n"


def main():
    cc = tool("CC", ROOT / "tools/llvm-mingw-20261020-ucrt-x86_64/bin/clang.exe", "/usr/bin/clang")
    exe = ".exe" if os.name == "nt" else ""
    gw = tool("GOOSE_WASM", ROOT / "build/mingw" / f"goose-wasm{exe}", ROOT / "build" / f"goose-wasm{exe}")
    wt = tool("WASMTIME", r"C:\Program Files\Wasmtime\bin\wasmtime.exe", Path.home() / ".wasmtime/bin/wasmtime")
    WORK.mkdir(parents=True, exist_ok=True)
    lib = build_reference(cc, fetch_musl())
    if "--tables" in sys.argv:
        sys.stdout.write(dump_tables(cc, lib))
        return
    failed = False
    for width in (64, 32):
        gsrc, csrc = harness(width)
        (WORK / f"h{width}.goose").write_text(gsrc)
        (WORK / f"h{width}.c").write_text(csrc)
        run([cc, "-O1", str(WORK / f"h{width}.c"), str(lib), "-o", str(WORK / f"h{width}{exe}")])
        want = run([str(WORK / f"h{width}{exe}")]).replace("\r\n", "\n").splitlines()
        for opt in ("-O0", "-O2"):
            wasm = WORK / f"h{width}.wasm"
            run([gw, str(WORK / f"h{width}.goose"), "-o", str(wasm), opt])
            got = run([wt, "run", "-W", "multi-memory=y", "-W", "max-wasm-stack=268435456", str(wasm)]).splitlines()
            bad = [(w, g) for w, g in zip(want, got) if w != g]
            if len(want) != len(got):
                bad.append((f"{len(want)} results", f"{len(got)} results"))
            print(f"f{width} {opt}: {len(want)} results, {len(bad)} differ")
            for w, g in bad[:5]:
                print(f"  musl {w}  goose-wasm {g}")
            failed |= bool(bad)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
