// The Wasm backend: turns a typechecked, optimized Goose program (the
// upstream Ast after BCE) into a module.
//
// It translates a growing subset of the language. Anything outside it stops
// the compile with "goose-wasm: unsupported: <what> at <file>:<line>", which
// scripts/difftest.py counts apart from failures.
//
// The subset so far: main's body over fixed scalar locals (integers of every
// width and bool) in Wasm locals; integer and bool expressions with
// upstream's release-build semantics (spec §6.2, upstream runtime.h); if,
// while, loop, block, range and count for, break, continue, and the bodies
// the optimizer inlined (InlineBlock), whose returns leave them; print of
// strings, integers and bools.
//
// Memory layout follows docs/design/memory-model.md: memory 0 holds the
// runtime's fixed area and static data, and is exported as `memory`, the one
// memory WASI preview 1 reads and writes.
#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

#include "floatfmt.h"
#include "handles.h"
#include "layout.h"
#include "runtime.h"

namespace goosewasm {

namespace g = goose;

inline const char *KindName(g::NodeKind k) {
    static const char *const names[] = {
        "IntLit", "FltLit", "BoolLit", "StrLit", "Ident", "ArrayLit", "StructLit", "Unary",
        "Binary", "Dot", "Call", "Index", "SliceExpr", "AsCast", "NullLit", "SelfRef",
        "RangeExpr", "Block", "IfExpr", "MatchExpr", "EarlyBlock", "While", "LoopExpr",
        "ForLoop", "ConstFor", "Return", "Break", "Continue", "Defer", "FunVal", "VarDecl",
        "Assign", "IncDec", "InlineBlock", "FnDecl", "StructDecl", "EnumDecl", "AliasDecl",
    };
    static_assert(sizeof(names) / sizeof(names[0]) == g::NK_AliasDecl + 1);
    return names[k];
}

// How a Goose value of a fixed scalar type is held in Wasm: nothing (void),
// an i32 (bool, and integers up to 32 bits, kept sign- or zero-extended to
// 32 bits after every operation), or an i64.
enum class Repr { None, I32, I64, F32, F64 };

// A place in memory: a base, either the current frame's (the frame pointer
// local) or a handle held in a local, plus a constant offset, holding a
// value of type t. A handle whose memory is not known to be memory 0 is
// accessed through the dispatching helpers (handles.h).
struct Loc {
    bool frame = true;     // base: the frame pointer, else the handle in `h`
    uint32_t h = 0;
    int64_t off = 0;
    g::TypeExpr *t = nullptr;
    bool mem0 = false;     // a handle known to be in memory 0
    bool abs = false;      // a fixed address in memory 0, `off` (a global)
    Loc At(int64_t more, g::TypeExpr *ft) const { return Loc { frame, h, off + more, ft, mem0, abs }; }
    bool Direct() const { return frame || mem0 || abs; }
    static Loc Abs(int64_t addr, g::TypeExpr *t) { return Loc { false, 0, addr, t, true, true }; }
};

// A resizable array variable (spec §5): its header gs_rhdr { base handle,
// len i64 } in memory, and the stack its elements are on, as an absolute
// stack index in an i32 local.
struct RzVar {
    Loc hdr;
    uint32_t stk = 0;
};

// A sequence's elements as indexing, `for` and slicing see them: their
// type, the handle of element 0 (in a local), whether that is known to be in
// memory 0, and how to push the current length (an i64).
struct Seq {
    g::TypeExpr *et = nullptr;
    uint32_t data = 0;
    bool mem0 = false;
    std::function<void()> len;
    // A slice read from its slot: reads data and len again, as a loop does
    // each iteration (a body may store another slice there).
    std::function<void()> reload;
};

struct Backend {
    g::Ast &ast;
    Module m;
    Runtime rt;
    Layouts lay;
    Handles hs;
    FloatFmt ff;
    uint32_t f2iwrap = 0;  // (d f64) -> i64
    uint32_t ssp = 0;      // global: the shadow stack's top
    uint32_t sslimit = 0;  // global: its lowest address
    uint32_t depth = 0;    // global: activations of recursive-cycle functions
    // Deeper recursion than upstream's 64 MB native stack holds with small
    // frames: at a million activations upstream reports the overflow too.
    // The engine's own stack must hold this many frames (README: hosts).
    static constexpr int32_t MaxDepth = 1000000;
    static constexpr int MaxMemories = 100;

    // Data stack use per specialization, for the module's stack count
    // (spec §10.3): the most indices its own body holds at once, and each
    // call with the indices in use there, which the callee's start above.
    struct StackUse {
        int own = 0;
        std::vector<std::pair<g::FnSpec *, int>> calls;
    };
    std::map<g::FnSpec *, StackUse> stackuse;  // nullptr: the global initializers

    // Globals (impl §6.3): fixed-size ones at fixed addresses in memory 0
    // (zero bytes appended to static data); each resizable one on a data
    // stack of its own, numbered before the indexed stacks, which therefore
    // start at gstacks (main's and init_globals' first stack index).
    std::map<g::VarDef *, Loc> gvars;
    std::map<g::VarDef *, int> gstk;             // a resizable global's dedicated stack
    std::map<g::VarDef *, uint32_t> gstklocal;   // per function: a local holding it
    std::set<g::VarDef *> gneeded;               // the globals init_globals initializes
    int gstacks = 0;
    std::map<std::string, uint32_t> strings;  // literal bytes -> address in memory 0
    std::string staticdata;                   // memory 0 from mem0::StaticData

    // Every specialization reached so far, by its Wasm function index, and
    // those whose bodies are still to emit.
    std::map<g::FnSpec *, uint32_t> funcs;
    std::vector<g::FnSpec *> pending;

    // The function being emitted, by index: FuncOf adds functions while a
    // body is emitted, which can move every Func in the module.
    uint32_t fn = 0;
    uint32_t nparams = 0;
    g::FnSpec *spec = nullptr;
    std::map<g::VarDef *, uint32_t> locals;      // scalars in Wasm locals
    std::map<g::VarDef *, Loc> memvars;          // variables in memory
    std::map<g::VarDef *, RzVar> rzvars;         // resizable arrays and references to them
    std::set<g::VarDef *> addrtaken;             // scalar variables a reference is taken to
    uint32_t fp = 0;                             // local: the frame's base
    int64_t frame = 0;                           // the frame's size so far
    int64_t retdst = -1;                         // param: where a fixed aggregate result goes
    int64_t atparam = -1;                        // param: the caller's location (Located)
    std::map<std::pair<int, int>, uint32_t> locrecs;  // static { file, len, line } records
    int64_t retrz = -1;                          // param: the stack a resizable result goes on
    static constexpr uint32_t SP = 0;            // param: the first stack index (upstream gs_sp)

    // Data stack indices, relative to SP, as upstream's codegen allocates
    // them (CodeGen::AllocStk, PushSc, PopSc): the next free one and the
    // most ever in use. A scope frees what it allocated and, on every exit,
    // restores the tops of the stacks its locals claimed (impl §6.2).
    int stknext = 0, stkmax = 0;
    struct Scope {
        int stkbase;
        std::vector<std::pair<uint32_t, uint32_t>> saves;  // (stack local, base local)
    };
    std::vector<Scope> scopes;

    // The structured instructions open around the code being emitted,
    // innermost last: what a br's depth counts. LK_FUNC is the block around
    // a whole function body, which a return leaves so that the frame is
    // released on the way out.
    enum LabelKind { LK_PLAIN, LK_BREAK, LK_CONTINUE, LK_INLINE, LK_FUNC };
    struct Label {
        LabelKind kind;
        g::SFunction *sf = nullptr;  // LK_INLINE: whose body
        g::TypeExpr *type = nullptr; // LK_BREAK, LK_INLINE: the value a branch out carries
        const Loc *dst = nullptr;    // where that value is built, for an aggregate
        size_t scopes = 0;           // scopes open when it was: a branch to it leaves the rest
        int64_t rzstk = -1;          // for a resizable value: the local of its stack
        g::TypeExpr *rzelem = nullptr;
    };
    std::vector<Label> labels;

    explicit Backend(g::Ast &a) : ast(a) {}

    [[noreturn]] void Unsupported(const g::Node *n, const std::string &what) {
        auto &file = ast.sources[n->line.fileidx].first;
        throw g::CompileError {
            g::cat("goose-wasm: unsupported: ", what, " at ", file, ":", n->line.line) };
    }

    // A string's bytes in static data, each distinct one once.
    uint32_t StaticString(const std::string &s) {
        auto it = strings.find(s);
        if (it != strings.end()) return it->second;
        auto addr = mem0::StaticData + uint32_t(staticdata.size());
        staticdata += s;
        strings[s] = addr;
        return addr;
    }

    Module Build() {
        g::FnSpec *mainspec = nullptr;
        for (auto sp : ast.fnspecs)
            if (sp->live && !sp->isfunval && !sp->lexparent && sp->sf->qname == "main")
                mainspec = sp;
        if (!mainspec) throw g::CompileError { "goose-wasm: no live main" };

        rt.Import(m);
        rt.Define(m);
        static const std::string lead = "goose runtime error: ";
        rt.DefineFail(m, StaticString(lead), uint32_t(lead.size()), StaticString(" ("));
        rt.DefinePanic(m, StaticString(lead), uint32_t(lead.size()));
        {
            std::string a = "index ", b = " out of bounds (length ", c = ")";
            rt.DefineIdxFail(m, StaticString(a), uint32_t(a.size()), StaticString(b),
                             uint32_t(b.size()), StaticString(c), uint32_t(c.size()));
        }
        hs.overflow = "data stack overflow: a value on main's data stacks grew past the 4 GB "
                      "a WebAssembly memory can hold";
        hs.overflowat = StaticString(hs.overflow);
        hs.Reserve(m);
        DefinePrintHelpers();
        ff.Define(m, StaticString("nan"), StaticString("inf"), StaticString("-inf"));
        DefineF2IWrap();
        // Their initial values are set below, once static data's size is known.
        ssp = m.AddGlobal(ValType::I32, true, 0);
        sslimit = m.AddGlobal(ValType::I32, false, 0);
        depth = m.AddGlobal(ValType::I32, true, 0);
        FindNeededGlobals();
        FindFreeVars();
        auto initfn = EmitGlobalInit();
        auto mainfn = FuncOf(mainspec, mainspec->body);
        // Functions, then the render functions printing reaches, which reach
        // others in turn.
        while (!pending.empty() || !pendingrenders.empty()) {
            if (!pending.empty()) {
                auto sp = pending.back();
                pending.pop_back();
                Function(sp, funcs[sp]);
                continue;
            }
            auto [t, index] = pendingrenders.back();
            pendingrenders.pop_back();
            EmitRenderFn(t, index);
        }
        // A WASI command's entry point: the globals' initializers, then
        // main, both with the indexed stacks after the globals' own.
        auto start = m.AddFunc({});
        m.FuncAt(start).code.I32Const(gstacks);
        m.FuncAt(start).code.Call(initfn);
        m.FuncAt(start).code.I32Const(gstacks);
        m.FuncAt(start).code.Call(mainfn);

        // One memory per data stack the program can reach (spec §10.3):
        // the globals' dedicated ones, then the indexed block.
        auto nstacks = gstacks + std::max(StackCount(mainspec), StackCount(nullptr));
        if (nstacks > int(mem0::MaxStacks)) throw g::CompileError { "goose-wasm: too many data stacks" };
        // Engines bound the memories of a module (wasmparser and V8: 100), a
        // limit the Wasm spec does not set; a program needing more data
        // stacks than that cannot be one memory per stack.
        if (1 + nstacks > MaxMemories)
            throw g::CompileError { g::cat("goose-wasm: unsupported: ", nstacks,
                                           " data stacks, more than the ", MaxMemories - 1,
                                           " memories engines accept beside memory 0 at ",
                                           ast.sources[0].first, ":1") };

        // Memory 0: the runtime's area, static data, then the shadow stack,
        // 8-aligned, growing down from the end.
        auto sslo = (uint64_t(mem0::StaticData) + staticdata.size() + 7) / 8 * 8;
        auto sshi = sslo + mem0::ShadowStackSize;
        m.globals[ssp].init = int64_t(sshi);
        m.globals[sslimit].init = int64_t(sslo);
        auto end = sshi;
        m.memories.push_back(Memory { (end + 0xffff) / 0x10000 });
        // The data stacks start empty and grow on their first push.
        for (int k = 0; k < nstacks; k++) m.memories.push_back(Memory { 0 });
        hs.Define(m, rt, uint32_t(1 + nstacks));
        if (!staticdata.empty()) m.data.push_back(DataSegment { 0, mem0::StaticData, staticdata });
        m.exports.push_back(Export { "memory", ExternKind::Memory, 0 });
        m.exports.push_back(Export { "_start", ExternKind::Func, start });
        return std::move(m);
    }

    // ---- Functions, locals and labels ----

    static ValType ValOf(Repr r) {
        switch (r) {
            case Repr::I64: return ValType::I64;
            case Repr::F32: return ValType::F32;
            case Repr::F64: return ValType::F64;
            default: return ValType::I32;
        }
    }

    // Whether a type is a fixed aggregate this backend holds in memory: a
    // struct, a fixed-size array, or a slice ({ data handle, len }, 16
    // bytes, which is how upstream lays it out in a struct too).
    bool IsAggregate(g::TypeExpr *t) {
        if (!t) return false;
        if (t->kind == g::TY_SLICE) return true;
        return (t->kind == g::TY_STRUCT || Layouts::IsFixedArray(t) || Layouts::IsLimited(t) ||
                t->kind == g::TY_ENUM ||
                t->kind == g::TY_VARIANT) &&
               Layouts::Known(t);
    }
    static bool IsSlice(g::TypeExpr *t) { return t && t->kind == g::TY_SLICE; }
    // A plain reference (T& or T?), or a relative one (§3.9), which reads
    // as the plain reference it holds and stores one: as values, both are
    // an i64 handle.
    static bool IsPlainRef(g::TypeExpr *t) { return Layouts::IsPlainRef(t) || Layouts::IsRelRef(t); }
    static bool IsRelRef(g::TypeExpr *t) { return Layouts::IsRelRef(t); }
    // ---- Globals ----

    // A global's storage in memory 0: its value, a resizable array's header
    // or a frame object, zero until init_globals runs.
    Loc GlobalSlot(g::VarDef *v) {
        auto it = gvars.find(v);
        if (it != gvars.end()) return it->second;
        auto t = v->type;
        int64_t size;
        if (IsRz(t)) size = 16;
        else if (IsRoot(t)) size = RootDescSize(t);
        else if (IsFrameObj(t)) size = FrameObjSize(t);
        else if (IsAggregate(t) || IsPlainRef(t) || IsInt(t) || t->kind == g::TY_BOOL) size = lay.FixedSize(t);
        else return gvars[v] = Loc {};  // reported where it is used
        while (staticdata.size() % 8) staticdata.push_back('\0');
        auto addr = mem0::StaticData + int64_t(staticdata.size());
        staticdata.append(size_t(size), '\0');
        return gvars[v] = Loc::Abs(addr, t);
    }

    int64_t FrameObjSize(g::TypeExpr *t) {
        auto si = Layouts::SI(t);
        return lay.StructLayout(si).offs[si->ftypes.size() - 1] + 16;
    }

    // A local holding a resizable global's dedicated stack index.
    uint32_t GlobalStackLocal(g::VarDef *v) {
        auto it = gstklocal.find(v);
        if (it != gstklocal.end()) return it->second;
        auto l = NewLocal(Repr::I32);
        // Set at the function's start, so every path sees it.
        Code set;
        set.I32Const(gstk.at(v));
        set.LocalSet(l);
        auto &body = C().b.b;
        body.insert(body.begin(), set.b.b.begin(), set.b.b.end());
        return gstklocal[v] = l;
    }

    // A node's children, and a function value call's spliced body (§7.6),
    // which Call::Children leaves out.
    static void Kids(g::Node *n, const std::function<void(g::Node *)> &f) {
        n->Children(f);
        if (n->nkind == g::NK_Call) {
            auto c = static_cast<g::Call *>(n);
            if (c->fvbody) f(c->fvbody);
        }
    }

    // The variables a body declares: locals, loop and match bindings, and
    // the parameters of the function values spliced into it.
    static std::set<g::VarDef *> Declared(g::Node *body) {
        std::set<g::VarDef *> out;
        std::function<void(g::Node *)> walk = [&](g::Node *n) {
            if (!n) return;
            switch (n->nkind) {
                case g::NK_VarDecl:
                    for (auto v : static_cast<g::VarDecl *>(n)->defs) {
                        out.insert(v);
                        for (auto c : v->components) out.insert(c);
                    }
                    break;
                case g::NK_ForLoop: {
                    auto f = static_cast<g::ForLoop *>(n);
                    out.insert(f->vdef);
                    out.insert(f->idxdef);
                    break;
                }
                case g::NK_MatchExpr:
                    for (auto &a : static_cast<g::MatchExpr *>(n)->arms) out.insert(a.binder);
                    break;
                case g::NK_Call:
                    for (auto v : static_cast<g::Call *>(n)->fvparams) out.insert(v);
                    break;
                default: break;
            }
            Kids(n, walk);
        };
        walk(body);
        out.erase(nullptr);
        return out;
    }

    // Whether evaluating n can do anything but compute a value: a call (or
    // the body of one the optimizer inlined) or a write.
    static bool Effectful(g::Node *n) {
        bool found = false;
        std::function<void(g::Node *)> walk = [&](g::Node *x) {
            if (!x || found) return;
            if (x->nkind == g::NK_Call || x->nkind == g::NK_InlineBlock || x->nkind == g::NK_Assign ||
                x->nkind == g::NK_IncDec) {
                found = true;
                return;
            }
            Kids(x, walk);
        };
        walk(n);
        return found;
    }

    // The globals to initialize: those live code names, those whose
    // initializer can have an effect, and the globals their initializers
    // name in turn. A global that is none of these can change no output,
    // so leaving it out is unobservable (upstream initializes every one).
    void FindNeededGlobals() {
        // An index type's checks read its array's length (§3.11), so the
        // array counts as named wherever a value of the type is made.
        auto ixarr = [&](g::TypeExpr *t) {
            if (t && t->kind == g::TY_INT && t->ix && t->ix->arr && t->ix->arr->isglobal)
                gneeded.insert(t->ix->arr);
        };
        std::function<void(g::Node *)> refs = [&](g::Node *x) {
            if (!x) return;
            if (x->nkind == g::NK_Ident) {
                auto v = static_cast<g::Ident *>(x)->vdef;
                if (v && v->isglobal) gneeded.insert(v);
            }
            ixarr(x->exprtype);
            if (x->nkind == g::NK_AsCast) ixarr(static_cast<g::AsCast *>(x)->totype);
            Kids(x, refs);
        };
        for (auto sp : ast.fnspecs)
            if (sp->live && sp->body) refs(sp->body);
        for (auto d : ast.globals)
            for (auto i : d->inits)
                if (Effectful(i))
                    for (auto v : d->defs) gneeded.insert(v);
        for (size_t before = 0; before != gneeded.size();) {
            before = gneeded.size();
            for (auto d : ast.globals) {
                bool need = false;
                for (auto v : d->defs) need |= gneeded.count(v) > 0;
                if (need)
                    for (auto i : d->inits) refs(i);
            }
        }
    }

    // init_globals(sp): the needed globals' initializers in declaration
    // order, as upstream's gs_init_globals runs them before main.
    uint32_t EmitGlobalInit() {
        auto index = m.AddFunc({ { ValType::I32 }, {} });
        fn = index;
        spec = nullptr;
        locals.clear();
        memvars.clear();
        rzvars.clear();
        gstklocal.clear();
        labels.clear();
        scopes.clear();
        addrtaken.clear();
        frame = 0;
        stknext = stkmax = 0;
        retdst = -1;
        retrz = -1;
        nparams = 1;
        stackuse[nullptr] = StackUse {};
        fp = NewLocal(Repr::I32);
        C().Block();
        labels.push_back(Label { LK_FUNC });
        PushScope();
        for (auto d : ast.globals) {
            bool need = false;
            for (auto v : d->defs) need |= gneeded.count(v) > 0;
            if (need) GlobalDecl(d);
        }
        PopScope();
        Close();
        stackuse[nullptr].own = stkmax;
        FinishFrame(false);
        return index;
    }

    void GlobalDecl(g::VarDecl *d) {
        if (d->byref || d->reusable || d->results || (d->defs.size() > 1 && d->inits.size() == 1))
            Unsupported(d, "this kind of global");
        for (size_t i = 0; i < d->defs.size(); i++) {
            auto v = d->defs[i];
            auto init = i < d->inits.size() ? d->inits[i] : nullptr;
            auto slot = GlobalSlot(v);
            if (!slot.abs) Unsupported(d, g::cat("a global of type ", TypeKindName(v->type)));
            if (IsRoot(v->type)) {
                GlobalRoot(d, v, slot, init);
                continue;
            }
            GlobalVar(d, v, slot, init);
        }
    }

    // A global root owner: each field a global of its own, from the
    // literal's initializer for it or, without one, the field's declared
    // default (§3.4); then the descriptor, in static data.
    void GlobalRoot(g::VarDecl *d, g::VarDef *v, const Loc &desc, g::Node *init) {
        auto si = Layouts::SI(v->type);
        auto sl = init && init->nkind == g::NK_StructLit ? static_cast<g::StructLit *>(init) : nullptr;
        if (init && !sl) Unsupported(d, "a root owner not built by a literal");
        if (v->components.size() != si->ftypes.size()) Unsupported(d, "this root owner's fields");
        for (size_t f = 0; f < si->ftypes.size(); f++) {
            auto c = v->components[f];
            if (!c) continue;
            auto ci = sl ? sl->InitFor(int(f)) : si->st->fields[f].defaultval;
            auto slot = GlobalSlot(c);
            if (!slot.abs) Unsupported(d, g::cat("a root field of type ", TypeKindName(c->type)));
            GlobalVar(d, c, slot, ci);
            auto at = desc.At(RootDescOff(v->type, int(f)), I64T());
            Store(d, at, [&] { HandleOf(slot); });
            if (IsRzField(c->type))
                Store(d, desc.At(at.off - desc.off + 8, I64T()), [&] { C().I64Const(gstk.at(c)); });
        }
    }

    void GlobalVar(g::VarDecl *d, g::VarDef *v, const Loc &slot, g::Node *init) {
        if (IsRz(v->type) || IsFrameObj(v->type)) {
            // Its own stack, empty: its base is address 0.
            gstk[v] = gstacks++;
            auto stk = GlobalStackLocal(v);
            auto base = NewLocal(Repr::I32);
            C().I32Const(0);
            C().LocalSet(base);
            if (IsRz(v->type)) InitRz(d, RzVar { slot, stk }, base, v->type->arr->sub, init);
            else InitFrameObj(d, slot, stk, base, v->type, init);
            return;
        }
        if (!init) return;  // zero, its type's default
        if (IsAggregate(v->type)) Construct(init, slot);
        else Store(d, slot, [&] { Expr(init, v->type); });
    }

    // ---- Nested functions and their free variables (spec §7.5) ----

    // Each specialization's free variables, in first-appearance order: the
    // variables its body names that another specialization owns (and that
    // are not globals), and those of the specializations it calls that it
    // does not own itself, through what a call passes for them where the
    // optimizer replaced them (Call::fvremap). As upstream's codegen
    // (sinfo.freevars), they are passed by reference, so a nested function
    // reads and writes the enclosing function's variables themselves.
    std::map<g::FnSpec *, std::vector<g::VarDef *>> freevars;
    std::set<g::VarDef *> captured;  // every free variable of any specialization

    void FindFreeVars() {
        std::map<g::FnSpec *, std::vector<std::pair<g::FnSpec *, g::Call *>>> calls;
        std::vector<g::FnSpec *> specs;
        for (auto sp : ast.fnspecs)
            if (sp->live && sp->body && !sp->isfunval) specs.push_back(sp);
        for (auto sp : specs) {
            auto &fv = freevars[sp];
            std::set<g::VarDef *> seen;
            // What the body declares itself, a spliced function value's
            // parameters and locals included, is no free variable of it.
            auto own = Declared(sp->body);
            std::function<void(g::Node *)> walk = [&](g::Node *n) {
                if (!n) return;
                if (n->nkind == g::NK_Ident) {
                    auto v = static_cast<g::Ident *>(n)->vdef;
                    if (v && !v->isglobal && v->ownerspec && v->ownerspec != sp && !own.count(v) &&
                        seen.insert(v).second)
                        fv.push_back(v);
                }
                if (n->nkind == g::NK_Call) {
                    auto c = static_cast<g::Call *>(n);
                    if (c->spec && c->spec != sp) calls[sp].push_back({ c->spec, c });
                    for (auto k : c->dispatch)
                        if (k != sp) calls[sp].push_back({ k, c });
                }
                Kids(n, walk);
            };
            walk(sp->body);
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (auto sp : specs)
                for (auto [k, c] : calls[sp]) {
                    auto it = freevars.find(k);
                    if (it == freevars.end()) continue;
                    for (auto v : std::vector<g::VarDef *>(it->second)) {
                        auto a = c->FreeVarArg(v);
                        if (!a || a->ownerspec == sp || a->isglobal) continue;
                        auto &mine = freevars[sp];
                        if (std::find(mine.begin(), mine.end(), a) == mine.end()) {
                            mine.push_back(a);
                            changed = true;
                        }
                    }
                }
        }
        for (auto &[sp, vs] : freevars) captured.insert(vs.begin(), vs.end());
    }

    // The variables of a body that a reference is taken to, as upstream's
    // CodeGen::RefdLocals finds them: the operand of a `&` (written, or
    // inserted by the checker where a by-reference parameter takes an
    // lvalue, §4.1), the initializer of a `.=` declaration and the right
    // side of a `.=` rebinding. A Wasm local has no address, so these live in
    // the frame instead.
    void FindAddrTaken(g::Node *body) {
        addrtaken.clear();
        // Aggregates are in memory already, and `&` of a variable that holds
        // a reference gives the reference it holds (§3.8), so only integer
        // and bool variables move.
        std::function<void(g::Node *)> root = [&](g::Node *n) {
            if (!n || n->nkind != g::NK_Ident) return;
            auto v = static_cast<g::Ident *>(n)->vdef;
            if (v && (IsInt(v->type) || IsFlt(v->type) || (v->type && v->type->kind == g::TY_BOOL)))
                addrtaken.insert(v);
        };
        // A variable a nested function uses is passed to it by reference,
        // so it has an address: scalars and plain references move too.
        for (auto v : captured)
            if (v->type && (IsInt(v->type) || IsFlt(v->type) || v->type->kind == g::TY_BOOL ||
                            IsPlainRef(v->type)))
                addrtaken.insert(v);
        std::function<void(g::Node *)> walk = [&](g::Node *n) {
            if (!n) return;
            if (n->nkind == g::NK_Unary && static_cast<g::Unary *>(n)->op == g::T_BITAND)
                root(static_cast<g::Unary *>(n)->child);
            if (n->nkind == g::NK_VarDecl && static_cast<g::VarDecl *>(n)->byref)
                for (auto i : static_cast<g::VarDecl *>(n)->inits) root(i);
            // A root owner's fields have places its descriptor names.
            if (n->nkind == g::NK_VarDecl)
                for (auto v : static_cast<g::VarDecl *>(n)->defs)
                    for (auto c : v->components)
                        if (c && c->type && !IsAggregate(c->type) && !IsRzField(c->type))
                            addrtaken.insert(c);
            if (n->nkind == g::NK_Assign && static_cast<g::Assign *>(n)->op == g::T_DOTASSIGN)
                root(static_cast<g::Assign *>(n)->rhs);
            Kids(n, walk);
        };
        walk(body);
    }

