# Builds the upstream Goose compiler (upstream/goose) with llvm-mingw, without
# MSVC, and optionally runs its test suite. Upstream files are not modified.
#
#   scripts/upstream.ps1          build only
#   scripts/upstream.ps1 -Test    build, then run test/run_tests.py
#
# The build goes to upstream/goose/build-mingw (ignored by upstream's
# .gitignore): the compiler finds stdlib/ by walking up from its executable.
# -static links libc++/libunwind in, as upstream's MSVC build links its runtime
# statically; tests that clear PATH cannot start a goose.exe that needs DLLs.
param([switch]$Test)
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$mingw = Get-ChildItem "$root\tools" -Directory -Filter 'llvm-mingw-*' | Select-Object -Last 1
if (-not $mingw) { throw "llvm-mingw not found under $root\tools" }
$env:Path = "$($mingw.FullName)\bin;$env:Path"

Push-Location "$root\upstream\goose"
try {
    cmake -S . -B build-mingw -G Ninja -DCMAKE_BUILD_TYPE=Debug `
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ `
        -DCMAKE_EXE_LINKER_FLAGS=-static
    if ($LASTEXITCODE) { throw "cmake configure failed" }
    cmake --build build-mingw
    if ($LASTEXITCODE) { throw "cmake build failed" }
    if ($Test) {
        python test/run_tests.py --exe build-mingw/goose.exe --profile baseline --cc clang
        if ($LASTEXITCODE) { throw "upstream tests failed" }
    }
} finally {
    Pop-Location
}
