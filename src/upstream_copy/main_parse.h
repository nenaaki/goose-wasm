// Upstream code this project cannot include directly: the program loader
// (ParseProgram) and the path helpers and standard-library search it uses.
// Upstream keeps them in src/main.cpp, which also includes jit.h and with it
// TinyCC (LGPL), so main.cpp itself is not compiled here.
//
// Copied from: upstream/goose/src/main.cpp, lines 61-488, commit 2ed1a63dde2ddff777fbb7f4790412b4f00cd47d.
// Original work: Goose, https://github.com/aardappel/goose, by Wouter van
// Oortmerssen, licensed under the Apache License, Version 2.0
// (upstream/goose/LICENSE).
// Changes: none to the copied lines, which sit between the markers below.
// This file adds this header and the enclosing namespace.
// scripts/upstream_copy.py check verifies the copy against upstream.
#pragma once

namespace goose {

// ---- BEGIN UPSTREAM COPY (verbatim; edit upstream, then run scripts/upstream_copy.py update) ----
// In version.cpp: the compiler's version, as the build found it.
const char *CompilerVersion();

string DirOf(const string &path) {
    auto pos = path.find_last_of("/\\");
    return pos == string::npos ? "" : path.substr(0, pos + 1);
}

string ExePath(const char *argv0);

static bool FileExists(const string &path) {
    auto f = fopen(path.c_str(), "rb");
    if (f) fclose(f);
    return f != nullptr;
}

// A path split on either separator, with "." and cancelled "x/.." dropped. A
// path with a leading separator keeps a leading empty component ("/a" ->
// {"", "a"}), so two rooted paths share it and a rooted and a relative one
// never do; "" and "./" are the relative {}.
static vector<string> PathParts(const string &path) {
    vector<string> parts;
    string cur;
    for (size_t i = 0; i <= path.size(); i++) {
        auto c = i < path.size() ? path[i] : '/';
        if (c != '/' && c != '\\') { cur += c; continue; }
        if (i == 0 && i < path.size()) parts.push_back(cur);
        else if (cur == "." || cur.empty()) {}
        else if (cur == ".." && !parts.empty() && parts.back() != "..") parts.pop_back();
        else parts.push_back(cur);
        cur.clear();
    }
    return parts;
}

// `path` as reached from `dir`. Returns it unchanged where no relative path
// exists or one cannot be computed by name alone: an absolute path against a
// relative directory or the other way round, two absolute paths under
// different Windows drives, or a leftover ".." that only the filesystem could
// resolve.
string RelativeTo(const string &path, const string &dir) {
    auto p = PathParts(path), d = PathParts(dir);
    if (p.empty()) return path;
    auto updir = [](const vector<string> &parts) {
        for (auto &part : parts) if (part == "..") return true;
        return false;
    };
    if (updir(p) || updir(d)) return path;
    // A leading "" (rooted) or "C:" (drive) both sit in the first component.
    auto rooted = [](const vector<string> &parts) {
        return !parts.empty() && (parts[0].empty() || parts[0].back() == ':');
    };
    if (rooted(p) != rooted(d)) return path;
    if (rooted(p) && p[0] != d[0]) return path;
    // The last component of `path` is the file itself, never a shared directory.
    size_t i = 0;
    while (i + 1 < p.size() && i < d.size() && p[i] == d[i]) i++;
    string rel;
    for (size_t up = i; up < d.size(); up++) rel += "../";
    for (; i < p.size(); i++) {
        rel += p[i];
        if (i + 1 < p.size()) rel += '/';
    }
    return rel;
}

// Where the standard library lives (§11.1): an explicit --stdlib or
// GOOSE_STDLIB, else the `stdlib/` directory of the source tree the compiler
// was built in, found by walking up from the executable. A directory more
// than one of those name is in the list once, as the first named it.
vector<string> StdlibDirs(const string &stdlibdir, const string &exe) {
    vector<string> cands;
    if (!stdlibdir.empty()) cands.push_back(cat(stdlibdir, "/"));
    if (auto env = getenv("GOOSE_STDLIB")) cands.push_back(cat(env, "/"));
    auto exedir = DirOf(exe);
    cands.push_back(cat(exedir, "stdlib/"));
    cands.push_back(cat(exedir, "../stdlib/"));
    cands.push_back(cat(exedir, "../../stdlib/"));
    cands.push_back(cat(exedir, "../../../stdlib/"));
    cands.push_back("stdlib/");
    vector<string> dirs;
    set<string> seen;
    for (auto &dir : cands) {
        std::error_code ec;
        auto canon = filesystem::weakly_canonical(filesystem::path(dir), ec);
        // Normalized: its files' paths are what diagnostics and runtime errors print.
        if (seen.insert(ec ? dir : canon.string()).second)
            dirs.push_back(filesystem::path(dir).lexically_normal().generic_string());
    }
    return dirs;
}

// A package the root file declared with `import name from "source";`
// (docs/design/packages.md §4.3).
struct DeclaredPackage {
    PackageSource source;
    int pkg = 0;             // Its index in Ast::packages.
    int line = 0;            // Where the root declared it.
};

// An error at an import in the file named `file`: over the whole `import ...;`.
static Diagnostic ImportDiagnostic(const string &file, const Import &imp, const string &msg) {
    Diagnostic d;
    d.where = LocOf(imp.line);
    if (imp.end.line > 0) {
        d.where.endline = imp.end.line;
        d.where.endcol = imp.end.col;
    }
    d.message = msg;
    d.text = RenderDiagnostic(d, file, {}, false);
    return d;
}

[[noreturn]] static void ImportError(const string &file, const Import &imp, const string &msg) {
    auto d = ImportDiagnostic(file, imp, msg);
    throw CompileError { d.text, { d } };
}

// Parses a root file and, transitively, everything it imports (each file once).
// `import a.b;` resolves relative to the importing file's package root (§11.1):
// the root file's directory for the program's own files, a library's
// directory for its modules; then as a module of a package the root
// declared; then in the standard library. `import .a.b;` resolves relative to
// the importing file's directory. `import name from "source";` declares a
// package in the root file and is a requirement anywhere else, which the
// root's declaration must satisfy (docs/design/packages.md §4.3). A file's
// text comes from `sources` where that has it (SourceProvider, utils.h),
// else from disk. `exedir` is the compiler's directory, where a release
// keeps the packages compiled into it. An import that fails is reported
// at the import (Ast::Report), and the program goes on without what it
// would have loaded: the names it would have declared are unknown where
// they are used.
void ParseProgram(Ast &ast, const string &rootpath, const vector<string> &stdlibdirs,
                  const string &exedir, const SourceProvider &sources = {}) {
    // The program is package 0; a standard library directory becomes a
    // package the first time a module in it is reached.
    auto rootdir = DirOf(rootpath);
    ast.packages.push_back(Package { PathStem(rootpath), rootdir, false });
    LoadSidecar(ast.packages[0], true);
    auto stdlibpkg = [&](const string &dir) {
        for (size_t i = 0; i < ast.packages.size(); i++)
            if (ast.packages[i].stdlib && ast.packages[i].root == dir) return (int)i;
        ast.packages.push_back(Package { "stdlib", dir, true });
        return (int)ast.packages.size() - 1;
    };
    map<string, DeclaredPackage> declared;
    // Loads a blessed package (§4.5) once, under its package name among the
    // declared ones: from where the compiler's build put it, or a release
    // beside the compiler, when it is compiled in, else from its source as a
    // declared package would be (a path in the table is relative to the
    // source tree the standard library is found in).
    auto loadblessed = [&](const BlessedEntry &be) -> int {
        auto it = declared.find(be.package);
        if (it != declared.end()) return it->second.pkg;
        auto builtin = BuiltinLookup(be.package);
        auto builtindir = builtin ? BuiltinSrcDir(*builtin, exedir) : string();
        PackageSource src;
        src.text = be.source;
        string root;
        if (builtin && PathExists(cat(builtindir, be.package, ".goose"))) {
            src.ispath = true;
            src.dir = root = builtindir;
        } else if (IsPathSource(be.source)) {
            auto rel = be.source.rfind("./", 0) == 0 ? be.source.substr(2) : be.source;
            for (auto &dir : stdlibdirs) {
                auto cand = cat(dir, "../", rel);
                if (PathExists(cat(cand, "/", be.package, ".goose"))) {
                    src.ispath = true;
                    src.dir = PackageDir(cand);
                    break;
                }
            }
            if (src.dir.empty())
                throw CompileError { cat("cannot find the blessed package ", be.package,
                                         " at ", be.source, " of the source tree") };
            root = PackageRoot(src, be.package);
        } else {
            src = ParsePackageSource(cat(be.source, "@", be.ref), "");
            root = PackageRoot(src, be.package);
        }
        ast.packages.push_back(Package { be.package, root, false });
        auto &p = ast.packages.back();
        if (!src.ispath) p.cachekey = GitCacheKey(src);
        if (builtin && root == builtindir) p.builtin = BuiltinIndex(builtin);
        LoadSidecar(p, false);
        declared.emplace(be.package, DeclaredPackage { src, (int)ast.packages.size() - 1, 0 });
        return (int)ast.packages.size() - 1;
    };
    vector<pair<string, int>> queue = { { rootpath, 0 } };   // (path, its package).
    set<string> loaded = { rootpath };
    map<string, int> fileof;                 // Resolved path -> index in ast.sources.
    vector<vector<string>> importsof;        // Per source, the paths it imports.
    // An imported file's first import, where an error opening it is.
    map<string, pair<string, Import>> importedat;
    // The packages whose declaration, or blessed load, failed: what imports
    // them again was reported at that import.
    set<string> failedpkgs;
    while (!queue.empty()) {
        auto [path, pkg] = queue.back();
        queue.pop_back();
        auto contents = make_unique<string>();
        if (auto text = sources ? sources(path) : nullptr) {
            *contents = *text;
            StripBom(*contents);
        } else if (!LoadSource(path, *contents)) {
            auto it = importedat.find(path);
            if (it == importedat.end()) throw CompileError { cat("cannot open file: ", path) };
            ast.Report(ImportDiagnostic(it->second.first, it->second.second,
                                        cat("cannot open file: ", path)));
            continue;
        }
        auto fileidx = (int)ast.sources.size();
        ast.sources.emplace_back(path, std::move(contents));
        ast.filepkg.push_back(pkg);
        fileof[path] = fileidx;
        importsof.emplace_back();
        auto &[filename, source] = ast.sources.back();
        Parser parser(ast, filename, source->c_str(), fileidx);
        parser.ParseTop();
        for (auto &imp : parser.imports) {
            // The package this import declares or loads, where it does.
            string pkgname;
            try {
                auto imppkg = pkg;
                string imppath;
                if (!imp.source.empty()) {
                    if (failedpkgs.count(imp.path)) continue;
                    pkgname = imp.path;
                    PackageSource src;
                    try {
                        src = ParsePackageSource(imp.source, DirOf(path));
                    } catch (CompileError &e) {
                        ImportError(filename, imp, e.msg);
                    }
                    auto it = declared.find(imp.path);
                    if (fileidx == 0) {
                        // The root declares the package, and imports its entry.
                        if (it != declared.end())
                            ImportError(filename, imp, cat("the package ", imp.path,
                                                                " is declared twice"));
                        string root;
                        try {
                            root = PackageRoot(src, imp.path);
                        } catch (CompileError &e) {
                            ImportError(filename, imp, e.msg);
                        }
                        ast.packages.push_back(Package { imp.path, root, false });
                        if (!src.ispath) ast.packages.back().cachekey = GitCacheKey(src);
                        LoadSidecar(ast.packages.back(), false);
                        auto dp = DeclaredPackage { src, (int)ast.packages.size() - 1,
                                                    imp.line.line };
                        it = declared.emplace(imp.path, dp).first;
                    } else {
                        // A requirement: satisfied by the root's declaration, or
                        // by the blessed table where the root declared nothing.
                        if (it == declared.end())
                            if (auto be = BlessedLookup(imp.path); be && be->package == imp.path) {
                                loadblessed(*be);
                                it = declared.find(imp.path);
                            }
                    }
                    if (it == declared.end()) {
                        // A requirement the root does not satisfy: the error is
                        // the line to add there.
                        ImportError(filename, imp,
                                    cat(ast.packages[pkg].name, " needs the package ", imp.path,
                                        ": add\n    import ", imp.path, " from \"",
                                        SourceForRoot(src, rootdir), "\";\nto ", rootpath,
                                        " (packages are declared by the root file only)"));
                    } else if (fileidx != 0 && !SameSource(it->second.source, src)) {
                        ImportError(filename, imp,
                                    cat(ast.packages[pkg].name, " needs the package ", imp.path,
                                        " from ", src.text, ", which ", rootpath, ":",
                                        it->second.line, " declared from a different source, ",
                                        it->second.source.text));
                    } else if (fileidx != 0 && it->second.source.ref != src.ref) {
                        ast.Warn(LocOf(imp.line),
                                 cat(ast.packages[pkg].name, " needs the package ", imp.path,
                                     " at ", src.ref, ", which ", rootpath, ":", it->second.line,
                                     " declares at ", it->second.source.ref));
                    }
                    imppkg = it->second.pkg;
                    // What a package requires links after it (§5.5).
                    if (fileidx != 0 && pkg != imppkg) {
                        auto &req = ast.packages[pkg].required;
                        if (std::find(req.begin(), req.end(), imppkg) == req.end())
                            req.push_back(imppkg);
                    }
                    imppath = cat(ast.packages[imppkg].root, imp.path, ".goose");
                } else if (imp.relative) {
                    imppath = cat(DirOf(path), imp.path, ".goose");
                } else {
                    // A file under this package's root; a module of a package the
                    // root declared (`import pkg;` is its entry, `import pkg.sub;`
                    // its sub.goose); a blessed name, its package loaded as if
                    // the root had declared it (§4.5); the standard library.
                    auto local = cat(ast.packages[pkg].root, imp.path, ".goose");
                    auto first = imp.path.substr(0, imp.path.find('/'));
                    auto rest = imp.path.size() > first.size() ? imp.path.substr(first.size() + 1)
                                                               : first;
                    auto it = declared.find(first);
                    // A package's own name, from inside it, is its own file.
                    if (it != declared.end() && it->second.pkg == pkg) it = declared.end();
                    if (it == declared.end() && failedpkgs.count(first)) continue;
                    auto blessed = it == declared.end() && !FileExists(local) ? BlessedLookup(first)
                                                                               : nullptr;
                    if (blessed) pkgname = blessed->package;
                    if (it != declared.end()) {
                        if (FileExists(local))
                            ImportError(filename, imp,
                                        cat(imp.path, " is both a file of this package (", local,
                                            ") and the declared package ", first, "'s module"));
                        imppkg = it->second.pkg;
                        imppath = cat(ast.packages[imppkg].root, rest, ".goose");
                    } else if (blessed) {
                        try {
                            imppkg = loadblessed(*blessed);
                        } catch (CompileError &e) {
                            ImportError(filename, imp, e.msg);
                        }
                        imppath = cat(ast.packages[imppkg].root, rest, ".goose");
                    } else {
                        imppath = local;
                        if (!FileExists(local)) {
                            for (auto &dir : stdlibdirs) {
                                auto cand = cat(dir, imp.path, ".goose");
                                if (FileExists(cand)) {
                                    imppath = cand;
                                    imppkg = stdlibpkg(dir);
                                    break;
                                }
                            }
                            // Not found: where it was looked for, the standard
                            // library being the directories that hold std.goose.
                            if (imppath == local) {
                                auto name = imp.path;
                                for (auto &c : name) if (c == '/') c = '.';
                                auto msg = cat("cannot find the module ", name,
                                               ", looked for\n    ",
                                               ForwardSlashes(AbsolutePath(local)));
                                auto havestd = false;
                                for (auto &dir : stdlibdirs)
                                    if (FileExists(cat(dir, "std.goose"))) {
                                        havestd = true;
                                        auto cand = cat(dir, imp.path, ".goose");
                                        Append(msg, "\n    ", ForwardSlashes(AbsolutePath(cand)));
                                    }
                                if (!havestd) {
                                    Append(msg, "\nand found no standard library, looking in");
                                    for (auto &dir : stdlibdirs)
                                        Append(msg, "\n    ", ForwardSlashes(AbsolutePath(dir)));
                                    Append(msg, "\nset --stdlib or GOOSE_STDLIB to its directory");
                                }
                                ImportError(filename, imp, msg);
                            }
                        }
                    }
                }
                importsof[fileidx].push_back(imppath);
                ast.imports.push_back({ imp.line, imp.end, imppath });
                if (loaded.insert(imppath).second) {
                    queue.push_back({ imppath, imppkg });
                    importedat.emplace(imppath, make_pair(filename, imp));
                }
            } catch (CompileError &e) {
                if (e.diags.empty()) ast.Report(ImportDiagnostic(filename, imp, e.msg));
                for (auto &d : e.diags) ast.Report(d);
                if (!pkgname.empty() && !declared.count(pkgname)) failedpkgs.insert(pkgname);
            }
        }
    }
    // Globals initialize in import order: everything a file imports runs before
    // the file's own, so a global initializer may name an imported global
    // (`let IDENT = Xf { ..., t: float3_0 }`). Files parse in worklist order,
    // which is not that, so rank them by a post-order walk of the import graph
    // and stable-sort the initialization list by rank. A file in an import
    // cycle is visited once, and its rank is then whatever the walk reached
    // first, which is all that order can mean there.
    vector<int> rank(ast.sources.size(), 0);
    vector<char> state(ast.sources.size(), 0);   // 0 unvisited, 1 on the stack, 2 ranked.
    auto next = 0;
    std::function<void(int)> visit = [&](int f) {
        if (state[f]) return;
        state[f] = 1;
        for (auto &p : importsof[f]) {
            auto it = fileof.find(p);
            if (it != fileof.end()) visit(it->second);
        }
        state[f] = 2;
        rank[f] = next++;
    };
    for (size_t i = 0; i < ast.sources.size(); i++) visit((int)i);
    std::stable_sort(ast.globals.begin(), ast.globals.end(),
                     [&](VarDecl *a, VarDecl *b) {
                         return rank[a->line.fileidx] < rank[b->line.fileidx];
                     });
    // A sidecar's dylibs entry names a library some extern block declares
    // (§7.10): the program's, one in any package it uses; a package's, one
    // of its own. A package's blocks are those of all its files, including
    // modules this program does not import, which only a scan of them finds
    // (PackageExternNames); it is made only for a name no parsed block has.
    vector<set<string>> scanned(ast.packages.size());
    vector<bool> isscanned(ast.packages.size());
    auto scannedlibs = [&](size_t p) -> const set<string> & {
        if (!isscanned[p]) {
            PackageExternNames(ast, (int)p, &scanned[p]);
            isscanned[p] = true;
        }
        return scanned[p];
    };
    for (size_t p = 0; p < ast.packages.size(); p++) {
        auto &pkg = ast.packages[p];
        for (auto &d : pkg.dylibs) {
            auto declared = false;
            for (auto sf : ast.functions)
                if (sf->dylib == d.name && (p == 0 || ast.filepkg[sf->line.fileidx] == (int)p))
                    declared = true;
            for (size_t q = 1; q < ast.packages.size() && !declared; q++)
                if ((p == 0 || q == p) && !ast.packages[q].stdlib && ast.packages[q].builtin < 0 &&
                    !ast.packages[q].root.empty() && scannedlibs(q).count(d.name))
                    declared = true;
            if (!declared)
                throw CompileError { cat(pkg.sidecar, ": dylibs maps ", d.name, ", which no "
                                         "extern block ", p == 0 ? "in the program" : "of the package",
                                         " names") };
        }
    }
}
// ---- END UPSTREAM COPY ----

}  // namespace goose