    // A resizable array (spec §5) of elements this backend can lay out.
    static bool IsRz(g::TypeExpr *t) {
        return t && t->kind == g::TY_ARRAY &&
               (t->arr->akind == g::A_GROW || t->arr->akind == g::A_GROWSHRINK) &&
               Layouts::Known(t->arr->sub);
    }
    // A frame object (spec Appendix C.2): a struct whose last field, its
    // tail, is a resizable array and whose other fields are fixed-size. It
    // lives in the frame as its fixed fields followed by the tail's header,
    // with only the tail's elements on a data stack.
    static bool IsFrameObj(g::TypeExpr *t) {
        if (!t || t->kind != g::TY_STRUCT) return false;
        auto si = Layouts::SI(t);
        if (!si || si->sclass != g::SC_RESIZABLE || si->ftypes.empty()) return false;
        auto n = si->ftypes.size();
        if (si->st->fields[n - 1].ispad || !IsRz(si->ftypes[n - 1])) return false;
        for (size_t i = 0; i + 1 < n; i++)
            if (si->ftypes[i] && !Layouts::Known(si->ftypes[i])) return false;
        return true;
    }
    // A plain, non-optional reference to a resizable array or a frame
    // object: upstream's fat reference gs_rref { hdr, stk } (spec Appendix
    // C), where hdr is the array's header or the object.
    static bool IsRzRef(g::TypeExpr *t) {
        return Layouts::IsFatRef(t) && !t->ref->optional &&
               (IsRz(t->ref->sub) || IsFrameObj(t->ref->sub));
    }

    // The Wasm function of a specialization, added (and its body queued)
    // the first time a call reaches it (docs/design/value-representation.md
    // §5). Its first parameter is the first data stack index it may use
    // (upstream's gs_sp, impl §6.2). Scalars are Wasm parameters and
    // results; a fixed aggregate parameter is a handle to a copy the caller
    // makes; a fixed aggregate result is built where a hidden handle
    // parameter says; a reference to a resizable array is its header's
    // handle and its stack's index.
    uint32_t FuncOf(g::FnSpec *sp, const g::Node *at) {
        auto it = funcs.find(sp);
        if (it != funcs.end()) return it->second;
        if (sp->isfunval) Unsupported(at, "a call of a function value");
        if (sp->sf->isextern && !KnownExtern(sp)) Unsupported(at, "a call of an extern fn");
        FuncType t;
        t.params.push_back(ValType::I32);
        if (sp->rets.size() == 1 && IsAggregate(sp->rets[0])) {
            t.params.push_back(ValType::I64);
        } else if (sp->rets.size() == 1 && IsRz(sp->rets[0])) {
            // A resizable result is built at the top of the destination
            // stack the caller names; the callee returns its count (§7.3).
            t.params.push_back(ValType::I32);
            t.results.push_back(ValType::I64);
        } else {
            for (auto r : sp->rets) t.results.push_back(ValOf(ReprOf(at, r)));
        }
        for (auto p : sp->params) {
            if (IsAggregate(p->type)) {
                t.params.push_back(ValType::I64);
                continue;
            }
            if (IsRzRef(p->type)) {
                t.params.push_back(ValType::I64);
                t.params.push_back(ValType::I32);
                continue;
            }
            auto r = ReprOf(at, p->type);
            if (r == Repr::None) Unsupported(at, "a void parameter");
            t.params.push_back(ValOf(r));
        }
        // Each free variable by reference: its place's handle, and for a
        // resizable one its stack too.
        for (auto v : freevars[sp]) {
            t.params.push_back(ValType::I64);
            if (IsRz(v->type) || IsFrameObj(v->type)) t.params.push_back(ValType::I32);
            else if (!IsAggregate(v->type) && !IsInt(v->type) && !IsFlt(v->type) &&
                     v->type->kind != g::TY_BOOL && !IsPlainRef(v->type))
                Unsupported(at, g::cat("a free variable of type ", TypeKindName(v->type)));
        }
        if (Located(sp)) t.params.push_back(ValType::I32);
        auto index = m.AddFunc(t);
        funcs[sp] = index;
        pending.push_back(sp);
        return index;
    }

    // A function's body inside a block that every return leaves, between
    // its frame's claim and release on the shadow stack. The frame's size is
    // known only once the body is emitted, so the claim is put in front of
    // the code afterwards; a function with no frame gets none.
    void Function(g::FnSpec *sp, uint32_t index) {
        if (sp->sf->isextern) return ExternBody(sp, index);
        fn = index;
        spec = sp;
        locals.clear();
        memvars.clear();
        rzvars.clear();
        gstklocal.clear();
        labels.clear();
        scopes.clear();
        frame = 0;
        stknext = stkmax = 0;
        bool aggret = sp->rets.size() == 1 && IsAggregate(sp->rets[0]);
        FindAddrTaken(sp->body);
        std::vector<std::pair<g::VarDef *, uint32_t>> spills;
        uint32_t pi = 1;  // after SP
        retdst = aggret ? int64_t(pi++) : -1;
        bool rzret = sp->rets.size() == 1 && IsRz(sp->rets[0]);
        retrz = rzret ? int64_t(pi++) : -1;
        for (auto p : sp->params) {
            if (addrtaken.count(p)) {
                spills.push_back({ p, pi++ });
                continue;
            }
            if (IsAggregate(p->type)) {
                memvars[p] = Loc { false, pi++, 0, p->type, true };
            } else if (IsRzRef(p->type)) {
                rzvars[p] = RzVar { Loc { false, pi, 0, nullptr }, pi + 1 };
                // A frame object's fixed fields are read through the handle.
                if (IsFrameObj(p->type->ref->sub))
                    memvars[p] = Loc { false, pi, 0, p->type->ref->sub, false };
                pi += 2;
            } else {
                locals[p] = pi++;
            }
        }
        for (auto v : freevars[sp]) {
            if (IsRz(v->type) || IsFrameObj(v->type)) {
                rzvars[v] = RzVar { Loc { false, pi, 0, nullptr, false }, pi + 1 };
                if (IsFrameObj(v->type)) memvars[v] = Loc { false, pi, 0, v->type, false };
                pi += 2;
            } else {
                memvars[v] = Loc { false, pi++, 0, v->type, false };
            }
        }
        atparam = Located(sp) ? int64_t(pi++) : -1;
        nparams = pi;
        stackuse[sp] = StackUse {};
        fp = NewLocal(Repr::I32);
        // A copy: TypeIndex may add to the types it lives in.
        auto results = m.types[F().type].results;
        if (results.size() > 1) C().BlockOfType(m.TypeIndex({ {}, results }));
        else C().Block(results.empty() ? BlockType::Empty : BlockOf(ReprOfVal(results[0])));
        labels.push_back(Label { LK_FUNC });
        for (auto [p, index] : spills) {
            auto slot = FrameSlot(p->type);
            Store(sp->body, slot, [&] { C().LocalGet(index); });
            memvars[p] = slot;
        }
        if (aggret) {
            Loc dst { false, uint32_t(retdst), 0, sp->rets[0], false };
            BlockConstruct(sp->body, dst);
        } else if (rzret) {
            BlockRz(sp->body, uint32_t(retrz), sp->rets[0]->arr->sub);
        } else if (sp->rets.size() > 1) {
            // Several results leave by return statements only.
            BlockValue(sp->body, nullptr);
            C().Unreachable();
        } else {
            BlockValue(sp->body, sp->rets.empty() ? nullptr : sp->rets[0]);
        }
        Close();
        stackuse[sp].own = stkmax;
        FinishFrame(sp->incycle);
    }

    // The frame's claim (put in front of the emitted body, now that its
    // size is known) and release, and for a function of a recursive cycle
    // its activation count.
    void FinishFrame(bool incycle) {
        // Running out of the shadow stack, or recursing deeper than
        // MaxDepth, is upstream's native call stack overflow, which has no
        // location (docs/design/value-representation.md §2).
        static const std::string ovf = "native call stack overflow";
        auto overflow = [&](Code &c) {
            c.I32Const(int32_t(StaticString(ovf)));
            c.I32Const(int32_t(ovf.size()));
            c.Call(rt.panic);
        };
        Code pro;
        if (frame) {
            frame = (frame + 7) / 8 * 8;
            // Release: the results stay on the stack below.
            C().LocalGet(fp);
            C().I32Const(int32_t(frame));
            C().I32Add();
            C().GlobalSet(ssp);
            // Claim, checked against the shadow stack's end.
            pro.GlobalGet(ssp);
            pro.I32Const(int32_t(frame));
            pro.I32Sub();
            pro.LocalTee(fp);
            pro.GlobalGet(sslimit);
            pro.I32LtU();
            pro.If();
            overflow(pro);
            pro.End();
            pro.LocalGet(fp);
            pro.GlobalSet(ssp);
        }
        // Only a function of a recursive cycle can recurse without bound
        // (§7.8); its activations are counted, so that a runaway recursion
        // whose frames never touch the shadow stack is reported as upstream
        // reports it rather than ending in the engine's own stack overflow.
        if (incycle) {
            C().GlobalGet(depth);
            C().I32Const(1);
            C().I32Sub();
            C().GlobalSet(depth);
            pro.GlobalGet(depth);
            pro.I32Const(1);
            pro.I32Add();
            pro.GlobalSet(depth);
            pro.GlobalGet(depth);
            pro.I32Const(MaxDepth);
            pro.Int(intop::GtU, false);
            pro.If();
            overflow(pro);
            pro.End();
        }
        auto &body = C().b.b;
        body.insert(body.begin(), pro.b.b.begin(), pro.b.b.end());
    }

    static Repr ReprOfVal(ValType v) {
        switch (v) {
            case ValType::I64: return Repr::I64;
            case ValType::F32: return Repr::F32;
            case ValType::F64: return Repr::F64;
            default: return Repr::I32;
        }
    }

    // A slot of `size` bytes in the current frame.
    Loc FrameSlot(g::TypeExpr *t) {
        auto size = lay.FixedSize(t);
        auto off = frame;
        frame += (size + 7) / 8 * 8;
        return Loc { true, 0, off, t };
    }

    // The handle of a place, as an i64 (memory 0: the address itself).
    void Handle(const Loc &l) {
        if (!l.frame) {
            HandleOf(l);
            return;
        }
        Address(l);
        if (l.off) {
            C().I32Const(int32_t(l.off));
            C().I32Add();
        }
        C().I64ExtendI32U();
    }

    // A call of a specialization: the arguments converted to its parameter
    // types, then the call, which leaves its results on the stack. A fixed
    // aggregate argument is passed as a copy (spec §4.1), made in this
    // frame; `dst` is where an aggregate result goes.
    // A call of c's specialization, or with `over` of one case of its
    // dispatch, whose arguments `argfn` passes.
    void CallSpec(g::Call *c, const Loc *dst = nullptr, int64_t rzstk = -1, g::FnSpec *over = nullptr,
                  const std::function<void(size_t)> *argfn = nullptr) {
        if (!c->dispatch.empty() && !over) return Dispatch(c, dst, rzstk);
        // A trailing block is bound in the callee's specialization, not
        // passed (§7.6).
        if (c->fvbody) Unsupported(c, "a call of a function value");
        if (!c->fvremap.empty()) Unsupported(c, "a call with free variables");
        auto sp = over ? over : c->spec;
        auto index = FuncOf(sp, c);
        auto args = c->ValueArgNodes();
        if (args.size() != sp->params.size()) Unsupported(c, "a call's argument count");
        bool aggret = sp->rets.size() == 1 && IsAggregate(sp->rets[0]);
        // The callee's stacks start above those in use here (upstream
        // SpTop); a call into this function's own recursive cycle holds
        // none (§7.8), which the checker guarantees (NoStackAcrossCycleCall).
        auto cycle = [](g::FnSpec *s) { while (s->cyclelink) s = s->cyclelink; return s; };
        if (stknext && spec && spec->incycle && sp->incycle && cycle(sp) == cycle(spec))
            Unsupported(c, "a data stack held across a call into a recursive cycle");
        stackuse[spec].calls.push_back({ sp, stknext });
        C().LocalGet(SP);
        C().I32Const(stknext);
        C().I32Add();
        if (sp->rets.size() == 1 && IsRz(sp->rets[0])) {
            if (rzstk < 0) Unsupported(c, "a resizable result here");
            C().LocalGet(uint32_t(rzstk));
        } else if (rzstk >= 0) {
            Unsupported(c, "this call as a resizable value");
        }
        if (aggret) {
            if (!dst) Unsupported(c, "an aggregate result here");
            Handle(*dst);
        } else if (dst) {
            Unsupported(c, "this call as an aggregate");
        }
        for (size_t i = 0; i < args.size(); i++) {
            auto pt = sp->params[i]->type;
            if (argfn) {
                (*argfn)(i);
            } else if (IsAggregate(pt)) {
                auto copy = FrameSlot(pt);
                Construct(args[i], copy);
                Handle(copy);
            } else if (IsRzRef(pt)) {
                auto rz = RzOf(args[i]);
                Handle(rz.hdr);
                C().LocalGet(rz.stk);
            } else {
                Expr(args[i], pt);
            }
        }
        for (auto v : freevars[sp]) {
            auto a = c->FreeVarArg(v);
            if (IsRz(a->type) || IsFrameObj(a->type)) {
                auto it = rzvars.find(a);
                if (it == rzvars.end()) Unsupported(c, g::cat("the free variable ", a->name));
                Handle(it->second.hdr);
                C().LocalGet(it->second.stk);
                continue;
            }
            auto it = memvars.find(a);
            if (it == memvars.end()) Unsupported(c, g::cat("the free variable ", a->name));
            Handle(it->second);
        }
        if (Located(sp)) CallerLoc(c->line);
        C().Call(index);
    }

    // Case-function tag dispatch (spec §8.2; upstream EmitDispatch): every
    // argument in source order, the dispatched one a copy of the ADT (or,
    // through a reference, the ADT itself); then on its tag, the call of
    // the case taking that variant, with the payload where the ADT was.
    void Dispatch(g::Call *c, const Loc *dst, int64_t rzstk) {
        auto sp0 = c->dispatch[0];
        auto args = c->ValueArgNodes();
        auto pos = size_t(c->dispatcharg);
        if (args.size() != sp0->params.size()) Unsupported(c, "a call's argument count");
        auto st = args[pos]->exprtype;
        auto et = OperandType(st);
        if (!IsAdt(et) || !Layouts::FixedAdt(et)) Unsupported(c, "a dispatch on a variable-mode ADT");
        if (rzstk >= 0 || sp0->rets.size() > 1) Unsupported(c, "this dispatch's results");
        std::vector<std::function<void()>> pass(args.size());
        Loc scr;
        for (size_t i = 0; i < args.size(); i++) {
            auto a = args[i];
            if (i == pos) {
                if (IsPlainRef(st)) {
                    scr = ObjectLoc(a);
                } else {
                    scr = FrameSlot(et);
                    Construct(a, scr);
                }
                continue;
            }
            auto pt = sp0->params[i]->type;
            if (IsAggregate(pt)) {
                auto copy = FrameSlot(pt);
                Construct(a, copy);
                pass[i] = [this, copy] { Handle(copy); };
            } else if (IsRzRef(pt)) {
                auto rz = Snapshot(RzOf(a));
                pass[i] = [this, rz] {
                    Handle(rz.hdr);
                    C().LocalGet(rz.stk);
                };
            } else {
                auto l = NewLocal(ReprOf(a, pt));
                Expr(a, pt);
                C().LocalSet(l);
                pass[i] = [this, l] { C().LocalGet(l); };
            }
        }
        auto tag = NewLocal(Repr::I32);
        LoadTag(scr);
        C().LocalSet(tag);
        Repr r = Repr::None;
        if (sp0->rets.size() == 1 && !IsAggregate(sp0->rets[0])) r = ReprOf(c, sp0->rets[0]);
        auto ei = Layouts::EI(et);
        auto n = ei->en->variants.size();
        for (size_t vi = 0; vi < n; vi++) {
            auto sp = c->dispatch[vi];
            auto pt = sp->params[pos]->type;
            C().LocalGet(tag);
            C().I32Const(int32_t(vi));
            C().Int(intop::Eq, false);
            OpenIf(r);
            std::function<void(size_t)> argfn = [&](size_t i) {
                if (i != pos) return pass[i]();
                if (IsPlainRef(pt)) {
                    HandleOf(PayloadLoc(scr, pt->ref->sub));
                } else if (IsPlainRef(st)) {
                    // A by-value case of a referenced ADT: a copy.
                    auto copy = FrameSlot(pt);
                    Copy(copy, PayloadLoc(scr, pt));
                    Handle(copy);
                } else {
                    Handle(PayloadLoc(scr, pt));
                }
            };
            CallSpec(c, dst, -1, sp, &argfn);
            C().Else();
        }
        C().Unreachable();
        for (size_t vi = 0; vi < n; vi++) Close();
    }

    // A call of a function value (§7.6; upstream EmitFvCall): its checked
    // body spliced in, its parameters locals of this function bound to the
    // arguments, in a scope of its own. `tail` takes the body's value.
    template <typename T> void FvCall(g::Call *c, T tail) {
        PushScope();
        auto decl = new g::VarDecl(c->line, true);
        for (size_t i = 0; i < c->fvparams.size(); i++)
            DeclareVar(decl, c->fvparams[i], i < c->args.size() ? c->args[i] : nullptr);
        for (auto st : c->fvbody->stmts) Stmt(st);
        tail(c->fvbody->tail);
        PopScope();
    }

    // The resizable value a node names, as its header (or object) and its
    // stack: a variable of a resizable array or frame object type, or a
    // reference to one, through any `&` the checker inserted; a frame
    // object's tail; a fat reference held in a field.
    RzVar RzOf(g::Node *n) {
        if (n->nkind == g::NK_Unary && static_cast<g::Unary *>(n)->op == g::T_BITAND)
            n = static_cast<g::Unary *>(n)->child;
        if (n->nkind == g::NK_Ident) {
            auto v = static_cast<g::Ident *>(n)->vdef;
            auto it = v ? rzvars.find(v) : rzvars.end();
            if (it != rzvars.end()) return it->second;
            if (v && v->isglobal && gstk.count(v)) {
                auto obj = GlobalSlot(v);
                return RzVar { obj, GlobalStackLocal(v) };
            }
        }
        if (n->nkind == g::NK_Dot && static_cast<g::Dot *>(n)->IsField()) {
            auto d = static_cast<g::Dot *>(n);
            auto ot = StoredType(d->obj);
            if (!ot) ot = OperandType(d->obj->exprtype);
            if (IsRzRef(ot)) ot = ot->ref->sub;
            if (IsRoot(ot) && IsRzField(Layouts::SI(ot)->ftypes[d->fieldidx])) {
                auto desc = ObjectLoc(d->obj);
                desc.t = ot;
                return RootRzField(n, desc, d->fieldidx);
            }
            // The tail of a frame object: its header sits in the object, on
            // the object's stack (spec Appendix C.2).
            if (IsFrameObj(ot)) {
                auto si = Layouts::SI(ot);
                if (d->fieldidx + 1 == int(si->ftypes.size())) {
                    auto obj = RzOf(d->obj);
                    return RzVar { obj.hdr.At(lay.StructLayout(si).offs[d->fieldidx], nullptr), obj.stk };
                }
            }
            // A fat reference stored in a field: { hdr handle, stack index }.
            Loc f;
            if (MemLoc(n, f) && IsRzRef(f.t)) {
                auto h = NewLocal(Repr::I64), stk = NewLocal(Repr::I32);
                Load(n, f.At(0, I64T()));
                C().LocalSet(h);
                Load(n, f.At(8, I64T()));
                C().I32WrapI64();
                C().LocalSet(stk);
                return RzVar { Loc { false, h, 0, nullptr, false }, stk };
            }
        }
        Unsupported(n, g::cat("a resizable array ", KindName(n->nkind)));
    }

    // The receiver of a member builtin as it is named, before its arguments
    // run (an argument may rebind the reference that named it): a header
    // reached through a variable's locals is read into locals of its own.
    RzVar Snapshot(RzVar rz) {
        if (rz.hdr.frame || rz.hdr.abs) return rz;
        auto h = NewLocal(Repr::I64), stk = NewLocal(Repr::I32);
        HandleOf(rz.hdr);
        C().LocalSet(h);
        C().LocalGet(rz.stk);
        C().LocalSet(stk);
        return RzVar { Loc { false, h, 0, rz.hdr.t, rz.hdr.mem0 }, stk };
    }

    // Stores the fat reference to what `n` names at a 16-byte field f.
    void StoreFatRef(g::Node *n, const Loc &f) {
        auto rz = RzOf(n);
        Store(n, f.At(0, I64T()), [&] { Handle(rz.hdr); });
        Store(n, f.At(8, I64T()), [&] {
            C().LocalGet(rz.stk);
            C().I64ExtendI32U();
        });
    }

    // The module's data stack count (spec §10.3): the most indices any path
    // of calls from main holds at once. A recursive cycle starts every
    // activation at the same index, so the fixpoint is reached; the bound
    // guards against a cycle that would not.
    int StackCount(g::FnSpec *mainspec) {
        std::map<g::FnSpec *, int> depth;
        for (auto &[sp, u] : stackuse) depth[sp] = u.own;
        for (int round = 0;; round++) {
            bool changed = false;
            for (auto &[sp, u] : stackuse)
                for (auto &[callee, off] : u.calls) {
                    auto d = off + depth[callee];
                    if (d > depth[sp]) {
                        depth[sp] = d;
                        changed = true;
                    }
                }
            if (!changed) break;
            if (depth[mainspec] > int(mem0::MaxStacks) || round > 100000)
                throw g::CompileError { "goose-wasm: the data stack count does not converge" };
        }
        return depth[mainspec];
    }

    Func &F() { return m.FuncAt(fn); }
    Code &C() { return F().code; }

    uint32_t NewLocal(Repr r) {
        F().locals.push_back(ValOf(r));
        return nparams + uint32_t(F().locals.size() - 1);
    }

    uint32_t LocalOf(const g::Node *n, g::VarDef *v) {
        auto it = locals.find(v);
        if (it == locals.end()) Unsupported(n, g::cat("variable ", v->name));
        return it->second;
    }

    BlockType BlockOf(Repr r) {
        return r == Repr::None ? BlockType::Empty : BlockType(uint8_t(ValOf(r)));
    }

    void Open(LabelKind k, Repr r, g::TypeExpr *t = nullptr, g::SFunction *sf = nullptr,
              bool loop = false) {
        if (loop) C().Loop(BlockOf(r));
        else C().Block(BlockOf(r));
        labels.push_back(Label { k, sf, t, nullptr, scopes.size() });
    }
    void OpenIf(Repr r) {
        C().If(BlockOf(r));
        labels.push_back(Label { LK_PLAIN, nullptr, nullptr, nullptr, scopes.size() });
    }
    void Close() {
        C().End();
        labels.pop_back();
    }
    // The br depth of the innermost label of kind k (and, for LK_INLINE,
    // of function sf), or -1.
    int Depth(LabelKind k, g::SFunction *sf = nullptr) {
        for (int i = int(labels.size()) - 1; i >= 0; i--)
            if (labels[i].kind == k && (k != LK_INLINE || labels[i].sf == sf))
                return int(labels.size()) - 1 - i;
        return -1;
    }

    // ---- Types ----

    static bool IsVoid(g::TypeExpr *t) { return !t || t->kind == g::TY_VOID; }

    Repr ReprOf(const g::Node *n, g::TypeExpr *t) {
        if (IsVoid(t)) return Repr::None;
        if (t->kind == g::TY_BOOL) return Repr::I32;
        if (t->kind == g::TY_INT && t->intstorage != g::IS_VARINT)
            return t->intstorage == g::IS_I64 || t->intstorage == g::IS_U64 ? Repr::I64 : Repr::I32;
        if (t->kind == g::TY_FLT) return t->fltstorage == g::FS_F32 ? Repr::F32 : Repr::F64;
        // A plain reference or optional is its handle; null is 0.
        if (IsPlainRef(t)) return Repr::I64;
        Unsupported(n, g::cat("a value of type ", TypeKindName(t), BlockingPart(t)));
    }

    // For the unsupported report: what in a struct or array keeps it from
    // being laid out, as " with a <kind> field".
    static std::string BlockingPart(g::TypeExpr *t) {
        std::function<g::TypeExpr *(g::TypeExpr *)> find = [&](g::TypeExpr *x) -> g::TypeExpr * {
            if (x->kind == g::TY_STRUCT) {
                auto si = Layouts::SI(x);
                if (!si) return x;
                if (si->sclass != g::SC_FIXED) {
                    for (auto ft : si->ftypes)
                        if (ft && !Layouts::Known(ft)) return find(ft);
                    return x;
                }
                for (auto ft : si->ftypes)
                    if (ft && !Layouts::Known(ft)) return find(ft);
                return nullptr;
            }
            if (x->kind == g::TY_ARRAY && Layouts::IsFixedArray(x)) return find(x->arr->sub);
            return Layouts::Known(x) ? nullptr : x;
        };
        if (t->kind != g::TY_STRUCT) return "";
        auto part = find(t);
        return part && part != t ? g::cat(" with a ", TypeKindName(part), " field") : "";
    }

    // A type's kind, for the unsupported report: grouping by it (rather than
    // by every type spelled out) is what tells which representation to add.
    static std::string TypeKindName(g::TypeExpr *t) {
        switch (t->kind) {
            case g::TY_INT: return "varint";
            case g::TY_FLT: return "float";
            case g::TY_STRUCT: return "struct";
            case g::TY_ENUM: return "enum";
            case g::TY_FN: return "fn";
            case g::TY_ARRAY: {
                static const char *const kinds[] = { "T[k]", "T[]", "T[..k]", "T[>..]", "T[>..<]" };
                return t->arr->akind <= g::A_GROWSHRINK ? kinds[t->arr->akind] : "array";
            }
            case g::TY_SLICE: return "slice";
            case g::TY_REF: return "reference";
            case g::TY_VARIANT: return "variant";
            default: {
                std::string s;
                t->Dump(s);
                return s;
            }
        }
    }
    Repr ReprOf(const g::Node *n) { return ReprOf(n, n->exprtype); }

    // Whether two scalar types hold their values the same way.
    static bool SameScalar(g::TypeExpr *a, g::TypeExpr *b) {
        if (a == b) return true;
        if (!a || !b || a->kind != b->kind) return false;
        return a->kind == g::TY_BOOL || (a->kind == g::TY_INT && a->intstorage == b->intstorage);
    }

    static bool IsInt(g::TypeExpr *t) {
        return t && t->kind == g::TY_INT && t->intstorage != g::IS_VARINT;
    }
    static bool IsSigned(g::TypeExpr *t) { return !g::IsUnsigned(t->intstorage); }
    static bool IsWide(g::TypeExpr *t) {
        return t->intstorage == g::IS_I64 || t->intstorage == g::IS_U64;
    }
    static int Bits(g::TypeExpr *t) { return g::IntBits(t->intstorage); }

    // Brings the i32 holding a narrow integer back to its type's range:
    // what C's truncating conversion to the type does.
    void Normalize(g::TypeExpr *t) {
        switch (t->intstorage) {
            case g::IS_I8: C().I32Extend8S(); break;
            case g::IS_I16: C().I32Extend16S(); break;
            case g::IS_U8: C().I32Const(0xff); C().I32And(); break;
            case g::IS_U16: C().I32Const(0xffff); C().I32And(); break;
            default: break;
        }
    }

    // Converts the value on the stack from type `from` to type `to`, as C's
    // conversion between the integer types upstream emits them as does:
    // widening by the source's signedness, narrowing by truncation. Bools
    // are 0 or 1 at any width.
    void Convert(const g::Node *n, g::TypeExpr *from, g::TypeExpr *to) {
        if (from == to || IsVoid(to)) return;
        if (IsPlainRef(from) && IsPlainRef(to)) return;
        auto fr = ReprOf(n, from), tr = ReprOf(n, to);
        bool fromint = IsInt(from), toint = IsInt(to);
        if (IsFlt(to)) return ToFloat(n, from, tr);
        if (IsFlt(from)) {
            // Float to integer: truncated toward zero and wrapped modulo
            // 2^64 (upstream gs_f2iwrap), then to the target's width.
            if (!toint) Unsupported(n, "a conversion of a float to this type");
            if (fr == Repr::F32) C().Op(0xbb);  // f64.promote_f32
            C().Call(f2iwrap);
            Convert(n, I64T(), to);
            return;
        }
        if (!toint) {
            if (to->kind == g::TY_BOOL && from->kind == g::TY_BOOL) return;
            Unsupported(n, "a conversion to this type");
        }
        if (fr == Repr::I32 && tr == Repr::I64) {
            if (fromint && IsSigned(from)) C().I64ExtendI32S();
            else C().I64ExtendI32U();
        } else if (fr == Repr::I64 && tr == Repr::I32) {
            C().I32WrapI64();
            Normalize(to);
        } else if (tr == Repr::I32) {
            Normalize(to);
        }
    }

