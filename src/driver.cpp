// goose-wasm: compiles a Goose program to a WebAssembly module.
//
// The front end is upstream Goose's, used unmodified: its headers are
// included in the order upstream's src/main.cpp includes them, as one
// translation unit, from the lexer through codegen (whose Node virtuals every
// node needs) and the package loader. jit.h (TinyCC) and the language server
// are left out. The program loader that upstream keeps in main.cpp is copied
// in src/upstream_copy/.
#include "includes.h"
#include "utils.h"
#include "small_vector.h"
#include "flat_map.h"
#include "lexer.h"
#include "ast.h"
#include "dump.h"
#include "clone.h"
#include "parser.h"
#include "resolve.h"
#include "builtins.h"
#include "gfx.h"
#include "typecheck.h"
#include "typecheck_types.h"
#include "typecheck_exprs.h"
#include "typecheck_expansion.h"
#include "typecheck_flow.h"
#include "typecheck_calls.h"
#include "typecheck_builtins.h"
#include "typecheck_nodes.h"
#include "optimize.h"
#include "optimize_basecase.h"
#include "optimize_tre.h"
#include "optimize_loops.h"
#include "optimize_reassoc.h"
#include "bce.h"
#include "codegen.h"
#include "codegen_types.h"
#include "codegen_frames.h"
#include "codegen_values.h"
#include "codegen_construct.h"
#include "codegen_stmts.h"
#include "codegen_calls.h"
#include "codegen_builtins.h"
#include "codegen_render.h"
#include "codegen_emit.h"
#include "codegen_stacks.h"
#include "codegen_nodes.h"
#include "runtime_inline.h"
#include "process.h"
#include "progress.h"
#include "buildfile.h"
#include "json.h"
#include "toolchain.h"
#include "builtin.h"
#include "build.h"
#include "packages.h"
#include "native.h"

#include "upstream_copy/main_parse.h"

#include "wasm/backend.h"
#include "lib_overrides.h"

