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
#include "wasm/ossyms.h"
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
    "                  GS_STACK_RESERVE, GS_MAX_STACKS, ...); one the backend does\n"
    "                  not implement is reported as unsupported.\n"
    "  --stack-reserve SIZE  -DGS_STACK_RESERVE=SIZE, with a K, M, G or T suffix.\n"
    "  --header out.h  Build the module for a host, as upstream builds its C: no\n"
    "                  _start; the host calls goose_init, then the export fns,\n"
    "                  which out.h declares (docs/design/exports.md).\n"
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

// A runtime configuration value as runtime.h spells its defaults and a -D
// may (upstream ParseConfigValue): an integer with C suffixes, or (N << M).
static bool ConfigValue(string s, uint64_t &out) {
    auto strip = [](string &t) {
        while (!t.empty() && isspace((unsigned char)t.back())) t.pop_back();
        size_t i = 0;
        while (i < t.size() && isspace((unsigned char)t[i])) i++;
        t.erase(0, i);
    };
    strip(s);
    while (s.size() >= 2 && s.front() == '(' && s.back() == ')') {
        s = s.substr(1, s.size() - 2);
        strip(s);
    }
    auto num = [&](string t, uint64_t &v) {
        strip(t);
        while (!t.empty() && (tolower((unsigned char)t.back()) == 'u' || tolower((unsigned char)t.back()) == 'l'))
            t.pop_back();
        if (t.empty() || t.size() > 19 || t.find_first_not_of("0123456789") != string::npos) return false;
        v = strtoull(t.c_str(), nullptr, 10);
        return true;
    };
    if (auto sh = s.find("<<"); sh != string::npos) {
        uint64_t a, b;
        if (!num(s.substr(0, sh), a) || !num(s.substr(sh + 2), b) || b > 63) return false;
        out = a << b;
        return true;
    }
    return num(s, out);
}

// A size with an optional K, M, G or T suffix (upstream ParseSize).
static bool SizeArg(string s, uint64_t &out) {
    if (!s.empty() && toupper((unsigned char)s.back()) == 'B') s.pop_back();
    if (s.empty()) return false;
    uint64_t mult = 1;
    switch (toupper((unsigned char)s.back())) {
        case 'K': mult = 1ull << 10; s.pop_back(); break;
        case 'M': mult = 1ull << 20; s.pop_back(); break;
        case 'G': mult = 1ull << 30; s.pop_back(); break;
        case 'T': mult = 1ull << 40; s.pop_back(); break;
    }
    if (s.empty() || s.size() > 18 || s.find_first_not_of("0123456789") != string::npos) return false;
    auto n = strtoull(s.c_str(), nullptr, 10);
    if (n > UINT64_MAX / mult) return false;
    out = n * mult;
    return true;
}

int Main(int argc, char **argv) {
    string filename, outfile, stdlibdir, headername;
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
        else if (a == "--header" && i + 1 < argc) headername = argv[++i];
        else if (a == "--stack-reserve" && i + 1 < argc) {
            // As upstream's: a whole page at least, 64 KB being the largest
            // a platform uses.
            uint64_t size;
            if (!SizeArg(argv[++i], size) || size < (64ull << 10) || size > (1ull << 48)) {
                fprintf(stderr, "--stack-reserve takes a size from 64 KB to 256 TB (2^48, "
                                "goose_spec.md 10.4), with an optional K, M, G or T suffix: %s\n",
                        argv[i]);
                return 1;
            }
            defines.push_back(cat("GS_STACK_RESERVE=", size));
        }
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
        // An extern fn bound to one of upstream's os runtime symbols calls
        // the function lib/os.goose implements it as (backend.h, OsTarget).
        // So that the checker specializes those functions, such a program is
        // loaded again with os imported and a thread_fn calling them, which
        // nothing spawns, appended on one line.
        string keep;
        std::vector<string> osfns;
        for (auto &o : goosewasm::os_symbols) {
            auto quoted = cat("\"", o.sym, "\"");
            for (auto &s : astp->sources)
                if (s.second->find(quoted) != string::npos) {
                    keep += o.call;
                    osfns.push_back(o.fn);
                    break;
                }
        }
        if (spans || !keep.empty()) {
            auto roottext = std::make_shared<string>();
            if (!LoadSource(filename, *roottext)) throw CompileError { cat("cannot open file: ", filename) };
            if (spans) *roottext += "\nimport slice_pool_internal;\n";
            if (!keep.empty())
                *roottext += cat("\nimport os; thread_fn gs_wasm_os_keep() { var b: u8[>..] = []; ", keep,
                                 " }\n");
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
        // A program built for a host may leave main out, and needs an
        // export fn (upstream's library mode).
        TypeCheckProgram(ast, !headername.empty());
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
        // So are the os functions extern fns stand for.
        for (auto sf : ast.functions)
            if (!sf->isexport && !sf->isextern && ast.StdlibFile(sf->line.fileidx) &&
                std::find(osfns.begin(), osfns.end(), string(sf->qname)) != osfns.end())
                spanfns.push_back(sf);
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

        // The runtime configuration defines (runtime.h): GS_DEBUG builds
        // the debug checks; the data stack ones size the memories and the
        // checks that go by them (StackConfigOf); GS_STACK_STATS reports
        // the stacks as the program ends.
        Backend backend(ast);
        backend.library = !headername.empty();
        for (auto &d : defines) {
            auto eq = d.find('=');
            auto name = d.substr(0, eq);
            uint64_t v = 1;
            if (eq != string::npos && !ConfigValue(d.substr(eq + 1), v))
                throw CompileError { cat("goose-wasm: unsupported: runtime define ", d, " at ", filename) };
            if (name == "GS_DEBUG") backend.debug = v != 0;
            else if (name == "GS_STACK_RESERVE") backend.stackreserve = v;
            else if (name == "GS_STACK_GAP") backend.stackgap = v;
            else if (name == "GS_STACK_BUDGET") backend.stackbudget = v;
            else if (name == "GS_MAX_STACKS") backend.maxstacks = int64_t(v);
            else if (name == "GS_STACK_STATS") backend.stackstats = v != 0;
            else throw CompileError { cat("goose-wasm: unsupported: runtime define ", d, " at ", filename) };
        }
        // runtime.h's own check (goose_spec.md 10.4).
        if (backend.stackreserve > (1ull << 48))
            throw CompileError { "GS_STACK_RESERVE exceeds the 2^48 limit (goose_spec.md 10.4)" };
        auto bytes = backend.Build().Encode();
        auto f = fopen(outfile.c_str(), "wb");
        if (!f) throw CompileError { cat("cannot write ", outfile) };
        fwrite(bytes.b.data(), 1, bytes.size(), f);
        fclose(f);
        if (!headername.empty()) {
            auto h = fopen(headername.c_str(), "wb");
            if (!h) throw CompileError { cat("cannot write ", headername) };
            fwrite(backend.header.data(), 1, backend.header.size(), h);
            fclose(h);
        }
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