    static bool IsFlt(g::TypeExpr *t) { return t && t->kind == g::TY_FLT; }

    // The value on the stack, of type from, as a float of repr tr: an
    // integer rounds to the nearest float by its signedness (as C converts,
    // spec §6.3), and floats widen or round to nearest.
    void ToFloat(const g::Node *n, g::TypeExpr *from, Repr tr) {
        auto fr = ReprOf(n, from);
        bool f32 = tr == Repr::F32;
        if (IsFlt(from)) {
            if (fr == Repr::F32 && !f32) C().Op(0xbb);  // f64.promote_f32
            if (fr == Repr::F64 && f32) C().Op(0xb6);   // f32.demote_f64
            return;
        }
        if (from->kind == g::TY_BOOL) fr = Repr::I32;
        else if (!IsInt(from)) Unsupported(n, "a conversion of this type to a float");
        bool s = from->kind != g::TY_BOOL && IsSigned(from);
        if (fr == Repr::I32) C().Op(f32 ? (s ? 0xb2 : 0xb3) : (s ? 0xb7 : 0xb8));
        else C().Op(f32 ? (s ? 0xb4 : 0xb5) : (s ? 0xb9 : 0xba));
    }

    // f2iwrap(d): `as` of a float to an integer, release semantics
    // (upstream gs_f2iwrap): truncated toward zero within the i64 range;
    // NaN is 0; beyond it, wrapped modulo 2^64. A double that large is an
    // integer, so its residue comes from its bits: the significand shifted
    // to its exponent, negated for a negative value. An infinity, whose C
    // conversion is undefined, gives 0, as x86-64 code built by clang does.
    void DefineF2IWrap() {
        using V = ValType;
        f2iwrap = m.AddFunc({ { V::F64 }, { V::I64 } }, { V::I64, V::I64 });
        auto &c = m.FuncAt(f2iwrap).code;
        enum { d, b, sh };
        // if (d >= -2^63 && d < 2^63) return (int64_t)d;
        c.LocalGet(d); c.F64Const(-9223372036854775808.0); c.Op(0x66);  // f64.ge
        c.LocalGet(d); c.F64Const(9223372036854775808.0); c.Op(0x63);   // f64.lt
        c.I32And();
        c.If();
        c.LocalGet(d); c.Op(0xb0); c.Return();  // i64.trunc_f64_s
        c.End();
        c.LocalGet(d); c.Op(0xbd); c.LocalSet(b);  // i64.reinterpret_f64
        // NaN and the infinities: the all-ones exponent.
        c.LocalGet(b); c.I64Const(52); c.Int(intop::ShrU, true); c.I64Const(0x7ff); c.Int(intop::And, true);
        c.LocalTee(sh);
        c.I64Const(0x7ff); c.Int(intop::Eq, true);
        c.If();
        c.I64Const(0); c.Return();
        c.End();
        // The shift of the 53-bit significand: at least 11 here.
        c.LocalGet(sh); c.I64Const(1075); c.Int(intop::Sub, true); c.LocalSet(sh);
        c.LocalGet(sh); c.I64Const(64); c.Int(intop::GeU, true);
        c.If();
        c.I64Const(0); c.Return();
        c.End();
        c.LocalGet(b); c.I64Const((int64_t(1) << 52) - 1); c.Int(intop::And, true);
        c.I64Const(int64_t(1) << 52); c.Int(intop::Or, true);
        c.LocalGet(sh); c.Int(intop::Shl, true);
        c.LocalSet(sh);
        c.I64Const(0); c.LocalGet(sh); c.Int(intop::Sub, true);
        c.LocalGet(sh);
        c.LocalGet(b); c.I64Const(0); c.Int(intop::LtS, true);
        c.Select();
    }

    // The type both operands of a binary operation are computed at: the
    // operands' when they agree, else the one the other widens to
    // implicitly (spec §6.1, §6.3).
    g::TypeExpr *Common(const g::Node *n, g::TypeExpr *a, g::TypeExpr *b) {
        // An integer meets a float as the float; f32 meets f64 as f64.
        if (IsFlt(a) || IsFlt(b)) {
            if (!IsFlt(a)) return b;
            if (!IsFlt(b)) return a;
            return a->fltstorage == g::FS_F64 ? a : b;
        }
        if (a == b || !IsInt(a) || !IsInt(b)) return a;
        if (a->intstorage == b->intstorage) return a;
        auto ba = Bits(a), bb = Bits(b);
        if (ba == bb) Unsupported(n, "operands of one width and different signedness");
        return ba > bb ? a : b;
    }

    // ---- Memory ----

    // The base address of a place in memory 0, without its offset, which
    // loads and stores carry in their memory argument. Only for Direct()
    // places; the others go through handles.
    void Address(const Loc &l) {
        if (l.abs) {
            C().I32Const(0);
        } else if (l.frame) {
            C().LocalGet(fp);
        } else {
            C().LocalGet(l.h);
            C().I32WrapI64();
        }
    }
    // The handle of a place, its offset included.
    void HandleOf(const Loc &l) {
        if (l.abs) {
            C().I64Const(l.off);
            return;
        }
        if (l.frame) {
            Handle(l);
            return;
        }
        C().LocalGet(l.h);
        if (l.off) {
            C().I64Const(l.off);
            C().Int(intop::Add, true);
        }
    }
    void AddressWithOffset(const Loc &l) {
        Address(l);
        if (l.off) {
            C().I32Const(int32_t(l.off));
            C().I32Add();
        }
    }

    // The load and store of a scalar of type t, at its width and
    // signedness (spec 5.4.7 opcodes).
    uint8_t LoadOpOf(const g::Node *n, g::TypeExpr *t) {
        if (t->kind == g::TY_BOOL) return 0x2d;  // i32.load8_u
        if (IsPlainRef(t)) return 0x29;          // i64.load
        if (IsFlt(t)) return t->fltstorage == g::FS_F32 ? 0x2a : 0x2b;
        if (!IsInt(t)) Unsupported(n, g::cat("a load of type ", TypeKindName(t)));
        switch (t->intstorage) {
            case g::IS_I8: return 0x2c;
            case g::IS_U8: return 0x2d;
            case g::IS_I16: return 0x2e;
            case g::IS_U16: return 0x2f;
            case g::IS_I32: case g::IS_U32: return 0x28;
            default: return 0x29;  // i64.load
        }
    }
    uint8_t StoreOpOf(const g::Node *n, g::TypeExpr *t) {
        if (t->kind == g::TY_BOOL) return 0x3a;  // i32.store8
        if (IsPlainRef(t)) return 0x37;          // i64.store
        if (IsFlt(t)) return t->fltstorage == g::FS_F32 ? 0x38 : 0x39;
        if (!IsInt(t)) Unsupported(n, g::cat("a store of type ", TypeKindName(t)));
        switch (Bits(t)) {
            case 8: return 0x3a;
            case 16: return 0x3b;
            case 32: return 0x36;
            default: return 0x37;  // i64.store
        }
    }

    void Load(const g::Node *n, const Loc &l) {
        if (IsRelRef(l.t)) return RelLoad(n, l);
        auto op = LoadOpOf(n, l.t);
        if (!l.Direct()) {
            HandleOf(l);
            C().Call(hs.Load(m, op));
            return;
        }
        Address(l);
        C().LoadOp(op, uint64_t(l.off));
    }
    // Stores what `value` emits, a scalar of l's type, at l.
    template <typename F> void Store(const g::Node *n, const Loc &l, F value) {
        if (IsRelRef(l.t)) return RelStore(n, l, std::function<void()>(value));
        auto op = StoreOpOf(n, l.t);
        if (!l.Direct()) {
            HandleOf(l);
            value();
            C().Call(hs.Store(m, op));
            return;
        }
        Address(l);
        value();
        C().StoreOp(op, uint64_t(l.off));
    }

    // A variable's own value, from its Wasm local or its frame slot.
    void VarValue(const g::Node *n, g::VarDef *v) {
        if (v->isglobal) {
            auto l = GlobalSlot(v);
            if (!l.abs) Unsupported(n, g::cat("a global of type ", TypeKindName(v->type)));
            Load(n, l);
            return;
        }
        auto it = memvars.find(v);
        if (it != memvars.end()) Load(n, it->second);
        else C().LocalGet(LocalOf(n, v));
    }

    // The handle of what n names: n's value when it is a reference already,
    // else the address of its place.
    void AddressOf(g::Node *n) {
        if (n->nkind == g::NK_Unary && static_cast<g::Unary *>(n)->op == g::T_BITAND)
            n = static_cast<g::Unary *>(n)->child;
        if (IsPlainRef(n->exprtype)) {
            Value(n);
            return;
        }
        Loc l;
        if (!MemLoc(n, l)) Unsupported(n, g::cat("a reference to ", KindName(n->nkind)));
        if (IsPlainRef(l.t) && !IsPlainRef(n->exprtype)) l = Deref(n, l);
        Handle(l);
    }

    // The place a reference stored at l points to.
    Loc Deref(const g::Node *n, const Loc &l) {
        auto h = NewLocal(Repr::I64);
        Load(n, l);
        C().LocalSet(h);
        return Loc { false, h, 0, l.t->ref->sub, false };
    }

    // The place in memory a node names, as a value of its own type: a
    // variable held in memory, the pointee of a reference variable, a field,
    // an element. A field or element that is itself a reference is the place
    // holding it (Deref reaches its pointee). False for anything else.
    bool MemLoc(g::Node *n, Loc &out) {
        if (n->nkind == g::NK_Index) {
            out = IndexLoc(static_cast<g::Index *>(n));
            return true;
        }
        if (n->nkind == g::NK_Ident) {
            auto v = static_cast<g::Ident *>(n)->vdef;
            if (!v) return false;
            if (IsPlainRef(v->type)) {
                // A reference variable names its pointee (§3.8).
                auto h = NewLocal(Repr::I64);
                VarValue(n, v);
                C().LocalSet(h);
                out = Loc { false, h, 0, v->type->ref->sub, false };
                return true;
            }
            if (v->isglobal) {
                out = GlobalSlot(v);
                if (!out.abs) Unsupported(n, g::cat("a global of type ", TypeKindName(v->type)));
                return true;
            }
            auto it = memvars.find(v);
            if (it == memvars.end()) return false;
            out = it->second;
            return true;
        }
        if (n->nkind == g::NK_Dot) {
            auto d = static_cast<g::Dot *>(n);
            if (!d->IsField()) return false;
            auto base = ObjectLoc(d->obj);
            if (IsRoot(base.t)) {
                if (IsRzField(Layouts::SI(base.t)->ftypes[d->fieldidx])) return false;
                out = RootField(n, base, d->fieldidx);
                return true;
            }
            if (base.t->kind == g::TY_VARIANT) {
                // A variant's payload field (a match binder, a variant value).
                auto ei = Layouts::EIVar(base.t);
                auto vi = Layouts::VariantIndex(base.t);
                out = base.At(lay.VariantLayout(ei, vi).offs[d->fieldidx], ei->vftypes[vi][d->fieldidx]);
                return true;
            }
            if (base.t->kind != g::TY_STRUCT) Unsupported(n, "a field of this kind of object");
            auto si = Layouts::SI(base.t);
            out = base.At(lay.StructLayout(si).offs[d->fieldidx], si->ftypes[d->fieldidx]);
            return true;
        }
        return false;
    }

    // The place of the object a field or element is taken from: through a
    // reference to it, or in a temporary for a value with no place of its
    // own (f().x).
    Loc ObjectLoc(g::Node *obj) {
        Loc base;
        if (MemLoc(obj, base)) {
            if (IsPlainRef(base.t)) base = Deref(obj, base);
            return base;
        }
        auto ot = obj->exprtype;
        if (IsPlainRef(ot)) {
            auto h = NewLocal(Repr::I64);
            Value(obj);
            C().LocalSet(h);
            return Loc { false, h, 0, ot->ref->sub, false };
        }
        if (!IsAggregate(ot)) Unsupported(obj, g::cat("an object ", KindName(obj->nkind)));
        base = FrameSlot(ot);
        Construct(obj, base);
        return base;
    }

    // Copies a fixed aggregate from src to dst; memory.copy is correct for
    // overlapping bytes too.
    void Copy(const Loc &dst, const Loc &src) {
        auto size = int32_t(lay.FixedSize(dst.t));
        if (!dst.Direct() || !src.Direct()) {
            HandleOf(dst);
            HandleOf(src);
            C().I32Const(size);
            C().Call(hs.copy);
            return;
        }
        AddressWithOffset(dst);
        AddressWithOffset(src);
        C().I32Const(size);
        C().MemoryCopy();
    }

    // Builds n, a fixed aggregate, at dst. A fresh value is built field by
    // field in place; an existing one is copied (spec §4.1, §4.3). Callers
    // pass a dst nothing in n can read: a fresh declaration's slot or a
    // temporary.
    void Construct(g::Node *n, const Loc &dst) {
        switch (n->nkind) {
            case g::NK_StructLit: {
                auto sl = static_cast<g::StructLit *>(n);
                if (sl->einst) return VariantInto(sl, dst);
                auto si = sl->sinst;
                if (!si || si->sclass != g::SC_FIXED) Unsupported(n, "this struct literal");
                auto &lo = lay.StructLayout(si);
                // The initializers in the order the checker left them, which
                // is the order they run in; a field with none is an omitted
                // optional, which is null.
                std::vector<bool> given(si->ftypes.size());
                for (size_t k = 0; k < sl->inits.size(); k++) {
                    auto fi = sl->fieldindices[k];
                    given[fi] = true;
                    auto ft = si->ftypes[fi];
                    auto floc = dst.At(lo.offs[fi], ft);
                    auto val = sl->inits[k].val;
                    if (val->nkind == g::NK_SelfRef) Store(val, floc, [&] { HandleOf(dst); });
                    else if (IsRzRef(ft)) StoreFatRef(val, floc);
                    else if (IsAggregate(ft)) Construct(val, floc);
                    else Store(val, floc, [&] { Expr(val, ft); });
                }
                for (size_t fi = 0; fi < si->ftypes.size(); fi++)
                    if (!given[fi] && !si->st->fields[fi].ispad)
                        OmittedField(n, dst.At(lo.offs[fi], si->ftypes[fi]));
                return;
            }
            case g::NK_MatchExpr:
                Match(static_cast<g::MatchExpr *>(n), Repr::None, [&](g::Node *b) {
                    if (b->nkind == g::NK_Block) BlockConstruct(static_cast<g::Block *>(b), dst);
                    else Construct(b, dst);
                });
                return;
            case g::NK_Ident:
            case g::NK_Dot:
            case g::NK_Index: {
                // A payload-less variant constant: its tag alone.
                if (n->nkind == g::NK_Dot && static_cast<g::Dot *>(n)->variantconst) {
                    auto d = static_cast<g::Dot *>(n);
                    if (!IsAdt(dst.t)) Unsupported(n, "a variant constant as this type");
                    StoreTag(dst, d->einst->en->VariantIndex(d->variantconst));
                    return;
                }
                // An array where a slice is expected is viewed whole (the
                // checker types the use as the slice, §3.10).
                auto st = StoredType(n);
                if (IsSlice(dst.t) && st && !IsSlice(st)) {
                    auto s = SeqOf(n, st);
                    StoreSlice(n, dst, [&] { C().LocalGet(s.data); }, s.len);
                    return;
                }
                Loc src;
                if (!MemLoc(n, src)) Unsupported(n, g::cat("an aggregate ", KindName(n->nkind)));
                if (IsPlainRef(src.t)) src = Deref(n, src);
                // A variant's payload made an ADT: the tag, then the payload.
                if (IsAdt(dst.t) && IsVariantT(src.t)) {
                    Copy(PayloadLoc(dst, src.t), src);
                    StoreTag(dst, Layouts::VariantIndex(src.t));
                    return;
                }
                Copy(dst, src);
                return;
            }
            case g::NK_ArrayLit: return ArrayInto(static_cast<g::ArrayLit *>(n), dst);
            case g::NK_SliceExpr: return SliceInto(static_cast<g::SliceExpr *>(n), dst);
            case g::NK_StrLit: {
                // A string literal is a u8 slice of static data (§3.7).
                if (Layouts::IsLimited(dst.t)) return LimInto(n, dst);
                if (!IsSlice(dst.t)) Unsupported(n, "a string literal as this type");
                auto &s = static_cast<g::StrLit *>(n)->val;
                auto addr = StaticString(s);
                StoreSlice(n, dst, [&] { C().I64Const(addr); }, [&] { C().I64Const(int64_t(s.size())); });
                return;
            }
            case g::NK_Call: {
                auto c = static_cast<g::Call *>(n);
                if (IsSlice(dst.t) && c->rettypes.size() == 1 && IsRz(c->rettypes[0])) {
                    auto s = TempRz(n, c->rettypes[0]->arr->sub);
                    StoreSlice(n, dst, [&] { C().LocalGet(s.data); }, s.len);
                    return;
                }
                if (c->builtin == g::B_POP) {
                    Copy(dst, Loc { false, Pop(c), 0, dst.t, false });
                    return;
                }
                if (c->builtin == g::B_DEFAULT) return DefaultInto(c, dst);
                if (c->builtin == g::B_BYTES_OF && IsSlice(dst.t)) {
                    // bytes_of(a) (§12): a view of an array's element bytes
                    // (upstream PayloadOf), little-endian as Wasm is.
                    auto sq = SeqOf(c->ArgNodes()[0]);
                    if (!Layouts::Known(sq.et)) Unsupported(n, "bytes_of these elements");
                    auto esz = lay.FixedSize(sq.et);
                    StoreSlice(n, dst, [&] { C().LocalGet(sq.data); }, [&] {
                        sq.len();
                        C().I64Const(esz);
                        C().Int(intop::Mul, true);
                    });
                    return;
                }
                if (c->fvbody) {
                    FvCall(c, [&](g::Node *tl) {
                        if (tl) Construct(tl, dst);
                        else C().Unreachable();
                    });
                    return;
                }
                if (c->builtin >= 0 || (!c->spec && c->dispatch.empty()))
                    Unsupported(n, "this call as an aggregate");
                // A returned reference to an aggregate: a copy of the pointee.
                if (c->rettypes.size() == 1 && IsPlainRef(c->rettypes[0])) {
                    CallSpec(c);
                    auto h = NewLocal(Repr::I64);
                    C().LocalSet(h);
                    Copy(dst, Loc { false, h, 0, c->rettypes[0]->ref->sub, false });
                    return;
                }
                CallSpec(c, &dst);
                return;
            }
            case g::NK_Block: return BlockConstruct(static_cast<g::Block *>(n), dst);
            case g::NK_IfExpr: {
                auto i = static_cast<g::IfExpr *>(n);
                if (!i->elseb) Unsupported(n, "an if of a value without else");
                Truth(i->cond);
                OpenIf(Repr::None);
                BlockConstruct(i->thenb, dst);
                C().Else();
                Construct(i->elseb, dst);
                Close();
                return;
            }
            case g::NK_InlineBlock: {
                auto ib = static_cast<g::InlineBlock *>(n);
                Open(LK_INLINE, Repr::None, n->exprtype, ib->sf);
                labels.back().dst = &dst;
                BlockConstruct(ib->body, dst);
                Close();
                return;
            }
            case g::NK_EarlyBlock: {
                Open(LK_BREAK, Repr::None, n->exprtype);
                labels.back().dst = &dst;
                BlockConstruct(static_cast<g::EarlyBlock *>(n)->body, dst);
                Close();
                return;
            }
            default:
                Unsupported(n, g::cat("an aggregate ", KindName(n->nkind)));
        }
    }

    // The type of what a variable, field or element node holds, before any
    // adaptation the checker recorded on the node's own type (an array used
    // as a slice); through a reference variable, its pointee's.
    g::TypeExpr *StoredType(g::Node *n) {
        if (n->nkind == g::NK_Ident) {
            auto v = static_cast<g::Ident *>(n)->vdef;
            if (!v) return nullptr;
            return IsPlainRef(v->type) || IsRzRef(v->type) ? v->type->ref->sub : v->type;
        }
        if (n->nkind == g::NK_Dot) {
            auto d = static_cast<g::Dot *>(n);
            auto ot = StoredType(d->obj);
            if (!ot) ot = d->obj->exprtype;
            if (IsPlainRef(ot)) ot = ot->ref->sub;
            if (!d->IsField() || !ot || ot->kind != g::TY_STRUCT) return nullptr;
            return Layouts::SI(ot)->ftypes[d->fieldidx];
        }
        return nullptr;
    }

    // Stores a slice { data, len } at dst from what the two callbacks push.
    template <typename D, typename L> void StoreSlice(const g::Node *n, const Loc &dst, D data, L len) {
        Store(n, dst.At(0, I64T()), data);
        Store(n, dst.At(8, I64T()), len);
    }

    // A fill's value, evaluated once into a frame slot of the element type.
    Loc FillValue(g::Node *val, g::TypeExpr *et) {
        auto slot = FrameSlot(et);
        if (IsAggregate(et)) Construct(val, slot);
        else Store(val, slot, [&] { Expr(val, et); });
        return slot;
    }

    // [a, b, c] or [v; n] into a fixed-size array at dst. A fill evaluates
    // its value once and copies it into every element (§4.3: the repeated
    // value of a fill is an ordinary copy).
    void ArrayInto(g::ArrayLit *lit, const Loc &dst) {
        if (Layouts::IsLimited(dst.t)) return LimInto(lit, dst);
        if (!Layouts::IsFixedArray(dst.t)) Unsupported(lit, "an array literal as this type");
        if (lit->capexpr) Unsupported(lit, "this array literal");
        auto et = dst.t->arr->sub;
        auto esz = lay.FixedSize(et);
        auto put = [&](g::Node *val, const Loc &at) {
            if (IsAggregate(et)) Construct(val, at);
            else Store(val, at, [&] { Expr(val, et); });
        };
        if (!lit->fillval) {
            for (size_t i = 0; i < lit->elems.size(); i++) put(lit->elems[i], dst.At(int64_t(i) * esz, et));
            return;
        }
        auto k = dst.t->arr->size;
        // The value once, before any element and even for none (upstream
        // FillScope), then copied to each.
        auto first = FillValue(lit->fillval, et);
        auto i = NewLocal(Repr::I64), h = NewLocal(Repr::I64);
        C().I64Const(0);
        C().LocalSet(i);
        Open(LK_PLAIN, Repr::None);
        Open(LK_PLAIN, Repr::None, nullptr, nullptr, true);
        C().LocalGet(i);
        C().I64Const(k);
        C().Int(intop::GeU, true);
        C().BrIf(1);
        HandleOf(dst);
        C().LocalGet(i);
        C().I64Const(esz);
        C().Int(intop::Mul, true);
        C().Int(intop::Add, true);
        C().LocalSet(h);
        Copy(Loc { false, h, 0, et, dst.Direct() }, first);
        C().LocalGet(i);
        C().I64Const(1);
        C().Int(intop::Add, true);
        C().LocalSet(i);
        C().Br(0);
        Close();
        Close();
    }

    // a[lo..hi] (upstream CodeGen::GenSlice): the bounds default to 0 and
    // the length, ^k counts from the end, and unless BCE proved them,
    // lo < 0 || hi < lo || hi > len aborts with "slice bounds out of range".
    void SliceInto(g::SliceExpr *se, const Loc &dst) {
        if (!IsSlice(dst.t)) Unsupported(se, "a slice as this type");
        auto s = SeqOf(se->obj);
        auto len = NewLocal(Repr::I64), lo = NewLocal(Repr::I64), hi = NewLocal(Repr::I64);
        s.len();
        C().LocalSet(len);
        auto bound = [&](g::Node *b, bool fromend, bool isend, uint32_t into) {
            if (!b) {
                if (isend) C().LocalGet(len);
                else C().I64Const(0);
            } else if (fromend) {
                C().LocalGet(len);
                Expr(b, I64T());
                C().Int(intop::Sub, true);
            } else {
                Expr(b, I64T());
            }
            C().LocalSet(into);
        };
        bound(se->lo, se->lo_from_end, false, lo);
        bound(se->hi, se->hi_from_end, true, hi);
        if (!se->nobc) {
            C().LocalGet(lo);
            C().I64Const(0);
            C().Int(intop::LtS, true);
            C().LocalGet(hi);
            C().LocalGet(lo);
            C().Int(intop::LtS, true);
            C().Int(intop::Or, false);
            C().LocalGet(hi);
            C().LocalGet(len);
            C().Int(intop::GtS, true);
            C().Int(intop::Or, false);
            C().If();
            Abort(se, "slice bounds out of range");
            C().End();
        }
        auto esz = lay.FixedSize(s.et);
        StoreSlice(se, dst, [&] {
            C().LocalGet(s.data);
            C().LocalGet(lo);
            C().I64Const(esz);
            C().Int(intop::Mul, true);
            C().Int(intop::Add, true);
        }, [&] {
            C().LocalGet(hi);
            C().LocalGet(lo);
            C().Int(intop::Sub, true);
        });
    }

    // A block whose value, an aggregate, is built at dst.
    void BlockConstruct(g::Block *b, const Loc &dst) {
        PushScope();
        for (auto s : b->stmts) Stmt(s);
        if (b->tail) Construct(b->tail, dst);
        PopScope();
    }

    // ---- Resizable values built on a stack ----

    // Builds an array value's elements at the top of stack `stk` (a local),
    // leaving their count, an i64. This is how a resizable result reaches
    // its destination (spec §7.3, §4.3): a literal's elements are pushed
    // there one by one, each evaluated before its slot is claimed; a call
    // returning a resizable builds there itself; control constructs build
    // there from each branch; anything else with elements has its run
    // copied there.
    void BuildRzInto(g::Node *n, uint32_t stk, g::TypeExpr *et) {
        auto esz = int32_t(lay.FixedSize(et));
        switch (n->nkind) {
            case g::NK_ArrayLit: {
                auto lit = static_cast<g::ArrayLit *>(n);
                if (lit->capexpr) Unsupported(n, "this array literal here");
                if (!lit->fillval) {
                    for (auto e : lit->elems) PushSlot(n, stk, et, e);
                    C().I64Const(int64_t(lit->elems.size()));
                    return;
                }
                // [v; n]: the value once, before the count and even for a
                // count of zero (upstream FillScope), then copies of it.
                auto first = FillValue(lit->fillval, et);
                auto count = NewLocal(Repr::I64), i = NewLocal(Repr::I64);
                Expr(lit->fillcount, I64T());
                C().LocalSet(count);
                C().I64Const(0);
                C().LocalSet(i);
                Open(LK_PLAIN, Repr::None);
                Open(LK_PLAIN, Repr::None, nullptr, nullptr, true);
                C().LocalGet(i);
                C().LocalGet(count);
                C().Int(intop::GeU, true);
                C().BrIf(1);
                auto h = NewLocal(Repr::I64);
                C().LocalGet(stk);
                C().I32Const(esz);
                C().Call(hs.pushslot);
                C().LocalSet(h);
                Copy(Loc { false, h, 0, et, false }, first);
                C().LocalGet(i);
                C().I64Const(1);
                C().Int(intop::Add, true);
                C().LocalSet(i);
                C().Br(0);
                Close();
                Close();
                C().LocalGet(count);
                return;
            }
            case g::NK_Call: {
                auto c = static_cast<g::Call *>(n);
                if (c->builtin < 0 && (c->spec || !c->dispatch.empty()) && c->rettypes.size() == 1 &&
                    IsRz(c->rettypes[0])) {
                    CallSpec(c, nullptr, stk);
                    return;
                }
                if (c->builtin == g::B_STR) return Str(c, stk);
                if (c->builtin == g::B_DEFAULT) {
                    C().I64Const(0);
                    return;
                }
                if (c->fvbody) {
                    FvCall(c, [&](g::Node *tl) {
                        if (tl) BuildRzInto(tl, stk, et);
                        else C().Unreachable();
                    });
                    return;
                }
                break;
            }
            case g::NK_Block: return BlockRz(static_cast<g::Block *>(n), stk, et);
            case g::NK_IfExpr: {
                auto i = static_cast<g::IfExpr *>(n);
                if (!i->elseb) Unsupported(n, "an if of a value without else");
                Truth(i->cond);
                OpenIf(Repr::I64);
                BlockRz(i->thenb, stk, et);
                C().Else();
                BuildRzInto(i->elseb, stk, et);
                Close();
                return;
            }
            case g::NK_InlineBlock: {
                auto ib = static_cast<g::InlineBlock *>(n);
                Open(LK_INLINE, Repr::I64, n->exprtype, ib->sf);
                labels.back().rzstk = stk;
                labels.back().rzelem = et;
                BlockRz(ib->body, stk, et);
                Close();
                return;
            }
            case g::NK_EarlyBlock: {
                Open(LK_BREAK, Repr::I64, n->exprtype);
                labels.back().rzstk = stk;
                labels.back().rzelem = et;
                BlockRz(static_cast<g::EarlyBlock *>(n)->body, stk, et);
                Close();
                return;
            }
            default: break;
        }
        // A value this function cannot build in place (a builtin's result,
        // say) has no run to copy either.
        if (IsTemporary(n)) Unsupported(n, g::cat("a resizable value from ", KindName(n->nkind)));
        // A run of elements: copied to the top of the stack.
        auto s = SeqOf(n);
        auto count = NewLocal(Repr::I64), h = NewLocal(Repr::I64);
        s.len();
        C().LocalSet(count);
        C().LocalGet(stk);
        C().LocalGet(count);
        C().I64Const(esz);
        C().Int(intop::Mul, true);
        C().I32WrapI64();
        C().Call(hs.pushslot);
        C().LocalSet(h);
        C().LocalGet(h);
        C().LocalGet(s.data);
        C().LocalGet(count);
        C().I64Const(esz);
        C().Int(intop::Mul, true);
        C().I32WrapI64();
        C().Call(hs.copy);
        C().LocalGet(count);
    }