namespace goosewasm {

using namespace goose;

const char *usage =
    "usage: goose-wasm file.goose -o out.wasm [options]\n"
    "\n"
    "Options:\n"
    "  -o out.wasm     Where to write the module (required).\n"
    "  -O0, -O1, -O2   Inlining level (default -O2), as upstream's.\n"
    "  --stdlib dir    The standard library's directory (or GOOSE_STDLIB).\n"
    "  -D NAME[=VALUE] A runtime configuration define, as upstream's (GS_DEBUG,\n"
    "                  GS_STACK_RESERVE, ...); one the backend does not implement\n"
    "                  is reported as unsupported.\n"
    "  --specs         Print the specializations of each function.\n";

// A path as compared here: forward slashes, no trailing one, and on Windows
// in one case.
static string PathForm(string p) {
    for (auto &c : p) {
        if (c == '\\') c = '/';
#ifdef _WIN32
        c = char(tolower((unsigned char)c));
#endif
    }
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

// The text the loader reads for `path` instead of the file there: goose-wasm's
// replacement for a standard library file whose extern fns upstream's C
// runtime implements (lib/), where path is that file in a standard library
// directory.
static SourceProvider LibOverrides(const vector<string> &stdlibdirs) {
    vector<string> dirs;
    for (auto &d : stdlibdirs) dirs.push_back(PathForm(d));
    return [dirs, texts = std::make_shared<map<string, string>>()](const string &path) -> const string * {
        auto p = PathForm(path);
        auto slash = p.rfind('/');
        if (slash == string::npos) return nullptr;
        auto dir = p.substr(0, slash), name = p.substr(slash + 1);
        if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) return nullptr;
        for (auto &o : lib_overrides) {
            if (name != PathForm(o.name)) continue;
            auto &t = (*texts)[name];
            if (t.empty()) t = o.text;
            return &t;
        }
        return nullptr;
    };
}

// Whether a source may declare a `reusable[]` pool: the keyword followed by
// `[]`, which nothing else spells. Text in a comment or string counts too,
// which costs only loading the span tree's module unused.
static bool UsesSlicePools(const string &text) {
    for (size_t at = text.find("reusable"); at != string::npos; at = text.find("reusable", at + 1)) {
        auto i = at + 8;
        if (at > 0 && (isalnum((unsigned char)text[at - 1]) || text[at - 1] == '_')) continue;
        while (i < text.size() && isspace((unsigned char)text[i])) i++;
        if (i >= text.size() || text[i] != '[') continue;
        i++;
        while (i < text.size() && isspace((unsigned char)text[i])) i++;
        if (i < text.size() && text[i] == ']') return true;
    }
    return false;
}

int Main(int argc, char **argv) {
    string filename, outfile, stdlibdir;
    int optlevel = 2;
    bool specs = false;
    vector<string> defines;
    for (int i = 1; i < argc; i++) {
        string_view a = argv[i];
        if (a == "-o" && i + 1 < argc) outfile = argv[++i];
        else if (a == "-D" && i + 1 < argc) defines.push_back(argv[++i]);
        else if (a.size() > 2 && a.substr(0, 2) == "-D") defines.push_back(string(a.substr(2)));
        else if (a == "-O0" || a == "-O1" || a == "-O2") optlevel = a[2] - '0';
        else if (a == "--stdlib" && i + 1 < argc) stdlibdir = argv[++i];
        else if (a == "--specs") specs = true;
        else if (a == "-h" || a == "--help") { fputs(usage, stdout); return 0; }
        else if (!a.empty() && a[0] != '-' && filename.empty()) filename = argv[i];
        else { fprintf(stderr, "unknown argument: %s\n%s", argv[i], usage); return 2; }
    }
    if (filename.empty() || outfile.empty()) {
        fputs(usage, stderr);
        return 2;
    }
    auto exe = ExePath(argv[0]);
#ifdef GOOSE_WASM_DEFAULT_STDLIB
    // This executable does not live in upstream's tree, where StdlibDirs
    // would find stdlib/ by walking up from it.
    if (stdlibdir.empty() && !getenv("GOOSE_STDLIB")) stdlibdir = GOOSE_WASM_DEFAULT_STDLIB;
#endif
    try {
        // The phases upstream's Main runs before codegen, in its order.
        auto stdlibdirs = StdlibDirs(stdlibdir, exe);
        auto overrides = LibOverrides(stdlibdirs);
        auto astp = std::make_unique<Ast>();
        ParseProgram(*astp, filename, stdlibdirs, DirOf(exe), overrides);
        if (!astp->errors.empty()) return 1;
        // A `reusable[]` pool keeps its freelist in the span tree upstream's
        // C runtime implements (gs_spans_*). Its algorithm is the one std's
        // slice_pool_internal calls (lib/slice_pool_internal.goose ports
        // it), so such a program is loaded again with that module imported
        // at the end of its root file, where no line moves.
        bool spans = false;
        for (auto &s : astp->sources) spans |= UsesSlicePools(*s.second);
        if (spans) {
            auto roottext = std::make_shared<string>();
            if (!LoadSource(filename, *roottext)) throw CompileError { cat("cannot open file: ", filename) };
            *roottext += "\nimport slice_pool_internal;\n";
            SourceProvider withspans = [&, roottext](const string &path) -> const string * {
                if (path == filename) return roottext.get();
                return overrides(path);
            };
            astp = std::make_unique<Ast>();
            ParseProgram(*astp, filename, stdlibdirs, DirOf(exe), withspans);
            if (!astp->errors.empty()) return 1;
        }
        auto &ast = *astp;
        ResolveTypeNames(ast);
        if (!ast.errors.empty()) return 1;
        TypeCheckProgram(ast);
        if (!ast.errors.empty()) return 1;
        // The span tree's functions are reached from where the backend
        // emits a pool's operations, which no call in the tree shows: the
        // optimizer is to treat them as the roots an export is, and they
        // are no exports once it has.
        std::vector<SFunction *> spanfns;
        if (spans)
            for (auto sf : ast.functions)
                if (sf->qname == "slice_pool_detail::place" || sf->qname == "slice_pool_detail::release_run" ||
                    sf->qname == "slice_pool_detail::regrow")
                    if (!sf->isexport) spanfns.push_back(sf);
        for (auto sf : spanfns) sf->isexport = true;
        Optimizer opt(ast, optlevel);
        for (auto sf : spanfns) sf->isexport = false;
        if (specs) {
            string s;
            opt.DumpSpecs(s);
            fputs(s.c_str(), stdout);
        }
        BCE bce(ast);
        bce.RunAll();
        if (!bce.checkfreefails.empty()) throw bce.CheckfreeError();

        // GS_DEBUG=1 builds the debug checks; the stack configuration
        // defines are not implemented.
        Backend backend(ast);
        for (auto &d : defines) {
            if (d == "GS_DEBUG=1" || d == "GS_DEBUG") {
                backend.debug = true;
                continue;
            }
            if (d == "GS_DEBUG=0") continue;
            throw CompileError { cat("goose-wasm: unsupported: runtime define ", d, " at ",
                                     filename) };
        }
        auto bytes = backend.Build().Encode();
        auto f = fopen(outfile.c_str(), "wb");
        if (!f) throw CompileError { cat("cannot write ", outfile) };
        fwrite(bytes.b.data(), 1, bytes.size(), f);
        fclose(f);
    } catch (CompileError &e) {
        PrintDiagnosticText(e.msg);
        return 1;
    }
    return 0;
}

}  // namespace goosewasm

// The system headers come after the whole compiler, as in upstream's
// main.cpp, so that none of their macros reach it.
#ifdef _WIN32
#include <windows.h>
#endif

// The compiler's own path, where StdlibDirs looks for the standard library.
// Declared by the upstream copy; this definition is ours.
std::string goose::ExePath(const char *argv0) {
#ifdef _WIN32
    char buf[MAX_PATH * 4];
    auto n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) return std::string(buf, n);
#endif
    return argv0;
}

int main(int argc, char **argv) {
    return goosewasm::Main(argc, argv);
}