    // A block whose value, a resizable array, is built on stack `stk`; its
    // count is left on the Wasm stack.
    void BlockRz(g::Block *b, uint32_t stk, g::TypeExpr *et) {
        PushScope();
        for (auto st : b->stmts) Stmt(st);
        if (b->tail) BuildRzInto(b->tail, stk, et);
        else C().Unreachable();
        PopScope();
    }

    // One element at the top of stack `stk`: evaluated first, then its slot
    // claimed and written (as Push does, without a header to update).
    // `ref` receives the slot's handle.
    void PushSlot(g::Node *at, uint32_t stk, g::TypeExpr *et, g::Node *val, uint32_t *ref = nullptr) {
        auto esz = int32_t(lay.FixedSize(et));
        if (HoldsRelRef(et)) {
            // Built where it stays (§3.9): the slot first, then the value
            // built in it.
            auto h = NewLocal(Repr::I64);
            C().LocalGet(stk);
            C().I32Const(esz);
            C().Call(hs.pushslot);
            C().LocalSet(h);
            Construct(val, Loc { false, h, 0, et, false });
            if (ref) *ref = h;
            return;
        }
        uint32_t tmp = 0;
        Loc tmploc;
        bool agg = IsAggregate(et);
        if (agg) {
            tmploc = FrameSlot(et);
            Construct(val, tmploc);
        } else {
            tmp = NewLocal(ReprOf(at, et));
            Expr(val, et);
            C().LocalSet(tmp);
        }
        auto h = NewLocal(Repr::I64);
        C().LocalGet(stk);
        C().I32Const(esz);
        C().Call(hs.pushslot);
        C().LocalSet(h);
        Loc slot { false, h, 0, et, false };
        if (agg) Copy(slot, tmploc);
        else Store(at, slot, [&] { C().LocalGet(tmp); });
        if (ref) *ref = h;
    }

    // Whether a node of a resizable type is a value with no variable of its
    // own: a call's result, a literal, a control construct's value.
    static bool IsTemporary(g::Node *n) {
        switch (n->nkind) {
            case g::NK_Call: case g::NK_ArrayLit: case g::NK_Block: case g::NK_IfExpr:
            case g::NK_InlineBlock: case g::NK_EarlyBlock:
                return true;
            default:
                return false;
        }
    }

    // xs.append(src) (upstream CodeGen::EmitAppend): src's elements as a
    // run at the top of xs's stack, the length grown by their count. A
    // call's resizable result is built there directly; copy(x) appends x.
    void Append(g::Call *call) {
        auto an = call->ArgNodes();
        Loc l;
        if (LimRecv(call, l)) {
            auto src = an[1];
            if (src->nkind == g::NK_Call && static_cast<g::Call *>(src)->builtin == g::B_COPY)
                src = static_cast<g::Call *>(src)->FirstArg();
            return LimAppend(call, l, src);
        }
        auto rz = Snapshot(RzOf(an[0]));
        auto at = an[0]->exprtype;
        if (at->kind == g::TY_REF) at = at->ref->sub;
        if (!IsRz(at)) Unsupported(call, "append to this kind of array");
        auto src = an[1];
        if (src->nkind == g::NK_Call && static_cast<g::Call *>(src)->builtin == g::B_COPY)
            src = static_cast<g::Call *>(src)->FirstArg();
        auto len = rz.hdr.At(8, I64T());
        auto count = NewLocal(Repr::I64);
        BuildRzInto(src, rz.stk, at->arr->sub);
        C().LocalSet(count);
        Store(call, len, [&] {
            Load(call, len);
            C().LocalGet(count);
            C().Int(intop::Add, true);
        });
    }

    // A temporary resizable value: built on a stack claimed in the current
    // scope, which frees it when the scope ends. Returns its view.
    Seq TempRz(g::Node *n, g::TypeExpr *et) {
        uint32_t base = 0;
        auto stk = ClaimStack(base);
        Seq s;
        s.et = et;
        s.data = NewLocal(Repr::I64);
        C().LocalGet(stk);
        C().I32Const(1);
        C().I32Add();
        C().I64ExtendI32U();
        C().I64Const(32);
        C().Int(intop::Shl, true);
        C().LocalGet(base);
        C().I64ExtendI32U();
        C().Int(intop::Or, true);
        C().LocalSet(s.data);
        auto count = NewLocal(Repr::I64);
        BuildRzInto(n, stk, et);
        C().LocalSet(count);
        s.len = [this, count] { C().LocalGet(count); };
        return s;
    }

    // ---- Scopes and data stacks ----

    void PushScope() { scopes.push_back(Scope { stknext, {} }); }
    // Each saved stack's top back to where it was when its local claimed it,
    // last claimed first (upstream CodeGen::EmitRestores).
    void EmitRestores(const Scope &s) {
        for (auto i = s.saves.size(); i-- > 0;) {
            TopAddr(s.saves[i].first);
            C().LocalGet(s.saves[i].second);
            C().I32Store(mem0::Tops);
        }
    }
    void PopScope() {
        EmitRestores(scopes.back());
        stknext = scopes.back().stkbase;
        scopes.pop_back();
    }
    // The restores of every scope a branch to a label opened `to` scopes
    // deep leaves (upstream CodeGen::EmitExitRestores).
    void ExitRestores(size_t to) {
        for (auto i = scopes.size(); i-- > to;) EmitRestores(scopes[i]);
    }

    // Where stack `stk`'s entry in the tops table is, less mem0::Tops,
    // which loads and stores add as their offset.
    void TopAddr(uint32_t stk) {
        C().LocalGet(stk);
        C().I32Const(16);
        C().Op(0x6c);  // i32.mul
    }

    g::TypeExpr *I64T() { return ast.inttypes[g::IS_I64]; }

    // A frame slot of `size` bytes.
    Loc FrameBytes(int64_t size) {
        auto off = frame;
        frame += (size + 7) / 8 * 8;
        return Loc { true, 0, off, nullptr };
    }

    // var xs: T[>..] = [...]: the next stack index, whose current top is the
    // array's base and is restored when the scope ends; the header
    // { base, len } in the frame; then each element pushed.
    void DeclareRz(g::VarDecl *d, g::VarDef *v, g::Node *init) {
        uint32_t base = 0;
        RzVar rz;
        rz.stk = ClaimStack(base);
        rz.hdr = FrameBytes(16);
        InitRz(d, rz, base, v->type->arr->sub, init);
        rzvars[v] = rz;
    }

    // A resizable array's header, empty at `base` (a local) on its stack,
    // then the elements of its initializer, an array literal, pushed.
    void InitRz(g::VarDecl *d, const RzVar &rz, uint32_t base, g::TypeExpr *et, g::Node *init) {
        EmptyHeader(d, rz.hdr, rz.stk, base);
        if (!init) return;
        // Built in place at the stack's top, which is the array's base
        // (§7.3: `let x = f();` constructs in x's fresh stack region).
        Store(d, rz.hdr.At(8, I64T()), [&] { BuildRzInto(init, rz.stk, et); });
    }

    // Claims the next stack index for a resizable local, as DeclareRz does:
    // its current top is the local's base, restored when the scope ends.
    // Returns the stack's local; `base` receives the base's.
    uint32_t ClaimStack(uint32_t &base) {
        auto rel = stknext++;
        stkmax = std::max(stkmax, stknext);
        auto stk = NewLocal(Repr::I32);
        C().LocalGet(SP);
        C().I32Const(rel);
        C().I32Add();
        C().LocalSet(stk);
        base = NewLocal(Repr::I32);
        TopAddr(stk);
        C().I32Load(mem0::Tops);
        C().LocalSet(base);
        scopes.back().saves.push_back({ stk, base });
        return stk;
    }

    // Stores an empty header { base, 0 } at hdr for an array starting at
    // `base` on stack `stk` (both locals).
    void EmptyHeader(const g::Node *at, const Loc &hdr, uint32_t stk, uint32_t base) {
        Store(at, hdr.At(0, I64T()), [&] {
            C().LocalGet(stk);
            C().I32Const(1);
            C().I32Add();
            C().I64ExtendI32U();
            C().I64Const(32);
            C().Int(intop::Shl, true);
            C().LocalGet(base);
            C().I64ExtendI32U();
            C().Int(intop::Or, true);
        });
        Store(at, hdr.At(8, I64T()), [&] { C().I64Const(0); });
    }

    // var s = S { f: …, tail: [...] } of a frame object (spec Appendix C.2):
    // the object in the frame, its fixed fields built in place, then the
    // tail's header at its offset and the tail's elements pushed, on a stack
    // claimed as a resizable array's.
    void DeclareFrameObj(g::VarDecl *d, g::VarDef *v, g::Node *init) {
        uint32_t base = 0;
        auto stk = ClaimStack(base);
        auto obj = FrameBytes(FrameObjSize(v->type));
        obj.t = v->type;
        InitFrameObj(d, obj, stk, base, v->type, init);
        memvars[v] = obj;
        rzvars[v] = RzVar { obj, stk };
    }

    // A frame object at obj: its fixed fields built in place, then its
    // tail's header, empty at `base` on stack `stk`, and the tail's
    // elements pushed.
    void InitFrameObj(g::VarDecl *d, const Loc &obj, uint32_t stk, uint32_t base, g::TypeExpr *t,
                      g::Node *init) {
        if (!init || init->nkind != g::NK_StructLit) Unsupported(d, "this frame object's initializer");
        auto sl = static_cast<g::StructLit *>(init);
        auto si = Layouts::SI(t);
        auto &lo = lay.StructLayout(si);
        auto tail = int(si->ftypes.size()) - 1;
        g::Node *tailinit = nullptr;
        std::vector<bool> given(si->ftypes.size());
        for (size_t k = 0; k < sl->inits.size(); k++) {
            auto fi = sl->fieldindices[k];
            auto val = sl->inits[k].val;
            given[fi] = true;
            if (fi == tail) {
                tailinit = val;
                continue;
            }
            auto ft = si->ftypes[fi];
            auto floc = obj.At(lo.offs[fi], ft);
            if (IsRzRef(ft)) StoreFatRef(val, floc);
            else if (IsAggregate(ft)) Construct(val, floc);
            else Store(val, floc, [&] { Expr(val, ft); });
        }
        for (int fi = 0; fi < tail; fi++)
            if (!given[fi] && !si->st->fields[fi].ispad) OmittedField(d, obj.At(lo.offs[fi], si->ftypes[fi]));
        RzVar rz { obj.At(lo.offs[tail], nullptr), stk };
        EmptyHeader(d, rz.hdr, stk, base);
        if (tailinit) {
            if (tailinit->nkind != g::NK_ArrayLit) Unsupported(d, "this tail's initializer");
            auto lit = static_cast<g::ArrayLit *>(tailinit);
            if (lit->fillval || lit->capexpr) Unsupported(d, "this tail's initializer");
            for (auto e : lit->elems) Push(d, rz, si->ftypes[tail]->arr->sub, e);
        }
    }

    // xs.push(v): the element is evaluated first, then its slot is claimed
    // at the top of the array's stack (upstream: "a fixed-size element is
    // evaluated before its slot is claimed"); the length grows by one.
    // Leaves nothing on the stack; `ref` receives the new element's handle.
    void Push(g::Node *at, const RzVar &rz, g::TypeExpr *et, g::Node *val, uint32_t *ref = nullptr) {
        auto esz = int32_t(lay.FixedSize(et));
        uint32_t tmp = 0;
        Loc tmploc;
        bool agg = IsAggregate(et);
        // A value holding relative references is built where it stays
        // (§3.9): the slot first. Any other is evaluated first (upstream
        // EmitPush).
        bool inplace = agg && HoldsRelRef(et);
        if (inplace) {
        } else if (agg) {
            tmploc = FrameSlot(et);
            Construct(val, tmploc);
        } else {
            tmp = NewLocal(ReprOf(at, et));
            Expr(val, et);
            C().LocalSet(tmp);
        }
        auto h = NewLocal(Repr::I64);
        C().LocalGet(rz.stk);
        C().I32Const(esz);
        C().Call(hs.pushslot);
        C().LocalSet(h);
        Loc slot { false, h, 0, et, false };
        if (inplace) Construct(val, slot);
        else if (agg) Copy(slot, tmploc);
        else Store(at, slot, [&] { C().LocalGet(tmp); });
        auto len = rz.hdr.At(8, I64T());
        Store(at, len, [&] {
            Load(at, len);
            C().I64Const(1);
            C().Int(intop::Add, true);
        });
        if (ref) *ref = h;
    }


    // The view of a resizable array, a fixed array or a slice, through a
    // reference too. A resizable array's elements never move (§5.1) and its
    // length is read again each time; a fixed array's length is its size; a
    // slice keeps the length it had.
    Seq SeqOf(g::Node *obj, g::TypeExpr *as = nullptr) {
        auto ot = as ? as : obj->exprtype;
        if (IsPlainRef(ot)) ot = ot->ref->sub;
        Seq s;
        s.data = NewLocal(Repr::I64);
        // A string literal's bytes, in static data whatever type it is
        // given (a u8 slice, or the source of a u8[>..], §3.7).
        if (obj->nkind == g::NK_StrLit) {
            auto &str = static_cast<g::StrLit *>(obj)->val;
            s.et = ast.inttypes[g::IS_U8];
            s.mem0 = true;
            C().I64Const(StaticString(str));
            C().LocalSet(s.data);
            auto k = int64_t(str.size());
            s.len = [this, k] { C().I64Const(k); };
            return s;
        }
        if (IsRz(ot) && IsTemporary(obj)) return TempRz(obj, ot->arr->sub);
        if (IsRz(ot) || (IsRzRef(ot) && IsRz(ot->ref->sub))) {
            if (IsRzRef(ot)) ot = ot->ref->sub;
            auto rz = RzOf(obj);
            s.et = ot->arr->sub;
            Load(obj, rz.hdr.At(0, I64T()));
            C().LocalSet(s.data);
            s.len = [this, obj, rz] { Load(obj, rz.hdr.At(8, I64T())); };
            return s;
        }
        if (Layouts::IsLimited(ot)) {
            auto base = ObjectLoc(obj);
            base.t = ot;
            return LimSeq(obj, base);
        }
        if (Layouts::IsFixedArray(ot)) {
            auto base = ObjectLoc(obj);
            s.et = ot->arr->sub;
            s.mem0 = base.Direct();
            HandleOf(base);
            C().LocalSet(s.data);
            auto k = ot->arr->size;
            s.len = [this, k] { C().I64Const(k); };
            return s;
        }
        if (IsSlice(ot)) {
            auto sl = ObjectLoc(obj);
            s.et = ot->sub;  // a slice's element type (TypeExpr::sub)
            auto n = NewLocal(Repr::I64);
            auto data = s.data;
            s.reload = [this, obj, sl, data, n] {
                Load(obj, sl.At(0, I64T()));
                C().LocalSet(data);
                Load(obj, sl.At(8, I64T()));
                C().LocalSet(n);
            };
            s.reload();
            s.len = [this, n] { C().LocalGet(n); };
            return s;
        }
        Unsupported(obj, g::cat("a sequence of type ", TypeKindName(ot)));
    }

    // The current length of the array an index type is over (a global or
    // a local variable), as an i64.
    void ArrayLen(const g::Node *at, g::VarDef *arr) {
        auto t = arr->type;
        if (Layouts::IsFixedArray(t)) {
            C().I64Const(t->arr->size);
            return;
        }
        if (!IsRz(t)) Unsupported(at, "an index type over this array");
        auto it = rzvars.find(arr);
        Loc hdr;
        if (it != rzvars.end()) hdr = it->second.hdr;
        else if (arr->isglobal) hdr = GlobalSlot(arr);
        else Unsupported(at, "an index type over this array");
        Load(at, hdr.At(8, I64T()));
    }

    // The place of element i (an i64 local) of a sequence.
    Loc ElemLoc(const Seq &s, uint32_t i) {
        auto h = NewLocal(Repr::I64);
        C().LocalGet(s.data);
        C().LocalGet(i);
        C().I64Const(lay.FixedSize(s.et));
        C().Int(intop::Mul, true);
        C().Int(intop::Add, true);
        C().LocalSet(h);
        return Loc { false, h, 0, s.et, s.mem0 };
    }

    // The place of xs[i]: the index checked against the length unless BCE
    // proved it (Index::nobc), with upstream's GS_IDX: an unsigned compare,
    // so a negative index fails too.
    Loc IndexLoc(g::Index *x) {
        auto s = SeqOf(x->obj);
        auto i = NewLocal(Repr::I64);
        Expr(x->idx, I64T());
        C().LocalSet(i);
        if (!x->nobc) {
            C().LocalGet(i);
            s.len();
            C().Int(intop::GeU, true);
            C().If();
            C().LocalGet(i);
            s.len();
            Location(x);
            C().Call(rt.idxfail);
            C().End();
        }
        return ElemLoc(s, i);
    }

    // A check in a standard library function fails where the program
    // called the library (upstream SpecInfo::located): such a function takes
    // its caller's location, the address of a static { file, len, line }
    // record in memory 0, as its last parameter, reports its own lines
    // there, and passes it on to the library functions it calls.
    bool Located(g::FnSpec *sp) {
        return ast.StdlibFile(sp->sf->line.fileidx) && !sp->sf->isexport && sp->sf != ast.MainFunction();
    }
    bool AtCaller(g::Line l) { return atparam >= 0 && ast.StdlibFile(l.fileidx); }
    // What a call at l hands a callee that takes its caller's location.
    void CallerLoc(g::Line l) {
        if (AtCaller(l)) {
            C().LocalGet(uint32_t(atparam));
            return;
        }
        if (ast.StdlibFile(l.fileidx))
            throw g::CompileError { "goose-wasm: unsupported: a library call outside the library's callers" };
        auto key = std::make_pair(l.fileidx, l.line);
        auto it = locrecs.find(key);
        if (it == locrecs.end()) {
            auto file = FileName(l);
            auto fa = StaticString(file);
            while (staticdata.size() % 4) staticdata.push_back('\0');
            auto addr = uint32_t(mem0::StaticData + staticdata.size());
            for (uint32_t w : { fa, uint32_t(file.size()), uint32_t(l.line) })
                for (int b = 0; b < 4; b++) staticdata.push_back(char((w >> (8 * b)) & 0xff));
            it = locrecs.emplace(key, addr).first;
        }
        C().I32Const(int32_t(it->second));
    }

    // A runtime check's location arguments: the file's name and length in
    // static data, and the line. A check in a standard library function
    // reports where the program called it, through a location parameter
    // (upstream CodeGen::AtCaller, SpecInfo::located); inlined library code
    // is anchored at the call already.
    void Location(const g::Node *at) {
        if (AtCaller(at->line)) {
            for (int k = 0; k < 3; k++) {
                C().LocalGet(uint32_t(atparam));
                C().I32Load(uint32_t(4 * k));
            }
            return;
        }
        if (ast.StdlibFile(at->line.fileidx))
            Unsupported(at, "a check in a standard library function");
        auto file = FileName(at->line);
        C().I32Const(int32_t(StaticString(file)));
        C().I32Const(int32_t(file.size()));
        C().I32Const(at->line.line);
    }

    // ---- Statements ----

    void Stmt(g::Node *n) {
        switch (n->nkind) {
            case g::NK_VarDecl: return VarDecl(static_cast<g::VarDecl *>(n));
            case g::NK_Assign: return Assign(static_cast<g::Assign *>(n));
            case g::NK_IncDec: return IncDec(static_cast<g::IncDec *>(n));
            case g::NK_While: return While(static_cast<g::While *>(n));
            case g::NK_ForLoop: return ForLoop(static_cast<g::ForLoop *>(n));
            case g::NK_Break: return Break(static_cast<g::Break *>(n));
            case g::NK_Continue: {
                auto d = Depth(LK_CONTINUE);
                if (d < 0) Unsupported(n, "continue outside a loop");
                ExitRestores(labels[labels.size() - 1 - d].scopes);
                C().Br(uint32_t(d));
                return;
            }
            case g::NK_Return: return Return(static_cast<g::Return *>(n));
            // A nested declaration runs nothing where it stands: its
            // specializations are emitted as functions of their own.
            case g::NK_FnDecl: case g::NK_StructDecl: case g::NK_EnumDecl: case g::NK_AliasDecl:
                return;
            default: {
                // A call for its effects drops every result it has.
                if (n->nkind == g::NK_Call) {
                    auto call = static_cast<g::Call *>(n);
                    if (call->builtin == g::B_PUSH) {
                        auto an = call->ArgNodes();
                        Loc l;
                        if (LimRecv(call, l)) return LimPush(call, l, an[1], nullptr);
                        auto rz = Snapshot(RzOf(an[0]));
                        auto at = an[0]->exprtype;
                        if (at->kind == g::TY_REF) at = at->ref->sub;
                        Push(call, rz, at->arr->sub, an[1]);
                        return;
                    }
                    if (call->builtin == g::B_APPEND) return Append(call);
                    switch (call->builtin) {
                        case g::B_CLEAR: return Clear(call);
                        case g::B_POP: Pop(call); return;
                        case g::B_POP_N: return PopN(call);
                        case g::B_RESIZE: return Resize(call);
                        case g::B_ASSERT: return Assert(call);
                        case g::B_ABORT: return AbortCall(call);
                        case g::B_FORMAT: return Format(call);
                        default: break;
                    }
                    if (call->fvbody) {
                        FvCall(call, [&](g::Node *tl) {
                            if (tl) Expr(tl, nullptr);
                        });
                        return;
                    }
                    if (call->builtin < 0 && (call->spec || !call->dispatch.empty()) &&
                        call->rettypes.size() > 1) {
                        CallSpec(call);
                        for (size_t i = 0; i < call->rettypes.size(); i++) C().Drop();
                        return;
                    }
                }
                // A resizable value computed for its effects is built on a
                // stack of the statement's, freed after it.
                if (IsRz(n->exprtype)) {
                    PushScope();
                    TempRz(n, n->exprtype->arr->sub);
                    PopScope();
                    return;
                }
                // An aggregate computed for its effects is built and dropped.
                if (IsAggregate(n->exprtype)) {
                    Construct(n, FrameSlot(n->exprtype));
                    return;
                }
                auto r = ReprOf(n);
                Expr(n, n->exprtype);
                if (r != Repr::None) C().Drop();
            }
        }
    }

    void VarDecl(g::VarDecl *d) {
        if (d->isglobal || d->reusable) Unsupported(d, "this kind of declaration");
        // `let a, b = f();`, and the synthetic `result#1, result#2` the
        // parser declares for the statement `a, b = f();` (results).
        if (d->results || (d->defs.size() > 1 && d->inits.size() == 1)) return VarDeclResults(d);
        for (size_t i = 0; i < d->defs.size(); i++)
            DeclareVar(d, d->defs[i], i < d->inits.size() ? d->inits[i] : nullptr);
    }

    // One variable of a declaration, with its initializer if it has one.
    void DeclareVar(g::VarDecl *d, g::VarDef *v, g::Node *init) {
        if (IsRoot(v->type)) return DeclareRoot(d, v, init);
        if (IsRz(v->type)) return DeclareRz(d, v, init);
        if (IsFrameObj(v->type)) return DeclareFrameObj(d, v, init);
        if (IsRzRef(v->type)) {
            // A reference to a resizable value: an inlined call's
            // parameter, or an un-annotated let or var of a resizable
            // lvalue, which binds by reference (§4.1). Its header handle
            // and stack live in locals, which `.=` rebinds.
            if (!init) Unsupported(d, "this reference declaration");
            auto rz = RzOf(init);
            auto h = NewLocal(Repr::I64), stk = NewLocal(Repr::I32);
            Handle(rz.hdr);
            C().LocalSet(h);
            C().LocalGet(rz.stk);
            C().LocalSet(stk);
            rzvars[v] = RzVar { Loc { false, h, 0, nullptr, false }, stk };
            // A frame object's fixed fields are reached through it too.
            if (IsFrameObj(v->type->ref->sub))
                memvars[v] = Loc { false, h, 0, v->type->ref->sub, false };
            return;
        }
        if (d->byref && !IsPlainRef(v->type)) Unsupported(d, "this binding by reference");
        if (IsAggregate(v->type)) {
            // Built in its own slot, which its initializer cannot read.
            if (!init) Unsupported(d, "an aggregate without an initializer");
            auto slot = FrameSlot(v->type);
            Construct(init, slot);
            memvars[v] = slot;
            return;
        }
        if (addrtaken.count(v)) {
            // A reference is taken to it: it lives in the frame.
            auto slot = FrameSlot(v->type);
            Store(d, slot, [&] {
                if (init) Expr(init, v->type);
                else C().IntConst(ReprOf(d, v->type) == Repr::I64, 0);
            });
            memvars[v] = slot;
            return;
        }
        auto local = NewLocal(ReprOf(d, v->type));
        if (init) {
            Expr(init, v->type);
        } else {
            // A declaration without an initializer holds its type's
            // default, zero, each time it runs.
            C().IntConst(ReprOf(d, v->type) == Repr::I64, 0);
        }
        C().LocalSet(local);
        locals[v] = local;
    }

    // One call's results, each into its variable. The last result is on top
    // of the stack, so they are stored last first.
    void VarDeclResults(g::VarDecl *d) {
        if (d->inits.size() != 1 || d->inits[0]->nkind != g::NK_Call)
            Unsupported(d, "this declaration of several results");
        auto call = static_cast<g::Call *>(d->inits[0]);
        if (call->builtin >= 0 || !call->spec || call->rettypes.size() != d->defs.size())
            Unsupported(d, "this declaration of several results");
        std::vector<uint32_t> ls;
        for (auto v : d->defs) {
            ls.push_back(NewLocal(ReprOf(d, v->type)));
            locals[v] = ls.back();
        }
        CallSpec(call);
        for (size_t i = d->defs.size(); i-- > 0;) {
            Convert(d, call->rettypes[i], d->defs[i]->type);
            C().LocalSet(ls[i]);
        }
    }

    // The local an lvalue names; only plain local variables so far.
    uint32_t LvalLocal(g::Node *lv) {
        if (lv->nkind != g::NK_Ident) Unsupported(lv, g::cat("assignment to ", KindName(lv->nkind)));
        auto v = static_cast<g::Ident *>(lv)->vdef;
        if (!v || v->isglobal) Unsupported(lv, "assignment to a global");
        return LocalOf(lv, v);
    }

    static g::TType OpOfCompound(g::TType op) {
        switch (op) {
            case g::T_PLUSEQ: return g::T_PLUS;
            case g::T_MINUSEQ: return g::T_MINUS;
            case g::T_MULEQ: return g::T_MUL;
            case g::T_DIVEQ: return g::T_DIV;
            case g::T_MODEQ: return g::T_MOD;
            case g::T_ANDEQ: return g::T_BITAND;
            case g::T_OREQ: return g::T_BITOR;
            case g::T_XOREQ: return g::T_XOR;
            case g::T_SHLEQ: return g::T_SHL;
            case g::T_SHREQ: return g::T_SHR;
            default: return g::T_ASSIGN;
        }
    }

    // r .= &x: the reference itself moves, in a variable, field or element
    // (§3.8). On a location that holds no reference, `.=` is `=` (generic
    // code).
    bool Rebind(g::Assign *a) {
        auto target = [&] {
            if (a->rhs->nkind == g::NK_NullLit) C().I64Const(0);
            else AddressOf(a->rhs);
        };
        if (a->lval->nkind == g::NK_Ident) {
            auto v = static_cast<g::Ident *>(a->lval)->vdef;
            // A reference to a resizable value: both of its locals. One a
            // nested function received is a copy, which a rebinding would
            // not reach the enclosing function through.
            if (v && IsRzRef(v->type) && spec && v->ownerspec != spec)
                Unsupported(a, "rebinding an enclosing function's reference");
            if (v && IsRzRef(v->type) && rzvars.count(v)) {
                auto rz = RzOf(a->rhs);
                auto &mine = rzvars[v];
                Handle(rz.hdr);
                C().LocalSet(mine.hdr.h);
                C().LocalGet(rz.stk);
                C().LocalSet(mine.stk);
                return true;
            }
            if (!v || !IsPlainRef(v->type)) return false;
            auto it = memvars.find(v);
            if (it != memvars.end()) {
                Store(a, it->second, target);
                return true;
            }
            target();
            C().LocalSet(LocalOf(a, v));
            return true;
        }
        Loc l;
        if (!MemLoc(a->lval, l)) Unsupported(a, "this rebinding");
        if (!IsPlainRef(l.t)) return false;
        Store(a, l, target);
        return true;
    }

    void Assign(g::Assign *a) {
        if (a->op == g::T_DOTASSIGN && Rebind(a)) return;
        // Through a reference, the operation is at the pointee's type.
        auto t = a->lval->exprtype;
        if (IsPlainRef(t) || IsRzRef(t)) t = t->ref->sub;
        // The value an assignment stores, the old one on the stack for a
        // compound operator.
        auto value = [&](auto load) {
            if (a->op == g::T_ASSIGN) {
                Expr(a->rhs, t);
                return;
            }
            auto op = OpOfCompound(a->op);
            if (op == g::T_ASSIGN && a->op != g::T_DOTASSIGN) Unsupported(a, "this assignment operator");
            if (a->op == g::T_DOTASSIGN) {
                Expr(a->rhs, t);
                return;
            }
            load();
            Arith(a, op, t, a->rhs);
        };
        if (IsRz(t) && a->op == g::T_ASSIGN) {
            // x = e: built on x's stack from x's base, replacing its
            // contents (§7.3), the header's count set to what was built.
            auto rz = RzOf(a->lval);
            TopAddr(rz.stk);
            Load(a, rz.hdr.At(0, I64T()));
            C().I32WrapI64();
            C().I32Store(mem0::Tops);
            Store(a, rz.hdr.At(8, I64T()), [&] { BuildRzInto(a->rhs, rz.stk, t->arr->sub); });
            return;
        }
        Loc l;
        if (MemLoc(a->lval, l)) {
            // A write through a reference a field or element holds writes
            // its pointee (Assign::pointee, §3.8).
            if (IsPlainRef(l.t) && !IsPlainRef(t)) l = Deref(a, l);
            if (IsAggregate(t)) {
                if (a->op != g::T_ASSIGN && a->op != g::T_DOTASSIGN)
                    Unsupported(a, "this assignment operator on an aggregate");
                // Built aside, then copied: the new value may read the old
                // one (p = P { x: p.y, y: p.x }).
                auto tmp = FrameSlot(t);
                Construct(a->rhs, tmp);
                Copy(l, tmp);
                return;
            }
            Store(a, l, [&] { value([&] { Load(a, l); }); });
            return;
        }
        auto local = LvalLocal(a->lval);
        value([&] { C().LocalGet(local); });
        C().LocalSet(local);
    }

    void IncDec(g::IncDec *n) {
        auto t = n->lval->exprtype;
        if (IsPlainRef(t)) t = t->ref->sub;
        if (!IsInt(t)) Unsupported(n, "++ or -- of this type");
        auto wide = IsWide(t);
        auto step = [&] {
            C().IntConst(wide, 1);
            C().Int(n->op == g::T_INC ? intop::Add : intop::Sub, wide);
            if (!wide) Normalize(t);
        };
        Loc l;
        if (MemLoc(n->lval, l)) {
            if (IsPlainRef(l.t)) l = Deref(n, l);
            Store(n, l, [&] {
                Load(n, l);
                step();
            });
            return;
        }
        auto local = LvalLocal(n->lval);
        C().LocalGet(local);
        step();
        C().LocalSet(local);
    }

    // while c { body }: the condition is tested before every iteration;
    // continue goes back to it.
    void While(g::While *w) {
        Open(LK_BREAK, Repr::None);
        Open(LK_CONTINUE, Repr::None, nullptr, nullptr, true);
        Truth(w->cond);
        C().I32Eqz();
        C().BrIf(1);
        Stmt(w->body);
        C().Br(0);
        Close();
        Close();
    }

    // for x in xs over an array or slice (spec §6.5): a resizable array's
    // current length is tested before every iteration, so elements the body
    // pushes are visited too; a slice keeps its length. x is a copy of a
    // fixed-size element, or with `&x` (or a non-fixed element) a reference
    // to it; the index is an i64.
    void ForArray(g::ForLoop *f) {
        auto s = SeqOf(f->iter);
        auto i = NewLocal(Repr::I64);
        C().I64Const(0);
        C().LocalSet(i);
        // The binding lives in the loop's own place; each iteration fills it.
        bool byref = f->byref || !f->vdef->copybind;
        if (byref && !IsPlainRef(f->vdef->type)) Unsupported(f, "for over these elements by reference");
        bool agg = !byref && IsAggregate(s.et);
        Loc xslot;
        uint32_t xlocal = 0;
        if (agg) {
            xslot = FrameSlot(s.et);
            memvars[f->vdef] = xslot;
        } else {
            xlocal = NewLocal(byref ? Repr::I64 : ReprOf(f, s.et));
            locals[f->vdef] = xlocal;
        }
        // An index binder of a narrower type (§6.5) holds every index the
        // array can have: a copy of the counter at its own width.
        uint32_t ix = 0;
        if (f->idxdef) {
            if (ReprOf(f, f->idxdef->type) == Repr::I64) {
                locals[f->idxdef] = i;
            } else {
                ix = NewLocal(Repr::I32);
                locals[f->idxdef] = ix;
            }
        }
        Open(LK_BREAK, Repr::None);
        Open(LK_PLAIN, Repr::None, nullptr, nullptr, true);
        // The slot iterated is read again each time (upstream re-reads it
        // where the body can store into it, which gives the same values).
        if (s.reload) s.reload();
        C().LocalGet(i);
        s.len();
        C().Int(intop::GeU, true);
        C().BrIf(1);
        if (ix) {
            C().LocalGet(i);
            C().I32WrapI64();
            C().LocalSet(ix);
        }
        auto elem = ElemLoc(s, i);
        if (byref) {
            Handle(elem);
            C().LocalSet(xlocal);
        } else if (agg) {
            Copy(xslot, elem);
        } else {
            Load(f, elem);
            C().LocalSet(xlocal);
        }
        Open(LK_CONTINUE, Repr::None);
        Stmt(f->body);
        Close();
        C().LocalGet(i);
        C().I64Const(1);
        C().Int(intop::Add, true);
        C().LocalSet(i);
        C().Br(0);
        Close();
        Close();
    }

    // for over a range or a count (spec §6.5): the bounds are evaluated
    // once, and the counter runs in 64 bits so that an end one past a
    // narrow type's largest value (`for i: u8 in 0..256`) never wraps it.
    void ForLoop(g::ForLoop *f) {
        if (f->iterkind == g::IK_ARRAY || f->iterkind == g::IK_SLICE) return ForArray(f);
        if (f->iterkind != g::IK_RANGE && f->iterkind != g::IK_COUNT)
            Unsupported(f, "for over a slice");
        if (f->byref) Unsupported(f, "for &x");
        auto vt = f->vdef->type;
        if (!IsInt(vt)) Unsupported(f, "a for variable of this type");
        g::Node *lo = nullptr, *hi = f->iter;
        if (f->iterkind == g::IK_RANGE) {
            auto r = static_cast<g::RangeExpr *>(f->iter);
            lo = r->lo;
            hi = r->hi;
        }
        // The counter and end as i64s: a signed type's values sign-extend,
        // an unsigned one's zero-extend, so one comparison serves all
        // types narrower than 64 bits; u64 compares unsigned.
        auto ctr = NewLocal(Repr::I64), end = NewLocal(Repr::I64);
        auto bound = [&](g::Node *b) {
            auto bt = IsInt(b->exprtype) ? b->exprtype : vt;
            Expr(b, bt);
            if (ReprOf(b, bt) == Repr::I32) {
                if (IsSigned(bt)) C().I64ExtendI32S();
                else C().I64ExtendI32U();
            }
        };
        if (lo) bound(lo);
        else C().I64Const(0);
        C().LocalSet(ctr);
        bound(hi);
        C().LocalSet(end);
        auto var = NewLocal(ReprOf(f, vt));
        locals[f->vdef] = var;
        uint32_t idx = 0;
        if (f->idxdef) {
            idx = NewLocal(Repr::I64);
            locals[f->idxdef] = idx;
            C().I64Const(0);
            C().LocalSet(idx);
        }
        Open(LK_BREAK, Repr::None);
        Open(LK_PLAIN, Repr::None, nullptr, nullptr, true);
        C().LocalGet(ctr);
        C().LocalGet(end);
        C().Int(vt->intstorage == g::IS_U64 ? intop::GeU : intop::GeS, true);
        C().BrIf(1);
        C().LocalGet(ctr);
        if (!IsWide(vt)) C().I32WrapI64();
        C().LocalSet(var);
        Open(LK_CONTINUE, Repr::None);
        Stmt(f->body);
        Close();
        C().LocalGet(ctr);
        C().I64Const(1);
        C().Int(intop::Add, true);
        C().LocalSet(ctr);
        if (f->idxdef) {
            C().LocalGet(idx);
            C().I64Const(1);
            C().Int(intop::Add, true);
            C().LocalSet(idx);
        }
        C().Br(0);
        Close();
        Close();
    }

    // break e?: leaves the innermost loop, while, for or block (spec §6.4),
    // with its value.
    void Break(g::Break *b) {
        auto d = Depth(LK_BREAK);
        if (d < 0) Unsupported(b, "break outside a loop or block");
        auto l = labels[labels.size() - 1 - d];
        if (b->val) {
            if (l.rzstk >= 0) BuildRzInto(b->val, uint32_t(l.rzstk), l.rzelem);
            else if (l.dst) Construct(b->val, *l.dst);
            else Expr(b->val, l.type);
        }
        ExitRestores(l.scopes);
        C().Br(uint32_t(Depth(LK_BREAK)));
    }

    // return: out of the inlined body of the function it returns from, or
    // out of the function itself.
    void Return(g::Return *r) {
        auto d = Depth(LK_INLINE, r->target);
        if (d >= 0) {
            if (r->vals.size() > 1) Unsupported(r, "a return of several values from an inlined body");
            auto l = labels[labels.size() - 1 - d];
            if (!r->vals.empty()) {
                if (l.rzstk >= 0) BuildRzInto(r->vals[0], uint32_t(l.rzstk), l.rzelem);
                else if (l.dst) Construct(r->vals[0], *l.dst);
                else Expr(r->vals[0], l.type);
            }
            ExitRestores(l.scopes);
            C().Br(uint32_t(Depth(LK_INLINE, r->target)));
            return;
        }
        if (r->target != spec->sf) Unsupported(r, "return from an enclosing function");
        if (r->vals.size() > spec->rets.size()) Unsupported(r, "a return of more values");
        auto out = uint32_t(Depth(LK_FUNC));
        if (retrz >= 0) {
            BuildRzInto(r->vals[0], uint32_t(retrz), spec->rets[0]->arr->sub);
            ExitRestores(0);
            C().Br(out);
            return;
        }
        if (retdst >= 0) {
            Construct(r->vals[0], Loc { false, uint32_t(retdst), 0, spec->rets[0], false });
            ExitRestores(0);
            C().Br(out);
            return;
        }
        if (r->vals.size() == 1 && spec->rets.size() > 1) {
            // return f(); forwarding a call of several results.
            auto call = r->vals[0]->nkind == g::NK_Call ? static_cast<g::Call *>(r->vals[0]) : nullptr;
            if (!call || call->builtin >= 0 || !call->spec ||
                call->rettypes.size() != spec->rets.size())
                Unsupported(r, "this return of several values");
            for (size_t i = 0; i < spec->rets.size(); i++)
                if (!SameScalar(call->rettypes[i], spec->rets[i]))
                    Unsupported(r, "a forwarded result of another type");
            CallSpec(call);
        } else {
            for (size_t i = 0; i < r->vals.size(); i++) Expr(r->vals[i], spec->rets[i]);
        }
        ExitRestores(0);
        C().Br(out);
    }

    // ---- Expressions ----

    // Emits n's value converted to type `to` (nothing when `to` is void,
    // after running n for its effects).
    void Expr(g::Node *n, g::TypeExpr *to) {
        auto t = n->exprtype;
        // A reference-typed destination binds an lvalue of the pointee type
        // by reference, without a `&` (spec §3.8, §4.1).
        if (IsPlainRef(to) && n->nkind == g::NK_NullLit) {
            C().I64Const(0);
            return;
        }
        if (IsPlainRef(to) && !IsPlainRef(t) && !(t && t->kind == g::TY_VOID)) {
            AddressOf(n);
            return;
        }
        Value(n);
        if (IsVoid(to)) {
            if (!IsVoid(t)) C().Drop();
            return;
        }
        // A void value where one is wanted: control left it (every arm
        // returned), which Wasm's validation cannot see past an if.
        if (IsVoid(t)) {
            C().Unreachable();
            return;
        }
        Convert(n, t, to);
    }

    // Emits n's value at its own type.
    void Value(g::Node *n) {
        switch (n->nkind) {
            case g::NK_FltLit: {
                auto lit = static_cast<g::FltLit *>(n);
                if (!IsFlt(n->exprtype)) Unsupported(n, "a float literal of this type");
                if (n->exprtype->fltstorage == g::FS_F32) C().F32Const(float(lit->val));
                else C().F64Const(lit->val);
                return;
            }
            case g::NK_IntLit: {
                auto lit = static_cast<g::IntLit *>(n);
                // An integer literal where a float is wanted is that float.
                if (IsFlt(n->exprtype)) {
                    double v = lit->uns ? double(uint64_t(lit->val)) : double(lit->val);
                    if (n->exprtype->fltstorage == g::FS_F32) C().F32Const(float(v));
                    else C().F64Const(v);
                    return;
                }
                if (!IsInt(n->exprtype)) Unsupported(n, "a literal of this type");
                C().IntConst(IsWide(n->exprtype), lit->val);
                if (!IsWide(n->exprtype)) Normalize(n->exprtype);
                return;
            }
            case g::NK_BoolLit:
                C().I32Const(static_cast<g::BoolLit *>(n)->val);
                return;
            case g::NK_Ident: {
                // The checker gives a use the type it widens to implicitly
                // (n += i of a u8 i into an i64 n), not a cast node: the
                // variable is read at its own type and converted.
                auto id = static_cast<g::Ident *>(n);
                auto v = id->vdef;
                if (!v) Unsupported(n, "a name that is not a variable");
                // A reference variable used as its pointee reads through it
                // (references are transparent, §3.8); used as a reference, it
                // is its handle.
                if (IsPlainRef(v->type) && !IsPlainRef(n->exprtype)) {
                    Loc l;
                    if (!MemLoc(n, l)) Unsupported(n, "this reference");
                    Load(n, l);
                    Convert(n, l.t, n->exprtype);
                    return;
                }
                VarValue(n, v);
                Convert(n, v->type, n->exprtype);
                return;
            }
            case g::NK_NullLit:
                C().I64Const(0);
                return;
            case g::NK_Index:
            case g::NK_Dot: {
                if (n->nkind == g::NK_Dot) {
                    auto d = static_cast<g::Dot *>(n);
                    if (d->member == g::B_LEN) {
                        auto s = SeqOf(d->obj);
                        s.len();
                        Convert(n, I64T(), n->exprtype);
                        return;
                    }
                    if (d->member == g::B_CAP) {
                        // A limited array's static capacity.
                        auto ot = OperandType(d->obj->exprtype);
                        if (!Layouts::IsLimited(ot)) Unsupported(n, "property .cap of this array");
                        C().I64Const(ot->arr->size);
                        Convert(n, I64T(), n->exprtype);
                        return;
                    }
                    if (!d->IsField()) Unsupported(n, g::cat("property .", d->name));
                }
                Loc l;
                if (!MemLoc(n, l)) Unsupported(n, "this access");
                // A reference stored there, used as its pointee, reads
                // through it (§3.8).
                if (IsPlainRef(l.t) && !IsPlainRef(n->exprtype)) l = Deref(n, l);
                Load(n, l);
                Convert(n, l.t, n->exprtype);
                return;
            }
            case g::NK_Unary: return Unary(static_cast<g::Unary *>(n));
            case g::NK_Binary: return Binary(static_cast<g::Binary *>(n));
            case g::NK_AsCast: {
                auto c = static_cast<g::AsCast *>(n);
                auto tt = c->totype ? c->totype : n->exprtype;
                // Into an index or bound type the value is checked against
                // its array first, in every build (§3.11; upstream
                // IndexTypeCheck), unless BCE proved it.
                if (tt && tt->kind == g::TY_INT && tt->ix && !c->nobc) {
                    auto st = c->child->exprtype;
                    if (!IsInt(st)) Unsupported(n, "an index type conversion from this type");
                    auto x = NewLocal(Repr::I64);
                    Expr(c->child, st);
                    Convert(n, st, I64T());
                    C().LocalSet(x);
                    auto len = [&] { ArrayLen(n, tt->ix->arr); };
                    C().LocalGet(x);
                    len();
                    C().Int(tt->ix->upto ? intop::GtU : intop::GeU, true);
                    C().If();
                    C().LocalGet(x);
                    len();
                    Location(n);
                    C().Call(rt.idxfail);
                    C().End();
                    C().LocalGet(x);
                    Convert(n, I64T(), n->exprtype);
                    return;
                }
                // Release semantics: `as` and `as!` both truncate or wrap
                // (spec §6.3); the debug build's range check is not emitted.
                // The conversion is to the cast's own type; the node's may be
                // a wider slot it lands in (`let d: f64 = x as f32` rounds to
                // f32 first).
                Expr(c->child, c->child->exprtype);
                Convert(n, c->child->exprtype, tt);
                Convert(n, tt, n->exprtype);
                return;
            }
            case g::NK_Block: return BlockValue(static_cast<g::Block *>(n), n->exprtype);
            case g::NK_IfExpr: return If(static_cast<g::IfExpr *>(n));
            case g::NK_MatchExpr: {
                auto t = n->exprtype;
                Match(static_cast<g::MatchExpr *>(n), ReprOf(n), [&](g::Node *b) {
                    if (b->nkind == g::NK_Block) BlockValue(static_cast<g::Block *>(b), t);
                    else Expr(b, t);
                });
                return;
            }
            case g::NK_EarlyBlock: {
                auto e = static_cast<g::EarlyBlock *>(n);
                auto r = ReprOf(n);
                Open(LK_BREAK, r, n->exprtype);
                BlockValue(e->body, n->exprtype);
                Close();
                return;
            }
            case g::NK_LoopExpr: {
                auto l = static_cast<g::LoopExpr *>(n);
                auto r = ReprOf(n);
                Open(LK_BREAK, r, n->exprtype);
                Open(LK_CONTINUE, Repr::None, nullptr, nullptr, true);
                Stmt(l->body);
                C().Br(0);
                Close();
                if (r != Repr::None) C().Unreachable();
                Close();
                return;
            }
            case g::NK_InlineBlock: {
                auto ib = static_cast<g::InlineBlock *>(n);
                // The body gives the callee's own result type (upstream
                // InlineBlock::EmitBody): a returned reference used as its
                // pointee is read through, as a call's is.
                auto rt = ib->spec && ib->spec->rets.size() == 1 ? ib->spec->rets[0] : nullptr;
                if (IsPlainRef(rt) && !IsPlainRef(n->exprtype) && !IsVoid(n->exprtype)) {
                    Open(LK_INLINE, Repr::I64, rt, ib->sf);
                    BlockValue(ib->body, rt);
                    Close();
                    auto h = NewLocal(Repr::I64);
                    C().LocalSet(h);
                    Loc l { false, h, 0, rt->ref->sub, false };
                    Load(n, l);
                    Convert(n, l.t, n->exprtype);
                    return;
                }
                auto r = ReprOf(n);
                Open(LK_INLINE, r, n->exprtype, ib->sf);
                BlockValue(ib->body, n->exprtype);
                Close();
                return;
            }
            case g::NK_Call: {
                auto call = static_cast<g::Call *>(n);
                if (call->builtin == g::B_PRINT) return Print(call);
                if (call->builtin == g::B_INDEX_OF || call->builtin == g::B_RANGE_INDEX) {
                    if (call->builtin == g::B_INDEX_OF) IndexOf(call);
                    else RangeIndex(call);
                    Convert(n, I64T(), n->exprtype);
                    return;
                }
                if (call->builtin == g::B_CHECK_LENGTH) {
                    CheckLength(call);
                    Convert(n, I64T(), n->exprtype);
                    return;
                }
                if (call->builtin == g::B_DEFAULT) {
                    // A scalar's default: zero, false, null.
                    auto rt = call->rettypes[0];
                    switch (ReprOf(n, rt)) {
                        case Repr::I64: C().I64Const(0); break;
                        case Repr::F32: C().F32Const(0); break;
                        case Repr::F64: C().F64Const(0); break;
                        default: C().I32Const(0); break;
                    }
                    Convert(n, rt, n->exprtype);
                    return;
                }
                if (call->builtin == g::B_POP) {
                    // The element as its slot holds it: a relative
                    // reference reads as the plain one it encodes.
                    auto at = call->ArgNodes()[0]->exprtype;
                    if (at->kind == g::TY_REF) at = at->ref->sub;
                    auto h = Pop(call);
                    Loc l { false, h, 0, at->arr->sub, false };
                    Load(n, l);
                    Convert(n, l.t, n->exprtype);
                    return;
                }
                if (call->builtin == g::B_PUSH) {
                    // Its value is a reference to the new element.
                    auto an = call->ArgNodes();
                    auto at = an[0]->exprtype;
                    if (at->kind == g::TY_REF) at = at->ref->sub;
                    uint32_t ref = 0;
                    Loc l;
                    if (LimRecv(call, l)) LimPush(call, l, an[1], &ref);
                    else Push(call, Snapshot(RzOf(an[0])), at->arr->sub, an[1], &ref);
                    // Used as the element, it reads through the reference.
                    if (IsPlainRef(n->exprtype)) {
                        C().LocalGet(ref);
                        return;
                    }
                    Loc el { false, ref, 0, at->arr->sub, false };
                    Load(n, el);
                    Convert(n, el.t, n->exprtype);
                    return;
                }
                if (call->builtin >= 0)
                    Unsupported(n, g::cat("builtin ", g::builtindefs[call->builtin].name));
                if (call->fvbody) {
                    auto t = n->exprtype;
                    FvCall(call, [&](g::Node *tl) {
                        if (tl) Expr(tl, t);
                        else if (!IsVoid(t)) C().Unreachable();
                    });
                    return;
                }
                if (!call->spec && call->dispatch.empty()) Unsupported(n, "call");
                CallSpec(call);
                // A call of several results used as a value is its first.
                for (size_t i = 1; i < call->rettypes.size(); i++) C().Drop();
                if (call->rettypes.empty()) return;
                // A returned reference used as its pointee reads through it
                // (a `let` of it copies a fixed pointee, §3.8).
                if (IsPlainRef(call->rettypes[0]) && !IsPlainRef(n->exprtype)) {
                    auto h = NewLocal(Repr::I64);
                    C().LocalSet(h);
                    Loc l { false, h, 0, call->rettypes[0]->ref->sub, false };
                    Load(n, l);
                    Convert(n, l.t, n->exprtype);
                    return;
                }
                Convert(n, call->rettypes[0], n->exprtype);
                return;
            }
            case g::NK_VarDecl: case g::NK_Assign: case g::NK_IncDec: case g::NK_While:
            case g::NK_ForLoop: case g::NK_Break: case g::NK_Continue: case g::NK_Return:
                return Stmt(n);
            default:
                Unsupported(n, KindName(n->nkind));
        }
    }

    // A block's statements, then its tail as a value of type t. A block of
    // a value whose end control never reaches (it ends in a return or
    // break) ends in `unreachable`, which is what Wasm's validation asks of
    // a block whose type promises a value there.
    void BlockValue(g::Block *b, g::TypeExpr *t) {
        PushScope();
        for (auto s : b->stmts) Stmt(s);
        if (b->tail) Expr(b->tail, t);
        else if (!IsVoid(t)) C().Unreachable();
        PopScope();
    }

    void If(g::IfExpr *i) {
        auto r = ReprOf(i);
        Truth(i->cond);
        OpenIf(r);
        BlockValue(i->thenb, i->exprtype);
        if (i->elseb) {
            C().Else();
            if (i->elseb->nkind == g::NK_Block) BlockValue(static_cast<g::Block *>(i->elseb), i->exprtype);
            else Expr(i->elseb, i->exprtype);
        } else if (r != Repr::None) {
            Unsupported(i, "an if of a value without else");
        }
        Close();
    }

    // A condition's truth, an i32: a bool's value, or for an optional
    // reference whether it is not null (`if n`, `while n`, §3.8).
    void Truth(g::Node *n) {
        Expr(n, n->exprtype);
        if (IsPlainRef(n->exprtype)) {
            C().I64Const(0);
            C().Int(intop::Ne, true);
        }
    }

    void Unary(g::Unary *u) {
        auto t = u->exprtype;
        switch (u->op) {
            case g::T_BITAND:
                AddressOf(u);
                return;
            case g::T_NOT:
                Truth(u->child);
                C().I32Eqz();
                return;
            case g::T_MINUS: {
                // At the operand's type, then into the node's (as Binary).
                auto ot = OperandType(u->child->exprtype);
                if (IsFlt(ot)) {
                    Expr(u->child, ot);
                    C().Op(ot->fltstorage == g::FS_F32 ? 0x8c : 0x9a);  // fN.neg
                    Convert(u, ot, t);
                    return;
                }
                if (!IsInt(ot)) Unsupported(u, "negation of this type");
                auto wide = IsWide(ot);
                C().IntConst(wide, 0);
                Expr(u->child, ot);
                C().Int(intop::Sub, wide);
                if (!wide) Normalize(ot);
                Convert(u, ot, t);
                return;
            }
            case g::T_BITNOT: {
                auto ot = OperandType(u->child->exprtype);
                if (!IsInt(ot)) Unsupported(u, "~ of this type");
                auto wide = IsWide(ot);
                Expr(u->child, ot);
                C().IntConst(wide, -1);
                C().Int(intop::Xor, wide);
                if (!wide) Normalize(ot);
                Convert(u, ot, t);
                return;
            }
            default:
                Unsupported(u, "this unary operator");
        }
    }

    static g::TypeExpr *OperandType(g::TypeExpr *t) { return IsPlainRef(t) ? t->ref->sub : t; }

    void Binary(g::Binary *b) {
        auto lt = b->left->exprtype, rt_ = b->right->exprtype;
        switch (b->op) {
            case g::T_ANDAND:
            case g::T_OROR: {
                // Short-circuit: the right operand runs only when it decides.
                Truth(b->left);
                OpenIf(Repr::I32);
                if (b->op == g::T_ANDAND) {
                    Truth(b->right);
                    C().Else();
                    C().I32Const(0);
                } else {
                    C().I32Const(1);
                    C().Else();
                    Truth(b->right);
                }
                Close();
                return;
            }
            case g::T_DOTEQ:
            case g::T_DOTNEQ:
                // Identity: the references themselves, storage taken by
                // reference as `.=` takes it (§3.8).
                AddressOf(b->left);
                AddressOf(b->right);
                C().Int(b->op == g::T_DOTEQ ? intop::Eq : intop::Ne, true);
                return;
            case g::T_EQ: case g::T_NEQ: case g::T_LT: case g::T_GT: case g::T_LTEQ:
            case g::T_GTEQ: {
                // A null test (§3.8) reads the reference itself, also of a
                // narrowed optional variable, whose use the checker may
                // already type as the pointee (upstream Binary::CgX).
                auto lnull = b->left->nkind == g::NK_NullLit, rnull = b->right->nkind == g::NK_NullLit;
                if ((b->op == g::T_EQ || b->op == g::T_NEQ) && (lnull || rnull)) {
                    if (lnull && rnull) {
                        C().I32Const(b->op == g::T_EQ);
                        return;
                    }
                    auto other = lnull ? b->right : b->left;
                    auto id = other->nkind == g::NK_Ident ? static_cast<g::Ident *>(other) : nullptr;
                    if (id && id->vdef && IsPlainRef(id->vdef->type)) VarValue(other, id->vdef);
                    else Expr(other, other->exprtype);
                    C().I64Const(0);
                    C().Int(b->op == g::T_EQ ? intop::Eq : intop::Ne, true);
                    return;
                }
                if ((b->op == g::T_EQ || b->op == g::T_NEQ) && AdtEquality(b, OperandType(lt))) return;
                auto ct = Common(b, OperandType(lt), OperandType(rt_));
                auto r = ReprOf(b, ct);
                Expr(b->left, ct);
                Expr(b->right, ct);
                if (IsFlt(ct)) {
                    // IEEE comparisons, as C's: any with a NaN is false but !=.
                    static const uint8_t f32ops[] = { 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60 };
                    static const uint8_t f64ops[] = { 0x61, 0x62, 0x63, 0x64, 0x65, 0x66 };
                    int k = b->op == g::T_EQ ? 0 : b->op == g::T_NEQ ? 1 : b->op == g::T_LT ? 2
                          : b->op == g::T_GT ? 3 : b->op == g::T_LTEQ ? 4 : 5;
                    C().Op(r == Repr::F32 ? f32ops[k] : f64ops[k]);
                    return;
                }
                bool wide = r == Repr::I64, s = !IsInt(ct) || IsSigned(ct);
                switch (b->op) {
                    case g::T_EQ: C().Int(intop::Eq, wide); break;
                    case g::T_NEQ: C().Int(intop::Ne, wide); break;
                    case g::T_LT: C().Int(s ? intop::LtS : intop::LtU, wide); break;
                    case g::T_GT: C().Int(s ? intop::GtS : intop::GtU, wide); break;
                    case g::T_LTEQ: C().Int(s ? intop::LeS : intop::LeU, wide); break;
                    default: C().Int(s ? intop::GeS : intop::GeU, wide); break;
                }
                return;
            }
            default: {
                // An operation computes at its operands' type, which the
                // checker unified (spec §6.2): the left operand's, as a
                // reference reads as its pointee (upstream OperandT). The
                // node's own type may be wider, the slot its value lands in
                // (`let j: i64 = k + 1` with an i8 k wraps at i8).
                auto t = OperandType(lt);
                if (IsFlt(t) || IsFlt(OperandType(rt_))) {
                    t = Common(b, t, OperandType(rt_));
                    Expr(b->left, t);
                    FloatArith(b, b->op, t, b->right);
                    Convert(b, t, b->exprtype);
                    return;
                }
                if (t->kind == g::TY_BOOL && (b->op == g::T_BITAND || b->op == g::T_BITOR ||
                                              b->op == g::T_XOR)) {
                    Expr(b->left, t);
                    Expr(b->right, t);
                    C().Int(b->op == g::T_BITAND ? intop::And : b->op == g::T_BITOR ? intop::Or
                                                                                    : intop::Xor,
                            false);
                    Convert(b, t, b->exprtype);
                    return;
                }
                if (!IsInt(t)) Unsupported(b, "arithmetic on this type");
                // A shift's count has a type of its own.
                Expr(b->left, t);
                Arith(b, b->op, t, b->right);
                Convert(b, t, b->exprtype);
            }
        }
    }

    // The integer operation `op` at type t, its left operand on the stack
    // and its right operand `right` still to emit. Release semantics (spec
    // §6.2, upstream runtime.h): signed arithmetic wraps, `/` and `%` check
    // for zero always, `%` is Euclidean, i64.min / -1 aborts, shift counts
    // are masked to the width.
    // The float operation `op` at type t, its left operand on the stack and
    // its right operand still to emit.
    void FloatArith(g::Node *at, g::TType op, g::TypeExpr *t, g::Node *right) {
        auto f32 = t->fltstorage == g::FS_F32;
        uint8_t code;
        switch (op) {
            case g::T_PLUS: code = f32 ? 0x92 : 0xa0; break;
            case g::T_MINUS: code = f32 ? 0x93 : 0xa1; break;
            case g::T_MUL: code = f32 ? 0x94 : 0xa2; break;
            case g::T_DIV: code = f32 ? 0x95 : 0xa3; break;
            // Float % is C's fmod, an exact remainder.
            default: Unsupported(at, "this operator on floats");
        }
        Expr(right, t);
        C().Op(code);
    }

    void Arith(g::Node *at, g::TType op, g::TypeExpr *t, g::Node *right) {
        if (IsFlt(t)) return FloatArith(at, op, t, right);
        auto wide = IsWide(t);
        auto r = wide ? Repr::I64 : Repr::I32;
        switch (op) {
            case g::T_PLUS: case g::T_MINUS: case g::T_MUL: case g::T_BITAND: case g::T_BITOR:
            case g::T_XOR: {
                Expr(right, t);
                C().Int(op == g::T_PLUS ? intop::Add : op == g::T_MINUS ? intop::Sub
                        : op == g::T_MUL ? intop::Mul : op == g::T_BITAND ? intop::And
                        : op == g::T_BITOR ? intop::Or : intop::Xor, wide);
                if (!wide) Normalize(t);
                return;
            }
            case g::T_SHL:
            case g::T_SHR: {
                // The count, of whatever integer type, masked to the
                // left operand's width (upstream gs_shl_*/gs_shr_*).
                auto ct = right->exprtype;
                if (!IsInt(ct)) Unsupported(right, "a shift count of this type");
                Expr(right, ct);
                Convert(right, ct, wide ? ast.inttypes[g::IS_I64] : ast.inttypes[g::IS_I32]);
                C().IntConst(wide, Bits(t) - 1);
                C().Int(intop::And, wide);
                if (op == g::T_SHL) C().Int(intop::Shl, wide);
                else C().Int(IsSigned(t) ? intop::ShrS : intop::ShrU, wide);
                if (!wide) Normalize(t);
                return;
            }
            case g::T_DIV:
            case g::T_MOD:
                return DivMod(at, op, t, r, right);
            default:
                Unsupported(at, "this operator");
        }
    }

    // The file name a runtime error at l names: the source's path with every
    // `\` made a `/`, as upstream's CodeGen::FileRef writes it.
    std::string FileName(g::Line l) {
        auto f = l.fileidx >= 0 && l.fileidx < int(ast.sources.size())
                     ? ast.sources[l.fileidx].first : std::string("?");
        for (auto &c : f) if (c == '\\') c = '/';
        return f;
    }

    // A failed check: upstream's gs_abort, the message and the location.
    void Abort(const g::Node *at, const char *msg) {
        std::string s = msg;
        C().I32Const(int32_t(StaticString(s)));
        C().I32Const(int32_t(s.size()));
        Location(at);
        C().Call(rt.fail);
    }

    // `/` and `%` (upstream runtime.h GS_DIVU, GS_MODU, GS_DIVS, GS_MODS,
    // gs_div_i64, gs_mod_i64).
    void DivMod(g::Node *at, g::TType op, g::TypeExpr *t, Repr r, g::Node *right) {
        auto wide = r == Repr::I64;
        auto a = NewLocal(r), b = NewLocal(r);
        C().LocalSet(a);
        Expr(right, t);
        C().LocalTee(b);
        C().Int(intop::Eqz, wide);
        C().If();
        Abort(at, "division by zero");
        C().End();
        if (!IsSigned(t)) {
            C().LocalGet(a);
            C().LocalGet(b);
            C().Int(op == g::T_DIV ? intop::DivU : intop::RemU, wide);
            return;
        }
        if (op == g::T_DIV) {
            if (wide) {
                // i64.min / -1 aborts in every build.
                C().LocalGet(a);
                C().I64Const(INT64_MIN);
                C().Int(intop::Eq, true);
                C().LocalGet(b);
                C().I64Const(-1);
                C().Int(intop::Eq, true);
                C().I32And();
                C().If();
                Abort(at, "integer overflow in division");
                C().End();
                C().LocalGet(a);
                C().LocalGet(b);
                C().Int(intop::DivS, true);
            } else {
                // Narrow signed division runs in 64 bits and truncates back,
                // so i32.min / -1 wraps as in the C, where i32.div_s would
                // trap.
                C().LocalGet(a);
                C().I64ExtendI32S();
                C().LocalGet(b);
                C().I64ExtendI32S();
                C().Int(intop::DivS, true);
                C().I32WrapI64();
                Normalize(t);
            }
            return;
        }
        // Euclidean %: the truncating remainder, plus |b| when negative.
        // Wasm's rem_s gives 0 for min % -1 rather than trapping, which is
        // what upstream computes. |b| of the most negative b wraps to itself,
        // which as an unsigned addend is its magnitude.
        // select(x, y, c) is c ? x : y.
        auto m = NewLocal(r);
        C().LocalGet(a);
        C().LocalGet(b);
        C().Int(intop::RemS, wide);
        C().LocalSet(m);
        C().LocalGet(m);
        C().IntConst(wide, 0);
        C().LocalGet(b);
        C().Int(intop::Sub, wide);
        C().LocalGet(b);
        C().LocalGet(b);
        C().IntConst(wide, 0);
        C().Int(intop::LtS, wide);
        C().Select();  // b < 0 ? -b : b
        C().Int(intop::Add, wide);
        C().LocalGet(m);
        C().LocalGet(m);
        C().IntConst(wide, 0);
        C().Int(intop::LtS, wide);
        C().Select();  // m < 0 ? m + |b| : m
        if (!wide) Normalize(t);
    }

    // ---- Root structs (spec §3.4, Appendix C.2) ----

    // A root owner's fields are variables of their own (the checker's
    // VarDef::components), each with its ordinary representation, a
    // resizable one on a stack of its own. The owner is a private
    // descriptor of references to them, which a whole-owner reference
    // points at: per field, the handle of a fixed one's place (8 bytes), or
    // a resizable one's { header handle, stack } (16).
    static bool IsRoot(g::TypeExpr *t) { return Layouts::IsRoot(t); }
    static bool IsRzField(g::TypeExpr *ft) { return IsRz(ft) || IsFrameObj(ft); }
    int64_t RootDescOff(g::TypeExpr *t, int f) {
        auto si = Layouts::SI(t);
        int64_t off = 0;
        for (int i = 0; i < f; i++)
            if (si->ftypes[i]) off += IsRzField(si->ftypes[i]) ? 16 : 8;
        return off;
    }
    int64_t RootDescSize(g::TypeExpr *t) {
        return RootDescOff(t, int(Layouts::SI(t)->ftypes.size()));
    }

    // A root owner's declaration: each field in field order, from the
    // literal's initializer for it (a field default where `..` gives it),
    // then the descriptor.
    void DeclareRoot(g::VarDecl *d, g::VarDef *v, g::Node *init) {
        auto si = Layouts::SI(v->type);
        auto sl = init && init->nkind == g::NK_StructLit ? static_cast<g::StructLit *>(init) : nullptr;
        if (!sl) Unsupported(d, "a root owner not built by a literal");
        if (v->components.size() != si->ftypes.size()) Unsupported(d, "this root owner's fields");
        for (size_t f = 0; f < si->ftypes.size(); f++) {
            auto c = v->components[f];
            if (!c) continue;
            auto ci = sl->InitFor(int(f));
            if (!ci) Unsupported(d, "a root field without an initializer");
            DeclareVar(d, c, ci);
        }
        auto desc = FrameBytes(RootDescSize(v->type));
        desc.t = v->type;
        for (size_t f = 0; f < si->ftypes.size(); f++) {
            auto c = v->components[f];
            if (!c) continue;
            auto at = desc.At(RootDescOff(v->type, int(f)), I64T());
            if (IsRzField(c->type)) {
                auto rz = rzvars.at(c);
                Store(d, at, [&] { Handle(rz.hdr); });
                Store(d, desc.At(at.off - desc.off + 8, I64T()), [&] {
                    C().LocalGet(rz.stk);
                    C().I64ExtendI32U();
                });
                continue;
            }
            auto it = memvars.find(c);
            if (it == memvars.end()) Unsupported(d, g::cat("the root field ", c->name));
            Store(d, at, [&] { Handle(it->second); });
        }
        memvars[v] = desc;
    }

    // Field f of the root owner whose descriptor is at desc: the place of a
    // fixed one.
    Loc RootField(const g::Node *at, const Loc &desc, int f) {
        auto ft = Layouts::SI(desc.t)->ftypes[f];
        auto h = NewLocal(Repr::I64);
        Load(at, desc.At(RootDescOff(desc.t, f), I64T()));
        C().LocalSet(h);
        return Loc { false, h, 0, ft, false };
    }
    // A resizable field: its header and stack.
    RzVar RootRzField(const g::Node *at, const Loc &desc, int f) {
        auto off = RootDescOff(desc.t, f);
        auto h = NewLocal(Repr::I64), stk = NewLocal(Repr::I32);
        Load(at, desc.At(off, I64T()));
        C().LocalSet(h);
        Load(at, desc.At(off + 8, I64T()));
        C().I32WrapI64();
        C().LocalSet(stk);
        return RzVar { Loc { false, h, 0, Layouts::SI(desc.t)->ftypes[f], false }, stk };
    }

    // ---- default<T>() (spec §4.2; upstream B_DEFAULT) ----

    // Whether a declared field default lies anywhere in t (upstream
    // HasFieldDefaults): an ADT's default is its first variant.
    bool HasFieldDefaults(g::TypeExpr *t) {
        auto run = [&](const std::vector<g::Field> &fs, const std::vector<g::TypeExpr *> &ts) {
            for (size_t i = 0; i < fs.size(); i++)
                if (!fs[i].ispad && (fs[i].defaultval || (ts[i] && HasFieldDefaults(ts[i])))) return true;
            return false;
        };
        switch (t->kind) {
            case g::TY_STRUCT: {
                auto si = Layouts::SI(t);
                return si && run(si->st->fields, si->ftypes);
            }
            case g::TY_ENUM: {
                if (t->enu->varmode) return false;
                auto ei = Layouts::EI(t);
                return ei && !ei->en->variants.empty() && run(ei->en->variants[0].fields, ei->vftypes[0]);
            }
            case g::TY_VARIANT: {
                auto ei = Layouts::EIVar(t);
                auto vi = Layouts::VariantIndex(t);
                return run(ei->en->variants[vi].fields, ei->vftypes[vi]);
            }
            case g::TY_ARRAY:
                return t->arr->akind == g::A_FIXED && t->arr->size > 0 && HasFieldDefaults(t->arr->sub);
            default: return false;
        }
    }

    // Zero bytes at dst, the whole of its type.
    void ZeroAt(const Loc &dst) {
        auto size = int32_t(lay.FixedSize(dst.t));
        if (!size) return;
        if (dst.Direct()) {
            AddressWithOffset(dst);
            C().I32Const(0);
            C().I32Const(size);
            C().MemoryFill();
            return;
        }
        // Elsewhere, a zeroed temporary copied over.
        auto tmp = FrameSlot(dst.t);
        ZeroAt(tmp);
        Copy(dst, tmp);
    }

    // default<T>() of a fixed T at dst: zero bytes, unless a field default
    // lies in it, which the checker's default initializer builds, once per
    // element of an array.
    void DefaultInto(g::Call *c, const Loc &dst) {
        auto t = dst.t;
        if (!HasFieldDefaults(t) || !c->defaultinit) return ZeroAt(dst);
        if (t->kind != g::TY_ARRAY) return Construct(c->defaultinit, dst);
        auto et = t->arr->sub;
        auto esz = lay.FixedSize(et);
        for (int64_t i = 0; i < t->arr->size; i++) Construct(c->defaultinit, dst.At(i * esz, et));
    }

    // ---- Relative references (spec §3.9) ----

    // A self-relative offset is measured from its own slot; an `in pool`
    // one from the pool's base, biased by one. Handles of one memory
    // subtract to the addresses' distance, and the checker keeps both ends
    // of a self-relative one in one array, so in one memory.

    // Whether a value of type t holds a relative reference anywhere: such
    // values are built where they stay, as upstream builds them (a copy
    // breaks a self-relative offset, and `self` names the value's final
    // place in either form).
    bool HoldsRelRef(g::TypeExpr *t) {
        std::set<g::TypeExpr *> open;
        std::function<bool(g::TypeExpr *)> holds = [&](g::TypeExpr *x) -> bool {
            if (!x || !open.insert(x).second) return false;
            switch (x->kind) {
                case g::TY_REF: return IsRelRef(x);
                case g::TY_ARRAY: return holds(x->arr->sub);
                case g::TY_STRUCT: {
                    auto si = Layouts::SI(x);
                    if (!si) return false;
                    for (auto ft : si->ftypes) if (holds(ft)) return true;
                    return false;
                }
                case g::TY_ENUM: case g::TY_VARIANT: {
                    auto ei = x->kind == g::TY_ENUM ? Layouts::EI(x) : Layouts::EIVar(x);
                    if (!ei) return false;
                    for (auto &fts : ei->vftypes) for (auto ft : fts) if (holds(ft)) return true;
                    return false;
                }
                default: return false;
            }
        };
        return holds(t);
    }

    // The base handle of an `in pool` reference's pool: its elements'
    // start, which never moves (a grow-only global's elements start at its
    // stack's base).
    void PoolBase(const g::Node *at, g::VarDef *pool) {
        auto slot = GlobalSlot(pool);
        if (!slot.abs || !IsRz(pool->type)) Unsupported(at, g::cat("the pool ", pool->name));
        Load(at, slot.At(0, I64T()));
    }

    // The offset's integer type: signed for a self-relative one.
    g::TypeExpr *RelIntT(g::TypeExpr *rt) {
        auto w = g::IntStorage(rt->ref->lenstorage);
        if (rt->ref->pool) return ast.inttypes[w];
        switch (w) {
            case g::IS_U8: return ast.inttypes[g::IS_I8];
            case g::IS_U16: return ast.inttypes[g::IS_I16];
            case g::IS_U32: return ast.inttypes[g::IS_I32];
            default: return ast.inttypes[g::IS_I64];
        }
    }
    // Where the offsets of the slot at l count from.
    void RelOrigin(const g::Node *at, const Loc &l) {
        if (!l.t->ref->pool) {
            HandleOf(l);
            return;
        }
        PoolBase(at, l.t->ref->pool);
        C().I64Const(1);
        C().Int(intop::Sub, true);
    }

    // The plain reference the relative slot at l holds; an optional one's
    // offset 0 is null.
    void RelLoad(const g::Node *at, const Loc &l) {
        auto it = RelIntT(l.t);
        auto off = NewLocal(Repr::I64);
        Loc nl = l;
        nl.t = it;
        Load(at, nl);
        Convert(at, it, I64T());
        C().LocalSet(off);
        if (l.t->ref->optional) {
            C().LocalGet(off);
            C().I64Eqz();
            C().If(BlockType(uint8_t(ValType::I64)));
            C().I64Const(0);
            C().Else();
        }
        RelOrigin(at, l);
        C().LocalGet(off);
        C().Int(intop::Add, true);
        if (l.t->ref->optional) C().End();
    }

    // Stores the plain reference `value` pushes as the slot's offset, range
    // checked (upstream RelOffset, EmitRelRangeCheck): an optional
    // self-relative slot cannot take a non-null target at offset 0, its
    // null, and a self-relative offset must fit the width signed, an `in
    // pool` one unsigned.
    void RelStore(const g::Node *at, const Loc &l, const std::function<void()> &value) {
        auto rt = l.t;
        auto v = NewLocal(Repr::I64), off = NewLocal(Repr::I64);
        value();
        C().LocalSet(v);
        C().LocalGet(v);
        RelOrigin(at, l);
        C().Int(intop::Sub, true);
        C().LocalSet(off);
        if (rt->ref->optional) {
            // null stores 0.
            C().LocalGet(off);
            C().I64Const(0);
            C().LocalGet(v);
            C().I64Eqz();
            C().I32Eqz();
            C().Select();
            C().LocalSet(off);
            if (!rt->ref->pool) {
                C().LocalGet(v);
                C().I64Eqz();
                C().I32Eqz();
                C().LocalGet(off);
                C().I64Eqz();
                C().I32And();
                C().If();
                Abort(at, "non-null relative reference encodes as null");
                C().End();
            }
        }
        auto bits = Layouts::IntSize(g::IntStorage(rt->ref->lenstorage)) * 8;
        if (bits < 64) {
            C().LocalGet(off);
            if (rt->ref->pool) {
                C().I64Const(int64_t((1ull << bits) - 1));
                C().Int(intop::GtU, true);
            } else {
                // off + 2^(bits-1) >= 2^bits, unsigned: off outside the range.
                C().I64Const(int64_t(1ull << (bits - 1)));
                C().Int(intop::Add, true);
                C().I64Const(int64_t(1ull << bits));
                C().Int(intop::GeU, true);
            }
            C().If();
            Abort(at, "relative reference offset overflow");
            C().End();
        }
        auto op = StoreOpOf(at, RelIntT(rt));
        auto narrow = [&] {
            C().LocalGet(off);
            if (bits < 64) C().I32WrapI64();
        };
        if (!l.Direct()) {
            HandleOf(l);
            narrow();
            C().Call(hs.Store(m, op));
            return;
        }
        Address(l);
        narrow();
        C().StoreOp(op, uint64_t(l.off));
    }

    // ---- Extern functions (spec §7.10) ----

    // A Wasm module has no C library to bind an extern to. The standard
    // library's externs that only compute are implemented here, each as
    // upstream's runtime defines it (runtime_impl.h, or the C library
    // function it names); the others, which reach the host, are unsupported.
    static std::string ExternSymbol(g::FnSpec *sp) {
        return sp->sf->cname.empty() ? std::string(sp->sf->name) : sp->sf->cname;
    }
    static bool KnownExtern(g::FnSpec *sp) {
        static const std::set<std::string> known = {
            "gs_sqrt", "gs_sqrtf", "floor", "floorf", "ceil", "ceilf", "trunc", "truncf",
            "round", "roundf", "gs_popcount", "gs_clz", "gs_ctz", "gs_find_any", "gs_find_pair",
            "frexp", "ldexp",
        };
        return known.count(ExternSymbol(sp)) > 0;
    }

    void ExternBody(g::FnSpec *sp, uint32_t index) {
        fn = index;
        spec = sp;
        locals.clear();
        memvars.clear();
        rzvars.clear();
        labels.clear();
        scopes.clear();
        frame = 0;
        nparams = uint32_t(m.types[F().type].params.size());
        auto sym = ExternSymbol(sp);
        // Parameter 0 is the stack index every function takes; the
        // extern's own parameters follow.
        auto unary = [&](uint8_t op) {
            C().LocalGet(1);
            C().Op(op);
        };
        if (sym == "gs_sqrt") return unary(0x9f);
        if (sym == "gs_sqrtf") return unary(0x91);
        if (sym == "floor") return unary(0x9c);
        if (sym == "floorf") return unary(0x8e);
        if (sym == "ceil") return unary(0x9b);
        if (sym == "ceilf") return unary(0x8d);
        if (sym == "trunc") return unary(0x9d);
        if (sym == "truncf") return unary(0x8f);
        if (sym == "gs_popcount") return unary(0x7b);
        // clz and ctz of zero are 64, as Wasm's are.
        if (sym == "gs_clz") return unary(0x79);
        if (sym == "gs_ctz") return unary(0x7a);
        if (sym == "round" || sym == "roundf") {
            // Halfway cases away from zero: t = trunc(x), and one more in
            // x's direction where x - t, which is exact, is half or more.
            bool f32 = sym == "roundf";
            auto t = NewLocal(f32 ? Repr::F32 : Repr::F64);
            unary(f32 ? 0x8f : 0x9d);
            C().LocalSet(t);
            C().LocalGet(t);
            if (f32) C().F32Const(1);
            else C().F64Const(1);
            C().LocalGet(1);
            C().Op(f32 ? 0x98 : 0xa6);  // copysign
            C().Op(f32 ? 0x92 : 0xa0);  // add
            C().LocalGet(t);
            C().LocalGet(1);
            C().LocalGet(t);
            C().Op(f32 ? 0x93 : 0xa1);  // sub
            C().Op(f32 ? 0x8b : 0x99);  // abs
            if (f32) C().F32Const(0.5f);
            else C().F64Const(0.5);
            C().Op(f32 ? 0x60 : 0x66);  // ge
            C().Select();
            return;
        }
        if (sym == "gs_find_any" || sym == "gs_find_pair") return FindAny(sym == "gs_find_pair");
        if (sym == "frexp") return Frexp();
        if (sym == "ldexp") return Ldexp();
    }

    // find_any(s, set) and find_pair's byte_set_pair(s, a, d, b): the first
    // i with s[i] in the set (and s[i + d] in b, i + d < s.len), -1 for none.
    // A set's `members` is the set (std ByteSet), its first field.
    void FindAny(bool pair) {
        auto u8 = ast.inttypes[g::IS_U8];
        auto data = NewLocal(Repr::I64), n = NewLocal(Repr::I64), i = NewLocal(Repr::I64);
        Loc slice { false, 1, 0, I64T(), false };
        Load(nullptr, slice);
        C().LocalSet(data);
        Load(nullptr, slice.At(8, I64T()));
        C().LocalSet(n);
        uint32_t seta = 2, d = 3, setb = 4;
        if (pair) {
            // Positions with a partner: n - d of them.
            C().LocalGet(n);
            C().LocalGet(d);
            C().Int(intop::Sub, true);
            C().LocalSet(n);
        }
        // members[s[k]] of the set at handle local `set`.
        auto member = [&](uint32_t set, auto at) {
            auto h = NewLocal(Repr::I64);
            C().LocalGet(data);
            at();
            C().Int(intop::Add, true);
            C().LocalSet(h);
            C().LocalGet(set);
            Load(nullptr, Loc { false, h, 0, u8, false });
            C().I64ExtendI32U();
            C().Int(intop::Add, true);
            C().LocalSet(h);
            Load(nullptr, Loc { false, h, 0, u8, false });
        };
        C().I64Const(0);
        C().LocalSet(i);
        C().Block();
        C().Loop();
        C().LocalGet(i);
        C().LocalGet(n);
        C().Int(intop::GeS, true);
        C().BrIf(1);
        member(seta, [&] { C().LocalGet(i); });
        if (pair) {
            C().If(BlockType(uint8_t(ValType::I32)));
            member(setb, [&] {
                C().LocalGet(i);
                C().LocalGet(d);
                C().Int(intop::Add, true);
            });
            C().Else();
            C().I32Const(0);
            C().End();
        }
        C().If();
        C().LocalGet(i);
        C().Return();
        C().End();
        C().LocalGet(i);
        C().I64Const(1);
        C().Int(intop::Add, true);
        C().LocalSet(i);
        C().Br(0);
        C().End();
        C().End();
        C().I64Const(-1);
    }

    // frexp(x, e&) (C): x as a fraction in [0.5, 1) times 2^e; zero,
    // infinities and NaN are themselves, with e = 0. A subnormal x is first
    // scaled into the normal range by 2^54.
    void Frexp() {
        auto bits = NewLocal(Repr::I64), ex = NewLocal(Repr::I64), adj = NewLocal(Repr::I64);
        auto storee = [&](auto value) {
            Store(nullptr, Loc { false, 2, 0, ast.inttypes[g::IS_I32], false }, value);
        };
        C().I64Const(0);
        C().LocalSet(adj);
        C().LocalGet(1);
        C().Op(0xbd);  // i64.reinterpret_f64
        C().LocalSet(bits);
        // Zero, infinities and NaN.
        C().LocalGet(1);
        C().F64Const(0);
        C().Op(0x61);  // f64.eq
        C().LocalGet(bits);
        C().I64Const(52);
        C().Int(intop::ShrU, true);
        C().I64Const(0x7ff);
        C().Int(intop::And, true);
        C().I64Const(0x7ff);
        C().Int(intop::Eq, true);
        C().Int(intop::Or, false);
        C().If();
        storee([&] { C().I32Const(0); });
        C().LocalGet(1);
        C().Return();
        C().End();
        // A subnormal: scaled by 2^54 first.
        C().LocalGet(bits);
        C().I64Const(52);
        C().Int(intop::ShrU, true);
        C().I64Const(0x7ff);
        C().Int(intop::And, true);
        C().I64Eqz();
        C().If();
        C().LocalGet(1);
        C().F64Const(18014398509481984.0);  // 2^54
        C().Op(0xa2);  // f64.mul
        C().Op(0xbd);
        C().LocalSet(bits);
        C().I64Const(-54);
        C().LocalSet(adj);
        C().End();
        C().LocalGet(bits);
        C().I64Const(52);
        C().Int(intop::ShrU, true);
        C().I64Const(0x7ff);
        C().Int(intop::And, true);
        C().LocalSet(ex);
        storee([&] {
            C().LocalGet(ex);
            C().I64Const(1022);
            C().Int(intop::Sub, true);
            C().LocalGet(adj);
            C().Int(intop::Add, true);
            C().I32WrapI64();
        });
        C().LocalGet(bits);
        C().I64Const(int64_t(0x800FFFFFFFFFFFFFull));
        C().Int(intop::And, true);
        C().I64Const(int64_t(0x3FE0000000000000ull));
        C().Int(intop::Or, true);
        C().Op(0xbf);  // f64.reinterpret_i64
    }

    // ldexp(x, n) (C): x * 2^n, rounded once. The scale goes in at most
    // three exact steps above 2^1023 or below 2^-1022, and the last step
    // is a power of two the double can hold, as C libraries compute it.
    void Ldexp() {
        auto y = NewLocal(Repr::F64), n = NewLocal(Repr::I32);
        C().LocalGet(1);
        C().LocalSet(y);
        C().LocalGet(2);
        C().LocalSet(n);
        auto mul = [&](double f) {
            C().LocalGet(y);
            C().F64Const(f);
            C().Op(0xa2);
            C().LocalSet(y);
        };
        auto addn = [&](int32_t k) {
            C().LocalGet(n);
            C().I32Const(k);
            C().I32Add();
            C().LocalSet(n);
        };
        auto cmp = [&](int32_t k, bool above) {
            C().LocalGet(n);
            C().I32Const(k);
            C().Int(above ? intop::GtS : intop::LtS, false);
        };
        const double up = std::ldexp(1.0, 1023);
        const double down = std::ldexp(1.0, -1022 + 53);
        cmp(1023, true);
        C().If();
        mul(up);
        addn(-1023);
        cmp(1023, true);
        C().If();
        mul(up);
        addn(-1023);
        cmp(1023, true);
        C().If();
        C().I32Const(1023);
        C().LocalSet(n);
        C().End();
        C().End();
        C().Else();
        cmp(-1022, false);
        C().If();
        mul(down);
        addn(1022 - 53);
        cmp(-1022, false);
        C().If();
        mul(down);
        addn(1022 - 53);
        cmp(-1022, false);
        C().If();
        C().I32Const(-1022);
        C().LocalSet(n);
        C().End();
        C().End();
        C().End();
        C().End();
        C().LocalGet(y);
        C().LocalGet(n);
        C().I32Const(0x3ff);
        C().I32Add();
        C().I64ExtendI32U();
        C().I64Const(52);
        C().Int(intop::Shl, true);
        C().Op(0xbf);
        C().Op(0xa2);
    }

    // ---- Limited arrays T[..k] of static capacity (spec §4.2) ----

    // Laid out { len, e[k] } (layout.h), they are fixed aggregates; the
    // builtins that grow them check the capacity (upstream GS_E_CAPACITY).
    g::TypeExpr *LimLenT(g::TypeExpr *t) { return ast.inttypes[Layouts::LimLenStore(t)]; }

    // The length of the limited array at l, as an i64.
    void LimLen(const g::Node *at, const Loc &l) {
        auto lt = LimLenT(l.t);
        Load(at, l.At(0, lt));
        if (!IsWide(lt)) C().I64ExtendI32U();
    }
    // Stores the i64 `value` pushes as its length.
    template <typename F> void LimSetLen(const g::Node *at, const Loc &l, F value) {
        auto lt = LimLenT(l.t);
        Store(at, l.At(0, lt), [&] {
            value();
            if (!IsWide(lt)) C().I32WrapI64();
        });
    }
    // The handle of element i (an i64 local) of the limited array at l.
    void LimElem(const Loc &l, uint32_t i) {
        HandleOf(l);
        C().I64Const(Layouts::LimLenSize(l.t));
        C().Int(intop::Add, true);
        C().LocalGet(i);
        C().I64Const(lay.FixedSize(l.t->arr->sub));
        C().Int(intop::Mul, true);
        C().Int(intop::Add, true);
    }
    // Its live elements as a sequence. The length is read where it is
    // used: a loop whose body shrinks the array ends early (upstream reads
    // it again where the body can store into the array).
    Seq LimSeq(const g::Node *at, const Loc &l) {
        Seq s;
        s.et = l.t->arr->sub;
        s.data = NewLocal(Repr::I64);
        s.mem0 = l.Direct();
        HandleOf(l);
        C().I64Const(Layouts::LimLenSize(l.t));
        C().Int(intop::Add, true);
        C().LocalSet(s.data);
        s.len = [this, at, l] { LimLen(at, l); };
        return s;
    }
    // Aborts unless the i64 local `need` is within l's capacity.
    void CapCheck(const g::Node *at, const Loc &l, uint32_t need) {
        C().LocalGet(need);
        C().I64Const(l.t->arr->size);
        C().Int(intop::GtS, true);
        C().If();
        Abort(at, "limited array capacity exceeded");
        C().End();
    }

    // The receiver of a member builtin when it is a limited array: its place.
    bool LimRecv(g::Call *call, Loc &l) {
        auto an = call->ArgNodes();
        auto at = an[0]->exprtype;
        if (at->kind == g::TY_REF) at = at->ref->sub;
        if (!Layouts::IsLimited(at)) return false;
        auto n = an[0];
        if (n->nkind == g::NK_Unary && static_cast<g::Unary *>(n)->op == g::T_BITAND)
            n = static_cast<g::Unary *>(n)->child;
        l = ObjectLoc(n);
        l.t = at;
        return true;
    }

    // A limited array at dst from a literal of elements, or copied from a
    // run (a string literal, a slice, another array); its length set.
    void LimInto(g::Node *n, const Loc &dst) {
        auto et = dst.t->arr->sub;
        auto esz = lay.FixedSize(et);
        auto elems = dst.At(Layouts::LimLenSize(dst.t), nullptr);
        if (n->nkind == g::NK_ArrayLit) {
            auto lit = static_cast<g::ArrayLit *>(n);
            if (lit->fillval || lit->capexpr) Unsupported(n, "this array literal as a limited array");
            if (int64_t(lit->elems.size()) > dst.t->arr->size) Unsupported(n, "a literal past the capacity");
            for (size_t i = 0; i < lit->elems.size(); i++) {
                auto at = elems.At(int64_t(i) * esz, et);
                auto val = lit->elems[i];
                if (IsAggregate(et)) Construct(val, at);
                else Store(val, at, [&] { Expr(val, et); });
            }
            LimSetLen(n, dst, [&] { C().I64Const(int64_t(lit->elems.size())); });
            return;
        }
        auto s = SeqOf(n);
        auto cnt = NewLocal(Repr::I64);
        s.len();
        C().LocalSet(cnt);
        CapCheck(n, dst, cnt);
        HandleOf(elems);
        C().LocalGet(s.data);
        C().LocalGet(cnt);
        C().I64Const(esz);
        C().Int(intop::Mul, true);
        C().I32WrapI64();
        C().Call(hs.copy);
        LimSetLen(n, dst, [&] { C().LocalGet(cnt); });
    }

    // xs.push(v): v first, then the capacity check, the slot and the length.
    void LimPush(g::Call *call, const Loc &l, g::Node *val, uint32_t *ref) {
        auto et = l.t->arr->sub;
        bool agg = IsAggregate(et);
        // One holding relative references is built in its slot (§3.9).
        bool inplace = agg && HoldsRelRef(et);
        Loc tmp;
        uint32_t v = 0;
        if (inplace) {
        } else if (agg) {
            tmp = FrameSlot(et);
            Construct(val, tmp);
        } else {
            v = NewLocal(ReprOf(call, et));
            Expr(val, et);
            C().LocalSet(v);
        }
        auto n = NewLocal(Repr::I64), nl = NewLocal(Repr::I64), h = NewLocal(Repr::I64);
        LimLen(call, l);
        C().LocalTee(n);
        C().I64Const(1);
        C().Int(intop::Add, true);
        C().LocalSet(nl);
        CapCheck(call, l, nl);
        LimElem(l, n);
        C().LocalSet(h);
        Loc slot { false, h, 0, et, l.Direct() };
        if (inplace) Construct(val, slot);
        else if (agg) Copy(slot, tmp);
        else Store(call, slot, [&] { C().LocalGet(v); });
        LimSetLen(call, l, [&] { C().LocalGet(nl); });
        if (ref) *ref = h;
    }

    // pop(): the length less one, the element's handle left in a local.
    uint32_t LimPop(g::Call *call, const Loc &l) {
        auto n = NewLocal(Repr::I64), h = NewLocal(Repr::I64);
        LimLen(call, l);
        C().LocalSet(n);
        if (!call->nobc) {
            C().LocalGet(n);
            C().I64Eqz();
            C().If();
            Abort(call, "pop on empty array");
            C().End();
        }
        C().LocalGet(n);
        C().I64Const(1);
        C().Int(intop::Sub, true);
        C().LocalSet(n);
        LimSetLen(call, l, [&] { C().LocalGet(n); });
        LimElem(l, n);
        C().LocalSet(h);
        return h;
    }

    void LimPopN(g::Call *call, const Loc &l) {
        auto k = NewLocal(Repr::I64), ol = NewLocal(Repr::I64);
        Expr(call->ArgNodes()[1], I64T());
        C().LocalSet(k);
        LimLen(call, l);
        C().LocalSet(ol);
        C().LocalGet(k);
        C().LocalGet(ol);
        C().Int(intop::GtU, true);
        C().If();
        Abort(call, "pop_n count negative or beyond the array's length");
        C().End();
        LimSetLen(call, l, [&] {
            C().LocalGet(ol);
            C().LocalGet(k);
            C().Int(intop::Sub, true);
        });
    }

    void LimAppend(g::Call *call, const Loc &l, g::Node *src) {
        auto esz = lay.FixedSize(l.t->arr->sub);
        auto s = SeqOf(src);
        auto cnt = NewLocal(Repr::I64), ol = NewLocal(Repr::I64), nl = NewLocal(Repr::I64);
        s.len();
        C().LocalSet(cnt);
        LimLen(call, l);
        C().LocalTee(ol);
        C().LocalGet(cnt);
        C().Int(intop::Add, true);
        C().LocalSet(nl);
        CapCheck(call, l, nl);
        LimElem(l, ol);
        C().LocalGet(s.data);
        C().LocalGet(cnt);
        C().I64Const(esz);
        C().Int(intop::Mul, true);
        C().I32WrapI64();
        C().Call(hs.copy);
        LimSetLen(call, l, [&] { C().LocalGet(nl); });
    }

    // resize(n, fill?): as a resizable's, with the capacity checked before
    // any slot is filled.
    void LimResize(g::Call *call, const Loc &l) {
        auto an = call->ArgNodes();
        auto et = l.t->arr->sub;
        auto nn = NewLocal(Repr::I64), ol = NewLocal(Repr::I64);
        Expr(an[1], I64T());
        C().LocalSet(nn);
        bool agg = IsAggregate(et);
        bool hasfill = an.size() > 2;
        Loc fillslot;
        uint32_t fill = 0;
        if (hasfill) {
            if (agg) {
                fillslot = FrameSlot(et);
                Construct(an[2], fillslot);
            } else {
                fill = NewLocal(ReprOf(call, et));
                Expr(an[2], et);
                C().LocalSet(fill);
            }
        }
        LimLen(call, l);
        C().LocalSet(ol);
        C().LocalGet(nn);
        C().I64Const(0);
        C().Int(intop::LtS, true);
        C().If();
        Abort(call, "resize to a negative length");
        C().End();
        C().LocalGet(nn);
        C().LocalGet(ol);
        C().Int(intop::LtS, true);
        C().If();
        LimSetLen(call, l, [&] { C().LocalGet(nn); });
        C().Else();
        C().LocalGet(nn);
        C().LocalGet(ol);
        C().Int(intop::GtS, true);
        C().If();
        if (!hasfill) {
            Abort(call, "resize growth requires a fill value");
        } else {
            CapCheck(call, l, nn);
            auto i = NewLocal(Repr::I64), h = NewLocal(Repr::I64);
            C().LocalGet(ol);
            C().LocalSet(i);
            C().Block();
            C().Loop();
            C().LocalGet(i);
            C().LocalGet(nn);
            C().Int(intop::GeS, true);
            C().BrIf(1);
            LimElem(l, i);
            C().LocalSet(h);
            Loc slot { false, h, 0, et, l.Direct() };
            if (agg) Copy(slot, fillslot);
            else Store(call, slot, [&] { C().LocalGet(fill); });
            C().LocalGet(i);
            C().I64Const(1);
            C().Int(intop::Add, true);
            C().LocalSet(i);
            C().Br(0);
            C().End();
            C().End();
            LimSetLen(call, l, [&] { C().LocalGet(nn); });
        }
        C().End();
        C().End();
    }

    // ---- Builtins on resizable arrays, and assert ----

    // The receiver of a member builtin on a resizable array: its header and
    // stack, and its element type.
    struct RzRecv {
        RzVar rz;
        g::TypeExpr *et;
        int32_t esz;
    };
    RzRecv Receiver(g::Call *call) {
        auto an = call->ArgNodes();
        auto at = an[0]->exprtype;
        if (at->kind == g::TY_REF) at = at->ref->sub;
        if (!IsRz(at)) Unsupported(call, g::cat("builtin ", g::builtindefs[call->builtin].name,
                                                " on this kind of array"));
        auto et = at->arr->sub;
        return RzRecv { Snapshot(RzOf(an[0])), et, int32_t(lay.FixedSize(et)) };
    }

    // Sets the top of the receiver's stack to its base plus `count` (an i64
    // local) elements.
    void TopAtCount(const RzRecv &r, const g::Node *at, uint32_t count) {
        TopAddr(r.rz.stk);
        Load(at, r.rz.hdr.At(0, I64T()));
        C().I32WrapI64();
        C().LocalGet(count);
        C().I32WrapI64();
        C().I32Const(r.esz);
        C().Op(0x6c);  // i32.mul
        C().I32Add();
        C().I32Store(mem0::Tops);
    }

    // clear(): the array, topmost on its stack (§1.3), hands its elements
    // back by dropping the top to its base (upstream B_CLEAR).
    void Clear(g::Call *call) {
        Loc l;
        if (LimRecv(call, l)) return LimSetLen(call, l, [&] { C().I64Const(0); });
        auto r = Receiver(call);
        auto zero = NewLocal(Repr::I64);
        TopAtCount(r, call, zero);
        Store(call, r.rz.hdr.At(8, I64T()), [&] { C().I64Const(0); });
    }

    // pop(): the last element, after the length and the top drop by one;
    // on an empty array, upstream's GS_E_POP (B_POP). Leaves the element's
    // handle in a local, for the value to be read from.
    uint32_t Pop(g::Call *call) {
        Loc l;
        if (LimRecv(call, l)) return LimPop(call, l);
        auto r = Receiver(call);
        auto len = r.rz.hdr.At(8, I64T());
        auto n = NewLocal(Repr::I64);
        Load(call, len);
        C().LocalSet(n);
        if (!call->nobc) {
            C().LocalGet(n);
            C().I64Const(0);
            C().Int(intop::LeS, true);
            C().If();
            Abort(call, "pop on empty array");
            C().End();
        }
        C().LocalGet(n);
        C().I64Const(1);
        C().Int(intop::Sub, true);
        C().LocalSet(n);
        Store(call, len, [&] { C().LocalGet(n); });
        TopAtCount(r, call, n);
        // The element's slot, now just above the top.
        auto h = NewLocal(Repr::I64);
        Load(call, r.rz.hdr.At(0, I64T()));
        C().LocalGet(n);
        C().I64Const(r.esz);
        C().Int(intop::Mul, true);
        C().Int(intop::Add, true);
        C().LocalSet(h);
        return h;
    }

    // pop_n(k): one unsigned compare rejects a negative count and one past
    // the length (upstream B_POP_N, GS_E_POPN).
    void PopN(g::Call *call) {
        Loc l;
        if (LimRecv(call, l)) return LimPopN(call, l);
        auto r = Receiver(call);
        auto an = call->ArgNodes();
        auto k = NewLocal(Repr::I64), ol = NewLocal(Repr::I64);
        Expr(an[1], I64T());
        C().LocalSet(k);
        Load(call, r.rz.hdr.At(8, I64T()));
        C().LocalSet(ol);
        C().LocalGet(k);
        C().LocalGet(ol);
        C().Int(intop::GtU, true);
        C().If();
        Abort(call, "pop_n count negative or beyond the array's length");
        C().End();
        C().LocalGet(ol);
        C().LocalGet(k);
        C().Int(intop::Sub, true);
        C().LocalSet(ol);
        Store(call, r.rz.hdr.At(8, I64T()), [&] { C().LocalGet(ol); });
        TopAtCount(r, call, ol);
    }

    // resize(n, fill?) (upstream B_RESIZE): a negative n aborts; a smaller
    // one drops the elements past it; a larger one adds copies of the fill
    // value, which it requires. n and the fill are evaluated before the
    // length is read.
    void Resize(g::Call *call) {
        Loc l;
        if (LimRecv(call, l)) return LimResize(call, l);
        auto r = Receiver(call);
        auto an = call->ArgNodes();
        auto nn = NewLocal(Repr::I64), ol = NewLocal(Repr::I64);
        Expr(an[1], I64T());
        C().LocalSet(nn);
        bool agg = IsAggregate(r.et);
        Loc fillslot;
        uint32_t fill = 0;
        bool hasfill = an.size() > 2;
        if (hasfill) {
            if (agg) {
                fillslot = FrameSlot(r.et);
                Construct(an[2], fillslot);
            } else {
                fill = NewLocal(ReprOf(call, r.et));
                Expr(an[2], r.et);
                C().LocalSet(fill);
            }
        }
        auto len = r.rz.hdr.At(8, I64T());
        Load(call, len);
        C().LocalSet(ol);
        C().LocalGet(nn);
        C().I64Const(0);
        C().Int(intop::LtS, true);
        C().If();
        Abort(call, "resize to a negative length");
        C().End();
        C().LocalGet(nn);
        C().LocalGet(ol);
        C().Int(intop::LtS, true);
        C().If();
        Store(call, len, [&] { C().LocalGet(nn); });
        TopAtCount(r, call, nn);
        C().Else();
        C().LocalGet(nn);
        C().LocalGet(ol);
        C().Int(intop::GtS, true);
        C().If();
        if (!hasfill) {
            Abort(call, "resize growth requires a fill value");
        } else {
            // One element at a time at the top, then the length.
            auto i = NewLocal(Repr::I64), h = NewLocal(Repr::I64);
            C().LocalGet(ol);
            C().LocalSet(i);
            C().Block();
            C().Loop();
            C().LocalGet(i);
            C().LocalGet(nn);
            C().Int(intop::GeS, true);
            C().BrIf(1);
            C().LocalGet(r.rz.stk);
            C().I32Const(r.esz);
            C().Call(hs.pushslot);
            C().LocalSet(h);
            Loc slot { false, h, 0, r.et, false };
            if (agg) Copy(slot, fillslot);
            else Store(call, slot, [&] { C().LocalGet(fill); });
            C().LocalGet(i);
            C().I64Const(1);
            C().Int(intop::Add, true);
            C().LocalSet(i);
            C().Br(0);
            C().End();
            C().End();
            Store(call, len, [&] { C().LocalGet(nn); });
        }
        C().End();
        C().End();
    }

    // a.check_length(n, msg?) (§3.3; upstream SliceLen): n, unless it is
    // negative or its elements' bytes exceed a data stack's reservation
    // (upstream's GS_STACK_RESERVE, kept so programs fail alike), which
    // aborts with "invalid array length" or the message given.
    void CheckLength(g::Call *call) {
        auto an = call->ArgNodes();
        auto at = an[0]->exprtype;
        if (at->kind == g::TY_REF) at = at->ref->sub;
        if (at->kind != g::TY_ARRAY || !Layouts::Known(at->arr->sub))
            Unsupported(call, "check_length on this array");
        auto esz = lay.FixedSize(at->arr->sub);
        auto nv = NewLocal(Repr::I64);
        Expr(an[1], an[1]->exprtype);
        Convert(call, an[1]->exprtype, I64T());
        C().LocalSet(nv);
        C().LocalGet(nv);
        if (esz) {
            C().I64Const(int64_t((2048ull << 20) / uint64_t(esz)));
            C().Int(intop::GtU, true);
        } else {
            C().I64Const(0);
            C().Int(intop::LtS, true);
        }
        C().If();
        auto msg = an.size() > 2 && an[2]->nkind == g::NK_StrLit ? static_cast<g::StrLit *>(an[2]) : nullptr;
        Abort(call, msg ? msg->val.c_str() : "invalid array length");
        C().End();
        C().LocalGet(nv);
    }

    // a.index_of(r) (§3.3): the checker proved r an element of a, so the
    // distance of its handle from the elements' is a whole number of
    // elements inside the length; nothing to check.
    void IndexOf(g::Call *call) {
        auto an = call->ArgNodes();
        auto sq = SeqOf(an[0]);
        AddressOf(an[1]);
        C().LocalGet(sq.data);
        C().Int(intop::Sub, true);
        auto esz = lay.FixedSize(sq.et);
        if (esz > 1) {
            C().I64Const(esz);
            C().Int(intop::DivU, true);
        }
    }

    // a.range_index(s, msg?) (§3.3; upstream B_RANGE_INDEX): where a
    // nonempty s starts in a, in elements, 0 for an empty one. Unless the
    // checker proved s a's own (Call::poolcheck), s must lie wholly inside
    // a's length on an element boundary, or the call fails with "slice not
    // within this array" or the message given. Handles in another memory are
    // far apart, so they fail the same test.
    void RangeIndex(g::Call *call) {
        auto an = call->ArgNodes();
        auto sq = SeqOf(an[0]);
        auto sv = SeqOf(an[1]);
        auto esz = lay.FixedSize(sq.et);
        auto i = NewLocal(Repr::I64), off = NewLocal(Repr::I64), sl = NewLocal(Repr::I64);
        sv.len();
        C().LocalSet(sl);
        C().I64Const(0);
        C().LocalSet(i);
        auto msg = an.size() > 2 && an[2]->nkind == g::NK_StrLit ? static_cast<g::StrLit *>(an[2]) : nullptr;
        auto fail = [&] { Abort(call, msg ? msg->val.c_str() : "slice not within this array"); };
        if (!esz) {
            C().LocalGet(sl);
            sq.len();
            C().Int(intop::GtU, true);
            C().If();
            fail();
            C().End();
            C().LocalGet(i);
            return;
        }
        C().LocalGet(sl);
        C().I64Eqz();
        C().I32Eqz();
        C().If();
        C().LocalGet(sv.data);
        C().LocalGet(sq.data);
        C().Int(intop::Sub, true);
        C().LocalSet(off);
        if (call->poolcheck) {
            // off % esz || off / esz >= len || s.len > len - off / esz
            C().LocalGet(off);
            C().I64Const(esz);
            C().Int(intop::RemU, true);
            C().I64Const(0);
            C().Int(intop::Ne, true);
            C().LocalGet(off);
            C().I64Const(esz);
            C().Int(intop::DivU, true);
            sq.len();
            C().Int(intop::GeU, true);
            C().Int(intop::Or, false);
            C().LocalGet(sl);
            sq.len();
            C().LocalGet(off);
            C().I64Const(esz);
            C().Int(intop::DivU, true);
            C().Int(intop::Sub, true);
            C().Int(intop::GtU, true);
            C().Int(intop::Or, false);
            C().If();
            fail();
            C().End();
        }
        C().LocalGet(off);
        C().I64Const(esz);
        C().Int(intop::DivU, true);
        C().LocalSet(i);
        C().End();
        C().LocalGet(i);
    }

    // abort(msg) (upstream B_ABORT, gs_abort_msg): the message, at the call.
    // One not in memory 0 is copied to the line buffer first.
    void AbortCall(g::Call *call) {
        auto m0 = call->ArgNodes()[0];
        if (m0->nkind == g::NK_StrLit) {
            Abort(call, static_cast<g::StrLit *>(m0)->val.c_str());
            return;
        }
        auto sq = SeqOf(m0);
        auto n = NewLocal(Repr::I64);
        sq.len();
        C().LocalSet(n);
        C().LocalGet(n);
        C().I64Const(int64_t(mem0::LineBufSize));
        C().LocalGet(n);
        C().I64Const(int64_t(mem0::LineBufSize));
        C().Int(intop::LtU, true);
        C().Select();
        C().LocalSet(n);
        C().I64Const(int64_t(mem0::LineBuf));
        C().LocalGet(sq.data);
        C().LocalGet(n);
        C().I32WrapI64();
        C().Call(hs.copy);
        C().I32Const(int32_t(mem0::LineBuf));
        C().LocalGet(n);
        C().I32WrapI64();
        Location(call);
        C().Call(rt.fail);
    }

    // assert(c): upstream's GS_E_ASSERT, "assert failed", at the call.
    void Assert(g::Call *call) {
        Truth(call->ArgNodes()[0]);
        C().I32Eqz();
        C().If();
        Abort(call, "assert failed");
        C().End();
    }

    // ---- ADTs in fixed mode (spec §3.5, §8.1; upstream impl §6.1) ----

    static bool IsAdt(g::TypeExpr *t) { return t && t->kind == g::TY_ENUM; }
    static bool IsVariantT(g::TypeExpr *t) { return t && t->kind == g::TY_VARIANT; }

    // A tag's load and store: u8 up to 256 variants, else u16.
    uint8_t TagLoadOp(g::SEnum *en) { return Layouts::TagSize(en) == 1 ? 0x2d : 0x2f; }
    uint8_t TagStoreOp(g::SEnum *en) { return Layouts::TagSize(en) == 1 ? 0x3a : 0x3b; }

    // The tag of the ADT at l, an i32.
    void LoadTag(const Loc &l) {
        auto en = Layouts::EI(l.t)->en;
        if (!l.Direct()) {
            HandleOf(l);
            C().Call(hs.Load(m, TagLoadOp(en)));
            return;
        }
        Address(l);
        C().LoadOp(TagLoadOp(en), uint64_t(l.off));
    }
    void StoreTag(const Loc &l, int vi) {
        auto en = Layouts::EI(l.t)->en;
        if (!l.Direct()) {
            HandleOf(l);
            C().I32Const(vi);
            C().Call(hs.Store(m, TagStoreOp(en)));
            return;
        }
        Address(l);
        C().I32Const(vi);
        C().StoreOp(TagStoreOp(en), uint64_t(l.off));
    }

    // Where variant vi's payload lies in the ADT at l: after the tag.
    Loc PayloadLoc(const Loc &l, g::TypeExpr *vt) {
        return l.At(Layouts::TagSize(Layouts::EI(l.t)->en), vt);
    }

    // A variant literal at dst, an ADT (tag and payload) or a variant type
    // (the payload alone).
    void VariantInto(g::StructLit *sl, const Loc &dst) {
        auto ei = sl->einst;
        auto vi = ei->en->VariantIndex(sl->variant);
        auto &lo = lay.VariantLayout(ei, vi);
        Loc payload = dst;
        if (IsAdt(dst.t)) {
            StoreTag(dst, vi);
            payload = dst.At(Layouts::TagSize(ei->en), nullptr);
        }
        auto &fts = ei->vftypes[vi];
        std::vector<bool> given(fts.size());
        for (size_t k = 0; k < sl->inits.size(); k++) {
            auto fi = sl->fieldindices[k];
            given[fi] = true;
            auto ft = fts[fi];
            auto floc = payload.At(lo.offs[fi], ft);
            auto val = sl->inits[k].val;
            if (IsRzRef(ft)) StoreFatRef(val, floc);
            else if (IsAggregate(ft)) Construct(val, floc);
            else Store(val, floc, [&] { Expr(val, ft); });
        }
        for (size_t fi = 0; fi < fts.size(); fi++)
            if (!given[fi] && !sl->variant->fields[fi].ispad) OmittedField(sl, payload.At(lo.offs[fi], fts[fi]));
    }

    // A literal's field with no initializer is an omitted optional one,
    // which is null (StructLit::InitFor).
    void OmittedField(const g::Node *at, const Loc &f) {
        if (!f.t || f.t->kind != g::TY_REF || !f.t->ref->optional) Unsupported(at, "an omitted field");
        if (IsRzRef(f.t) || Layouts::IsFatRef(f.t)) Unsupported(at, "an omitted optional fat reference");
        Store(at, f, [&] { C().I64Const(0); });
    }

    // A value of variant type vt from n, made an ADT at dst: the tag, then
    // the payload (upstream GenAdtAdapted).
    void AdaptInto(g::Node *n, g::TypeExpr *vt, const Loc &dst) {
        auto vi = Layouts::VariantIndex(vt);
        auto payload = PayloadLoc(dst, vt);
        Construct(n, payload);
        StoreTag(dst, vi);
    }

    // match (spec §8.1): the arms as a chain of tests, in order; the last
    // arm of an exhaustive ADT match and `_` take what is left. `body`
    // emits an arm's body, as a value, an aggregate or a statement.
    template <typename B> void Match(g::MatchExpr *me, Repr r, B body) {
        auto st = OperandType(me->scrutinee->exprtype);
        uint32_t key = 0;
        Loc scr;
        bool adt = IsAdt(st);
        if (adt) {
            if (!Layouts::FixedAdt(st)) Unsupported(me, "a match of a variable-mode ADT");
            scr = ObjectLoc(me->scrutinee);
            if (IsPlainRef(scr.t)) scr = Deref(me, scr);
            key = NewLocal(Repr::I32);
            LoadTag(scr);
            C().LocalSet(key);
        } else if (IsInt(st)) {
            key = NewLocal(ReprOf(me, st));
            Expr(me->scrutinee, st);
            C().LocalSet(key);
        } else {
            Unsupported(me, g::cat("a match of type ", TypeKindName(st)));
        }
        std::function<void(size_t)> arm = [&](size_t i) {
            auto &a = me->arms[i];
            bool last = i + 1 == me->arms.size();
            bool any = a.pat.kind == g::P_WILDCARD || (last && adt);
            if (!any) {
                // Any listed variant, or any listed value or range.
                bool first = true;
                auto orNext = [&] {
                    if (!first) C().Int(intop::Or, false);
                    first = false;
                };
                if (adt) {
                    auto ei = Layouts::EI(st);
                    for (auto v : a.variants) {
                        C().LocalGet(key);
                        C().I32Const(ei->en->VariantIndex(v));
                        C().Int(intop::Eq, false);
                        orNext();
                    }
                } else {
                    bool wide = IsWide(st), s = IsSigned(st);
                    for (auto &rg : a.ranges) {
                        C().LocalGet(key);
                        C().IntConst(wide, rg.lo);
                        C().Int(s ? intop::GeS : intop::GeU, wide);
                        C().LocalGet(key);
                        C().IntConst(wide, rg.hi);
                        C().Int(s ? intop::LeS : intop::LeU, wide);
                        C().I32And();
                        orNext();
                    }
                }
                if (first) C().I32Const(0);
                OpenIf(r);
            }
            if (a.binder) {
                if (a.pat.byref) Unsupported(me, "a payload bound by reference");
                // By value: a copy of the payload (§8.1).
                auto vt = a.binder->type;
                if (!IsAggregate(vt)) Unsupported(me, "this payload binding");
                auto slot = FrameSlot(vt);
                Copy(slot, PayloadLoc(scr, vt));
                memvars[a.binder] = slot;
            }
            body(a.body);
            if (!any) {
                C().Else();
                if (last) {
                    // No arm took it: an integer match always has `_`.
                    C().Unreachable();
                } else {
                    arm(i + 1);
                }
                Close();
            }
        };
        if (me->arms.empty()) Unsupported(me, "a match without arms");
        arm(0);
    }

    // == and != of fixed-mode ADTs whose variants carry no payload: their
    // tags (upstream compares tag and payload, padding aside).
    bool AdtEquality(g::Binary *b, g::TypeExpr *t) {
        if (!IsAdt(t) || !Layouts::FixedAdt(t)) return false;
        auto ei = Layouts::EI(t);
        for (auto &v : ei->en->variants)
            if (!v.fields.empty()) Unsupported(b, "comparing ADTs with payloads");
        auto side = [&](g::Node *n) {
            auto l = ObjectLoc(n);
            if (IsPlainRef(l.t)) l = Deref(n, l);
            LoadTag(l);
        };
        side(b->left);
        side(b->right);
        C().Int(b->op == g::T_EQ ? intop::Eq : intop::Ne, false);
        return true;
    }

    // ---- print: a line built on a data stack (upstream TempBuilder) ----

    // print renders its whole line into a builder before writing it, as
    // upstream's print does (codegen_builtins.h, B_PRINT): a stack claimed
    // for the statement, whose top each piece is appended at. A line of any
    // length fits; a print inside an argument has a builder of its own; and
    // a line whose rendering aborts is never written.
    uint32_t bput = 0;      // (stk i32, h i64, n i64): n bytes from handle h at stk's top
    uint32_t bint = 0;      // (stk i32, v i64, signed i32): v in decimal at stk's top
    uint32_t quoted = 0;    // (stk i32, h i64, n i64): quoted and escaped at stk's top
    uint32_t writeout = 0;  // (h i64, n i64): n bytes from handle h to stdout

    void DefinePrintHelpers() {
        using V = ValType;
        // bput(stk, h, n): claims n bytes at the top of stack stk and copies
        // them from h.
        bput = m.AddFunc({ { V::I32, V::I64, V::I64 }, {} });
        {
            auto &c = m.FuncAt(bput).code;
            enum { stk, h, n };
            c.LocalGet(n);
            c.I64Eqz();
            c.If();
            c.Return();
            c.End();
            c.LocalGet(stk);
            c.LocalGet(n);
            c.I32WrapI64();
            c.Call(hs.pushslot);
            c.LocalGet(h);
            c.LocalGet(n);
            c.I32WrapI64();
            c.Call(hs.copy);
        }
        // bint(stk, v, signed): the digits are made in memory 0's message
        // scratch, then appended.
        bint = m.AddFunc({ { V::I32, V::I64, V::I32 }, {} });
        {
            auto &c = m.FuncAt(bint).code;
            enum { stk, v, sgn };
            c.LocalGet(stk);
            c.I64Const(mem0::MsgBuf);
            // Each branch pushes its own operands: a block takes none from
            // outside unless its type says so.
            c.LocalGet(sgn);
            c.If(BlockType::I32);
            c.LocalGet(v);
            c.I32Const(mem0::MsgBuf);
            c.Call(rt.fmt_i64);
            c.Else();
            c.LocalGet(v);
            c.I32Const(mem0::MsgBuf);
            c.Call(rt.fmt_u64);
            c.End();
            c.I32Const(mem0::MsgBuf);
            c.I32Sub();
            c.I64ExtendI32U();
            c.Call(bput);
        }
        // quoted(stk, h, n): a u8 array nested in a rendered value, in
        // double quotes with ", \, newline, carriage return and tab escaped,
        // every other byte as it is (upstream gs_fmt_quoted). Staged in
        // memory 0's message scratch, appended a part at a time.
        quoted = m.AddFunc({ { V::I32, V::I64, V::I64 }, {} }, { V::I64, V::I32, V::I32, V::I32 });
        {
            auto &c = m.FuncAt(quoted).code;
            enum { stk, h, n, i, b, esc, p };
            auto flush = [&] {
                c.LocalGet(stk);
                c.I64Const(mem0::MsgBuf);
                c.LocalGet(p);
                c.I32Const(mem0::MsgBuf);
                c.I32Sub();
                c.I64ExtendI32U();
                c.Call(bput);
                c.I32Const(mem0::MsgBuf);
                c.LocalSet(p);
            };
            auto byte = [&](auto push) {
                c.LocalGet(p);
                push();
                c.I32Store8();
                c.LocalGet(p);
                c.I32Const(1);
                c.I32Add();
                c.LocalSet(p);
            };
            c.I32Const(mem0::MsgBuf);
            c.LocalSet(p);
            byte([&] { c.I32Const('"'); });
            c.Block();
            c.Loop();
            c.LocalGet(i);
            c.LocalGet(n);
            c.Int(intop::GeU, true);
            c.BrIf(1);
            // Room for an escape and the closing quote.
            c.LocalGet(p);
            c.I32Const(mem0::MsgBuf + mem0::MsgBufSize - 3);
            c.Int(intop::GeU, false);
            c.If();
            flush();
            c.End();
            c.LocalGet(h);
            c.LocalGet(i);
            c.Int(intop::Add, true);
            c.Call(hs.Load(m, 0x2d));  // i32.load8_u
            c.LocalSet(b);
            // The escape letter, or 0 for a byte written as it is.
            c.I32Const(0);
            c.LocalSet(esc);
            for (auto [from, to] : { std::pair { '"', '"' }, { '\\', '\\' }, { '\n', 'n' },
                                     { '\r', 'r' }, { '\t', 't' } }) {
                c.LocalGet(b);
                c.I32Const(from);
                c.Int(intop::Eq, false);
                c.If();
                c.I32Const(to);
                c.LocalSet(esc);
                c.End();
            }
            c.LocalGet(esc);
            c.If();
            byte([&] { c.I32Const('\\'); });
            byte([&] { c.LocalGet(esc); });
            c.Else();
            byte([&] { c.LocalGet(b); });
            c.End();
            c.LocalGet(i);
            c.I64Const(1);
            c.Int(intop::Add, true);
            c.LocalSet(i);
            c.Br(0);
            c.End();
            c.End();
            byte([&] { c.I32Const('"'); });
            flush();
        }
        // writeout(h, n): WASI writes from memory 0 only, so the line goes
        // through the line buffer there, a buffer's worth per write.
        writeout = m.AddFunc({ { V::I64, V::I64 }, {} }, { V::I64 });
        {
            auto &c = m.FuncAt(writeout).code;
            enum { h, n, chunk };
            c.Block();
            c.Loop();
            c.LocalGet(n);
            c.I64Eqz();
            c.BrIf(1);
            // chunk = min(n, the buffer); select(x, y, c) is c ? x : y.
            c.LocalGet(n);
            c.I64Const(mem0::LineBufSize);
            c.LocalGet(n);
            c.I64Const(mem0::LineBufSize);
            c.Int(intop::LtU, true);
            c.Select();
            c.LocalSet(chunk);
            c.I64Const(mem0::LineBuf);
            c.LocalGet(h);
            c.LocalGet(chunk);
            c.I32WrapI64();
            c.Call(hs.copy);
            c.I32Const(1);  // stdout
            c.I32Const(mem0::LineBuf);
            c.LocalGet(chunk);
            c.I32WrapI64();
            c.Call(rt.write);
            c.LocalGet(h);
            c.LocalGet(chunk);
            c.Int(intop::Add, true);
            c.LocalSet(h);
            c.LocalGet(n);
            c.LocalGet(chunk);
            c.Int(intop::Sub, true);
            c.LocalSet(n);
            c.Br(0);
            c.End();
            c.End();
        }
    }

    // A u8 array or slice, which print renders as its bytes (§3.7).
    static bool IsBytes(g::TypeExpr *t) {
        if (IsPlainRef(t) || IsRzRef(t)) t = t->ref->sub;
        auto u8 = [](g::TypeExpr *e) { return e->kind == g::TY_INT && e->intstorage == g::IS_U8; };
        if (IsSlice(t)) return u8(t->sub);
        return t && t->kind == g::TY_ARRAY && u8(t->arr->sub);
    }

    // ---- Structural rendering (spec §3.7; upstream CodeGen::RenderLoc) ----

    uint32_t rstk = 0;  // the local holding the builder's stack
    std::map<std::string, uint32_t> renderfns;
    std::vector<std::pair<g::TypeExpr *, uint32_t>> pendingrenders;

    void RenderText(const std::string &s) {
        if (s.empty()) return;
        C().LocalGet(rstk);
        C().I64Const(StaticString(s));
        C().I64Const(int64_t(s.size()));
        C().Call(bput);
    }

    // A struct's render function, render(stk, h), one per type: a type that
    // reaches itself through references renders level by level through it,
    // as upstream's RenderFn does.
    uint32_t RenderFnOf(g::TypeExpr *t) {
        std::string key;
        t->Dump(key);
        auto it = renderfns.find(key);
        if (it != renderfns.end()) return it->second;
        auto index = m.AddFunc({ { ValType::I32, ValType::I64 }, {} });
        renderfns[key] = index;
        pendingrenders.push_back({ t, index });
        return index;
    }

    // A struct's positional literal with its type's name: `Name { a, b }`,
    // pads skipped (upstream RenderLoc, TY_STRUCT).
    void EmitRenderFn(g::TypeExpr *t, uint32_t index) {
        fn = index;
        spec = nullptr;
        locals.clear();
        memvars.clear();
        rzvars.clear();
        gstklocal.clear();
        labels.clear();
        scopes.clear();
        frame = 0;
        stknext = stkmax = 0;
        retdst = retrz = atparam = -1;
        nparams = 2;
        fp = NewLocal(Repr::I32);
        rstk = 0;
        Loc obj { false, 1, 0, t, false };
        if (IsAdt(t)) {
            // Each variant as its literal (upstream RenderLoc, TY_ENUM).
            auto ei = Layouts::EI(t);
            auto tag = NewLocal(Repr::I32);
            LoadTag(obj);
            C().LocalSet(tag);
            for (size_t vi = 0; vi < ei->en->variants.size(); vi++) {
                C().LocalGet(tag);
                C().I32Const(int32_t(vi));
                C().Int(intop::Eq, false);
                C().If();
                RenderVariant(ei, int(vi), PayloadLoc(obj, nullptr));
                C().End();
            }
            FinishFrame(true);
            return;
        }
        if (IsVariantT(t)) {
            RenderVariant(Layouts::EIVar(t), Layouts::VariantIndex(t), obj);
            FinishFrame(true);
            return;
        }
        auto si = Layouts::SI(t);
        auto &lo = lay.StructLayout(si);
        std::string name;
        t->Dump(name);
        RenderText(name + " { ");
        bool first = true;
        for (size_t i = 0; i < si->st->fields.size(); i++) {
            if (si->st->fields[i].ispad) continue;
            if (!first) RenderText(", ");
            first = false;
            RenderAt(nullptr, obj.At(lo.offs[i], si->ftypes[i]), si->ftypes[i], true);
        }
        RenderText(" }");
        // A value whose references lead back into itself never finishes
        // rendering: like upstream, it runs out of stack, which is reported
        // as the native call stack overflow (§3.7), so these count their
        // activations as recursive functions do.
        FinishFrame(true);
    }

    // A variant's positional literal (upstream RenderVariant): a payload-less
    // one as `Enum.Name`, else `Name { a, b }`.
    void RenderVariant(g::EnumInst *ei, int vi, const Loc &payload) {
        auto &v = ei->en->variants[vi];
        if (v.fields.empty()) {
            RenderText(g::cat(ei->en->name, ".", v.name));
            return;
        }
        auto &lo = lay.VariantLayout(ei, vi);
        RenderText(g::cat(v.name, " { "));
        bool first = true;
        for (size_t i = 0; i < v.fields.size(); i++) {
            if (v.fields[i].ispad) continue;
            if (!first) RenderText(", ");
            first = false;
            auto ft = ei->vftypes[vi][i];
            RenderAt(nullptr, payload.At(lo.offs[i], ft), ft, true);
        }
        RenderText(" }");
    }

    // The elements of a sequence: a u8 run as its bytes, quoted when nested
    // in a value; anything else as `[a, b, c]`.
    void RenderSeq(const g::Node *at, const Seq &s, bool nested) {
        if (s.et->kind == g::TY_INT && s.et->intstorage == g::IS_U8) {
            C().LocalGet(rstk);
            C().LocalGet(s.data);
            s.len();
            C().Call(nested ? quoted : bput);
            return;
        }
        RenderText("[");
        // The count is read once, as upstream's rendering does.
        auto n = NewLocal(Repr::I64), i = NewLocal(Repr::I64);
        s.len();
        C().LocalSet(n);
        C().I64Const(0);
        C().LocalSet(i);
        Open(LK_PLAIN, Repr::None);
        Open(LK_PLAIN, Repr::None, nullptr, nullptr, true);
        C().LocalGet(i);
        C().LocalGet(n);
        C().Int(intop::GeU, true);
        C().BrIf(1);
        C().LocalGet(i);
        C().I64Eqz();
        C().I32Eqz();
        C().If();
        RenderText(", ");
        C().End();
        RenderAt(at, ElemLoc(s, i), s.et, true);
        C().LocalGet(i);
        C().I64Const(1);
        C().Int(intop::Add, true);
        C().LocalSet(i);
        C().Br(0);
        Close();
        Close();
        RenderText("]");
    }

    // An integer of type t on the Wasm stack, appended in decimal.
    void RenderInt(g::TypeExpr *t) {
        if (!IsWide(t)) {
            if (IsSigned(t)) C().I64ExtendI32S();
            else C().I64ExtendI32U();
        }
        auto v = NewLocal(Repr::I64);
        C().LocalSet(v);
        C().LocalGet(rstk);
        C().LocalGet(v);
        C().I32Const(IsSigned(t));
        C().Call(bint);
    }

    // A float of type t on the Wasm stack, appended as upstream spells it
    // (gs_fmt_f32 or gs_fmt_f64, floatfmt.h), through the message scratch.
    void RenderFloat(g::TypeExpr *t) {
        auto f32 = t->fltstorage == g::FS_F32;
        auto v = NewLocal(f32 ? Repr::F32 : Repr::F64);
        C().LocalSet(v);
        C().LocalGet(rstk);
        C().I64Const(mem0::MsgBuf);
        C().I32Const(mem0::MsgBuf);
        C().LocalGet(v);
        C().Call(f32 ? ff.fmt_f32 : ff.fmt_f64);
        C().I64ExtendI32U();
        C().Call(bput);
    }

    // A bool on the Wasm stack, appended as true or false.
    void RenderBool() {
        C().If();
        RenderText("true");
        C().Else();
        RenderText("false");
        C().End();
    }

    // The value of type t at l, rendered at the builder's top (upstream
    // RenderLoc): integers in decimal, bools as true or false, a reference
    // as its pointee and a null optional as `null`, arrays and slices (a
    // resizable's header has a slice's layout), structs through their render
    // function.
    void RenderAt(const g::Node *at, Loc l, g::TypeExpr *t, bool nested) {
        if (IsPlainRef(l.t) && !IsPlainRef(t)) l = Deref(at, l);
        switch (t->kind) {
            case g::TY_INT: {
                if (t->intstorage == g::IS_VARINT) break;
                Load(at, l);
                RenderInt(t);
                return;
            }
            case g::TY_BOOL:
                Load(at, l);
                RenderBool();
                return;
            case g::TY_FLT:
                Load(at, l);
                RenderFloat(t);
                return;
            case g::TY_REF: {
                if (!IsPlainRef(t)) break;
                auto h = NewLocal(Repr::I64);
                Load(at, l);
                C().LocalSet(h);
                Loc p { false, h, 0, t->ref->sub, false };
                if (t->ref->optional) {
                    C().LocalGet(h);
                    C().I64Eqz();
                    C().If();
                    RenderText("null");
                    C().Else();
                    RenderAt(at, p, t->ref->sub, nested);
                    C().End();
                    return;
                }
                RenderAt(at, p, t->ref->sub, nested);
                return;
            }
            case g::TY_SLICE:
            case g::TY_ARRAY: {
                Seq s;
                s.et = t->kind == g::TY_SLICE ? t->sub : t->arr->sub;
                if (!Layouts::Known(s.et)) break;
                s.data = NewLocal(Repr::I64);
                if (Layouts::IsLimited(t)) {
                    l.t = t;
                    s = LimSeq(at, l);
                } else if (Layouts::IsFixedArray(t)) {
                    HandleOf(l);
                    C().LocalSet(s.data);
                    s.mem0 = l.Direct();
                    auto k = t->arr->size;
                    s.len = [this, k] { C().I64Const(k); };
                } else if (t->kind == g::TY_SLICE || IsRz(t)) {
                    Load(at, l.At(0, I64T()));
                    C().LocalSet(s.data);
                    auto n = NewLocal(Repr::I64);
                    Load(at, l.At(8, I64T()));
                    C().LocalSet(n);
                    s.len = [this, n] { C().LocalGet(n); };
                } else {
                    break;
                }
                RenderSeq(at, s, nested);
                return;
            }
            case g::TY_ENUM:
            case g::TY_VARIANT:
            case g::TY_STRUCT: {
                if (!IsAggregate(t) && !IsFrameObj(t)) break;
                C().LocalGet(rstk);
                HandleOf(l);
                C().Call(RenderFnOf(t));
                return;
            }
            default: break;
        }
        if (!at) throw g::CompileError { g::cat("goose-wasm: unsupported: rendering a ", TypeKindName(t)) };
        Unsupported(at, g::cat("print of a value of type ", TypeKindName(t)));
    }

    // A print argument rendered structurally: its place, or for a value
    // with none (a call's result, a literal) a temporary.
    void RenderArg(g::Node *a) {
        auto t = a->exprtype;
        if (IsPlainRef(t)) {
            // A reference value: its pointee, or null.
            auto h = NewLocal(Repr::I64);
            Value(a);
            C().LocalSet(h);
            auto slot = FrameBytes(8);
            slot.t = t;
            Store(a, slot, [&] { C().LocalGet(h); });
            RenderAt(a, slot, t, false);
            return;
        }
        auto st = IsRzRef(t) ? t->ref->sub : t;
        if (IsRz(st) || IsSlice(st) || Layouts::IsFixedArray(st)) {
            auto s = SeqOf(a);
            if (!Layouts::Known(s.et)) Unsupported(a, "print of these elements");
            RenderSeq(a, s, false);
            return;
        }
        if (IsAggregate(st) || IsFrameObj(st)) {
            Loc l;
            if (IsFrameObj(st)) {
                auto rz = RzOf(a);
                l = rz.hdr;
                l.t = st;
            } else {
                l = ObjectLoc(a);
            }
            RenderAt(a, l, st, false);
            return;
        }
        Unsupported(a, g::cat("print of a value of type ", TypeKindName(t)));
    }

    // print(args...): the whole line rendered into a builder, newline
    // included, then written (spec §3.7; upstream codegen_builtins.h,
    // B_PRINT). Each argument is evaluated just before it is rendered.
    void Print(g::Call *call) {
        PushScope();
        uint32_t base = 0;
        auto stk = ClaimStack(base);
        RenderArgs(call, stk);
        auto saved = rstk;
        rstk = stk;
        RenderText("\n");
        // The builder's bytes, from its base to its top.
        C().LocalGet(stk);
        C().I32Const(1);
        C().I32Add();
        C().I64ExtendI32U();
        C().I64Const(32);
        C().Int(intop::Shl, true);
        C().LocalGet(base);
        C().I64ExtendI32U();
        C().Int(intop::Or, true);
        TopAddr(stk);
        C().I32Load(mem0::Tops);
        C().LocalGet(base);
        C().I32Sub();
        C().I64ExtendI32U();
        C().Call(writeout);
        rstk = saved;
        PopScope();
    }

    // format(out, ...) (§3.7): the text print would write, without its
    // newline, appended to out, a growable u8 array. A resizable one is
    // topmost on its stack, so the text is built at its end; a limited one
    // takes a copy under the capacity check.
    void Format(g::Call *call) {
        Loc l;
        if (LimRecv(call, l)) {
            PushScope();
            uint32_t base = 0;
            auto stk = ClaimStack(base);
            RenderArgs(call, stk, 1);
            auto cnt = NewLocal(Repr::I64), ol = NewLocal(Repr::I64), nl = NewLocal(Repr::I64);
            TopAddr(stk);
            C().I32Load(mem0::Tops);
            C().LocalGet(base);
            C().I32Sub();
            C().I64ExtendI32U();
            C().LocalSet(cnt);
            LimLen(call, l);
            C().LocalTee(ol);
            C().LocalGet(cnt);
            C().Int(intop::Add, true);
            C().LocalSet(nl);
            CapCheck(call, l, nl);
            LimElem(l, ol);
            C().LocalGet(stk);
            C().I32Const(1);
            C().I32Add();
            C().I64ExtendI32U();
            C().I64Const(32);
            C().Int(intop::Shl, true);
            C().LocalGet(base);
            C().I64ExtendI32U();
            C().Int(intop::Or, true);
            C().LocalGet(cnt);
            C().I32WrapI64();
            C().Call(hs.copy);
            LimSetLen(call, l, [&] { C().LocalGet(nl); });
            PopScope();
            return;
        }
        auto r = Receiver(call);
        auto len = r.rz.hdr.At(8, I64T());
        auto cnt = NewLocal(Repr::I64);
        auto before = NewLocal(Repr::I32);
        TopAddr(r.rz.stk);
        C().I32Load(mem0::Tops);
        C().LocalSet(before);
        RenderArgs(call, r.rz.stk, 1);
        TopAddr(r.rz.stk);
        C().I32Load(mem0::Tops);
        C().LocalGet(before);
        C().I32Sub();
        C().I64ExtendI32U();
        C().LocalSet(cnt);
        Store(call, len, [&] {
            Load(call, len);
            C().LocalGet(cnt);
            C().Int(intop::Add, true);
        });
    }

    // str(...) (§3.7): the text print would write, without its newline, as
    // a u8[>..] built at the top of stack stk; its count on the Wasm stack.
    void Str(g::Call *call, uint32_t stk) {
        auto before = NewLocal(Repr::I32);
        TopAddr(stk);
        C().I32Load(mem0::Tops);
        C().LocalSet(before);
        RenderArgs(call, stk);
        TopAddr(stk);
        C().I32Load(mem0::Tops);
        C().LocalGet(before);
        C().I32Sub();
        C().I64ExtendI32U();
    }

    // print's and str's arguments rendered at the top of stack stk: text as
    // it is, values structurally.
    void RenderArgs(g::Call *call, uint32_t stk, size_t first = 0) {
        // A user `format` overload renders its type instead (§3.7).
        if (!call->fmtspecs.empty()) Unsupported(call, "print through a format overload");
        auto saved = rstk;
        rstk = stk;
        auto args = call->ArgNodes();
        for (size_t ai = first; ai < args.size(); ai++) {
            auto a = args[ai];
            auto t = a->exprtype;
            if (a->nkind == g::NK_StrLit) {
                RenderText(static_cast<g::StrLit *>(a)->val);
            } else if (IsBytes(t)) {
                auto s = SeqOf(a);
                C().LocalGet(stk);
                C().LocalGet(s.data);
                s.len();
                C().Call(bput);
            } else if (IsInt(t)) {
                Expr(a, t);
                RenderInt(t);
            } else if (IsFlt(t)) {
                Expr(a, t);
                RenderFloat(t);
            } else if (t && t->kind == g::TY_BOOL) {
                Expr(a, t);
                RenderBool();
            } else {
                RenderArg(a);
            }
        }
        rstk = saved;
    }
};

}  // namespace goosewasm
