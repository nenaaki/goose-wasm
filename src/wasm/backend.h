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
    uint32_t rfglobal = 0; // global: the long-distance return in flight, its target's id or 0
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
    // Of several results, those that are aggregates go where the caller
    // says, one handle parameter each; -1 for one returned as a value.
    std::vector<int64_t> retdsts;
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
        std::vector<g::Block *> defers;                      // defer bodies run past so far
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
        g::TypeExpr *vart = nullptr; // for a variable-size value: its type, built on rzstk
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
        rfglobal = m.AddGlobal(ValType::I32, true, 0);
        FindNeededGlobals();
        FindFreeVars();
        auto initfn = EmitGlobalInit();
        auto mainfn = FuncOf(mainspec, mainspec->body);
        // Functions, then the render functions printing reaches, which reach
        // others in turn.
        while (!pending.empty() || !pendingrenders.empty() || !pendingsizes.empty()) {
            if (!pendingsizes.empty()) {
                auto [t, index] = pendingsizes.back();
                pendingsizes.pop_back();
                EmitSizeFn(t, index);
                continue;
            }
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
        if (t->kind == g::TY_SLICE || IsFlat(t)) return true;
        return (t->kind == g::TY_STRUCT || Layouts::IsFixedArray(t) || Layouts::IsLimited(t) ||
                t->kind == g::TY_ENUM ||
                t->kind == g::TY_VARIANT) &&
               Layouts::Known(t);
    }
    static bool IsSlice(g::TypeExpr *t) { return t && t->kind == g::TY_SLICE; }
    // A variable-size array T[] of fixed-size elements held on its own (a
    // variable, a parameter, a result): its elements lie on a data stack,
    // claimed where it is declared, and its place holds a view of them, as
    // a slice's { data, len } (docs/design/value-representation.md). Its
    // image's length prefix is never needed apart: inside another value it
    // makes that value variable-size, which is unsupported.
    static bool IsFlat(g::TypeExpr *) { return false; }
    // A slice, or a flat array's view.
    static bool IsView(g::TypeExpr *t) { return IsSlice(t) || IsFlat(t); }
    static g::TypeExpr *ViewElem(g::TypeExpr *t) { return IsSlice(t) ? t->sub : t->arr->sub; }
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
        if (IsRz(t) || Layouts::IsFatRef(t)) size = 16;
        else if (IsRoot(t)) size = RootDescSize(t);
        else if (IsFrameObj(t)) size = FrameObjSize(t);
        else if (IsFlat(t)) size = 16;
        else if (IsVar(t)) size = 8;
        else if (IsAggregate(t) || IsPlainRef(t) || IsInt(t) || IsFlt(t) || t->kind == g::TY_BOOL) size = lay.FixedSize(t);
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
        stkconsts.clear();
        fls.clear();
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
        if (d->byref || d->reusable == g::RU_SLICES || d->results || (d->defs.size() > 1 && d->inits.size() == 1))
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
            if (d->reusable) {
                // Its freelist: an empty resizable on a dedicated stack.
                if (!IsRz(v->type)) Unsupported(d, "this reusable global");
                while (staticdata.size() % 8) staticdata.push_back('\0');
                auto hdr = Loc::Abs(mem0::StaticData + int64_t(staticdata.size()), nullptr);
                staticdata.append(16, '\0');
                auto k = gstacks++;
                gfls[v] = { hdr, k };
                Store(d, hdr.At(0, I64T()), [&] { C().I64Const(int64_t(k + 1) << 32); });
            }
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
        if (Layouts::IsFatRef(v->type)) {
            // A reference to a resizable value: { header, stack }.
            if (init) StoreFatRef(init, slot);
            return;
        }
        if (IsDynLim(v->type)) {
            if (!init) Unsupported(d, "a T[..] without an initializer");
            auto k = gstacks++;
            BuildDynLim(d, init, StackConstLocal(k), v->type);
            Store(d, slot.At(0, I64T()), [&] { C().I64Const(int64_t(k + 1) << 32); });
            return;
        }
        if (IsVar(v->type) && !IsDynLim(v->type)) {
            auto k = gstacks++;
            auto stk = StackConstLocal(k);
            if (init) BuildVar(init, stk, v->type);
            else BuildDefault(d, stk, v->type);
            Store(d, slot.At(0, I64T()), [&] { C().I64Const(int64_t(k + 1) << 32); });
            return;
        }
        if (IsFlat(v->type)) {
            // A T[] global's elements: on a dedicated stack, from its base.
            auto k = gstacks++;
            auto stk = StackConstLocal(k);
            auto n = NewLocal(Repr::I64);
            if (init) BuildRzInto(init, stk, v->type->arr->sub);
            else C().I64Const(0);
            C().LocalSet(n);
            StoreSlice(d, slot, [&] { C().I64Const(int64_t(k + 1) << 32); }, [&] { C().LocalGet(n); });
            return;
        }
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
        // What a call passes for a callee's free variable is passed by
        // reference too, the optimizer's copy of it included (fvremap).
        for (auto sp : specs)
            for (auto [k, c] : calls[sp]) {
                auto it = freevars.find(k);
                if (it == freevars.end()) continue;
                for (auto v : it->second)
                    if (auto a = c->FreeVarArg(v)) captured.insert(a);
            }
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
            // A format overload may take a printed variable by reference.
            if (n->nkind == g::NK_Call && !static_cast<g::Call *>(n)->fmtspecs.empty())
                for (auto a : static_cast<g::Call *>(n)->ArgNodes()) root(a);
            Kids(n, walk);
        };
        walk(body);
    }

    // A resizable array (spec §5) of elements this backend can lay out.
    static bool IsRz(g::TypeExpr *t) {
        return t && t->kind == g::TY_ARRAY &&
               (t->arr->akind == g::A_GROW || t->arr->akind == g::A_GROWSHRINK) &&
               (Layouts::Known(t->arr->sub) || IsVar(t->arr->sub));
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

    // Of a specialization's several results, whether r is one built where
    // the caller says (an aggregate other than a T[]'s view).
    bool MultiAggregate(g::FnSpec *sp, g::TypeExpr *r) {
        return sp->rets.size() > 1 && IsAggregate(r) && !IsFlat(r);
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
        if (sp->rets.size() == 1 && IsAggregate(sp->rets[0]) && !IsFlat(sp->rets[0])) {
            t.params.push_back(ValType::I64);
        } else if (sp->rets.size() == 1 && (IsRz(sp->rets[0]) || IsVar(sp->rets[0]))) {
            // A resizable result is built at the top of the destination
            // stack the caller names; the callee returns its count (§7.3).
            t.params.push_back(ValType::I32);
            t.results.push_back(ValType::I64);
        } else {
            for (auto r : sp->rets) {
                if (MultiAggregate(sp, r)) t.params.push_back(ValType::I64);
                else t.results.push_back(ValOf(ReprOf(at, r)));
            }
        }
        for (size_t pix = 0; pix < sp->params.size(); pix++) {
            auto p = sp->params[pix];
            if (IsAggregate(p->type) || IsVar(p->type)) {
                t.params.push_back(ValType::I64);
                continue;
            }
            if (IsRzRef(p->type)) {
                t.params.push_back(ValType::I64);
                t.params.push_back(ValType::I32);
                if (IsPoolParam(sp, pix)) {
                    t.params.push_back(ValType::I64);
                    t.params.push_back(ValType::I32);
                }
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
            if (IsPoolVar(v)) {
                t.params.push_back(ValType::I32);
                t.params.push_back(ValType::I64);
                t.params.push_back(ValType::I32);
            } else if (IsRz(v->type) || IsFrameObj(v->type) || IsRzRef(v->type)) t.params.push_back(ValType::I32);
            else if (!IsAggregate(v->type) && !IsInt(v->type) && !IsFlt(v->type) && !IsRzRef(v->type) &&
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
        stkconsts.clear();
        fls.clear();
        labels.clear();
        scopes.clear();
        frame = 0;
        stknext = stkmax = 0;
        bool aggret = sp->rets.size() == 1 && IsAggregate(sp->rets[0]) && !IsFlat(sp->rets[0]);
        FindAddrTaken(sp->body);
        std::vector<std::pair<g::VarDef *, uint32_t>> spills;
        uint32_t pi = 1;  // after SP
        retdst = aggret ? int64_t(pi++) : -1;
        bool rzret = sp->rets.size() == 1 && (IsRz(sp->rets[0]) || IsVar(sp->rets[0]));
        retrz = rzret ? int64_t(pi++) : -1;
        retdsts.clear();
        if (sp->rets.size() > 1)
            for (auto r : sp->rets) retdsts.push_back(MultiAggregate(sp, r) ? int64_t(pi++) : -1);
        for (size_t pix = 0; pix < sp->params.size(); pix++) {
            auto p = sp->params[pix];
            if (addrtaken.count(p)) {
                spills.push_back({ p, pi++ });
                continue;
            }
            if (IsVar(p->type)) {
                // Its image, a copy the caller made.
                memvars[p] = Loc { false, pi++, 0, p->type, false };
            } else if (IsAggregate(p->type)) {
                memvars[p] = Loc { false, pi++, 0, p->type, true };
            } else if (IsRzRef(p->type)) {
                rzvars[p] = RzVar { Loc { false, pi, 0, nullptr }, pi + 1 };
                // A frame object's fixed fields are read through the handle.
                if (IsFrameObj(p->type->ref->sub))
                    memvars[p] = Loc { false, pi, 0, p->type->ref->sub, false };
                pi += 2;
                if (IsPoolParam(sp, pix)) {
                    fls[p] = RzVar { Loc { false, pi, 0, nullptr }, pi + 1 };
                    pi += 2;
                }
            } else {
                locals[p] = pi++;
            }
        }
        for (auto v : freevars[sp]) {
            if (IsRz(v->type) || IsFrameObj(v->type) || IsRzRef(v->type)) {
                // A resizable one, or a reference to one, as its header and
                // stack: the same either way.
                rzvars[v] = RzVar { Loc { false, pi, 0, nullptr, false }, pi + 1 };
                auto ot = IsRzRef(v->type) ? v->type->ref->sub : v->type;
                if (IsFrameObj(ot)) memvars[v] = Loc { false, pi, 0, ot, false };
                pi += 2;
                if (IsPoolVar(v)) {
                    fls[v] = RzVar { Loc { false, pi, 0, nullptr }, pi + 1 };
                    pi += 2;
                }
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
        } else if (rzret && IsVar(sp->rets[0])) {
            BuildVar(sp->body, uint32_t(retrz), sp->rets[0]);
            C().I64Const(0);
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
        auto size = IsFlat(t) ? 16 : lay.FixedSize(t);
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
                  const std::function<void(size_t)> *argfn = nullptr, const std::vector<Loc> *mdst = nullptr) {
        if (!c->dispatch.empty() && !over) return Dispatch(c, dst, rzstk);
        // A trailing block is bound in the callee's specialization, not
        // passed (§7.6).
        if (c->fvbody) Unsupported(c, "a call of a function value");
        auto sp = over ? over : c->spec;
        auto index = FuncOf(sp, c);
        auto args = c->ValueArgNodes();
        if (args.size() != sp->params.size()) Unsupported(c, "a call's argument count");
        bool aggret = sp->rets.size() == 1 && IsAggregate(sp->rets[0]) && !IsFlat(sp->rets[0]);
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
        if (sp->rets.size() == 1 && (IsRz(sp->rets[0]) || IsVar(sp->rets[0]))) {
            if (rzstk < 0) Unsupported(c, "a resizable result here");
            C().LocalGet(uint32_t(rzstk));
        } else if (rzstk >= 0) {
            Unsupported(c, "this call as a resizable value");
        }
        if (aggret) {
            if (!dst) Unsupported(c, "an aggregate result here");
            Handle(*dst);
        } else if (sp->rets.size() > 1) {
            // Several results: an aggregate's goes where the caller says,
            // the first to dst when there is one, an unwanted one to a
            // temporary.
            size_t k = 0;
            for (size_t i = 0; i < sp->rets.size(); i++) {
                if (!MultiAggregate(sp, sp->rets[i])) continue;
                if (mdst && k < mdst->size()) Handle((*mdst)[k]);
                else if (i == 0 && dst) Handle(*dst);
                else Handle(FrameSlot(sp->rets[i]));
                k++;
            }
        } else if (dst) {
            Unsupported(c, "this call as an aggregate");
        }
        for (size_t i = 0; i < args.size(); i++) {
            auto pt = sp->params[i]->type;
            if (argfn) {
                (*argfn)(i);
            } else if (IsVar(pt)) {
                HandleOf(TempVar(args[i], pt));
            } else if (IsAggregate(pt)) {
                auto copy = FrameSlot(pt);
                Construct(args[i], copy);
                Handle(copy);
            } else if (IsRzRef(pt)) {
                auto rz = RzOf(args[i]);
                Handle(rz.hdr);
                C().LocalGet(rz.stk);
                if (IsPoolParam(sp, i)) {
                    auto fl = PoolOf(args[i]);
                    Handle(fl.hdr);
                    C().LocalGet(fl.stk);
                }
            } else {
                Expr(args[i], pt);
            }
        }
        for (auto v : freevars[sp]) {
            auto a = c->FreeVarArg(v);
            if (IsRz(a->type) || IsFrameObj(a->type) || IsRzRef(a->type)) {
                auto it = rzvars.find(a);
                if (it == rzvars.end()) Unsupported(c, g::cat("the free variable ", a->name));
                Handle(it->second.hdr);
                C().LocalGet(it->second.stk);
                if (IsPoolVar(v)) {
                    auto ft = fls.find(a);
                    if (ft == fls.end()) Unsupported(c, g::cat("the pool ", a->name));
                    Handle(ft->second.hdr);
                    C().LocalGet(ft->second.stk);
                }
                continue;
            }
            auto it = memvars.find(a);
            if (it == memvars.end()) Unsupported(c, g::cat("the free variable ", a->name));
            Handle(it->second);
        }
        if (Located(sp)) CallerLoc(c->line);
        C().Call(index);
        RfCheck(c, sp);
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
        if (!IsAdt(et) || !(Layouts::FixedAdt(et) || IsVar(et))) Unsupported(c, "a dispatch on this ADT");
        if (sp0->rets.size() > 1) Unsupported(c, "this dispatch's results");
        std::vector<std::function<void()>> pass(args.size());
        // The scrutinee as evaluated (upstream EmitDispatch): a fixed-mode
        // ADT is copied then, through a reference too; a variable-mode one
        // stays where it is for the cases taking it by reference, and is
        // copied for those taking a payload by value, which must not see
        // what later arguments write.
        Loc scr, orig;
        bool byvalue = false;
        for (auto k : c->dispatch) byvalue |= !IsPlainRef(k->params[pos]->type);
        for (size_t i = 0; i < args.size(); i++) {
            auto a = args[i];
            if (i == pos) {
                if (IsVar(et)) {
                    scr = orig = ObjectLoc(a);
                    scr.t = orig.t = et;
                    if (byvalue) {
                        uint32_t base = 0;
                        auto stk = ClaimStack(base);
                        auto h = NewLocal(Repr::I64);
                        TopHandle(stk);
                        C().LocalSet(h);
                        CopyImage(orig, stk);
                        scr = Loc { false, h, 0, et, false };
                    }
                } else {
                    scr = FrameSlot(et);
                    if (IsPlainRef(st)) {
                        auto src = ObjectLoc(a);
                        src.t = et;
                        Copy(scr, src);
                    } else {
                        Construct(a, scr);
                    }
                    orig = scr;
                    if (IsPlainRef(st)) orig = ObjectLoc(a), orig.t = et;
                }
                continue;
            }
            auto pt = sp0->params[i]->type;
            if (IsVar(pt)) {
                auto img = TempVar(a, pt);
                pass[i] = [this, img] { HandleOf(img); };
            } else if (IsAggregate(pt)) {
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
        if (sp0->rets.size() == 1 && (IsRz(sp0->rets[0]) || IsVar(sp0->rets[0])))
            r = Repr::I64;  // built on the stack named, its count returned
        else if (sp0->rets.size() == 1 && !IsAggregate(sp0->rets[0]))
            r = ReprOf(c, sp0->rets[0]);
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
                    HandleOf(PayloadLoc(orig, pt->ref->sub));
                } else if (IsVar(pt) || (IsVar(et) && !IsAggregate(pt))) {
                    // A variable-size payload, of the copy.
                    HandleOf(PayloadLoc(scr, pt));
                } else {
                    // A by-value case: a copy of its own.
                    auto copy = FrameSlot(pt);
                    Copy(copy, PayloadLoc(scr, pt));
                    Handle(copy);
                }
            };
            CallSpec(c, dst, rzstk, sp, &argfn);
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
            if (v && v->isglobal && Layouts::IsFatRef(v->type)) {
                auto f = GlobalSlot(v);
                auto h = NewLocal(Repr::I64), stk = NewLocal(Repr::I32);
                Load(n, f.At(0, I64T()));
                C().LocalSet(h);
                Load(n, f.At(8, I64T()));
                C().I32WrapI64();
                C().LocalSet(stk);
                return RzVar { Loc { false, h, 0, nullptr, false }, stk };
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
        // A choice between resizable values (`if c { a } else { b }`):
        // the chosen one's header and stack.
        if (n->nkind == g::NK_IfExpr || n->nkind == g::NK_Block) {
            auto h = NewLocal(Repr::I64), stk = NewLocal(Repr::I32);
            std::function<void(g::Node *)> pick = [&](g::Node *x) {
                if (x->nkind == g::NK_Block) {
                    auto b = static_cast<g::Block *>(x);
                    for (auto st : b->stmts) Stmt(st);
                    if (!b->tail) {
                        C().Unreachable();
                        return;
                    }
                    return pick(b->tail);
                }
                if (x->nkind == g::NK_IfExpr) {
                    auto i = static_cast<g::IfExpr *>(x);
                    if (!i->elseb) Unsupported(x, "an if of a value without else");
                    Truth(i->cond);
                    OpenIf(Repr::None);
                    pick(i->thenb);
                    C().Else();
                    pick(i->elseb);
                    Close();
                    return;
                }
                auto rz = RzOf(x);
                Handle(rz.hdr);
                C().LocalSet(h);
                C().LocalGet(rz.stk);
                C().LocalSet(stk);
            };
            pick(n);
            return RzVar { Loc { false, h, 0, nullptr, false }, stk };
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
        if (t->kind == g::TY_INT)
            return t->intstorage == g::IS_I64 || t->intstorage == g::IS_U64 || t->intstorage == g::IS_VARINT
                       ? Repr::I64 : Repr::I32;
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
        // A varint's value is the i64 it encodes (§3.6).
        auto vi = [&](g::TypeExpr *t) { return t && t->kind == g::TY_INT && t->intstorage == g::IS_VARINT; };
        if (vi(from)) from = I64T();
        if (vi(to)) to = I64T();
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
        if (l.t && l.t->kind == g::TY_INT && l.t->intstorage == g::IS_VARINT) {
            // A varint reads as the i64 it encodes (§3.6).
            DefineLeb();
            HandleOf(l);
            C().Call(zigfn);
            return;
        }
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
        if (l.t && l.t->kind == g::TY_INT && l.t->intstorage == g::IS_VARINT)
            Unsupported(n, "a store into a varint");
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
                if (IsVar(v->type)) {
                    // Its image's handle, which its slot holds.
                    auto h = NewLocal(Repr::I64);
                    Load(n, out.At(0, I64T()));
                    C().LocalSet(h);
                    out = Loc { false, h, 0, v->type, false };
                }
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
            if (IsVar(base.t) && base.t->kind == g::TY_STRUCT) {
                out = FieldLoc(base, base.t, d->fieldidx);
                return true;
            }
            if (IsRoot(base.t)) {
                if (IsRzField(Layouts::SI(base.t)->ftypes[d->fieldidx])) return false;
                out = RootField(n, base, d->fieldidx);
                return true;
            }
            if (base.t->kind == g::TY_VARIANT) {
                // A variant's payload field (a match binder, a variant value).
                auto ei = Layouts::EIVar(base.t);
                auto vi = Layouts::VariantIndex(base.t);
                if (!Layouts::Known(base.t)) out = PayloadField(base, ei, vi, d->fieldidx);
                else out = base.At(lay.VariantLayout(ei, vi).offs[d->fieldidx], ei->vftypes[vi][d->fieldidx]);
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
        // A call giving a reference names the storage it refers to, though
        // the use reads it as its pointee (§3.8).
        if (auto rt = RefResult(obj)) {
            auto h = NewLocal(Repr::I64);
            if (obj->nkind == g::NK_Call) {
                CallSpec(static_cast<g::Call *>(obj));
            } else {
                auto ib = static_cast<g::InlineBlock *>(obj);
                Open(LK_INLINE, Repr::I64, rt, ib->sf);
                BlockValue(ib->body, rt);
                Close();
            }
            C().LocalSet(h);
            return Loc { false, h, 0, rt->ref->sub, false };
        }
        if (IsVar(ot)) return VarPlace(obj, ot);
        if (!IsAggregate(ot)) Unsupported(obj, g::cat("an object ", KindName(obj->nkind)));
        base = FrameSlot(ot);
        // A slice taken where a T[] is read is viewed, not copied: its view
        // has a T[]'s layout.
        if (IsFlat(ot) && obj->nkind == g::NK_SliceExpr) {
            SliceInto(static_cast<g::SliceExpr *>(obj), base);
            return base;
        }
        Construct(obj, base);
        return base;
    }

    // The reference type a call (or an inlined one) gives, where it gives
    // a single plain reference; null otherwise.
    static g::TypeExpr *RefResult(g::Node *n) {
        if (n->nkind == g::NK_Call) {
            auto c = static_cast<g::Call *>(n);
            if (c->builtin < 0 && !c->fvbody && (c->spec || !c->dispatch.empty()) &&
                c->rettypes.size() == 1 && IsPlainRef(c->rettypes[0]))
                return c->rettypes[0];
        }
        if (n->nkind == g::NK_InlineBlock) {
            auto ib = static_cast<g::InlineBlock *>(n);
            if (ib->spec && ib->spec->rets.size() == 1 && IsPlainRef(ib->spec->rets[0]))
                return ib->spec->rets[0];
        }
        return nullptr;
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
        if (IsFlat(dst.t)) return FlatInto(n, dst);
        // A call (or an inlined one) giving a fixed or limited array where
        // a slice is expected: built as its own type, then viewed (§3.10).
        if (IsSlice(dst.t)) {
            g::TypeExpr *own = nullptr;
            if (n->nkind == g::NK_Call && static_cast<g::Call *>(n)->builtin < 0 &&
                static_cast<g::Call *>(n)->rettypes.size() == 1)
                own = static_cast<g::Call *>(n)->rettypes[0];
            if (n->nkind == g::NK_InlineBlock && static_cast<g::InlineBlock *>(n)->spec &&
                static_cast<g::InlineBlock *>(n)->spec->rets.size() == 1)
                own = static_cast<g::InlineBlock *>(n)->spec->rets[0];
            if (own && (Layouts::IsFixedArray(own) || Layouts::IsLimited(own)) && IsAggregate(own)) {
                auto tmp = FrameSlot(own);
                Construct(n, tmp);
                auto sq = SeqAt(n, tmp);
                StoreSlice(n, dst, [&] { C().LocalGet(sq.data); }, sq.len);
                return;
            }
            // A variable-size array result: its image, a temporary of the
            // caller's statement.
            if (own && IsVarArray(own)) {
                auto sq = ImageSeq(n, TempVar(n, own));
                StoreSlice(n, dst, [&] { C().LocalGet(sq.data); }, sq.len);
                return;
            }
        }
        // A call (or an inlined one) giving another kind of array than a
        // limited destination: built as its own type, then its elements
        // copied under the capacity check (§4.2).
        if (IsAnyLim(dst.t)) {
            g::TypeExpr *own = nullptr;
            if (n->nkind == g::NK_Call && static_cast<g::Call *>(n)->builtin < 0 &&
                static_cast<g::Call *>(n)->rettypes.size() == 1)
                own = static_cast<g::Call *>(n)->rettypes[0];
            if (n->nkind == g::NK_InlineBlock && static_cast<g::InlineBlock *>(n)->spec &&
                static_cast<g::InlineBlock *>(n)->spec->rets.size() == 1)
                own = static_cast<g::InlineBlock *>(n)->spec->rets[0];
            if (own && IsAggregate(own) && (IsView(own) || own->kind == g::TY_ARRAY) &&
                !Layouts::Same(own, dst.t) && !(Layouts::IsLimited(own) && own->arr->size == dst.t->arr->size)) {
                auto tmp = FrameSlot(own);
                Construct(n, tmp);
                return LimFromSeq(n, SeqAt(n, tmp), dst);
            }
        }
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
            case g::NK_Unary: {
                // &x where a value is wanted (a branch's or an inlined
                // reference result's): a copy of x (§4.1).
                auto u = static_cast<g::Unary *>(n);
                if (u->op == g::T_MINUS && ElementwiseInto(n, dst)) return;
                if (u->op != g::T_BITAND) Unsupported(n, "an aggregate Unary");
                return Construct(u->child, dst);
            }
            case g::NK_Binary:
                if (ElementwiseInto(n, dst)) return;
                Unsupported(n, "an aggregate Binary");
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
                // Another array kind into a limited array: its elements,
                // under the capacity check (§4.2).
                if (Layouts::IsLimited(dst.t) && src.t && !Layouts::Same(src.t, dst.t) &&
                    (IsView(src.t) || src.t->kind == g::TY_ARRAY)) {
                    return LimFromSeq(n, SeqAt(n, src), dst);
                }
                // An array element where a slice is expected: viewed whole.
                if (IsSlice(dst.t) && src.t && src.t->kind == g::TY_ARRAY) {
                    auto sq = SeqAt(n, src);
                    StoreSlice(n, dst, [&] { C().LocalGet(sq.data); }, sq.len);
                    return;
                }
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
                if (Layouts::IsFixedArray(dst.t)) {
                    // Its bytes, as many as the array holds (the checker
                    // matched the lengths).
                    auto &str = static_cast<g::StrLit *>(n)->val;
                    auto addr = StaticString(str);
                    HandleOf(dst);
                    C().I64Const(addr);
                    C().I32Const(int32_t(std::min<int64_t>(int64_t(str.size()), lay.FixedSize(dst.t))));
                    C().Call(hs.copy);
                    return;
                }
                if (!IsSlice(dst.t)) Unsupported(n, "a string literal as this type");
                auto &s = static_cast<g::StrLit *>(n)->val;
                auto addr = StaticString(s);
                StoreSlice(n, dst, [&] { C().I64Const(addr); }, [&] { C().I64Const(int64_t(s.size())); });
                return;
            }
            case g::NK_Call: {
                auto c = static_cast<g::Call *>(n);
                if (IsSlice(dst.t) && c->rettypes.size() == 1 && (IsRz(c->rettypes[0]) || IsFlat(c->rettypes[0]))) {
                    auto s = TempRz(n, c->rettypes[0]->arr->sub);
                    StoreSlice(n, dst, [&] { C().LocalGet(s.data); }, s.len);
                    return;
                }
                if (c->builtin == g::B_POP) {
                    auto at = c->ArgNodes()[0]->exprtype;
                    if (at->kind == g::TY_REF) at = at->ref->sub;
                    Loc el { false, Pop(c), 0, at->arr->sub, false };
                    // A popped reference's value is a copy of what it led to.
                    if (IsPlainRef(el.t)) el = Deref(n, el);
                    el.t = dst.t;
                    Copy(dst, el);
                    return;
                }
                if (c->builtin == g::B_DEFAULT) return DefaultInto(c, dst);
                // copy(x) (§4.1): a copy of x (a slice's is a copy of its
                // elements, which the slice then views).
                if (c->builtin == g::B_COPY && !IsSlice(dst.t)) return Construct(c->FirstArg(), dst);
                if (c->builtin == g::B_COPY) {
                    auto sq = TempRz(c, dst.t->sub);
                    StoreSlice(n, dst, [&] { C().LocalGet(sq.data); }, sq.len);
                    return;
                }
                // push and alloc_ref give a reference to the new element; a
                // value of it is a copy.
                if (c->builtin == g::B_PUSH || c->builtin == g::B_ALLOC_REF) {
                    auto an = c->ArgNodes();
                    auto at = an[0]->exprtype;
                    if (at->kind == g::TY_REF) at = at->ref->sub;
                    uint32_t ref = 0;
                    Loc l;
                    if (c->builtin == g::B_ALLOC_REF) ref = Alloc(c);
                    else if (LimRecv(c, l)) LimPush(c, l, an[1], &ref);
                    else Push(c, Snapshot(RzOf(an[0])), at->arr->sub, an[1], &ref);
                    Copy(dst, Loc { false, ref, 0, dst.t, false });
                    return;
                }
                // str(...) into a limited array: built aside, then copied
                // under the capacity check.
                if (c->builtin == g::B_STR && IsAnyLim(dst.t)) {
                    PushScope();
                    uint32_t base = 0;
                    auto stk = ClaimStack(base);
                    auto cnt = NewLocal(Repr::I64);
                    Str(c, stk);
                    C().LocalSet(cnt);
                    Seq sq;
                    sq.et = dst.t->arr->sub;
                    sq.data = NewLocal(Repr::I64);
                    C().LocalGet(stk);
                    C().I32Const(1);
                    C().I32Add();
                    C().I64ExtendI32U();
                    C().I64Const(32);
                    C().Int(intop::Shl, true);
                    C().LocalGet(base);
                    C().I64ExtendI32U();
                    C().Int(intop::Or, true);
                    C().LocalSet(sq.data);
                    sq.len = [this, cnt] { C().LocalGet(cnt); };
                    LimFromSeq(n, sq, dst);
                    PopScope();
                    return;
                }
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
                PushScope();
                Construct(i->elseb, dst);
                PopScope();
                Close();
                return;
            }
            case g::NK_InlineBlock: {
                auto ib = static_cast<g::InlineBlock *>(n);
                // A resizable result viewed as a slice is built on a stack of
                // the caller's statement, to outlive the body making it.
                if (IsSlice(dst.t) && ib->spec && ib->spec->rets.size() == 1 && IsRz(ib->spec->rets[0])) {
                    auto sq = TempRz(n, ib->spec->rets[0]->arr->sub);
                    StoreSlice(n, dst, [&] { C().LocalGet(sq.data); }, sq.len);
                    return;
                }
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
            case g::NK_LoopExpr: {
                // loop { … break v; }: each break builds v at dst.
                Open(LK_BREAK, Repr::None, n->exprtype);
                labels.back().dst = &dst;
                Open(LK_CONTINUE, Repr::None, nullptr, nullptr, true);
                Stmt(static_cast<g::LoopExpr *>(n)->body);
                C().Br(0);
                Close();
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
        if (IsSlice(dst.t)) {
            // A literal where a slice is expected: built as a temporary
            // array on a stack of the scope's, which the slice views.
            if (!Layouts::Known(dst.t->sub)) Unsupported(lit, "an array literal of these elements");
            auto sq = TempRz(lit, dst.t->sub);
            StoreSlice(lit, dst, [&] { C().LocalGet(sq.data); }, sq.len);
            return;
        }
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
        if (!IsView(dst.t) && !IsVarArray(dst.t)) Unsupported(se, "a slice as this type");
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
        // A call (or an inlined one) giving a fixed array, a limited one
        // say, builds it as its own type, checks included, then its
        // elements are copied.
        g::TypeExpr *own = nullptr;
        if (n->nkind == g::NK_Call && static_cast<g::Call *>(n)->builtin < 0 &&
            static_cast<g::Call *>(n)->rettypes.size() == 1)
            own = static_cast<g::Call *>(n)->rettypes[0];
        if (n->nkind == g::NK_InlineBlock && static_cast<g::InlineBlock *>(n)->spec &&
            static_cast<g::InlineBlock *>(n)->spec->rets.size() == 1)
            own = static_cast<g::InlineBlock *>(n)->spec->rets[0];
        if (own && (Layouts::IsLimited(own) || Layouts::IsFixedArray(own)) && IsAggregate(own)) {
            auto tmp = FrameSlot(own);
            Construct(n, tmp);
            return CopyRun(n, SeqAt(n, tmp), stk, esz);
        }
        if (own && (IsVarArray(own) || IsDynLim(own))) {
            // A variable-size array result: its image aside, then its
            // elements.
            return CopyRun(n, ImageSeq(n, TempVar(n, own)), stk, esz);
        }
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
                    (IsRz(c->rettypes[0]) || IsFlat(c->rettypes[0]))) {
                    CallSpec(c, nullptr, stk);
                    return;
                }
                if (c->builtin == g::B_STR) return Str(c, stk);
                if (c->builtin == g::B_TO_BYTES && c->ArgNodes().size() == 1) return ToBytes(c, stk);
                // copy(x) (§4.1): x's elements, copied to the destination.
                if (c->builtin == g::B_COPY) return BuildRzInto(c->FirstArg(), stk, et);
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
            case g::NK_MatchExpr:
                Match(static_cast<g::MatchExpr *>(n), Repr::I64, [&](g::Node *b) {
                    if (b->nkind == g::NK_Block) BlockRz(static_cast<g::Block *>(b), stk, et);
                    else BuildRzInto(b, stk, et);
                });
                return;
            case g::NK_IfExpr: {
                auto i = static_cast<g::IfExpr *>(n);
                if (!i->elseb) Unsupported(n, "an if of a value without else");
                Truth(i->cond);
                OpenIf(Repr::I64);
                BlockRz(i->thenb, stk, et);
                C().Else();
                PushScope();  // what the branch claims is released on its path
                BuildRzInto(i->elseb, stk, et);
                PopScope();
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
            case g::NK_LoopExpr: {
                Open(LK_BREAK, Repr::I64, n->exprtype);
                labels.back().rzstk = stk;
                labels.back().rzelem = et;
                Open(LK_CONTINUE, Repr::None, nullptr, nullptr, true);
                Stmt(static_cast<g::LoopExpr *>(n)->body);
                C().Br(0);
                Close();
                C().Unreachable();
                Close();
                return;
            }
            default: break;
        }
        // A value this function cannot build in place (a builtin's result,
        // say) has no run to copy either.
        if (IsTemporary(n)) Unsupported(n, g::cat("a resizable value from ", KindName(n->nkind)));
        // A run of elements: copied to the top of the stack.
        CopyRun(n, SeqOf(n), stk, esz);
    }

    // The elements of s copied to the top of stack stk; their count on the
    // Wasm stack.
    void CopyRun(const g::Node *n, const Seq &s, uint32_t stk, int32_t esz) {
        auto count = NewLocal(Repr::I64), h = NewLocal(Repr::I64);
        s.len();
        C().LocalSet(count);
        if (!Layouts::Known(s.et)) {
            auto size = NewLocal(Repr::I64);
            RegionSize(s, count, size);
            C().LocalGet(stk);
            C().LocalGet(size);
            C().I32WrapI64();
            C().Call(hs.pushslot);
            C().LocalSet(h);
            C().LocalGet(h);
            C().LocalGet(s.data);
            C().LocalGet(size);
            C().I32WrapI64();
            C().Call(hs.copy);
            C().LocalGet(count);
            return;
        }
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
        if (IsVar(et)) {
            // A variable-size element: its image, built where it stays.
            auto h = NewLocal(Repr::I64);
            TopHandle(stk);
            C().LocalSet(h);
            BuildVar(val, stk, et);
            if (ref) *ref = h;
            return;
        }
        auto esz = int32_t(lay.FixedSize(et));
        if (HoldsRelRef(et) && IsAggregate(et)) {
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
            case g::NK_InlineBlock: case g::NK_EarlyBlock: case g::NK_MatchExpr:
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

    void PushScope() { scopes.push_back(Scope { stknext, {}, {} }); }
    // Each saved stack's top back to where it was when its local claimed it,
    // last claimed first (upstream CodeGen::EmitRestores).
    // An exit of scope k: the defer bodies run past in it, latest first
    // (spec §6.6), then its stacks' release. A body is emitted at each exit
    // it runs at, evaluating what it names there.
    void EmitRestores(size_t k) {
        auto defers = scopes[k].defers;
        for (auto i = defers.size(); i-- > 0;) Stmt(defers[i]);
        auto &s = scopes[k];
        for (auto i = s.saves.size(); i-- > 0;) {
            auto rb = s.saves[i].second;
            C().LocalGet(rb);
            C().I32Const(-1);
            C().Int(intop::Ne, false);
            C().If();
            TopAddr(s.saves[i].first);
            C().LocalGet(rb);
            C().I32Store(mem0::Tops);
            C().I32Const(-1);
            C().LocalSet(rb);
            C().End();
        }
    }
    void PopScope() {
        EmitRestores(scopes.size() - 1);
        stknext = scopes.back().stkbase;
        scopes.pop_back();
    }
    // The restores of every scope a branch to a label opened `to` scopes
    // deep leaves (upstream CodeGen::EmitExitRestores).
    void ExitRestores(size_t to) {
        for (auto i = scopes.size(); i-- > to;) EmitRestores(i);
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
        // The top to restore, kept apart: -1 (set at the function's start)
        // until the claim runs and once a restore has, so a scope's exit on
        // a path that never claimed restores nothing.
        auto rb = NewLocal(Repr::I32);
        Code init;
        init.I32Const(-1);
        init.LocalSet(rb);
        auto &body = C().b.b;
        body.insert(body.begin(), init.b.b.begin(), init.b.b.end());
        C().LocalGet(base);
        C().LocalSet(rb);
        scopes.back().saves.push_back({ stk, rb });
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
        if (IsVar(et)) {
            uint32_t h = 0;
            PushSlot(at, rz.stk, et, val, &h);
            auto len = rz.hdr.At(8, I64T());
            Store(at, len, [&] {
                Load(at, len);
                C().I64Const(1);
                C().Int(intop::Add, true);
            });
            if (ref) *ref = h;
            return;
        }
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
        // A variable's own type, before the use's adaptation (a resizable
        // returned as a T[] is still read through its header).
        if (!as && (obj->nkind == g::NK_Ident || obj->nkind == g::NK_Dot))
            if (auto st = StoredType(obj)) ot = st;
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
            if ((obj->nkind == g::NK_Dot || obj->nkind == g::NK_Ident) && !rz.hdr.frame && !rz.hdr.abs &&
                rz.hdr.off == 0) {
                // Through a reference a field holds, which a loop's body may
                // rebind: read again each iteration, as upstream does.
                auto hl = rz.hdr.h;
                auto data = s.data;
                s.reload = [this, obj, hl, data] {
                    auto again = RzOf(obj);
                    Handle(again.hdr);
                    C().LocalSet(hl);
                    Load(obj, Loc { false, hl, 0, I64T(), false });
                    C().LocalSet(data);
                };
            }
            s.len = [this, obj, rz] { Load(obj, rz.hdr.At(8, I64T())); };
            return s;
        }
        if (IsAnyLim(ot)) {
            auto base = ObjectLoc(obj);
            base.t = ot;
            return LimSeq(obj, base);
        }
        if (IsVarArray(ot)) {
            // Through a `&` too: what it names.
            if (obj->nkind == g::NK_Unary && static_cast<g::Unary *>(obj)->op == g::T_BITAND)
                obj = static_cast<g::Unary *>(obj)->child;
            Loc l;
            if (MemLoc(obj, l)) {
                bool viaref = IsPlainRef(l.t) && !IsPlainRef(ot);
                if (viaref) l = Deref(obj, l);
                // A reference variable names its pointee already.
                if (obj->nkind == g::NK_Ident && static_cast<g::Ident *>(obj)->vdef &&
                    IsPlainRef(static_cast<g::Ident *>(obj)->vdef->type))
                    viaref = true;
                if (IsVarArray(l.t)) {
                    auto sv = ImageSeq(obj, l);
                    if (!viaref) return sv;
                    // Through a reference, which a loop's body may rebind:
                    // read again each iteration (§6.5).
                    auto n = NewLocal(Repr::I64);
                    sv.len();
                    C().LocalSet(n);
                    auto data = sv.data;
                    sv.len = [this, n] { C().LocalGet(n); };
                    sv.reload = [this, obj, ot, data, n] {
                        Loc l2;
                        MemLoc(obj, l2);
                        if (IsPlainRef(l2.t)) l2 = Deref(obj, l2);
                        l2.t = ot;
                        auto again = ImageSeq(obj, l2);
                        C().LocalGet(again.data);
                        C().LocalSet(data);
                        again.len();
                        C().LocalSet(n);
                    };
                    return sv;
                }
            }
            if (obj->nkind == g::NK_Unary || IsPlainRef(obj->exprtype))
                Unsupported(obj, "a reference to a variable-size array here");
            if (obj->nkind == g::NK_SliceExpr) {
                // A slice taken where a T[] is read: its view.
                auto view = FrameBytes(16);
                view.t = ot;
                SliceInto(static_cast<g::SliceExpr *>(obj), view);
                Seq sv;
                sv.et = ot->arr->sub;
                sv.data = NewLocal(Repr::I64);
                Load(obj, view.At(0, I64T()));
                C().LocalSet(sv.data);
                auto n = NewLocal(Repr::I64);
                Load(obj, view.At(8, I64T()));
                C().LocalSet(n);
                sv.len = [this, n] { C().LocalGet(n); };
                return sv;
            }
            return ImageSeq(obj, TempVar(obj, ot));
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
        if (IsView(ot)) {
            auto sl = ObjectLoc(obj);
            s.et = ViewElem(ot);
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

    // The elements of the array of type l.t at l: a fixed or limited one
    // in place, a resizable one through its header.
    Seq SeqAt(const g::Node *at, const Loc &l) {
        auto t = l.t;
        if (IsAnyLim(t)) return LimSeq(at, l);
        if (IsVarArray(t)) return ImageSeq(at, l);
        Seq s;
        s.data = NewLocal(Repr::I64);
        if (IsView(t)) {
            // A slice's or a T[]'s view: { data, len }.
            s.et = ViewElem(t);
            Load(at, l.At(0, I64T()));
            C().LocalSet(s.data);
            auto n = NewLocal(Repr::I64);
            Load(at, l.At(8, I64T()));
            C().LocalSet(n);
            s.len = [this, n] { C().LocalGet(n); };
            return s;
        }
        s.et = t->arr->sub;
        if (Layouts::IsFixedArray(t)) {
            s.mem0 = l.Direct();
            HandleOf(l);
            C().LocalSet(s.data);
            auto k = t->arr->size;
            s.len = [this, k] { C().I64Const(k); };
            return s;
        }
        if (!IsRz(t)) Unsupported(at, "a sequence of this array");
        Load(at, l.At(0, I64T()));
        C().LocalSet(s.data);
        auto n = NewLocal(Repr::I64);
        Load(at, l.At(8, I64T()));
        C().LocalSet(n);
        s.len = [this, n] { C().LocalGet(n); };
        return s;
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
    // The bytes of the first `count` (a local) elements of s, sequential
    // ones walked, into local size.
    void RegionSize(const Seq &s, uint32_t count, uint32_t size) {
        auto cur = NewLocal(Repr::I64), i = NewLocal(Repr::I64);
        C().LocalGet(s.data);
        C().LocalSet(cur);
        C().I64Const(0);
        C().LocalSet(i);
        Open(LK_PLAIN, Repr::None);
        Open(LK_PLAIN, Repr::None, nullptr, nullptr, true);
        C().LocalGet(i);
        C().LocalGet(count);
        C().Int(intop::GeU, true);
        C().BrIf(1);
        Skip(s.et, cur);
        C().LocalGet(i);
        C().I64Const(1);
        C().Int(intop::Add, true);
        C().LocalSet(i);
        C().Br(0);
        Close();
        Close();
        C().LocalGet(cur);
        C().LocalGet(s.data);
        C().Int(intop::Sub, true);
        C().LocalSet(size);
    }

    Loc ElemLoc(const Seq &s, uint32_t i) {
        if (!Layouts::Known(s.et))
            throw g::CompileError { "goose-wasm: unsupported: indexing a sequential array" };
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
            case g::NK_Defer:
                if (scopes.empty()) Unsupported(n, "a defer outside a block");
                scopes.back().defers.push_back(static_cast<g::Defer *>(n)->body);
                return;
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
                        case g::B_MOVE_FROM: {
                            // dst.move_from(src) (§3.3): equal lengths, then
                            // the elements, overlapping safely.
                            auto an = call->ArgNodes();
                            auto dq = SeqOf(an[0]);
                            auto count = NewLocal(Repr::I64);
                            dq.len();
                            C().LocalSet(count);
                            auto sq = SeqOf(an[1]);
                            C().LocalGet(count);
                            sq.len();
                            C().Int(intop::Ne, true);
                            C().If();
                            Abort(call, "move_from lengths differ");
                            C().End();
                            C().LocalGet(dq.data);
                            C().LocalGet(sq.data);
                            C().LocalGet(count);
                            C().I64Const(lay.FixedSize(dq.et));
                            C().Int(intop::Mul, true);
                            C().I32WrapI64();
                            C().Call(hs.copy);
                            return;
                        }
                        case g::B_EXIT:
                            // exit(code) (upstream gs_exit): the process ends;
                            // what print wrote is out already.
                            Expr(call->ArgNodes()[0], I64T());
                            C().I32WrapI64();
                            C().Call(rt.proc_exit);
                            C().Unreachable();
                            return;
                        case g::B_FORMAT: return Format(call);
                        case g::B_TO_BYTES:
                            if (call->ArgNodes().size() == 2) {
                                // Appended to a builder the caller owns.
                                auto out = Snapshot(RzOf(call->ArgNodes()[1]));
                                auto len = out.hdr.At(8, I64T());
                                auto n = NewLocal(Repr::I64);
                                ToBytes(call, out.stk);
                                C().LocalSet(n);
                                Store(call, len, [&] {
                                    Load(call, len);
                                    C().LocalGet(n);
                                    C().Int(intop::Add, true);
                                });
                                return;
                            }
                            break;
                        case g::B_FREE: return Free(call);
                        case g::B_ALLOC_INDEX: case g::B_ALLOC_REF: Alloc(call); return;
                        default: break;
                    }
                    if (call->fvbody) {
                        FvCall(call, [&](g::Node *tl) {
                            if (tl) Stmt(tl);  // a value for its effects
                        });
                        return;
                    }
                    if (call->builtin < 0 && (call->spec || !call->dispatch.empty()) &&
                        call->rettypes.size() > 1) {
                        CallSpec(call);
                        for (size_t i = 0; i < call->rettypes.size(); i++)
                            if (!call->spec || !MultiAggregate(call->spec, call->spec->rets[i])) C().Drop();
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
                // So is a variable-size value, on a stack of the statement's.
                if (IsVar(n->exprtype)) {
                    PushScope();
                    TempVar(n, n->exprtype);
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
        if (d->isglobal || d->reusable == g::RU_SLICES) Unsupported(d, "this kind of declaration");
        if (d->reusable) {
            for (size_t i = 0; i < d->defs.size(); i++) {
                auto v = d->defs[i];
                if (!IsRz(v->type)) Unsupported(d, "this reusable declaration");
                DeclareVar(d, v, i < d->inits.size() ? d->inits[i] : nullptr);
                DeclareFreelist(d, v);
            }
            return;
        }
        // `let a, b = f();`, and the synthetic `result#1, result#2` the
        // parser declares for the statement `a, b = f();` (results).
        if (d->results || (d->defs.size() > 1 && d->inits.size() == 1)) return VarDeclResults(d);
        for (size_t i = 0; i < d->defs.size(); i++)
            DeclareVar(d, d->defs[i], i < d->inits.size() ? d->inits[i] : nullptr);
    }

    // One variable of a declaration, with its initializer if it has one.
    void DeclareVar(g::VarDecl *d, g::VarDef *v, g::Node *init) {
        if (IsRoot(v->type)) return DeclareRoot(d, v, init);
        if (IsDynLim(v->type)) return DeclareDynLim(d, v, init);
        if (IsVar(v->type)) return DeclareVarImage(d, v, init);
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
            // Bound to a pool (an inlined pool parameter), it carries the
            // pool's freelist.
            RzVar fl;
            if (TryPoolOf(init, fl)) fls[v] = fl;
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
        // The aggregates are built in their variables' slots; the others
        // come back as values, the last on top.
        std::vector<int64_t> ls;
        std::vector<Loc> dsts;
        for (size_t i = 0; i < d->defs.size(); i++) {
            auto v = d->defs[i];
            if (MultiAggregate(call->spec, call->spec->rets[i])) {
                if (!Layouts::Same(v->type, call->spec->rets[i]) && v->type->kind != call->spec->rets[i]->kind)
                    Unsupported(d, "a result of another type");
                auto slot = FrameSlot(call->spec->rets[i]);
                slot.t = v->type;
                memvars[v] = slot;
                dsts.push_back(slot);
                ls.push_back(-1);
                continue;
            }
            ls.push_back(NewLocal(ReprOf(d, v->type)));
            locals[v] = uint32_t(ls.back());
        }
        CallSpec(call, nullptr, -1, nullptr, nullptr, &dsts);
        for (size_t i = d->defs.size(); i-- > 0;) {
            if (ls[i] < 0) continue;
            Convert(d, call->rettypes[i], d->defs[i]->type);
            C().LocalSet(uint32_t(ls[i]));
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
            if (v && v->isglobal && Layouts::IsFatRef(v->type)) {
                StoreFatRef(a->rhs, GlobalSlot(v));
                return true;
            }
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
            if (v->isglobal) {
                // A global reference: its slot in memory 0.
                auto slot = GlobalSlot(v);
                if (!slot.abs) Unsupported(a, "this global rebinding");
                Store(a, slot, target);
                return true;
            }
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
        // A reference to a resizable value held in a field or element: its
        // { header, stack } pair (gs_rref).
        if (Layouts::IsFatRef(l.t)) {
            StoreFatRef(a->rhs, l);
            return true;
        }
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
            if (IsDynLim(t) && a->op == g::T_ASSIGN) {
                l.t = t;
                // The source as its place holds it, whatever the use's type.
                Loc src;
                if (MemLoc(a->rhs, src)) {
                    if (IsPlainRef(src.t)) src = Deref(a->rhs, src);
                    if (src.t && (src.t->kind == g::TY_ARRAY || IsView(src.t)))
                        return LimFromSeq(a, SeqAt(a, src), l);
                }
                return LimFromSeq(a, SeqOf(a->rhs), l);
            }
            if (IsAggregate(t)) {
                if (a->op != g::T_ASSIGN && a->op != g::T_DOTASSIGN) {
                    // v op= w: the destination once, its old value read
                    // before w runs (§6.1).
                    auto bop = OpOfCompound(a->op);
                    if (bop == g::T_ASSIGN) Unsupported(a, "this assignment operator on an aggregate");
                    EwOperand old;
                    old.l = FrameSlot(t);
                    Copy(old.l, Loc { l.frame, l.h, l.off, t, l.mem0, l.abs });
                    auto r = EwEval(a->rhs, t);
                    Loc d = l;
                    d.t = t;
                    Elementwise(a, bop, d, t, old, &r);
                    return;
                }
                // A new T[] needs storage that outlives this statement's
                // scope where the variable's is wider.
                if (IsFlat(t)) Unsupported(a, "assigning a whole T[]");
                // A value holding relative references is built where it
                // stays (§3.9), as upstream builds it.
                if (HoldsRelRef(t)) {
                    Construct(a->rhs, l);
                    return;
                }
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
        if (!Layouts::Known(s.et)) return ForSequential(f, s);
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
        auto spills = LoopSpills({ f->vdef, f->idxdef });
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
        if (byref && IsPlainRef(s.et)) {
            // An element that is a reference binds as the reference it
            // holds: there are no references to references (§3.8).
            Load(f, elem);
            C().LocalSet(xlocal);
        } else if (byref) {
            Handle(elem);
            C().LocalSet(xlocal);
        } else if (agg) {
            Copy(xslot, elem);
        } else {
            Load(f, elem);
            C().LocalSet(xlocal);
        }
        EmitSpills(f, spills);
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

    // The loop variables among vs that a reference is taken to or a nested
    // function reaches (FindAddrTaken) live in the frame: each iteration
    // copies their locals there (EmitSpills).
    std::vector<std::pair<Loc, uint32_t>> LoopSpills(std::initializer_list<g::VarDef *> vs) {
        std::vector<std::pair<Loc, uint32_t>> out;
        for (auto v : vs) {
            if (!v || !addrtaken.count(v)) continue;
            auto it = locals.find(v);
            if (it == locals.end()) continue;
            auto slot = FrameSlot(v->type);
            out.push_back({ slot, it->second });
            locals.erase(it);
            memvars[v] = slot;
        }
        return out;
    }
    void EmitSpills(const g::Node *at, const std::vector<std::pair<Loc, uint32_t>> &spills) {
        for (auto &[slot, local] : spills) {
            auto vt = slot.t;
            Store(at, slot, [&] {
                C().LocalGet(local);
                // An index counter is an i64 whatever its binder's type.
                auto lt = F().locals[local - nparams];
                if (lt == ValType::I64 && ReprOf(at, vt) == Repr::I32) C().I32WrapI64();
            });
        }
    }

    // for over an array of variable-size elements (§3.3: sequential): a
    // cursor walks the images, the binding naming each where it lies (such
    // an element is never copied implicitly, §4.1).
    void ForSequential(g::ForLoop *f, const Seq &s) {
        auto i = NewLocal(Repr::I64), cur = NewLocal(Repr::I64), at = NewLocal(Repr::I64);
        auto n = NewLocal(Repr::I64);
        s.len();
        C().LocalSet(n);
        C().I64Const(0);
        C().LocalSet(i);
        C().LocalGet(s.data);
        C().LocalSet(cur);
        // Bound as a reference, its local holds the element's handle.
        if (IsPlainRef(f->vdef->type)) locals[f->vdef] = at;
        else memvars[f->vdef] = Loc { false, at, 0, s.et, false };
        if (f->idxdef) {
            if (ReprOf(f, f->idxdef->type) != Repr::I64) Unsupported(f, "a narrow index binder here");
            locals[f->idxdef] = i;
        }
        Open(LK_BREAK, Repr::None);
        Open(LK_PLAIN, Repr::None, nullptr, nullptr, true);
        if (s.reload) {
            // The path to the sequence is read again each iteration
            // (§6.5): the cursor walks to element i from the start of
            // what it leads to now, the length read again.
            s.reload();
            s.len();
            C().LocalSet(n);
            auto k = NewLocal(Repr::I64);
            C().LocalGet(s.data);
            C().LocalSet(cur);
            C().I64Const(0);
            C().LocalSet(k);
            C().LocalGet(i);
            C().LocalGet(n);
            C().Int(intop::GeU, true);
            C().BrIf(1);
            Open(LK_PLAIN, Repr::None);
            Open(LK_PLAIN, Repr::None, nullptr, nullptr, true);
            C().LocalGet(k);
            C().LocalGet(i);
            C().Int(intop::GeU, true);
            C().BrIf(1);
            Skip(s.et, cur);
            C().LocalGet(k);
            C().I64Const(1);
            C().Int(intop::Add, true);
            C().LocalSet(k);
            C().Br(0);
            Close();
            Close();
        }
        C().LocalGet(i);
        C().LocalGet(n);
        C().Int(intop::GeU, true);
        C().BrIf(1);
        C().LocalGet(cur);
        C().LocalSet(at);
        // The next element's start, before the body runs.
        Skip(s.et, cur);
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
        auto spills = LoopSpills({ f->vdef, f->idxdef });
        Open(LK_BREAK, Repr::None);
        Open(LK_PLAIN, Repr::None, nullptr, nullptr, true);
        C().LocalGet(ctr);
        C().LocalGet(end);
        C().Int(vt->intstorage == g::IS_U64 ? intop::GeU : intop::GeS, true);
        C().BrIf(1);
        C().LocalGet(ctr);
        if (!IsWide(vt)) C().I32WrapI64();
        C().LocalSet(var);
        EmitSpills(f, spills);
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
            if (l.vart) BuildVar(b->val, uint32_t(l.rzstk), l.vart);
            else if (l.rzstk >= 0) BuildRzInto(b->val, uint32_t(l.rzstk), l.rzelem);
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
                if (l.vart) BuildVar(r->vals[0], uint32_t(l.rzstk), l.vart);
                else if (l.rzstk >= 0) BuildRzInto(r->vals[0], uint32_t(l.rzstk), l.rzelem);
                else if (l.dst) Construct(r->vals[0], *l.dst);
                else Expr(r->vals[0], l.type);
            }
            ExitRestores(l.scopes);
            C().Br(uint32_t(Depth(LK_INLINE, r->target)));
            return;
        }
        if (r->target != spec->sf) return ReturnFrom(r);
        if (r->vals.size() > spec->rets.size()) Unsupported(r, "a return of more values");
        auto out = uint32_t(Depth(LK_FUNC));
        if (retrz >= 0 && IsVar(spec->rets[0])) {
            BuildVar(r->vals[0], uint32_t(retrz), spec->rets[0]);
            ExitRestores(0);
            C().I64Const(0);
            C().Br(out);
            return;
        }
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
            for (size_t i = 0; i < r->vals.size(); i++) {
                if (i < retdsts.size() && retdsts[i] >= 0)
                    Construct(r->vals[i], Loc { false, uint32_t(retdsts[i]), 0, spec->rets[i], false });
                else
                    Expr(r->vals[i], spec->rets[i]);
            }
        }
        ExitRestores(0);
        C().Br(out);
    }

    // ---- Expressions ----

    // Emits n's value converted to type `to` (nothing when `to` is void,
    // after running n for its effects).
    void Expr(g::Node *n, g::TypeExpr *to) {
        // &x where a value is wanted (a branch's): x's value (§4.1).
        if (n->nkind == g::NK_Unary && static_cast<g::Unary *>(n)->op == g::T_BITAND && to &&
            !IsPlainRef(to) && !IsVoid(to) && to->kind != g::TY_REF)
            return Expr(static_cast<g::Unary *>(n)->child, to);
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
        // A reference where its pointee is wanted (`if c { &a } else
        // { &b }` as a value): a copy of the pointee (§3.8, §4.1).
        if (IsPlainRef(t) && !IsPlainRef(to) && ReprOf(n, to) != Repr::None) {
            auto h = NewLocal(Repr::I64);
            C().LocalSet(h);
            Loc l { false, h, 0, t->ref->sub, false };
            Load(n, l);
            Convert(n, l.t, to);
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
                if (n->exprtype && n->exprtype->kind == g::TY_INT && n->exprtype->intstorage == g::IS_VARINT) {
                    C().I64Const(lit->val);  // a varint's value is an i64
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
                        if (!IsAnyLim(ot)) Unsupported(n, "property .cap of this array");
                        auto l = ObjectLoc(d->obj);
                        l.t = ot;
                        LimCap(n, l);
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
                if (debug && !c->unchecked && !c->implicit && IsInt(tt) &&
                    (IsInt(c->child->exprtype) || IsFlt(c->child->exprtype)))
                    CheckedAs(n, c->child->exprtype, tt);
                else
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
                if (call->builtin == g::B_ALLOC_INDEX || call->builtin == g::B_ALLOC_REF) {
                    auto r = Alloc(call);
                    auto pt = call->ArgNodes()[0]->exprtype;
                    if (pt->kind == g::TY_REF) pt = pt->ref->sub;
                    if (call->builtin == g::B_ALLOC_INDEX ||
                        (IsPlainRef(n->exprtype) && !IsPlainRef(pt->arr->sub))) {
                        C().LocalGet(r);
                        Convert(n, call->builtin == g::B_ALLOC_INDEX ? I64T() : n->exprtype, n->exprtype);
                        return;
                    }
                    auto at = call->ArgNodes()[0]->exprtype;
                    if (at->kind == g::TY_REF) at = at->ref->sub;
                    Loc el { false, r, 0, at->arr->sub, false };
                    Load(n, el);
                    Convert(n, el.t, n->exprtype);
                    return;
                }
                if (call->builtin == g::B_COPY && ReprOf(n, n->exprtype) != Repr::None) {
                    // copy(x) of a scalar is its value.
                    Expr(call->FirstArg(), n->exprtype);
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
                    // Used as the element, it reads through the reference;
                    // an element that is a reference gives the one it holds
                    // (upstream NewElemResult).
                    if (IsPlainRef(n->exprtype) && !IsPlainRef(at->arr->sub)) {
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
                for (size_t i = 1; i < call->rettypes.size(); i++)
                    if (!call->spec || !MultiAggregate(call->spec, call->spec->rets[i])) C().Drop();
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
            if (i->elseb->nkind == g::NK_Block) {
                BlockValue(static_cast<g::Block *>(i->elseb), i->exprtype);
            } else {
                PushScope();
                Expr(i->elseb, i->exprtype);
                PopScope();
            }
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
                if (debug && IsSigned(ot)) {
                    Expr(u->child, ot);
                    CheckedNeg(u, ot);
                    Convert(u, ot, t);
                    return;
                }
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
                // What it builds on data stacks is released in its own
                // branch, so a path skipping it restores nothing it never
                // claimed; its value is a bool, which needs none of them.
                Truth(b->left);
                OpenIf(Repr::I32);
                auto right = [&] {
                    PushScope();
                    Truth(b->right);
                    PopScope();
                };
                if (b->op == g::T_ANDAND) {
                    right();
                    C().Else();
                    C().I32Const(0);
                } else {
                    C().I32Const(1);
                    C().Else();
                    right();
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
                if ((b->op == g::T_EQ || b->op == g::T_NEQ) && StructEquality(b)) return;
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
    // x % y on floats is C's fmod, an exact remainder with x's sign. The
    // usual bit-level algorithm: both mantissas normalized, the dividend's
    // reduced a bit at a time down to the divisor's exponent, the rest
    // renormalized (as C libraries compute it). An f32's operands are
    // exact as f64s, and so is their remainder as an f32.
    uint32_t fmodfn = 0;
    uint32_t FmodFn() {
        if (fmodfn) return fmodfn;
        using V = ValType;
        fmodfn = m.AddFunc({ { V::F64, V::F64 }, { V::F64 } }, { V::I64, V::I64, V::I64, V::I64, V::I64, V::I64 });
        auto &c = m.FuncAt(fmodfn).code;
        enum { x, y, ux, uy, ex, ey, sx, i };
        auto bits = [&](uint32_t f, uint32_t u) {
            c.LocalGet(f);
            c.Op(0xbd);
            c.LocalSet(u);
        };
        auto i64c = [&](int64_t v) { c.I64Const(v); };
        auto zero_x = [&] {  // return 0 * x
            c.F64Const(0);
            c.LocalGet(x);
            c.Op(0xa2);
            c.Return();
        };
        bits(x, ux);
        bits(y, uy);
        c.LocalGet(ux);
        i64c(52);
        c.Int(intop::ShrU, true);
        i64c(0x7ff);
        c.Int(intop::And, true);
        c.LocalSet(ex);
        c.LocalGet(uy);
        i64c(52);
        c.Int(intop::ShrU, true);
        i64c(0x7ff);
        c.Int(intop::And, true);
        c.LocalSet(ey);
        c.LocalGet(ux);
        i64c(63);
        c.Int(intop::ShrU, true);
        c.LocalSet(sx);
        // y == 0, y NaN, x not finite: NaN.
        c.LocalGet(uy);
        i64c(1);
        c.Int(intop::Shl, true);
        c.I64Eqz();
        c.LocalGet(y);
        c.LocalGet(y);
        c.Op(0x62);  // f64.ne: NaN
        c.Int(intop::Or, false);
        c.LocalGet(ex);
        i64c(0x7ff);
        c.Int(intop::Eq, true);
        c.Int(intop::Or, false);
        c.If();
        c.LocalGet(x);
        c.LocalGet(y);
        c.Op(0xa2);
        c.LocalGet(x);
        c.LocalGet(y);
        c.Op(0xa2);
        c.Op(0xa3);
        c.Return();
        c.End();
        // |x| <= |y|.
        c.LocalGet(ux);
        i64c(1);
        c.Int(intop::Shl, true);
        c.LocalGet(uy);
        i64c(1);
        c.Int(intop::Shl, true);
        c.Int(intop::LeU, true);
        c.If();
        c.LocalGet(ux);
        i64c(1);
        c.Int(intop::Shl, true);
        c.LocalGet(uy);
        i64c(1);
        c.Int(intop::Shl, true);
        c.Int(intop::Eq, true);
        c.If();
        zero_x();
        c.End();
        c.LocalGet(x);
        c.Return();
        c.End();
        // Normalize a mantissa: u with exponent e.
        auto norm = [&](uint32_t u, uint32_t e) {
            c.LocalGet(e);
            c.I64Eqz();
            c.If();
            // Subnormal: e goes down past the leading zeros.
            c.LocalGet(u);
            i64c(12);
            c.Int(intop::Shl, true);
            c.LocalSet(i);
            c.Block();
            c.Loop();
            c.LocalGet(i);
            i64c(63);
            c.Int(intop::ShrU, true);
            c.I64Eqz();
            c.I32Eqz();
            c.BrIf(1);
            c.LocalGet(e);
            i64c(1);
            c.Int(intop::Sub, true);
            c.LocalSet(e);
            c.LocalGet(i);
            i64c(1);
            c.Int(intop::Shl, true);
            c.LocalSet(i);
            c.Br(0);
            c.End();
            c.End();
            c.LocalGet(u);
            i64c(1);
            c.LocalGet(e);
            c.Int(intop::Sub, true);
            c.Int(intop::Shl, true);
            c.LocalSet(u);
            c.Else();
            c.LocalGet(u);
            i64c(int64_t(-1ull >> 12));
            c.Int(intop::And, true);
            i64c(int64_t(1) << 52);
            c.Int(intop::Or, true);
            c.LocalSet(u);
            c.End();
        };
        norm(ux, ex);
        norm(uy, ey);
        // One step: i = ux - uy; when not negative, zero ends at 0 * x, else
        // ux = i.
        auto step = [&] {
            c.LocalGet(ux);
            c.LocalGet(uy);
            c.Int(intop::Sub, true);
            c.LocalTee(i);
            i64c(0);
            c.Int(intop::GeS, true);
            c.If();
            c.LocalGet(i);
            c.I64Eqz();
            c.If();
            zero_x();
            c.End();
            c.LocalGet(i);
            c.LocalSet(ux);
            c.End();
        };
        c.Block();
        c.Loop();
        c.LocalGet(ex);
        c.LocalGet(ey);
        c.Int(intop::LeS, true);
        c.BrIf(1);
        step();
        c.LocalGet(ux);
        i64c(1);
        c.Int(intop::Shl, true);
        c.LocalSet(ux);
        c.LocalGet(ex);
        i64c(1);
        c.Int(intop::Sub, true);
        c.LocalSet(ex);
        c.Br(0);
        c.End();
        c.End();
        step();
        // Renormalize.
        c.Block();
        c.Loop();
        c.LocalGet(ux);
        i64c(52);
        c.Int(intop::ShrU, true);
        c.I64Eqz();
        c.I32Eqz();
        c.BrIf(1);
        c.LocalGet(ux);
        i64c(1);
        c.Int(intop::Shl, true);
        c.LocalSet(ux);
        c.LocalGet(ex);
        i64c(1);
        c.Int(intop::Sub, true);
        c.LocalSet(ex);
        c.Br(0);
        c.End();
        c.End();
        // Scale.
        c.LocalGet(ex);
        i64c(0);
        c.Int(intop::GtS, true);
        c.If();
        c.LocalGet(ux);
        i64c(int64_t(1) << 52);
        c.Int(intop::Sub, true);
        c.LocalGet(ex);
        i64c(52);
        c.Int(intop::Shl, true);
        c.Int(intop::Or, true);
        c.LocalSet(ux);
        c.Else();
        c.LocalGet(ux);
        i64c(1);
        c.LocalGet(ex);
        c.Int(intop::Sub, true);
        c.Int(intop::ShrU, true);
        c.LocalSet(ux);
        c.End();
        c.LocalGet(ux);
        c.LocalGet(sx);
        i64c(63);
        c.Int(intop::Shl, true);
        c.Int(intop::Or, true);
        c.Op(0xbf);
        return fmodfn;
    }

    void FloatArith(g::Node *at, g::TType op, g::TypeExpr *t, g::Node *right) {
        FloatArithV(at, op, t, [&] { Expr(right, t); });
    }
    void FloatArithV(g::Node *at, g::TType op, g::TypeExpr *t, const std::function<void()> &rv) {
        auto f32 = t->fltstorage == g::FS_F32;
        uint8_t code;
        switch (op) {
            case g::T_PLUS: code = f32 ? 0x92 : 0xa0; break;
            case g::T_MINUS: code = f32 ? 0x93 : 0xa1; break;
            case g::T_MUL: code = f32 ? 0x94 : 0xa2; break;
            case g::T_DIV: code = f32 ? 0x95 : 0xa3; break;
            case g::T_MOD: {
                // Float % is C's fmod, an exact remainder.
                auto fx = NewLocal(Repr::F64);
                if (f32) C().Op(0xbb);
                C().LocalSet(fx);
                C().LocalGet(fx);
                rv();
                if (f32) C().Op(0xbb);
                C().Call(FmodFn());
                if (f32) C().Op(0xb6);
                return;
            }
            default: Unsupported(at, "this operator on floats");
        }
        rv();
        C().Op(code);
    }

    void Arith(g::Node *at, g::TType op, g::TypeExpr *t, g::Node *right) {
        if (op == g::T_SHL || op == g::T_SHR || IsFlt(t)) return ArithN(at, op, t, right);
        ArithV(at, op, t, [&] { Expr(right, t); });
    }
    void ArithN(g::Node *at, g::TType op, g::TypeExpr *t, g::Node *right) {
        if (IsFlt(t)) return FloatArith(at, op, t, right);
        auto wide = IsWide(t);
        auto r = wide ? Repr::I64 : Repr::I32;
        switch (op) {
            case g::T_PLUS: case g::T_MINUS: case g::T_MUL: case g::T_BITAND: case g::T_BITOR:
            case g::T_XOR: {
                if (debug && IsSigned(t) && (op == g::T_PLUS || op == g::T_MINUS || op == g::T_MUL)) {
                    auto a = NewLocal(r), b = NewLocal(r);
                    C().LocalSet(a);
                    Expr(right, t);
                    C().LocalSet(b);
                    return CheckedArith(at, op, t, a, b);
                }
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

    void ArithV(g::Node *at, g::TType op, g::TypeExpr *t, const std::function<void()> &rv) {
        if (IsFlt(t)) return FloatArithV(at, op, t, rv);
        auto wide = IsWide(t);
        auto r = wide ? Repr::I64 : Repr::I32;
        switch (op) {
            case g::T_PLUS: case g::T_MINUS: case g::T_MUL: case g::T_BITAND: case g::T_BITOR:
            case g::T_XOR: {
                if (debug && IsSigned(t) && (op == g::T_PLUS || op == g::T_MINUS || op == g::T_MUL)) {
                    auto a = NewLocal(r), b = NewLocal(r);
                    C().LocalSet(a);
                    rv();
                    C().LocalSet(b);
                    return CheckedArith(at, op, t, a, b);
                }
                rv();
                C().Int(op == g::T_PLUS ? intop::Add : op == g::T_MINUS ? intop::Sub
                        : op == g::T_MUL ? intop::Mul : op == g::T_BITAND ? intop::And
                        : op == g::T_BITOR ? intop::Or : intop::Xor, wide);
                if (!wide) Normalize(t);
                return;
            }
            case g::T_DIV:
            case g::T_MOD:
                return DivModV(at, op, t, r, rv);
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
        DivModV(at, op, t, r, [&] { Expr(right, t); });
    }
    void DivModV(g::Node *at, g::TType op, g::TypeExpr *t, Repr r, const std::function<void()> &rv) {
        auto wide = r == Repr::I64;
        auto a = NewLocal(r), b = NewLocal(r);
        C().LocalSet(a);
        rv();
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
                // trap; the debug build aborts there.
                if (debug) {
                    DefineDebug();
                    C().LocalGet(a);
                    C().I32Const(int32_t(-(int64_t(1) << (Bits(t) - 1))));
                    C().Int(intop::Eq, false);
                    C().LocalGet(b);
                    C().I32Const(-1);
                    C().Int(intop::Eq, false);
                    C().I32And();
                    C().If();
                    C().LocalGet(a);
                    C().I64ExtendI32S();
                    C().LocalGet(b);
                    C().I64ExtendI32S();
                    StrArg("/");
                    StrArg(TypeText(t));
                    C().I32Const(0);
                    Location(at);
                    C().Call(ovffn);
                    C().End();
                }
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

    // ---- Reusable pools (spec §5.4, deprecated) ----

    // A `reusable` grow-only array is paired with a hidden freelist, a
    // resizable of i64 slot indices on a stack of its own (upstream's fl and
    // flstk): a pool variable's, a global's, or what a pool reference
    // carries beside the array. Slice pools (`reusable[]`) are unsupported.
    std::map<g::VarDef *, RzVar> fls;
    std::map<g::VarDef *, std::pair<Loc, int>> gfls;  // a global pool's freelist header, stack

    static bool IsPoolParam(g::FnSpec *sp, size_t i) {
        return i < sp->roots.size() && sp->roots[i].reusable == g::RU_SLOTS && IsRzRef(sp->params[i]->type);
    }
    // A local pool, or a pool reference parameter: what a nested function
    // reaching it is handed its freelist with.
    static bool IsPoolVar(g::VarDef *v) {
        if (v->reusable == g::RU_SLOTS && !v->isglobal) return true;
        auto sp = v->ownerspec;
        if (!v->isparam || !sp) return false;
        for (size_t i = 0; i < sp->params.size(); i++)
            if (sp->params[i] == v) return IsPoolParam(sp, i);
        return false;
    }

    // The freelist of the pool n names: a pool variable, or a reference to
    // one bound to a pool.
    RzVar PoolOf(g::Node *n) {
        RzVar fl;
        if (!TryPoolOf(n, fl)) Unsupported(n, "a pool this backend cannot name");
        return fl;
    }
    bool TryPoolOf(g::Node *n, RzVar &fl) {
        if (n->nkind == g::NK_Unary && static_cast<g::Unary *>(n)->op == g::T_BITAND)
            n = static_cast<g::Unary *>(n)->child;
        if (n->nkind != g::NK_Ident) return false;
        auto v = static_cast<g::Ident *>(n)->vdef;
        auto it = v ? fls.find(v) : fls.end();
        if (it != fls.end()) {
            fl = it->second;
            return true;
        }
        auto gt = v ? gfls.find(v) : gfls.end();
        if (gt == gfls.end()) return false;
        fl = RzVar { gt->second.first, StackConstLocal(gt->second.second) };
        return true;
    }

    // A local holding a constant stack index, set at the function's start.
    std::map<int, uint32_t> stkconsts;
    uint32_t StackConstLocal(int k) {
        auto it = stkconsts.find(k);
        if (it != stkconsts.end()) return it->second;
        auto l = NewLocal(Repr::I32);
        Code set;
        set.I32Const(k);
        set.LocalSet(l);
        auto &body = C().b.b;
        body.insert(body.begin(), set.b.b.begin(), set.b.b.end());
        return stkconsts[k] = l;
    }

    // A local pool's freelist: an empty resizable on a stack of its own.
    void DeclareFreelist(g::VarDecl *d, g::VarDef *v) {
        uint32_t base = 0;
        RzVar fl;
        fl.stk = ClaimStack(base);
        fl.hdr = FrameBytes(16);
        EmptyHeader(d, fl.hdr, fl.stk, base);
        fls[v] = fl;
    }

    // a.alloc_index(v), a.alloc_ref(v) (upstream EmitAlloc): the element
    // first (or, holding relative references, built at its slot), then a
    // freed slot if there is one, else a fresh one at the top; its index,
    // or a reference to it.
    uint32_t Alloc(g::Call *call) {
        auto an = call->ArgNodes();
        auto r = Receiver(call);
        auto fl = Snapshot(PoolOf(an[0]));
        auto et = r.et;
        bool agg = IsAggregate(et);
        bool inplace = agg && HoldsRelRef(et);
        Loc tmp;
        uint32_t v = 0;
        if (inplace) {
        } else if (agg) {
            tmp = FrameSlot(et);
            Construct(an[1], tmp);
        } else {
            v = NewLocal(ReprOf(call, et));
            Expr(an[1], et);
            C().LocalSet(v);
        }
        auto iv = NewLocal(Repr::I64), h = NewLocal(Repr::I64), n = NewLocal(Repr::I64);
        auto len = r.rz.hdr.At(8, I64T()), fllen = fl.hdr.At(8, I64T());
        Load(call, fllen);
        C().LocalTee(n);
        C().I64Const(0);
        C().Int(intop::GtS, true);
        C().If();
        {
            // The last freed slot: off the freelist's top.
            C().LocalGet(n);
            C().I64Const(1);
            C().Int(intop::Sub, true);
            C().LocalSet(n);
            Store(call, fllen, [&] { C().LocalGet(n); });
            TopAtCount(RzRecv { fl, I64T(), 8 }, call, n);
            auto e = NewLocal(Repr::I64);
            Load(call, fl.hdr.At(0, I64T()));
            C().LocalGet(n);
            C().I64Const(8);
            C().Int(intop::Mul, true);
            C().Int(intop::Add, true);
            C().LocalSet(e);
            Load(call, Loc { false, e, 0, I64T(), false });
            C().LocalSet(iv);
            Load(call, r.rz.hdr.At(0, I64T()));
            C().LocalGet(iv);
            C().I64Const(r.esz);
            C().Int(intop::Mul, true);
            C().Int(intop::Add, true);
            C().LocalSet(h);
        }
        C().Else();
        {
            // A fresh slot at the top, its room made but the top left where
            // it was: it joins the array only once its element is written,
            // so what the element's initializers run sees the pool as it
            // was, and one leaving the function leaves it so (upstream
            // EmitAlloc).
            Load(call, len);
            C().LocalSet(iv);
            C().LocalGet(r.rz.stk);
            C().I32Const(r.esz);
            C().Call(hs.pushslot);
            C().LocalSet(h);
            TopAtCount(r, call, iv);
        }
        C().End();
        Loc slot { false, h, 0, et, false };
        if (inplace) Construct(an[1], slot);
        else if (agg) Copy(slot, tmp);
        else Store(call, slot, [&] { C().LocalGet(v); });
        C().LocalGet(iv);
        Load(call, len);
        C().Int(intop::Eq, true);
        C().If();
        auto nl = NewLocal(Repr::I64);
        C().LocalGet(iv);
        C().I64Const(1);
        C().Int(intop::Add, true);
        C().LocalSet(nl);
        Store(call, len, [&] { C().LocalGet(nl); });
        TopAtCount(r, call, nl);
        C().End();
        return call->builtin == g::B_ALLOC_INDEX ? iv : h;
    }

    // a.free(i): i, checked against the length as an index is, onto the
    // freelist.
    void Free(g::Call *call) {
        auto an = call->ArgNodes();
        auto r = Receiver(call);
        auto fl = Snapshot(PoolOf(an[0]));
        auto i = NewLocal(Repr::I64);
        Expr(an[1], I64T());
        C().LocalSet(i);
        auto len = r.rz.hdr.At(8, I64T());
        C().LocalGet(i);
        Load(call, len);
        C().Int(intop::GeU, true);
        C().If();
        C().LocalGet(i);
        Load(call, len);
        Location(call);
        C().Call(rt.idxfail);
        C().End();
        auto h = NewLocal(Repr::I64);
        C().LocalGet(fl.stk);
        C().I32Const(8);
        C().Call(hs.pushslot);
        C().LocalSet(h);
        Store(call, Loc { false, h, 0, I64T(), false }, [&] { C().LocalGet(i); });
        auto fllen = fl.hdr.At(8, I64T());
        Store(call, fllen, [&] {
            Load(call, fllen);
            C().I64Const(1);
            C().Int(intop::Add, true);
        });
    }

    // ---- Flat variable-size arrays (spec §3.3) ----

    // A T[] value at dst (IsFlat): its elements built or copied onto a stack
    // claimed in the current scope, which the declaration lives in, and the
    // view of them stored. Its length must fit the length field (upstream
    // aborts with "array length exceeds storage range").
    void FlatInto(g::Node *n, const Loc &dst) {
        auto et = dst.t->arr->sub;
        auto sq = TempRz(n, et);
        auto ls = dst.t->arr->lenstorage < 0 ? g::IS_U32 : g::IntStorage(dst.t->arr->lenstorage);
        if (ls != g::IS_VARINT && Layouts::IntSize(ls) < 8) {
            static const std::string msg = "array length exceeds storage range";
            sq.len();
            C().I64Const(int64_t((1ull << (8 * Layouts::IntSize(ls))) - 1));
            C().Int(intop::GtU, true);
            C().If();
            C().I32Const(int32_t(StaticString(msg)));
            C().I32Const(int32_t(msg.size()));
            C().Call(rt.panic);
            C().End();
        }
        StoreSlice(n, dst, [&] { C().LocalGet(sq.data); }, sq.len);
    }

    // A T[..] variable: its image built on a stack of its own (upstream
    // GenArrayLit, EmitArrayCount). [..n] reserves n slots, none in use (n
    // must fit u32, else "invalid capacity"); any other initializer's
    // elements fill it, its capacity their count.
    void DeclareDynLim(g::VarDecl *d, g::VarDef *v, g::Node *init) {
        if (!init) Unsupported(d, "a T[..] without an initializer");
        uint32_t base = 0;
        auto stk = ClaimStack(base);
        memvars[v] = BuildDynLim(d, init, stk, v->type);
    }
    // A T[..]'s image at the top of stack stk, from its initializer; its
    // place.
    Loc BuildDynLim(const g::Node *d, g::Node *init, uint32_t stk, g::TypeExpr *t) {
        auto et = t->arr->sub;
        auto esz = int32_t(lay.FixedSize(et));
        auto h = NewLocal(Repr::I64);
        auto img = Loc { false, h, 0, t, false };
        auto u32 = ast.inttypes[g::IS_U32];
        auto lit = init->nkind == g::NK_ArrayLit ? static_cast<g::ArrayLit *>(init) : nullptr;
        if (lit && lit->capexpr) {
            auto cap = NewLocal(Repr::I64);
            Expr(lit->capexpr, I64T());
            C().LocalTee(cap);
            C().I64Const(int64_t(UINT32_MAX));
            C().Int(intop::GtU, true);
            C().If();
            Abort(lit, "invalid capacity");
            C().End();
            C().LocalGet(stk);
            C().LocalGet(cap);
            C().I64Const(esz);
            C().Int(intop::Mul, true);
            C().I64Const(8);
            C().Int(intop::Add, true);
            C().I32WrapI64();
            C().Call(hs.pushslot);
            C().LocalSet(h);
            Store(d, img.At(0, u32), [&] {
                C().LocalGet(cap);
                C().I32WrapI64();
            });
            Store(d, img.At(4, u32), [&] { C().I32Const(0); });
        } else {
            C().LocalGet(stk);
            C().I32Const(8);
            C().Call(hs.pushslot);
            C().LocalSet(h);
            auto n = NewLocal(Repr::I64);
            BuildRzInto(init, stk, et);
            C().LocalSet(n);
            static const std::string msg = "array length exceeds storage range";
            C().LocalGet(n);
            C().I64Const(int64_t(UINT32_MAX));
            C().Int(intop::GtU, true);
            C().If();
            C().I32Const(int32_t(StaticString(msg)));
            C().I32Const(int32_t(msg.size()));
            C().Call(rt.panic);
            C().End();
            for (int64_t off : { 0, 4 })
                Store(d, img.At(off, u32), [&] {
                    C().LocalGet(n);
                    C().I32WrapI64();
                });
        }
        return img;
    }

    // ---- Long-distance returns (spec §7.9) ----

    // `return E from f` stores E in f's channel and sets the global rf to
    // f's id; each function on the way back checks rf after a call that can
    // propagate one (FnSpec::needs), and either is the target, which takes
    // the value and returns it as its own result, or returns at once itself
    // (upstream gs_rf, EmitRfCheck, PropagateReturn). Every exit runs the
    // frames' defer bodies and releases their stacks. Channels hold scalar
    // results; a target returning an aggregate or a resizable is
    // unsupported.
    std::map<g::FnSpec *, int> fromids;
    std::map<std::pair<int, size_t>, int64_t> channels;  // static addresses

    int FromId(g::FnSpec *t) {
        auto it = fromids.find(t);
        if (it != fromids.end()) return it->second;
        return fromids[t] = int(fromids.size()) + 1;
    }
    Loc Channel(const g::Node *at, g::FnSpec *t, size_t i) {
        auto key = std::make_pair(FromId(t), i);
        auto it = channels.find(key);
        if (it == channels.end()) {
            auto rt = t->rets[i];
            if (ReprOf(at, rt) == Repr::None || IsAggregate(rt))
                Unsupported(at, "a long-distance return of this type");
            while (staticdata.size() % 8) staticdata.push_back('\0');
            auto addr = mem0::StaticData + int64_t(staticdata.size());
            staticdata.append(8, '\0');
            it = channels.emplace(key, addr).first;
        }
        return Loc::Abs(it->second, t->rets[i]);
    }

    // Leaves the current function now: its exits' work, zeros for what it
    // returns as values.
    void PropagateOut() {
        ExitRestores(0);
        for (auto r : m.types[F().type].results) {
            switch (r) {
                case ValType::I64: C().I64Const(0); break;
                case ValType::F32: C().F32Const(0); break;
                case ValType::F64: C().F64Const(0); break;
                default: C().I32Const(0); break;
            }
        }
        C().Br(uint32_t(Depth(LK_FUNC)));
    }

    // After a call of callee, which can propagate a long-distance return.
    void RfCheck(const g::Node *at, g::FnSpec *callee) {
        if (callee->needs.empty()) return;
        C().GlobalGet(rfglobal);
        OpenIf(Repr::None);
        bool caught = spec && callee->needs.count(spec);
        if (caught) {
            C().GlobalGet(rfglobal);
            C().I32Const(FromId(spec));
            C().Int(intop::Eq, false);
            OpenIf(Repr::None);
            C().I32Const(0);
            C().GlobalSet(rfglobal);
            ExitRestores(0);
            if (spec->rets.size() > 1 || (spec->rets.size() == 1 && !IsVoid(spec->rets[0]) &&
                                          (IsAggregate(spec->rets[0]) || IsRz(spec->rets[0]))))
                Unsupported(at, "a long-distance return to this function");
            for (size_t i = 0; i < spec->rets.size(); i++)
                if (!IsVoid(spec->rets[i])) Load(at, Channel(at, spec, i));
            C().Br(uint32_t(Depth(LK_FUNC)));
            C().Else();
        }
        if (spec && !spec->needs.empty()) {
            PropagateOut();
        } else {
            static const std::string msg = "unexpected long-distance return";
            C().I32Const(int32_t(StaticString(msg)));
            C().I32Const(int32_t(msg.size()));
            C().Call(rt.panic);
        }
        if (caught) Close();
        Close();
    }

    // return E from f: E into f's channel, then out.
    void ReturnFrom(g::Return *r) {
        auto t = r->targetspec;
        if (!t) Unsupported(r, "return from an enclosing function");
        for (size_t i = 0; i < r->vals.size(); i++) {
            auto ch = Channel(r, t, i);
            Store(r, ch, [&] { Expr(r->vals[i], t->rets[i]); });
        }
        C().I32Const(FromId(t));
        C().GlobalSet(rfglobal);
        PropagateOut();
    }

    // ---- Variable-size values (spec §1.1, §3.3, §3.6; Appendix C.2) ----

    // A value of the variable class is an image of upstream's packed layout:
    // a T[] its length field ([len][elements]), a varint its LEB128 bytes, a
    // struct its fields in order, a variable one where it is reached by
    // walking those before it, a T[..] [cap][len][slots]. A variable, a
    // parameter or a result holds its image on a data stack, and its place
    // is the image; an array of variable elements is sequential, walked
    // element by element. Variable-mode ADTs are not among these yet.
    static bool IsVar(g::TypeExpr *t) {
        if (!t) return false;
        switch (t->kind) {
            case g::TY_INT: return t->intstorage == g::IS_VARINT;
            case g::TY_ARRAY:
                if (t->arr->akind == g::A_VAR) return Layouts::Known(t->arr->sub) || IsVar(t->arr->sub);
                return IsDynLim(t);
            case g::TY_ENUM: {
                // A variable-mode ADT (§3.5): its tag, then the payload of
                // the variant it names, packed.
                if (!t->enu->varmode) return false;
                auto ei = Layouts::EI(t);
                if (!ei) return false;
                for (size_t vi = 0; vi < ei->vftypes.size(); vi++)
                    if (!PayloadImageable(ei, int(vi))) return false;
                return true;
            }
            case g::TY_VARIANT: {
                auto ei = Layouts::EIVar(t);
                if (!ei || Layouts::Known(t)) return false;
                return PayloadImageable(ei, Layouts::VariantIndex(t));
            }
            case g::TY_STRUCT: {
                auto si = Layouts::SI(t);
                if (!si || si->sclass != g::SC_VARIABLE) return false;
                for (size_t i = 0; i < si->ftypes.size(); i++) {
                    if (si->st->fields[i].ispad) {
                        if (si->st->fields[i].padsize < 0) return false;
                        continue;
                    }
                    auto ft = si->ftypes[i];
                    if (!ft || !(Layouts::Known(ft) || IsVar(ft))) return false;
                }
                return true;
            }
            default: return false;
        }
    }
    // Whether two types are the same, as their spellings say.
    static bool SameType(g::TypeExpr *a, g::TypeExpr *b) {
        if (a == b) return true;
        if (!a || !b) return false;
        std::string x, y;
        a->Dump(x);
        b->Dump(y);
        return x == y;
    }
    static bool PayloadImageable(g::EnumInst *ei, int vi) {
        auto &fs = ei->en->variants[vi].fields;
        for (size_t i = 0; i < fs.size(); i++) {
            if (fs[i].ispad) {
                if (fs[i].padsize < 0) return false;
                continue;
            }
            auto ft = ei->vftypes[vi][i];
            if (!ft || !(Layouts::Known(ft) || IsVar(ft))) return false;
        }
        return true;
    }
    // Known or variable: what the image code can lay out.
    static bool Imageable(g::TypeExpr *t) { return Layouts::Known(t) || IsVar(t); }
    static bool IsVarArray(g::TypeExpr *t) { return IsVar(t) && t->kind == g::TY_ARRAY; }
    // A T[]'s length field: u32 unless written, varint as ULEB128.
    static g::IntStorage VarLenStore(g::TypeExpr *t) {
        return t->arr->lenstorage < 0 ? g::IS_U32 : g::IntStorage(t->arr->lenstorage);
    }

    // The LEB128 helpers (upstream gs_uleb_read, gs_zig_read, gs_uleb_write,
    // gs_zig_write), made on first use.
    uint32_t vlenfn = 0, ulebfn = 0, zigfn = 0, ulebwfn = 0, zigwfn = 0;
    void DefineLeb() {
        if (vlenfn) return;
        using V = ValType;
        auto ld = hs.Load(m, 0x2d), st = hs.Store(m, 0x3a);
        // vlen(h): the bytes of the LEB128 number at h.
        vlenfn = m.AddFunc({ { V::I64 }, { V::I64 } }, { V::I64 });
        {
            auto &c = m.FuncAt(vlenfn).code;
            enum { h, n };
            c.Loop();
            c.LocalGet(h);
            c.LocalGet(n);
            c.Int(intop::Add, true);
            c.Call(ld);
            c.LocalGet(n);
            c.I64Const(1);
            c.Int(intop::Add, true);
            c.LocalSet(n);
            c.I32Const(0x80);
            c.I32And();
            c.BrIf(0);
            c.End();
            c.LocalGet(n);
        }
        // uleb(h): its unsigned value.
        ulebfn = m.AddFunc({ { V::I64 }, { V::I64 } }, { V::I64, V::I64, V::I32 });
        {
            auto &c = m.FuncAt(ulebfn).code;
            enum { h, r, sh, b };
            c.Loop();
            c.LocalGet(h);
            c.Call(ld);
            c.LocalSet(b);
            c.LocalGet(r);
            c.LocalGet(b);
            c.I32Const(0x7f);
            c.I32And();
            c.I64ExtendI32U();
            c.LocalGet(sh);
            c.Int(intop::Shl, true);
            c.Int(intop::Or, true);
            c.LocalSet(r);
            c.LocalGet(sh);
            c.I64Const(7);
            c.Int(intop::Add, true);
            c.LocalSet(sh);
            c.LocalGet(h);
            c.I64Const(1);
            c.Int(intop::Add, true);
            c.LocalSet(h);
            c.LocalGet(b);
            c.I32Const(0x80);
            c.I32And();
            c.BrIf(0);
            c.End();
            c.LocalGet(r);
        }
        // zig(h): its signed value, zigzag-decoded.
        zigfn = m.AddFunc({ { V::I64 }, { V::I64 } }, { V::I64 });
        {
            auto &c = m.FuncAt(zigfn).code;
            enum { h, u };
            c.LocalGet(h);
            c.Call(ulebfn);
            c.LocalTee(u);
            c.I64Const(1);
            c.Int(intop::ShrU, true);
            c.I64Const(0);
            c.LocalGet(u);
            c.I64Const(1);
            c.Int(intop::And, true);
            c.Int(intop::Sub, true);
            c.Int(intop::Xor, true);
        }
        // ulebw(stk, v): v's shortest LEB128 at the top of stack stk.
        ulebwfn = m.AddFunc({ { V::I32, V::I64 }, {} }, { V::I32, V::I64 });
        {
            auto &c = m.FuncAt(ulebwfn).code;
            enum { stk, v, b, h };
            c.Loop();
            c.LocalGet(v);
            c.I32WrapI64();
            c.I32Const(0x7f);
            c.I32And();
            c.LocalSet(b);
            c.LocalGet(v);
            c.I64Const(7);
            c.Int(intop::ShrU, true);
            c.LocalTee(v);
            c.I64Eqz();
            c.I32Eqz();
            c.If();
            c.LocalGet(b);
            c.I32Const(0x80);
            c.Int(intop::Or, false);
            c.LocalSet(b);
            c.End();
            c.LocalGet(stk);
            c.I32Const(1);
            c.Call(hs.pushslot);
            c.LocalSet(h);
            c.LocalGet(h);
            c.LocalGet(b);
            c.Call(st);
            c.LocalGet(v);
            c.I64Eqz();
            c.I32Eqz();
            c.BrIf(0);
            c.End();
        }
        // zigw(stk, v): v zigzag-encoded, then as ulebw.
        zigwfn = m.AddFunc({ { V::I32, V::I64 }, {} });
        {
            auto &c = m.FuncAt(zigwfn).code;
            enum { stk, v };
            c.LocalGet(stk);
            c.LocalGet(v);
            c.I64Const(1);
            c.Int(intop::Shl, true);
            c.LocalGet(v);
            c.I64Const(63);
            c.Int(intop::ShrS, true);
            c.Int(intop::Xor, true);
            c.Call(ulebwfn);
        }
    }

    // The size of the image of type t at handle h: a function per type,
    // emitted with the others (EmitSizeFn).
    std::map<std::string, uint32_t> sizefns;
    std::vector<std::pair<g::TypeExpr *, uint32_t>> pendingsizes;
    uint32_t SizeFnOf(g::TypeExpr *t) {
        std::string key;
        t->Dump(key);
        auto it = sizefns.find(key);
        if (it != sizefns.end()) return it->second;
        DefineLeb();
        auto index = m.AddFunc({ { ValType::I64 }, { ValType::I64 } });
        sizefns[key] = index;
        pendingsizes.push_back({ t, index });
        return index;
    }
    void EmitSizeFn(g::TypeExpr *t, uint32_t index) {
        fn = index;
        spec = nullptr;
        locals.clear();
        memvars.clear();
        rzvars.clear();
        gstklocal.clear();
        stkconsts.clear();
        fls.clear();
        labels.clear();
        scopes.clear();
        frame = 0;
        stknext = stkmax = 0;
        retdst = retrz = atparam = -1;
        nparams = 1;
        fp = NewLocal(Repr::I32);
        auto cur = NewLocal(Repr::I64);
        C().LocalGet(0);
        C().LocalSet(cur);
        Skip(t, cur);
        C().LocalGet(cur);
        C().LocalGet(0);
        C().Int(intop::Sub, true);
        FinishFrame(false);
    }

    // The size of the value of type t at l, an i64.
    void SizeOf(const Loc &l, g::TypeExpr *t) {
        if (Layouts::Known(t)) {
            C().I64Const(lay.FixedSize(t));
            return;
        }
        HandleOf(l);
        C().Call(SizeFnOf(t));
    }
    // Advances the handle in local cur past a value of type t.
    void Skip(g::TypeExpr *t, uint32_t cur) {
        C().LocalGet(cur);
        if (Layouts::Known(t)) {
            C().I64Const(lay.FixedSize(t));
        } else if (t->kind == g::TY_INT) {
            DefineLeb();
            C().LocalGet(cur);
            C().Call(vlenfn);
        } else if (t->kind == g::TY_ARRAY && t->arr->akind == g::A_VAR) {
            // The length field, then the elements.
            C().Drop();
            auto n = NewLocal(Repr::I64);
            auto et = t->arr->sub;
            auto ls = VarLenStore(t);
            ReadVarLen(Loc { false, cur, 0, t, false }, ls, n);
            C().LocalGet(cur);
            if (ls == g::IS_VARINT) {
                C().LocalGet(cur);
                C().Call(vlenfn);
            } else {
                C().I64Const(Layouts::IntSize(ls));
            }
            C().Int(intop::Add, true);
            C().LocalSet(cur);
            if (Layouts::Known(et)) {
                C().LocalGet(cur);
                C().LocalGet(n);
                C().I64Const(lay.FixedSize(et));
                C().Int(intop::Mul, true);
                C().Int(intop::Add, true);
                C().LocalSet(cur);
            } else {
                Open(LK_PLAIN, Repr::None);
                Open(LK_PLAIN, Repr::None, nullptr, nullptr, true);
                C().LocalGet(n);
                C().I64Eqz();
                C().BrIf(1);
                C().LocalGet(cur);
                C().LocalGet(cur);
                C().Call(SizeFnOf(et));
                C().Int(intop::Add, true);
                C().LocalSet(cur);
                C().LocalGet(n);
                C().I64Const(1);
                C().Int(intop::Sub, true);
                C().LocalSet(n);
                C().Br(0);
                Close();
                Close();
            }
            return;
        } else if (IsDynLim(t)) {
            Load(nullptr, Loc { false, cur, 0, ast.inttypes[g::IS_U32], false });
            C().I64ExtendI32U();
            C().I64Const(lay.FixedSize(t->arr->sub));
            C().Int(intop::Mul, true);
            C().I64Const(8);
            C().Int(intop::Add, true);
        } else if (t->kind == g::TY_ENUM || t->kind == g::TY_VARIANT) {
            C().Drop();
            auto ei = t->kind == g::TY_ENUM ? Layouts::EI(t) : Layouts::EIVar(t);
            auto skipfields = [&](int vi) {
                auto &fs = ei->en->variants[vi].fields;
                for (size_t i = 0; i < fs.size(); i++) {
                    if (fs[i].ispad) {
                        C().LocalGet(cur);
                        C().I64Const(fs[i].padsize);
                        C().Int(intop::Add, true);
                        C().LocalSet(cur);
                        continue;
                    }
                    Skip(ei->vftypes[vi][i], cur);
                }
            };
            if (t->kind == g::TY_VARIANT) return skipfields(Layouts::VariantIndex(t));
            auto tag = NewLocal(Repr::I32);
            LoadTag(Loc { false, cur, 0, t, false });
            C().LocalSet(tag);
            C().LocalGet(cur);
            C().I64Const(Layouts::TagSize(ei->en));
            C().Int(intop::Add, true);
            C().LocalSet(cur);
            for (size_t vi = 0; vi < ei->en->variants.size(); vi++) {
                if (ei->en->variants[vi].fields.empty()) continue;
                C().LocalGet(tag);
                C().I32Const(int32_t(vi));
                C().Int(intop::Eq, false);
                OpenIf(Repr::None);
                skipfields(int(vi));
                Close();
            }
            return;
        } else if (t->kind == g::TY_STRUCT) {
            C().Drop();
            auto si = Layouts::SI(t);
            for (size_t i = 0; i < si->ftypes.size(); i++) {
                if (si->st->fields[i].ispad) {
                    C().LocalGet(cur);
                    C().I64Const(si->st->fields[i].padsize);
                    C().Int(intop::Add, true);
                    C().LocalSet(cur);
                    continue;
                }
                Skip(si->ftypes[i], cur);
            }
            return;
        } else {
            throw g::CompileError { "goose-wasm: unsupported: the size of this type" };
        }
        C().Int(intop::Add, true);
        C().LocalSet(cur);
    }

    // A T[] image's length, into local n.
    void ReadVarLen(const Loc &l, g::IntStorage ls, uint32_t n) {
        if (ls == g::IS_VARINT) {
            DefineLeb();
            HandleOf(l);
            C().Call(ulebfn);
        } else {
            auto lt = ast.inttypes[ls];
            Load(nullptr, Loc { l.frame, l.h, l.off, lt, l.mem0, l.abs });
            Convert(nullptr, lt, I64T());
        }
        C().LocalSet(n);
    }

    // Field f of the struct of type t whose image is at l: at its offset,
    // fixed while every field before it is, else walked to.
    Loc FieldLoc(const Loc &l, g::TypeExpr *t, int f) {
        auto si = Layouts::SI(t);
        return FieldsLoc(l, si->st->fields, si->ftypes, f);
    }
    // A variant's payload field f, its payload at l.
    Loc PayloadField(const Loc &l, g::EnumInst *ei, int vi, int f) {
        return FieldsLoc(l, ei->en->variants[vi].fields, ei->vftypes[vi], f);
    }
    Loc FieldsLoc(const Loc &l, const std::vector<g::Field> &fields, const std::vector<g::TypeExpr *> &fts, int f) {
        struct { const std::vector<g::Field> &fs; const std::vector<g::TypeExpr *> &ts; } sv { fields, fts };
        auto ft = fts[f];
        int64_t off = 0;
        int i = 0;
        for (; i < f; i++) {
            if (sv.fs[i].ispad) {
                off += sv.fs[i].padsize;
                continue;
            }
            if (!Layouts::Known(sv.ts[i])) break;
            off += lay.FixedSize(sv.ts[i]);
        }
        if (i == f) return l.At(off, ft);
        auto cur = NewLocal(Repr::I64);
        HandleOf(l);
        C().I64Const(off);
        C().Int(intop::Add, true);
        C().LocalSet(cur);
        for (; i < f; i++) {
            if (sv.fs[i].ispad) {
                C().LocalGet(cur);
                C().I64Const(sv.fs[i].padsize);
                C().Int(intop::Add, true);
                C().LocalSet(cur);
                continue;
            }
            Skip(sv.ts[i], cur);
        }
        return Loc { false, cur, 0, ft, false };
    }

    // The elements of the array image at l (a T[], or a T[..]).
    Seq ImageSeq(const g::Node *at, const Loc &l) {
        auto t = l.t;
        if (IsDynLim(t)) return LimSeq(at, l);
        Seq s;
        s.et = t->arr->sub;
        s.data = NewLocal(Repr::I64);
        auto n = NewLocal(Repr::I64);
        auto ls = VarLenStore(t);
        ReadVarLen(l, ls, n);
        HandleOf(l);
        if (ls == g::IS_VARINT) {
            HandleOf(l);
            C().Call(vlenfn);
        } else {
            C().I64Const(Layouts::IntSize(ls));
        }
        C().Int(intop::Add, true);
        C().LocalSet(s.data);
        s.len = [this, n] { C().LocalGet(n); };
        return s;
    }

    // The handle of the top of stack stk, an i64.
    void TopHandle(uint32_t stk) {
        C().LocalGet(stk);
        C().I32Const(1);
        C().I32Add();
        C().I64ExtendI32U();
        C().I64Const(32);
        C().Int(intop::Shl, true);
        TopAddr(stk);
        C().I32Load(mem0::Tops);
        C().I64ExtendI32U();
        C().Int(intop::Or, true);
    }

    // A temporary image of n as type t, on a stack of the scope's.
    Loc TempVar(g::Node *n, g::TypeExpr *t) {
        uint32_t base = 0;
        auto stk = ClaimStack(base);
        auto h = NewLocal(Repr::I64);
        TopHandle(stk);
        C().LocalSet(h);
        BuildVar(n, stk, t);
        return Loc { false, h, 0, t, false };
    }
    // The image of a variable-size value n: its place, or a temporary.
    Loc VarPlace(g::Node *n, g::TypeExpr *t) {
        if (n->nkind == g::NK_Unary && static_cast<g::Unary *>(n)->op == g::T_BITAND)
            n = static_cast<g::Unary *>(n)->child;
        Loc l;
        if (MemLoc(n, l)) {
            if (IsPlainRef(l.t) && !IsPlainRef(t)) l = Deref(n, l);
            if (IsVar(l.t)) return l;
        }
        return TempVar(n, t);
    }

    // A length field of type ls at the top of stack stk, from local count,
    // which must fit it (upstream EmitLenStore).
    void WriteVarLen(uint32_t stk, g::IntStorage ls, uint32_t count) {
        if (ls == g::IS_VARINT) {
            DefineLeb();
            C().LocalGet(stk);
            C().LocalGet(count);
            C().Call(ulebwfn);
            return;
        }
        auto size = Layouts::IntSize(ls);
        if (size < 8) {
            static const std::string msg = "array length exceeds storage range";
            C().LocalGet(count);
            C().I64Const(int64_t((1ull << (8 * size)) - 1));
            C().Int(intop::GtU, true);
            C().If();
            C().I32Const(int32_t(StaticString(msg)));
            C().I32Const(int32_t(msg.size()));
            C().Call(rt.panic);
            C().End();
        }
        auto h = NewLocal(Repr::I64);
        C().LocalGet(stk);
        C().I32Const(int32_t(size));
        C().Call(hs.pushslot);
        C().LocalSet(h);
        auto lt = ast.inttypes[ls];
        Store(nullptr, Loc { false, h, 0, lt, false }, [&] {
            C().LocalGet(count);
            if (size < 8) C().I32WrapI64();
        });
    }

    // Appends the image of n, as type t, at the top of stack stk (upstream
    // GenConstruct onto a stack): built part by part, a fixed part in its
    // slot, so a relative reference in it is measured from where it stays.
    void BuildVar(g::Node *n, uint32_t stk, g::TypeExpr *t) {
        switch (n->nkind) {
            case g::NK_Block: {
                auto b = static_cast<g::Block *>(n);
                PushScope();
                for (auto st : b->stmts) Stmt(st);
                if (b->tail) BuildVar(b->tail, stk, t);
                else C().Unreachable();
                PopScope();
                return;
            }
            case g::NK_IfExpr: {
                auto i = static_cast<g::IfExpr *>(n);
                if (!i->elseb) Unsupported(n, "an if of a value without else");
                Truth(i->cond);
                OpenIf(Repr::None);
                BuildVar(i->thenb, stk, t);
                C().Else();
                PushScope();
                BuildVar(i->elseb, stk, t);
                PopScope();
                Close();
                return;
            }
            case g::NK_MatchExpr:
                Match(static_cast<g::MatchExpr *>(n), Repr::None, [&](g::Node *b) { BuildVar(b, stk, t); });
                return;
            case g::NK_InlineBlock: {
                auto ib = static_cast<g::InlineBlock *>(n);
                if (ib->spec && ib->spec->rets.size() == 1 && !SameType(ib->spec->rets[0], t)) break;
                Open(LK_INLINE, Repr::None, n->exprtype, ib->sf);
                labels.back().rzstk = stk;
                labels.back().vart = t;
                BuildVar(ib->body, stk, t);
                Close();
                return;
            }
            case g::NK_EarlyBlock: {
                Open(LK_BREAK, Repr::None, n->exprtype);
                labels.back().rzstk = stk;
                labels.back().vart = t;
                BuildVar(static_cast<g::EarlyBlock *>(n)->body, stk, t);
                Close();
                return;
            }
            case g::NK_Call: {
                auto c = static_cast<g::Call *>(n);
                if (c->builtin == g::B_COPY) return BuildVar(c->FirstArg(), stk, t);
                if (c->fvbody) {
                    FvCall(c, [&](g::Node *tl) {
                        if (tl) BuildVar(tl, stk, t);
                        else C().Unreachable();
                    });
                    return;
                }
                if (c->builtin >= 0 && c->rettypes.size() == 1 && IsRz(c->rettypes[0]) &&
                    t->kind == g::TY_ARRAY && t->arr->akind == g::A_VAR) {
                    // A builtin's resizable result (str's text, to_bytes'
                    // image), built aside, then as a T[]: its length field
                    // and its elements.
                    PushScope();
                    uint32_t base = 0;
                    auto tmp = ClaimStack(base);
                    auto count = NewLocal(Repr::I64);
                    Seq sq;
                    sq.et = t->arr->sub;
                    sq.data = NewLocal(Repr::I64);
                    TopHandle(tmp);
                    C().LocalSet(sq.data);
                    BuildRzInto(n, tmp, c->rettypes[0]->arr->sub);
                    C().LocalSet(count);
                    sq.len = [this, count] { C().LocalGet(count); };
                    WriteVarLen(stk, VarLenStore(t), count);
                    CopyRun(n, sq, stk, int32_t(lay.FixedSize(sq.et)));
                    C().Drop();
                    PopScope();
                    return;
                }
                if (c->builtin < 0 && (c->spec || !c->dispatch.empty()) && c->rettypes.size() == 1 &&
                    IsVar(c->rettypes[0]) && SameType(c->rettypes[0], t)) {
                    CallSpec(c, nullptr, stk);
                    C().Drop();
                    return;
                }
                break;
            }
            default: break;
        }
        if (Layouts::Known(t)) {
            // A fixed part in its slot.
            auto h = NewLocal(Repr::I64);
            C().LocalGet(stk);
            C().I32Const(int32_t(lay.FixedSize(t)));
            C().Call(hs.pushslot);
            C().LocalSet(h);
            Loc slot { false, h, 0, t, false };
            if (n->nkind == g::NK_NullLit || (IsAggregate(t) == false && ReprOf(n, t) != Repr::None))
                Store(n, slot, [&] { Expr(n, t); });
            else
                Construct(n, slot);
            return;
        }
        if (t->kind == g::TY_INT) {
            DefineLeb();
            auto v = NewLocal(Repr::I64);
            Expr(n, IsInt(n->exprtype) && n->exprtype->intstorage != g::IS_VARINT ? n->exprtype : I64T());
            if (IsInt(n->exprtype) && n->exprtype->intstorage != g::IS_VARINT)
                Convert(n, n->exprtype, I64T());
            C().LocalSet(v);
            C().LocalGet(stk);
            C().LocalGet(v);
            C().Call(zigwfn);
            return;
        }
        // The image of an existing value of the same type: its bytes.
        Loc src;
        if (MemLoc(n, src)) {
            if (IsPlainRef(src.t) && !IsPlainRef(t)) src = Deref(n, src);
            if (IsVar(src.t) && SameType(src.t, t)) return CopyImage(src, stk);
        }
        // A call giving a reference to such a value: what it names.
        if (auto rt = RefResult(n)) {
            if (SameType(rt->ref->sub, t)) {
                auto l = ObjectLoc(n);
                l.t = t;
                return CopyImage(l, stk);
            }
        }
        if (t->kind == g::TY_ENUM || t->kind == g::TY_VARIANT) return BuildAdt(n, stk, t);
        if (t->kind == g::TY_STRUCT) {
            if (n->nkind != g::NK_StructLit) Unsupported(n, "this variable-size struct");
            auto sl = static_cast<g::StructLit *>(n);
            auto si = Layouts::SI(t);
            for (size_t i = 0; i < si->ftypes.size(); i++) {
                if (si->st->fields[i].ispad) {
                    auto h = NewLocal(Repr::I64);
                    C().LocalGet(stk);
                    C().I32Const(int32_t(si->st->fields[i].padsize));
                    C().Call(hs.pushslot);
                    C().LocalSet(h);
                    continue;
                }
                auto ft = si->ftypes[i];
                auto init = sl->InitFor(int(i));
                if (!init) {
                    // An omitted optional field: null.
                    auto h = NewLocal(Repr::I64);
                    C().LocalGet(stk);
                    C().I32Const(int32_t(lay.FixedSize(ft)));
                    C().Call(hs.pushslot);
                    C().LocalSet(h);
                    OmittedField(n, Loc { false, h, 0, ft, false });
                    continue;
                }
                BuildVar(init, stk, ft);
            }
            return;
        }
        if (IsDynLim(t)) {
            BuildDynLim(n, n, stk, t);
            return;
        }
        if (t->kind != g::TY_ARRAY || t->arr->akind != g::A_VAR) Unsupported(n, "this variable-size value");
        auto et = t->arr->sub;
        auto ls = VarLenStore(t);
        auto count = NewLocal(Repr::I64);
        if (n->nkind == g::NK_ArrayLit) {
            auto lit = static_cast<g::ArrayLit *>(n);
            if (lit->capexpr) Unsupported(n, "a capacity literal as a T[]");
            if (!lit->fillval) {
                C().I64Const(int64_t(lit->elems.size()));
                C().LocalSet(count);
                WriteVarLen(stk, ls, count);
                for (auto e : lit->elems) BuildVar(e, stk, et);
                return;
            }
            if (!Layouts::Known(et)) {
                // The value once (upstream FillScope), then copies of its
                // image.
                Expr(lit->fillcount, I64T());
                C().LocalSet(count);
                PushScope();
                auto img = TempVar(lit->fillval, et);
                WriteVarLen(stk, ls, count);
                auto i = NewLocal(Repr::I64);
                C().I64Const(0);
                C().LocalSet(i);
                Open(LK_PLAIN, Repr::None);
                Open(LK_PLAIN, Repr::None, nullptr, nullptr, true);
                C().LocalGet(i);
                C().LocalGet(count);
                C().Int(intop::GeU, true);
                C().BrIf(1);
                CopyImage(img, stk);
                C().LocalGet(i);
                C().I64Const(1);
                C().Int(intop::Add, true);
                C().LocalSet(i);
                C().Br(0);
                Close();
                Close();
                PopScope();
                return;
            }
            // The value once, then copies (upstream FillScope).
            auto first = FillValue(lit->fillval, et);
            Expr(lit->fillcount, I64T());
            C().LocalSet(count);
            WriteVarLen(stk, ls, count);
            auto i = NewLocal(Repr::I64), h = NewLocal(Repr::I64);
            C().I64Const(0);
            C().LocalSet(i);
            Open(LK_PLAIN, Repr::None);
            Open(LK_PLAIN, Repr::None, nullptr, nullptr, true);
            C().LocalGet(i);
            C().LocalGet(count);
            C().Int(intop::GeU, true);
            C().BrIf(1);
            C().LocalGet(stk);
            C().I32Const(int32_t(lay.FixedSize(et)));
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
            return;
        }
        // A call (or an inlined one) giving a fixed array, a limited one
        // say: built as its own type, checks included, then its elements.
        g::TypeExpr *own = nullptr;
        if (n->nkind == g::NK_Call && static_cast<g::Call *>(n)->builtin < 0 &&
            static_cast<g::Call *>(n)->rettypes.size() == 1)
            own = static_cast<g::Call *>(n)->rettypes[0];
        if (n->nkind == g::NK_InlineBlock && static_cast<g::InlineBlock *>(n)->spec &&
            static_cast<g::InlineBlock *>(n)->spec->rets.size() == 1)
            own = static_cast<g::InlineBlock *>(n)->spec->rets[0];
        Seq sq;
        if (own && IsAggregate(own) && (IsView(own) || own->kind == g::TY_ARRAY)) {
            auto tmp = FrameSlot(own);
            Construct(n, tmp);
            sq = SeqAt(n, tmp);
        } else if (own && IsRz(own)) {
            // A resizable result: built on a stack of the statement's.
            sq = TempRz(n, own->arr->sub);
        } else if (own && IsVarArray(own) && !SameType(own, t)) {
            // A variable-size array of another kind: its image as its own
            // type, then its elements.
            sq = ImageSeq(n, TempVar(n, own));
        } else {
            // Another sequence of the elements: its count, then its run.
            if (IsVarArray(n->exprtype) && !(n->nkind == g::NK_Ident || n->nkind == g::NK_Dot ||
                                             n->nkind == g::NK_Index || n->nkind == g::NK_StrLit ||
                                             n->nkind == g::NK_SliceExpr))
                Unsupported(n, g::cat("a variable-size value from ", KindName(n->nkind)));
            sq = SeqOf(n);
        }
        sq.len();
        C().LocalSet(count);
        WriteVarLen(stk, ls, count);
        if (Layouts::Known(et)) {
            CopyRun(n, sq, stk, int32_t(lay.FixedSize(et)));
            C().Drop();
            return;
        }
        // Variable elements: their region, walked for its size.
        auto cur = NewLocal(Repr::I64), i = NewLocal(Repr::I64), h = NewLocal(Repr::I64);
        C().LocalGet(sq.data);
        C().LocalSet(cur);
        C().I64Const(0);
        C().LocalSet(i);
        Open(LK_PLAIN, Repr::None);
        Open(LK_PLAIN, Repr::None, nullptr, nullptr, true);
        C().LocalGet(i);
        C().LocalGet(count);
        C().Int(intop::GeU, true);
        C().BrIf(1);
        Skip(et, cur);
        C().LocalGet(i);
        C().I64Const(1);
        C().Int(intop::Add, true);
        C().LocalSet(i);
        C().Br(0);
        Close();
        Close();
        auto size = NewLocal(Repr::I64);
        C().LocalGet(cur);
        C().LocalGet(sq.data);
        C().Int(intop::Sub, true);
        C().LocalSet(size);
        C().LocalGet(stk);
        C().LocalGet(size);
        C().I32WrapI64();
        C().Call(hs.pushslot);
        C().LocalSet(h);
        C().LocalGet(h);
        C().LocalGet(sq.data);
        C().LocalGet(size);
        C().I32WrapI64();
        C().Call(hs.copy);
    }

    // A variable-mode ADT's image (or a variant's payload): the tag at the
    // top, then the fields of the variant the value is.
    void BuildAdt(g::Node *n, uint32_t stk, g::TypeExpr *t) {
        bool adt = t->kind == g::TY_ENUM;
        auto ei = adt ? Layouts::EI(t) : Layouts::EIVar(t);
        auto tag = [&](int vi) {
            if (!adt) return;
            auto h = NewLocal(Repr::I64);
            C().LocalGet(stk);
            C().I32Const(int32_t(Layouts::TagSize(ei->en)));
            C().Call(hs.pushslot);
            C().LocalSet(h);
            StoreTag(Loc { false, h, 0, t, false }, vi);
        };
        if (n->nkind == g::NK_Dot && static_cast<g::Dot *>(n)->variantconst) {
            tag(ei->en->VariantIndex(static_cast<g::Dot *>(n)->variantconst));
            return;
        }
        if (n->nkind == g::NK_StructLit && static_cast<g::StructLit *>(n)->einst) {
            auto sl = static_cast<g::StructLit *>(n);
            auto vi = ei->en->VariantIndex(sl->variant);
            tag(vi);
            auto &fs = sl->variant->fields;
            for (size_t i = 0; i < fs.size(); i++) {
                auto h = NewLocal(Repr::I64);
                if (fs[i].ispad) {
                    C().LocalGet(stk);
                    C().I32Const(int32_t(fs[i].padsize));
                    C().Call(hs.pushslot);
                    C().LocalSet(h);
                    continue;
                }
                auto ft = ei->vftypes[vi][i];
                auto init = sl->InitFor(int(i));
                if (!init) {
                    C().LocalGet(stk);
                    C().I32Const(int32_t(lay.FixedSize(ft)));
                    C().Call(hs.pushslot);
                    C().LocalSet(h);
                    OmittedField(n, Loc { false, h, 0, ft, false });
                    continue;
                }
                BuildVar(init, stk, ft);
            }
            return;
        }
        auto st = OperandType(n->exprtype);
        // What a variable holds, before the use's adaptation.
        if (n->nkind == g::NK_Ident || n->nkind == g::NK_Dot)
            if (auto own = StoredType(n)) st = own;
        // What a call gives, as its own type.
        if (n->nkind == g::NK_Call && static_cast<g::Call *>(n)->builtin < 0 &&
            static_cast<g::Call *>(n)->rettypes.size() == 1)
            st = static_cast<g::Call *>(n)->rettypes[0];
        if (n->nkind == g::NK_InlineBlock && static_cast<g::InlineBlock *>(n)->spec &&
            static_cast<g::InlineBlock *>(n)->spec->rets.size() == 1)
            st = static_cast<g::InlineBlock *>(n)->spec->rets[0];
        bool popped = n->nkind == g::NK_Call && static_cast<g::Call *>(n)->builtin == g::B_POP;
        if (popped) {
            auto at = static_cast<g::Call *>(n)->ArgNodes()[0]->exprtype;
            if (at->kind == g::TY_REF) at = at->ref->sub;
            st = at->arr->sub;
        }
        // The fixed-mode ADT of the same enum: its tag, then just the
        // payload of the variant it names.
        if (adt && st && st->kind == g::TY_ENUM && !st->enu->varmode && Layouts::FixedAdt(st) &&
            Layouts::EI(st)->en == ei->en) {
            Loc src;
            if (popped) {
                src = Loc { false, Pop(static_cast<g::Call *>(n)), 0, st, false };
            } else if (MemLoc(n, src)) {
                if (IsPlainRef(src.t)) src = Deref(n, src);
            } else {
                src = FrameSlot(st);
                Construct(n, src);
            }
            src.t = st;
            auto sei = Layouts::EI(st);
            auto ts = Layouts::TagSize(ei->en);
            auto tagl = NewLocal(Repr::I32), h = NewLocal(Repr::I64);
            LoadTag(src);
            C().LocalSet(tagl);
            C().LocalGet(stk);
            C().I32Const(int32_t(ts));
            C().Call(hs.pushslot);
            C().LocalSet(h);
            Store(n, Loc { false, h, 0, ast.inttypes[ts == 1 ? g::IS_U8 : g::IS_U16], false },
                  [&] { C().LocalGet(tagl); });
            for (size_t vi = 0; vi < sei->en->variants.size(); vi++) {
                auto size = lay.VariantLayout(sei, int(vi)).size;
                if (!size) continue;
                C().LocalGet(tagl);
                C().I32Const(int32_t(vi));
                C().Int(intop::Eq, false);
                OpenIf(Repr::None);
                C().LocalGet(stk);
                C().I32Const(int32_t(size));
                C().Call(hs.pushslot);
                HandleOf(src.At(ts, nullptr));
                C().I32Const(int32_t(size));
                C().Call(hs.copy);
                Close();
            }
            return;
        }
        // A value of one of its variant types: the tag, then its payload.
        if (adt && st && st->kind == g::TY_VARIANT) {
            tag(Layouts::VariantIndex(st));
            Loc src;
            if (MemLoc(n, src)) {
                if (IsPlainRef(src.t) && !IsPlainRef(st)) src = Deref(n, src);
                src.t = st;
                if (Layouts::Known(st)) {
                    auto h = NewLocal(Repr::I64);
                    C().LocalGet(stk);
                    C().I32Const(int32_t(lay.FixedSize(st)));
                    C().Call(hs.pushslot);
                    C().LocalSet(h);
                    Copy(Loc { false, h, 0, st, false }, src);
                } else {
                    CopyImage(src, stk);
                }
                return;
            }
            return BuildVar(n, stk, st);
        }
        Unsupported(n, "this variable-mode ADT value");
    }

    // The bytes of the image at src, appended at the top of stack stk.
    void CopyImage(const Loc &src, uint32_t stk) {
        auto size = NewLocal(Repr::I64), h = NewLocal(Repr::I64);
        SizeOf(src, src.t);
        C().LocalSet(size);
        C().LocalGet(stk);
        C().LocalGet(size);
        C().I32WrapI64();
        C().Call(hs.pushslot);
        C().LocalSet(h);
        C().LocalGet(h);
        HandleOf(src);
        C().LocalGet(size);
        C().I32WrapI64();
        C().Call(hs.copy);
    }

    // The default image of type t (no initializer): empty arrays, zero
    // scalars, field defaults where declared.
    void BuildDefault(const g::Node *at, uint32_t stk, g::TypeExpr *t) {
        if (Layouts::Known(t)) {
            auto h = NewLocal(Repr::I64);
            C().LocalGet(stk);
            C().I32Const(int32_t(lay.FixedSize(t)));
            C().Call(hs.pushslot);
            C().LocalSet(h);
            ZeroAt(Loc { false, h, 0, t, false });
            return;
        }
        if (t->kind == g::TY_INT) {
            DefineLeb();
            C().LocalGet(stk);
            C().I64Const(0);
            C().Call(zigwfn);
            return;
        }
        if (t->kind == g::TY_ARRAY && t->arr->akind == g::A_VAR) {
            auto z = NewLocal(Repr::I64);
            C().I64Const(0);
            C().LocalSet(z);
            return WriteVarLen(stk, VarLenStore(t), z);
        }
        if (t->kind == g::TY_STRUCT) {
            auto si = Layouts::SI(t);
            for (size_t i = 0; i < si->ftypes.size(); i++) {
                if (si->st->fields[i].ispad) {
                    auto h = NewLocal(Repr::I64);
                    C().LocalGet(stk);
                    C().I32Const(int32_t(si->st->fields[i].padsize));
                    C().Call(hs.pushslot);
                    C().LocalSet(h);
                    continue;
                }
                auto dv = si->st->fields[i].defaultval;
                if (dv) BuildVar(dv, stk, si->ftypes[i]);
                else BuildDefault(at, stk, si->ftypes[i]);
            }
            return;
        }
        Unsupported(at, "a default of this variable-size type");
    }

    // A variable-size local: its image on a stack of its own.
    void DeclareVarImage(g::VarDecl *d, g::VarDef *v, g::Node *init) {
        uint32_t base = 0;
        auto stk = ClaimStack(base);
        auto h = NewLocal(Repr::I64);
        TopHandle(stk);
        C().LocalSet(h);
        if (init) BuildVar(init, stk, v->type);
        else BuildDefault(d, stk, v->type);
        memvars[v] = Loc { false, h, 0, v->type, false };
    }

    // to_bytes(a) (§12; upstream EmitToBytes): a's image, a ULEB128 byte
    // count then its element region, at the top of stack stk; the bytes
    // added, as a count, on the Wasm stack.
    void ToBytes(g::Call *call, uint32_t stk) {
        DefineLeb();
        auto sq = SeqOf(call->ArgNodes()[0]);
        auto count = NewLocal(Repr::I64), size = NewLocal(Repr::I64), before = NewLocal(Repr::I32);
        sq.len();
        C().LocalSet(count);
        if (Layouts::Known(sq.et)) {
            C().LocalGet(count);
            C().I64Const(lay.FixedSize(sq.et));
            C().Int(intop::Mul, true);
            C().LocalSet(size);
        } else {
            RegionSize(sq, count, size);
        }
        TopAddr(stk);
        C().I32Load(mem0::Tops);
        C().LocalSet(before);
        C().LocalGet(stk);
        C().LocalGet(size);
        C().Call(ulebwfn);
        auto h = NewLocal(Repr::I64);
        C().LocalGet(stk);
        C().LocalGet(size);
        C().I32WrapI64();
        C().Call(hs.pushslot);
        C().LocalSet(h);
        C().LocalGet(h);
        C().LocalGet(sq.data);
        C().LocalGet(size);
        C().I32WrapI64();
        C().Call(hs.copy);
        TopAddr(stk);
        C().I32Load(mem0::Tops);
        C().LocalGet(before);
        C().I32Sub();
        C().I64ExtendI32U();
    }

    // ---- The debug build (runtime define GS_DEBUG=1; spec §6.2, §6.3) ----

    // Signed overflow aborts at the operation's width, and `as` (not `as!`)
    // aborts where the value changes, with upstream's messages (gs_ovf,
    // gs_ovf_neg, gs_asfail_*), assembled in memory 0's message scratch.
    bool debug = false;
    uint32_t ovffn = 0, asifn = 0, asffn = 0;

    void DefineDebug() {
        if (ovffn) return;
        using V = ValType;
        // A message builder over a cursor local p: put(ptr, len).
        auto put = [&](Code &c, uint32_t pl, std::function<void()> ptr, std::function<void()> len) {
            c.LocalGet(pl);
            ptr();
            len();
            c.MemoryCopy();
            c.LocalGet(pl);
            len();
            c.I32Add();
            c.LocalSet(pl);
        };
        auto lit = [&](Code &c, uint32_t pl, const std::string &str) {
            auto a = StaticString(str);
            auto n = int32_t(str.size());
            put(c, pl, [&] { c.I32Const(int32_t(a)); }, [&] { c.I32Const(n); });
        };
        auto arg = [&](Code &c, uint32_t pl, uint32_t ptr, uint32_t len) {
            put(c, pl, [&] { c.LocalGet(ptr); }, [&] { c.LocalGet(len); });
        };
        auto finish = [&](Code &c, uint32_t pl, uint32_t file) {
            c.I32Const(mem0::MsgBuf);
            c.LocalGet(pl);
            c.I32Const(mem0::MsgBuf);
            c.I32Sub();
            c.LocalGet(file);
            c.LocalGet(file + 1);
            c.LocalGet(file + 2);
            c.Call(rt.fail);
        };
        // ovf(a, b, op, oplen, ty, tylen, unary, file, flen, line)
        ovffn = m.AddFunc({ { V::I64, V::I64, V::I32, V::I32, V::I32, V::I32, V::I32, V::I32, V::I32, V::I32 }, {} },
                          { V::I32 });
        {
            auto &c = m.FuncAt(ovffn).code;
            enum { a, b, op, oplen, ty, tylen, unary, file, flen, line, pl };
            c.I32Const(mem0::MsgBuf);
            c.LocalSet(pl);
            lit(c, pl, "integer overflow (debug): ");
            c.LocalGet(unary);
            c.If();
            lit(c, pl, "-(");
            c.LocalGet(a);
            c.LocalGet(pl);
            c.Call(rt.fmt_i64);
            c.LocalSet(pl);
            lit(c, pl, ")");
            c.Else();
            c.LocalGet(a);
            c.LocalGet(pl);
            c.Call(rt.fmt_i64);
            c.LocalSet(pl);
            lit(c, pl, " ");
            arg(c, pl, op, oplen);
            lit(c, pl, " ");
            c.LocalGet(b);
            c.LocalGet(pl);
            c.Call(rt.fmt_i64);
            c.LocalSet(pl);
            c.End();
            lit(c, pl, " at ");
            arg(c, pl, ty, tylen);
            finish(c, pl, file);
        }
        // asi(why, whylen, v, unsigned, ty, tylen, file, flen, line)
        asifn = m.AddFunc({ { V::I32, V::I32, V::I64, V::I32, V::I32, V::I32, V::I32, V::I32, V::I32 }, {} },
                          { V::I32 });
        {
            auto &c = m.FuncAt(asifn).code;
            enum { why, whylen, v, uns, ty, tylen, file, flen, line, pl };
            c.I32Const(mem0::MsgBuf);
            c.LocalSet(pl);
            lit(c, pl, "as conversion ");
            arg(c, pl, why, whylen);
            lit(c, pl, " (debug): ");
            // A block takes no operands from outside: each branch pushes its own.
            c.LocalGet(uns);
            c.If(BlockType::I32);
            c.LocalGet(v);
            c.LocalGet(pl);
            c.Call(rt.fmt_u64);
            c.Else();
            c.LocalGet(v);
            c.LocalGet(pl);
            c.Call(rt.fmt_i64);
            c.End();
            c.LocalSet(pl);
            lit(c, pl, " as ");
            arg(c, pl, ty, tylen);
            finish(c, pl, file);
        }
        // asf(why, whylen, d, f32, ty, tylen, file, flen, line)
        asffn = m.AddFunc({ { V::I32, V::I32, V::F64, V::I32, V::I32, V::I32, V::I32, V::I32, V::I32 }, {} },
                          { V::I32 });
        {
            auto &c = m.FuncAt(asffn).code;
            enum { why, whylen, d, f32, ty, tylen, file, flen, line, pl };
            c.I32Const(mem0::MsgBuf);
            c.LocalSet(pl);
            lit(c, pl, "as conversion ");
            arg(c, pl, why, whylen);
            lit(c, pl, " (debug): ");
            c.LocalGet(pl);
            c.LocalGet(f32);
            c.If(BlockType::I32);
            c.LocalGet(pl);
            c.LocalGet(d);
            c.Op(0xb6);  // f32.demote_f64
            c.Call(ff.fmt_f32);
            c.Else();
            c.LocalGet(pl);
            c.LocalGet(d);
            c.Call(ff.fmt_f64);
            c.End();
            c.I32Add();
            c.LocalSet(pl);
            lit(c, pl, " as ");
            arg(c, pl, ty, tylen);
            finish(c, pl, file);
        }
    }

    std::string TypeText(g::TypeExpr *t) {
        std::string k;
        t->Dump(k);
        return k;
    }
    void StrArg(const std::string &str) {
        C().I32Const(int32_t(StaticString(str)));
        C().I32Const(int32_t(str.size()));
    }

    // Signed a op b at type t's width, its operands in locals a and b (of
    // its repr), the result left at that repr; aborts on overflow.
    void CheckedArith(const g::Node *at, g::TType op, g::TypeExpr *t, uint32_t a, uint32_t b) {
        DefineDebug();
        auto wide = IsWide(t);
        auto r = NewLocal(Repr::I64);
        auto ext = [&](uint32_t l) {
            C().LocalGet(l);
            if (!wide) C().I64ExtendI32S();
        };
        auto bad = NewLocal(Repr::I32);
        ext(a);
        ext(b);
        C().Int(op == g::T_PLUS ? intop::Add : op == g::T_MINUS ? intop::Sub : intop::Mul, true);
        C().LocalSet(r);
        if (!wide) {
            int64_t lo = -(int64_t(1) << (Bits(t) - 1)), hi = (int64_t(1) << (Bits(t) - 1)) - 1;
            C().LocalGet(r);
            C().I64Const(lo);
            C().Int(intop::LtS, true);
            C().LocalGet(r);
            C().I64Const(hi);
            C().Int(intop::GtS, true);
            C().Int(intop::Or, false);
            C().LocalSet(bad);
        } else if (op == g::T_PLUS || op == g::T_MINUS) {
            // ((a ^ r) & (b ^ r)) < 0 for +, ((a ^ b) & (a ^ r)) < 0 for -.
            C().LocalGet(a);
            C().LocalGet(op == g::T_PLUS ? r : b);
            C().Int(intop::Xor, true);
            C().LocalGet(op == g::T_PLUS ? b : a);
            C().LocalGet(r);
            C().Int(intop::Xor, true);
            C().Int(intop::And, true);
            C().I64Const(0);
            C().Int(intop::LtS, true);
            C().LocalSet(bad);
        } else {
            // a != 0 && (a == -1 ? b == MIN : r / a != b)
            C().LocalGet(a);
            C().I64Const(-1);
            C().Int(intop::Eq, true);
            C().If(BlockType::I32);
            C().LocalGet(b);
            C().I64Const(INT64_MIN);
            C().Int(intop::Eq, true);
            C().Else();
            C().LocalGet(a);
            C().I64Eqz();
            C().If(BlockType::I32);
            C().I32Const(0);
            C().Else();
            C().LocalGet(r);
            C().LocalGet(a);
            C().Int(intop::DivS, true);
            C().LocalGet(b);
            C().Int(intop::Ne, true);
            C().End();
            C().End();
            C().LocalSet(bad);
        }
        C().LocalGet(bad);
        C().If();
        ext(a);
        ext(b);
        StrArg(op == g::T_PLUS ? "+" : op == g::T_MINUS ? "-" : "*");
        StrArg(TypeText(t));
        C().I32Const(0);
        Location(at);
        C().Call(ovffn);
        C().End();
        C().LocalGet(r);
        if (!wide) {
            C().I32WrapI64();
            Normalize(t);
        }
    }

    // -x of a signed x (on the Wasm stack, at t's repr): MIN aborts.
    void CheckedNeg(const g::Node *at, g::TypeExpr *t) {
        DefineDebug();
        auto wide = IsWide(t);
        auto x = NewLocal(wide ? Repr::I64 : Repr::I32);
        C().LocalSet(x);
        C().LocalGet(x);
        C().IntConst(wide, wide ? INT64_MIN : -(int64_t(1) << (Bits(t) - 1)));
        C().Int(intop::Eq, wide);
        C().If();
        C().LocalGet(x);
        if (!wide) C().I64ExtendI32S();
        C().I64Const(0);
        StrArg("-");
        StrArg(TypeText(t));
        C().I32Const(1);
        Location(at);
        C().Call(ovffn);
        C().End();
        C().IntConst(wide, 0);
        C().LocalGet(x);
        C().Int(intop::Sub, wide);
        if (!wide) Normalize(t);
    }

    // `as` into an integer type (the value at type from on the Wasm stack):
    // aborts where the value changes, then converts.
    void CheckedAs(const g::Node *at, g::TypeExpr *from, g::TypeExpr *to) {
        DefineDebug();
        auto bits = Bits(to);
        bool tos = IsSigned(to);
        if (IsFlt(from)) {
            bool f32 = from->fltstorage == g::FS_F32;
            auto v = NewLocal(Repr::F64);
            if (f32) C().Op(0xbb);  // f64.promote_f32
            C().LocalSet(v);
            // The range is [lo, hi) in doubles, exact powers of two.
            double lo = tos ? -std::ldexp(1.0, bits - 1) : 0.0;
            double hi = std::ldexp(1.0, tos ? bits - 1 : bits);
            auto fail = [&](const char *why) {
                StrArg(why);
                C().LocalGet(v);
                C().I32Const(f32);
                StrArg(TypeText(to));
                Location(at);
                C().Call(asffn);
            };
            // Out of range, NaN included.
            C().LocalGet(v);
            C().F64Const(lo);
            C().Op(0x66);  // f64.ge
            C().LocalGet(v);
            C().F64Const(hi);
            C().Op(0x63);  // f64.lt
            C().I32And();
            C().I32Eqz();
            C().If();
            fail("out of range");
            C().End();
            C().LocalGet(v);
            C().LocalGet(v);
            C().Op(0x9d);  // f64.trunc
            C().Op(0x62);  // f64.ne
            C().If();
            fail("changes the value");
            C().End();
            C().LocalGet(v);
            if (f32) C().Op(0xb6);
            Convert(at, from, to);
            return;
        }
        bool froms = IsSigned(from);
        auto fr = ReprOf(at, from);
        auto x = NewLocal(fr);
        C().LocalSet(x);
        auto v = NewLocal(Repr::I64);
        C().LocalGet(x);
        if (fr == Repr::I32) {
            if (froms) C().I64ExtendI32S();
            else C().I64ExtendI32U();
        }
        C().LocalSet(v);
        // In range: as a signed i64 for a signed source, an unsigned one
        // for an unsigned source.
        auto cmp = [&](bool ge, int64_t k) {
            C().LocalGet(v);
            C().I64Const(k);
            C().Int(froms ? (ge ? intop::GeS : intop::LeS) : (ge ? intop::GeU : intop::LeU), true);
        };
        int64_t hi = bits == 64 ? (tos ? INT64_MAX : -1) : int64_t((uint64_t(1) << (tos ? bits - 1 : bits)) - 1);
        int64_t lo = tos ? (bits == 64 ? INT64_MIN : -(int64_t(1) << (bits - 1))) : 0;
        if (froms) {
            cmp(true, lo);
            if (!tos && bits == 64) C().I32Const(1);  // any i64 >= 0 fits u64
            else cmp(false, hi);
        } else {
            C().I32Const(1);
            if (tos && bits == 64) cmp(false, INT64_MAX);
            else if (!tos && bits == 64) C().I32Const(1);
            else cmp(false, hi);
        }
        C().I32And();
        C().I32Eqz();
        C().If();
        StrArg("out of range");
        C().LocalGet(v);
        C().I32Const(!froms);
        StrArg(TypeText(to));
        Location(at);
        C().Call(asifn);
        C().End();
        C().LocalGet(x);
        Convert(at, from, to);
    }

    // ---- Elementwise math (spec §6.1) ----

    // An operand of an elementwise operation: an aggregate's place, or a
    // scalar in a local (`v * 2.0`, `2.0 / v`), of type st.
    struct EwOperand {
        bool scalar = false;
        Loc l;
        uint32_t local = 0;
        g::TypeExpr *st = nullptr;
    };

    // Evaluates an operand once, before anything is written: an aggregate
    // into a temporary of its own, a scalar into a local.
    EwOperand EwEval(g::Node *n, g::TypeExpr *agg) {
        EwOperand o;
        auto t = OperandType(n->exprtype);
        if (IsAggregate(t)) {
            o.l = FrameSlot(agg);
            Construct(n, o.l);
            return o;
        }
        o.scalar = true;
        o.st = LeafType(agg);
        o.local = NewLocal(ReprOf(n, o.st));
        Expr(n, o.st);
        C().LocalSet(o.local);
        return o;
    }
    // The one numeric leaf type of an aggregate (a scalar operand's).
    g::TypeExpr *LeafType(g::TypeExpr *t) {
        if (Layouts::IsFixedArray(t)) return LeafType(t->arr->sub);
        if (t->kind == g::TY_STRUCT) {
            auto si = Layouts::SI(t);
            for (size_t i = 0; i < si->ftypes.size(); i++)
                if (!si->st->fields[i].ispad) return LeafType(si->ftypes[i]);
        }
        return t;
    }

    // dst = a op b, leaf by leaf (a or b may be a scalar), or dst = -a.
    void Elementwise(g::Node *at, g::TType op, const Loc &dst, g::TypeExpr *t, const EwOperand &a,
                     const EwOperand *b) {
        auto part = [&](const EwOperand &o, int64_t off, g::TypeExpr *ft) {
            if (o.scalar) return o;
            EwOperand r = o;
            r.l = o.l.At(off, ft);
            return r;
        };
        if (Layouts::IsFixedArray(t)) {
            auto et = t->arr->sub;
            auto esz = lay.FixedSize(et);
            if (t->arr->size > 16) {
                // A loop over the elements, each operand's handle stepped.
                auto i = NewLocal(Repr::I64);
                auto at_ = [&](const Loc &l) {
                    auto h = NewLocal(Repr::I64);
                    HandleOf(l);
                    C().LocalGet(i);
                    C().I64Const(esz);
                    C().Int(intop::Mul, true);
                    C().Int(intop::Add, true);
                    C().LocalSet(h);
                    return Loc { false, h, 0, et, l.Direct() };
                };
                C().I64Const(0);
                C().LocalSet(i);
                Open(LK_PLAIN, Repr::None);
                Open(LK_PLAIN, Repr::None, nullptr, nullptr, true);
                C().LocalGet(i);
                C().I64Const(t->arr->size);
                C().Int(intop::GeU, true);
                C().BrIf(1);
                auto ea = a;
                if (!a.scalar) ea.l = at_(a.l);
                EwOperand eb;
                if (b) {
                    eb = *b;
                    if (!b->scalar) eb.l = at_(b->l);
                }
                Elementwise(at, op, at_(dst), et, ea, b ? &eb : nullptr);
                C().LocalGet(i);
                C().I64Const(1);
                C().Int(intop::Add, true);
                C().LocalSet(i);
                C().Br(0);
                Close();
                Close();
                return;
            }
            for (int64_t i = 0; i < t->arr->size; i++) {
                auto pb = b ? part(*b, i * esz, et) : EwOperand {};
                Elementwise(at, op, dst.At(i * esz, et), et, part(a, i * esz, et), b ? &pb : nullptr);
            }
            return;
        }
        if (t->kind == g::TY_STRUCT) {
            auto si = Layouts::SI(t);
            auto &lo = lay.StructLayout(si);
            for (size_t i = 0; i < si->ftypes.size(); i++) {
                if (si->st->fields[i].ispad) continue;
                auto ft = si->ftypes[i];
                auto pb = b ? part(*b, lo.offs[i], ft) : EwOperand {};
                Elementwise(at, op, dst.At(lo.offs[i], ft), ft, part(a, lo.offs[i], ft), b ? &pb : nullptr);
            }
            return;
        }
        if (!IsInt(t) && !IsFlt(t)) Unsupported(at, "elementwise math over these members");
        auto load = [&](const EwOperand &o) {
            if (o.scalar) C().LocalGet(o.local);
            else Load(at, Loc { o.l.frame, o.l.h, o.l.off, t, o.l.mem0, o.l.abs });
        };
        Store(at, dst, [&] {
            load(a);
            if (!b) {
                // Unary -.
                if (IsFlt(t)) {
                    C().Op(t->fltstorage == g::FS_F32 ? 0x8c : 0x9a);
                } else if (debug && IsSigned(t)) {
                    CheckedNeg(at, t);
                } else {
                    auto wide = IsWide(t);
                    auto x = NewLocal(wide ? Repr::I64 : Repr::I32);
                    C().LocalSet(x);
                    C().IntConst(wide, 0);
                    C().LocalGet(x);
                    C().Int(intop::Sub, wide);
                    if (!wide) Normalize(t);
                }
                return;
            }
            ArithV(const_cast<g::Node *>(at), op, t, [&] { load(*b); });
        });
    }

    // An elementwise binary operation's value at dst; false where n is no
    // such operation.
    bool ElementwiseInto(g::Node *n, const Loc &dst) {
        if (n->nkind == g::NK_Unary) {
            auto u = static_cast<g::Unary *>(n);
            if (u->op != g::T_MINUS) return false;
            auto a = EwEval(u->child, dst.t);
            Elementwise(n, g::T_MINUS, dst, dst.t, a, nullptr);
            return true;
        }
        if (n->nkind != g::NK_Binary) return false;
        auto b = static_cast<g::Binary *>(n);
        switch (b->op) {
            case g::T_PLUS: case g::T_MINUS: case g::T_MUL: case g::T_DIV: case g::T_MOD: break;
            default: return false;
        }
        auto l = EwEval(b->left, dst.t);
        auto r = EwEval(b->right, dst.t);
        Elementwise(n, b->op, dst, dst.t, l, &r);
        return true;
    }

    // ---- Limited arrays T[..k] of static capacity (spec §4.2) ----

    // Laid out { len, e[k] } (layout.h), they are fixed aggregates; the
    // builtins that grow them check the capacity (upstream GS_E_CAPACITY).
    // A limited array of a capacity chosen at run time, T[..] (spec §3.3):
    // variable-size, laid out [cap u32][len u32][cap slots] (Appendix C.2)
    // on a data stack claimed where it is declared; its place is that image.
    static bool IsDynLim(g::TypeExpr *t) {
        return t && t->kind == g::TY_ARRAY && t->arr->akind == g::A_LIMITED && t->arr->size < 0 &&
               Layouts::Known(t->arr->sub);
    }
    static bool IsAnyLim(g::TypeExpr *t) { return Layouts::IsLimited(t) || IsDynLim(t); }
    g::TypeExpr *LimLenT(g::TypeExpr *t) {
        return ast.inttypes[IsDynLim(t) ? g::IS_U32 : Layouts::LimLenStore(t)];
    }
    static int64_t LimLenOff(g::TypeExpr *t) { return IsDynLim(t) ? 4 : 0; }
    static int64_t LimElemsOff(g::TypeExpr *t) { return IsDynLim(t) ? 8 : Layouts::LimLenSize(t); }

    // The length of the limited array at l, as an i64.
    void LimLen(const g::Node *at, const Loc &l) {
        auto lt = LimLenT(l.t);
        Load(at, l.At(LimLenOff(l.t), lt));
        if (!IsWide(lt)) C().I64ExtendI32U();
    }
    // Its capacity, as an i64.
    void LimCap(const g::Node *at, const Loc &l) {
        if (!IsDynLim(l.t)) {
            C().I64Const(l.t->arr->size);
            return;
        }
        Load(at, l.At(0, ast.inttypes[g::IS_U32]));
        C().I64ExtendI32U();
    }
    // Stores the i64 `value` pushes as its length.
    template <typename F> void LimSetLen(const g::Node *at, const Loc &l, F value) {
        auto lt = LimLenT(l.t);
        Store(at, l.At(LimLenOff(l.t), lt), [&] {
            value();
            if (!IsWide(lt)) C().I32WrapI64();
        });
    }
    // The handle of element i (an i64 local) of the limited array at l.
    void LimElem(const Loc &l, uint32_t i) {
        HandleOf(l);
        C().I64Const(LimElemsOff(l.t));
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
        C().I64Const(LimElemsOff(l.t));
        C().Int(intop::Add, true);
        C().LocalSet(s.data);
        s.len = [this, at, l] { LimLen(at, l); };
        return s;
    }
    // Aborts unless the i64 local `need` is within l's capacity.
    void CapCheck(const g::Node *at, const Loc &l, uint32_t need) {
        C().LocalGet(need);
        LimCap(at, l);
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
        if (!IsAnyLim(at)) return false;
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
        LimFromSeq(n, SeqOf(n), dst);
    }
    void LimFromSeq(const g::Node *n, const Seq &s, const Loc &dst) {
        auto esz = lay.FixedSize(dst.t->arr->sub);
        auto elems = dst.At(LimElemsOff(dst.t), nullptr);
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
        if (!Layouts::Known(r.et)) Unsupported(call, "shrinking a sequential array");
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
        if (!Layouts::Known(r.et)) Unsupported(call, "shrinking a sequential array");
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
        if (!Layouts::Known(r.et)) Unsupported(call, "shrinking a sequential array");
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
        // Into the line buffer's upper half: fail assembles the line from
        // its start, the message's copy moving down as it does.
        auto sq = SeqOf(m0);
        auto n = NewLocal(Repr::I64);
        const int64_t half = mem0::LineBufSize / 2, at = mem0::LineBuf + half;
        sq.len();
        C().LocalSet(n);
        C().LocalGet(n);
        C().I64Const(half);
        C().LocalGet(n);
        C().I64Const(half);
        C().Int(intop::LtU, true);
        C().Select();
        C().LocalSet(n);
        C().I64Const(at);
        C().LocalGet(sq.data);
        C().LocalGet(n);
        C().I32WrapI64();
        C().Call(hs.copy);
        C().I32Const(int32_t(at));
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
            if (!Layouts::FixedAdt(st) && !IsVar(st)) Unsupported(me, "a match of this ADT");
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
                auto vt = a.binder->type;
                if (a.pat.byref || IsPlainRef(vt)) {
                    // By reference (§8.1): the payload where it lies.
                    auto sub = IsPlainRef(vt) ? vt->ref->sub : vt;
                    auto h = NewLocal(Repr::I64);
                    HandleOf(PayloadLoc(scr, sub));
                    C().LocalSet(h);
                    if (IsPlainRef(vt)) locals[a.binder] = h;
                    else memvars[a.binder] = Loc { false, h, 0, sub, false };
                } else if (IsVar(vt) || IsVar(st)) {
                    // A variable-size payload is never copied implicitly:
                    // read where it lies.
                    memvars[a.binder] = PayloadLoc(scr, vt);
                } else {
                    // By value: a copy of the payload (§8.1).
                    if (!IsAggregate(vt)) Unsupported(me, "this payload binding");
                    auto slot = FrameSlot(vt);
                    Copy(slot, PayloadLoc(scr, vt));
                    memvars[a.binder] = slot;
                }
            }
            // In a scope of its own: what an arm claims is released on its
            // own path.
            PushScope();
            body(a.body);
            PopScope();
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

    // ---- Structural equality (spec §4.5) ----

    // == and != of values that are not scalars: structs, fixed arrays and
    // ADTs memberwise, sequences by length then elements (two kinds of
    // array compare as slices). A direct operand that is a reference
    // compares its pointee; references and slices that are members compare
    // by identity. A float compares by IEEE value wherever it sits.
    bool StructEquality(g::Binary *b) {
        auto lt = OperandType(b->left->exprtype), rt_ = OperandType(b->right->exprtype);
        if (!lt || !rt_) return false;
        auto seq = [](g::TypeExpr *t) {
            return t->kind == g::TY_SLICE || (t->kind == g::TY_ARRAY && !Layouts::IsFixedArray(t));
        };
        if (seq(lt) || seq(rt_) || (Layouts::IsFixedArray(lt) && !Layouts::IsFixedArray(rt_))) {
            auto sa = SeqOf(b->left), sb = SeqOf(b->right);
            if (!Imageable(sa.et)) Unsupported(b, "comparing sequences of these elements");
            SeqEq(b, sa, sb);
        } else if (IsAggregate(lt) || IsVar(lt)) {
            auto la = ObjectLoc(b->left);
            la.t = lt;
            auto lb = ObjectLoc(b->right);
            lb.t = lt;
            EqAt(b, la, lb, lt);
        } else {
            return false;
        }
        if (b->op == g::T_NEQ) C().I32Eqz();
        return true;
    }

    // Whether two sequences hold equal elements: their lengths, then each
    // element pair, stopping at the first unequal one.
    void SeqEq(const g::Node *at, const Seq &a, const Seq &b) {
        auto r = NewLocal(Repr::I32), n = NewLocal(Repr::I64), i = NewLocal(Repr::I64);
        C().I32Const(0);
        C().LocalSet(r);
        a.len();
        C().LocalTee(n);
        b.len();
        C().Int(intop::Eq, true);
        C().If();
        C().I32Const(1);
        C().LocalSet(r);
        C().I64Const(0);
        C().LocalSet(i);
        bool seq = !Layouts::Known(a.et);
        auto ca = NewLocal(Repr::I64), cb = NewLocal(Repr::I64);
        auto ha = NewLocal(Repr::I64), hb = NewLocal(Repr::I64);
        if (seq) {
            C().LocalGet(a.data);
            C().LocalSet(ca);
            C().LocalGet(b.data);
            C().LocalSet(cb);
        }
        C().Block();
        C().Loop();
        C().LocalGet(i);
        C().LocalGet(n);
        C().Int(intop::GeU, true);
        C().BrIf(1);
        if (seq) {
            C().LocalGet(ca);
            C().LocalSet(ha);
            C().LocalGet(cb);
            C().LocalSet(hb);
            Skip(a.et, ca);
            Skip(b.et, cb);
            EqAt(at, Loc { false, ha, 0, a.et, false }, Loc { false, hb, 0, b.et, false }, a.et);
        } else {
            EqAt(at, ElemLoc(a, i), ElemLoc(b, i), a.et);
        }
        C().I32Eqz();
        C().If();
        C().I32Const(0);
        C().LocalSet(r);
        C().Br(2);
        C().End();
        C().LocalGet(i);
        C().I64Const(1);
        C().Int(intop::Add, true);
        C().LocalSet(i);
        C().Br(0);
        C().End();
        C().End();
        C().End();
        C().LocalGet(r);
    }

    // Whether the values of type t at a and b are equal, as members are
    // compared: an i32.
    void EqAt(const g::Node *at, Loc a, Loc b, g::TypeExpr *t) {
        a.t = b.t = t;
        switch (t->kind) {
            case g::TY_INT: case g::TY_BOOL: {
                Load(at, a);
                Load(at, b);
                C().Int(intop::Eq, ReprOf(at, t) == Repr::I64);
                return;
            }
            case g::TY_FLT: {
                Load(at, a);
                Load(at, b);
                C().Op(t->fltstorage == g::FS_F32 ? 0x5b : 0x61);  // f32.eq, f64.eq
                return;
            }
            case g::TY_REF: {
                // By identity: the addresses.
                if (!IsPlainRef(t)) break;
                Load(at, a);
                Load(at, b);
                C().Int(intop::Eq, true);
                return;
            }
            case g::TY_SLICE: {
                // By identity: the address and the length.
                Load(at, a.At(0, I64T()));
                Load(at, b.At(0, I64T()));
                C().Int(intop::Eq, true);
                Load(at, a.At(8, I64T()));
                Load(at, b.At(8, I64T()));
                C().Int(intop::Eq, true);
                C().I32And();
                return;
            }
            case g::TY_ARRAY: {
                if (IsAnyLim(t)) return SeqEq(at, LimSeq(at, a), LimSeq(at, b));
                if (IsVarArray(t)) return SeqEq(at, ImageSeq(at, a), ImageSeq(at, b));
                if (!Layouts::IsFixedArray(t)) break;
                auto fixed = [&](const Loc &l) {
                    Seq sq;
                    sq.et = t->arr->sub;
                    sq.data = NewLocal(Repr::I64);
                    sq.mem0 = l.Direct();
                    HandleOf(l);
                    C().LocalSet(sq.data);
                    auto k = t->arr->size;
                    sq.len = [this, k] { C().I64Const(k); };
                    return sq;
                };
                return SeqEq(at, fixed(a), fixed(b));
            }
            case g::TY_STRUCT: {
                auto si = Layouts::SI(t);
                if (!si || !(IsAggregate(t) || IsVar(t))) break;
                C().I32Const(1);
                for (size_t i = 0; i < si->ftypes.size(); i++) {
                    if (si->st->fields[i].ispad) continue;
                    auto ft = si->ftypes[i];
                    Loc fa, fb;
                    if (IsVar(t)) {
                        fa = FieldLoc(a, t, int(i));
                        fb = FieldLoc(b, t, int(i));
                    } else {
                        auto &lo = lay.StructLayout(si);
                        fa = a.At(lo.offs[i], ft);
                        fb = b.At(lo.offs[i], ft);
                    }
                    EqAt(at, fa, fb, ft);
                    C().I32And();
                }
                return;
            }
            case g::TY_ENUM: {
                // The tags, then the payload of the variant they name.
                if (!Layouts::FixedAdt(t) && !IsVar(t)) break;
                auto ei = Layouts::EI(t);
                auto tag = NewLocal(Repr::I32), r = NewLocal(Repr::I32);
                LoadTag(a);
                C().LocalTee(tag);
                LoadTag(b);
                C().Int(intop::Eq, false);
                C().LocalSet(r);
                for (size_t vi = 0; vi < ei->en->variants.size(); vi++) {
                    if (ei->en->variants[vi].fields.empty()) continue;
                    C().LocalGet(r);
                    C().LocalGet(tag);
                    C().I32Const(int32_t(vi));
                    C().Int(intop::Eq, false);
                    C().I32And();
                    C().If();
                    VariantEq(at, ei, int(vi), PayloadLoc(a, nullptr), PayloadLoc(b, nullptr));
                    C().LocalSet(r);
                    C().End();
                }
                C().LocalGet(r);
                return;
            }
            case g::TY_VARIANT:
                return VariantEq(at, Layouts::EIVar(t), Layouts::VariantIndex(t), a, b);
            default: break;
        }
        Unsupported(at, g::cat("comparing values of type ", TypeKindName(t)));
    }

    void VariantEq(const g::Node *at, g::EnumInst *ei, int vi, const Loc &a, const Loc &b) {
        auto &v = ei->en->variants[vi];
        bool fixed = PayloadFixed(ei, vi);
        C().I32Const(1);
        for (size_t i = 0; i < v.fields.size(); i++) {
            if (v.fields[i].ispad) continue;
            auto ft = ei->vftypes[vi][i];
            if (fixed) {
                auto &lo = lay.VariantLayout(ei, vi);
                EqAt(at, a.At(lo.offs[i], ft), b.At(lo.offs[i], ft), ft);
            } else {
                EqAt(at, PayloadField(a, ei, vi, int(i)), PayloadField(b, ei, vi, int(i)), ft);
            }
            C().I32And();
        }
    }
    static bool PayloadFixed(g::EnumInst *ei, int vi) {
        for (auto ft : ei->vftypes[vi])
            if (ft && !Layouts::Known(ft)) return false;
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
        stkconsts.clear();
        fls.clear();
        labels.clear();
        scopes.clear();
        frame = 0;
        stknext = stkmax = 0;
        retdst = retrz = atparam = -1;
        nparams = 2;
        fp = NewLocal(Repr::I32);
        rstk = 0;
        hooks = nullptr;
        RenderBody(nullptr, Loc { false, 1, 0, t, false }, t);
        // A value whose references lead back into itself never finishes
        // rendering: like upstream, it runs out of stack, which is reported
        // as the native call stack overflow (§3.7), so these count their
        // activations as recursive functions do.
        FinishFrame(true);
    }

    // A struct's, an ADT's or a variant's literal, rendered from the value
    // at obj.
    void RenderBody(const g::Node *at, const Loc &where, g::TypeExpr *t) {
        auto obj = where;
        if (IsAdt(t)) {
            // A fixed-mode ADT renders from a copy taken as its tag is read:
            // an overload rendering a part may overwrite the value with
            // another variant (§3.7).
            if (Layouts::FixedAdt(t)) {
                auto copy = FrameSlot(t);
                Copy(copy, Loc { obj.frame, obj.h, obj.off, t, obj.mem0, obj.abs });
                obj = copy;
            }
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
                RenderVariant(at, ei, int(vi), PayloadLoc(obj, nullptr));
                C().End();
            }
            return;
        }
        if (IsVariantT(t)) return RenderVariant(at, Layouts::EIVar(t), Layouts::VariantIndex(t), obj);
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
            auto floc = IsVar(t) ? FieldLoc(obj, t, int(i)) : obj.At(lo.offs[i], si->ftypes[i]);
            RenderAt(at, floc, si->ftypes[i], true);
        }
        RenderText(" }");
    }

    // ---- User format overloads (spec §3.7) ----

    // The overloads the print, str or format being rendered takes for the
    // types in its arguments (Call::fmtspecs), the call, and the handle of
    // the builder's start, for the header an overload is given.
    const std::vector<std::pair<g::TypeExpr *, g::FnSpec *>> *hooks = nullptr;
    g::Call *hookcall = nullptr;
    uint32_t rbase = 0;
    // str's result header, which overloads are given as it is: upstream
    // counts in its length only what they append, its own text bumping the
    // top alone (print's builder and format's array follow every piece).
    Loc strhdr;
    bool usestrhdr = false;

    g::FnSpec *HookFor(g::TypeExpr *t) {
        if (!hooks || !t) return nullptr;
        std::string k;
        t->Dump(k);
        for (auto &[ht, sp] : *hooks) {
            std::string hk;
            ht->Dump(hk);
            if (hk == k) return sp;
        }
        return nullptr;
    }
    // Whether rendering a t reaches a type an overload renders: then it is
    // rendered in place, as upstream does (a render function runs none).
    bool ReachesHook(g::TypeExpr *t) {
        if (!hooks) return false;
        std::set<std::string> seen;
        std::function<bool(g::TypeExpr *)> walk = [&](g::TypeExpr *x) -> bool {
            if (!x) return false;
            std::string k;
            x->Dump(k);
            if (!seen.insert(k).second) return false;
            if (HookFor(x)) return true;
            switch (x->kind) {
                case g::TY_REF: return walk(x->ref->sub);
                case g::TY_SLICE: return walk(x->sub);
                case g::TY_ARRAY: return walk(x->arr->sub);
                case g::TY_STRUCT: {
                    auto si = Layouts::SI(x);
                    if (!si) return false;
                    for (auto ft : si->ftypes) if (walk(ft)) return true;
                    return false;
                }
                case g::TY_ENUM: case g::TY_VARIANT: {
                    auto ei = x->kind == g::TY_ENUM ? Layouts::EI(x) : Layouts::EIVar(x);
                    if (!ei) return false;
                    for (auto &fts : ei->vftypes) for (auto ft : fts) if (walk(ft)) return true;
                    return false;
                }
                default: return false;
            }
        };
        return walk(t);
    }

    // A user `format(out, v)` overload applied to the value at l (upstream
    // EmitUserFormat): out is the builder, with a header { start, length }
    // in the frame; v by value is a copy, by reference the place itself.
    // The callee's stacks start above everything live here, the builder's
    // included, as any call's do.
    void UserFormat(const g::Node *at, const Loc &l, g::TypeExpr *t, g::FnSpec *sp) {
        if (!at) at = hookcall;
        if (!freevars[sp].empty()) Unsupported(at, "a format overload with free variables");
        auto index = FuncOf(sp, at);
        auto hdr = strhdr;
        if (!usestrhdr) {
            hdr = FrameBytes(16);
            Store(at, hdr.At(0, I64T()), [&] { C().LocalGet(rbase); });
            Store(at, hdr.At(8, I64T()), [&] {
                TopAddr(rstk);
                C().I32Load(mem0::Tops);
                C().LocalGet(rbase);
                C().I32WrapI64();
                C().I32Sub();
                C().I64ExtendI32U();
            });
        }
        stackuse[spec].calls.push_back({ sp, stknext });
        C().LocalGet(SP);
        C().I32Const(stknext);
        C().I32Add();
        Handle(hdr);
        C().LocalGet(rstk);
        auto pt = sp->params[1]->type;
        Loc v = l;
        v.t = t;
        if (IsAggregate(pt)) {
            auto copy = FrameSlot(pt);
            Copy(copy, v);
            Handle(copy);
        } else if (IsPlainRef(pt)) {
            Handle(v);
        } else if (IsRzRef(pt)) {
            Unsupported(at, "a format overload taking a resizable value");
        } else {
            Load(at, v);
            Convert(at, t, pt);
        }
        if (Located(sp)) CallerLoc(hookcall->line);
        C().Call(index);
    }


    // A variant's positional literal (upstream RenderVariant): a payload-less
    // one as `Enum.Name`, else `Name { a, b }`.
    void RenderVariant(const g::Node *at, g::EnumInst *ei, int vi, const Loc &payload) {
        auto &v = ei->en->variants[vi];
        if (v.fields.empty()) {
            RenderText(g::cat(ei->en->name, ".", v.name));
            return;
        }
        bool fixed = PayloadFixed(ei, vi);
        RenderText(g::cat(v.name, " { "));
        bool first = true;
        for (size_t i = 0; i < v.fields.size(); i++) {
            if (v.fields[i].ispad) continue;
            if (!first) RenderText(", ");
            first = false;
            auto ft = ei->vftypes[vi][i];
            auto fl = fixed ? payload.At(lay.VariantLayout(ei, vi).offs[i], ft) : PayloadField(payload, ei, vi, int(i));
            RenderAt(at, fl, ft, true);
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
        bool seq = !Layouts::Known(s.et);
        auto cur = NewLocal(Repr::I64), here = NewLocal(Repr::I64);
        s.len();
        C().LocalSet(n);
        C().I64Const(0);
        C().LocalSet(i);
        if (seq) {
            C().LocalGet(s.data);
            C().LocalSet(cur);
        }
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
        if (seq) {
            // Sequential elements: a cursor walks them.
            C().LocalGet(cur);
            C().LocalSet(here);
            Skip(s.et, cur);
            RenderAt(at, Loc { false, here, 0, s.et, false }, s.et, true);
        } else {
            RenderAt(at, ElemLoc(s, i), s.et, true);
        }
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
        if (auto sp = HookFor(t)) return UserFormat(at, l, t, sp);
        switch (t->kind) {
            case g::TY_INT: {
                Load(at, l);
                RenderInt(t->intstorage == g::IS_VARINT ? I64T() : t);
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
                if (!Imageable(s.et)) break;
                s.data = NewLocal(Repr::I64);
                if (IsAnyLim(t)) {
                    l.t = t;
                    s = LimSeq(at, l);
                } else if (Layouts::IsFixedArray(t)) {
                    HandleOf(l);
                    C().LocalSet(s.data);
                    s.mem0 = l.Direct();
                    auto k = t->arr->size;
                    s.len = [this, k] { C().I64Const(k); };
                } else if (IsVarArray(t)) {
                    l.t = t;
                    s = ImageSeq(at, l);
                } else if (t->kind == g::TY_SLICE || IsRz(t) || IsFlat(t)) {
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
                if (IsRoot(t)) {
                    // A root owner: its fields' literal, each from where
                    // the descriptor names it.
                    Loc desc = l;
                    desc.t = t;
                    auto si = Layouts::SI(t);
                    std::string name;
                    t->Dump(name);
                    RenderText(name + " { ");
                    for (size_t i = 0; i < si->ftypes.size(); i++) {
                        if (i) RenderText(", ");
                        auto ft = si->ftypes[i];
                        if (IsRzField(ft)) {
                            auto rz = RootRzField(at, desc, int(i));
                            if (!IsRz(ft)) Unsupported(at, "printing this root field");
                            Seq sq;
                            sq.et = ft->arr->sub;
                            sq.data = NewLocal(Repr::I64);
                            Load(at, rz.hdr.At(0, I64T()));
                            C().LocalSet(sq.data);
                            auto hdr = rz.hdr;
                            sq.len = [this, at, hdr] { Load(at, hdr.At(8, I64T())); };
                            RenderSeq(at, sq, true);
                        } else {
                            RenderAt(at, RootField(at, desc, int(i)), ft, true);
                        }
                    }
                    RenderText(" }");
                    return;
                }
                if (!IsAggregate(t) && !IsFrameObj(t) && !IsVar(t)) break;
                if (ReachesHook(t)) {
                    Loc obj = l;
                    obj.t = t;
                    return RenderBody(at, obj, t);
                }
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
        if (IsVar(st) && st->kind != g::TY_ARRAY) {
            RenderAt(a, VarPlace(a, st), st, false);
            return;
        }
        if (IsRz(st) || IsView(st) || Layouts::IsFixedArray(st) || IsDynLim(st) || IsVarArray(st)) {
            auto s = SeqOf(a);
            if (!Imageable(s.et)) Unsupported(a, "print of these elements");
            RenderSeq(a, s, false);
            return;
        }
        if (IsRoot(st)) {
            auto l = ObjectLoc(a);
            l.t = st;
            RenderAt(a, l, st, false);
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
        auto start = NewLocal(Repr::I64), len0 = NewLocal(Repr::I64), top0 = NewLocal(Repr::I32);
        Load(call, r.rz.hdr.At(0, I64T()));
        C().LocalSet(start);
        Load(call, len);
        C().LocalSet(len0);
        TopAddr(r.rz.stk);
        C().I32Load(mem0::Tops);
        C().LocalSet(top0);
        // The length grows by each argument's text, as upstream appends
        // piece by piece: an argument reading out.len sees what is there.
        RenderArgs(call, r.rz.stk, 1, start, [&] {
            Store(call, len, [&] {
                C().LocalGet(len0);
                TopAddr(r.rz.stk);
                C().I32Load(mem0::Tops);
                C().LocalGet(top0);
                C().I32Sub();
                C().I64ExtendI32U();
                C().Int(intop::Add, true);
            });
        });
    }

    // str(...) (§3.7): the text print would write, without its newline, as
    // a u8[>..] built at the top of stack stk; its count on the Wasm stack.
    void Str(g::Call *call, uint32_t stk) {
        auto before = NewLocal(Repr::I32);
        TopAddr(stk);
        C().I32Load(mem0::Tops);
        C().LocalSet(before);
        auto savedhdr = strhdr;
        auto savedusehdr = usestrhdr;
        usestrhdr = !call->fmtspecs.empty();
        if (usestrhdr) {
            strhdr = FrameBytes(16);
            Store(call, strhdr.At(0, I64T()), [&] {
                C().LocalGet(stk);
                C().I32Const(1);
                C().I32Add();
                C().I64ExtendI32U();
                C().I64Const(32);
                C().Int(intop::Shl, true);
                C().LocalGet(before);
                C().I64ExtendI32U();
                C().Int(intop::Or, true);
            });
            Store(call, strhdr.At(8, I64T()), [&] { C().I64Const(0); });
        }
        RenderArgs(call, stk);
        strhdr = savedhdr;
        usestrhdr = savedusehdr;
        TopAddr(stk);
        C().I32Load(mem0::Tops);
        C().LocalGet(before);
        C().I32Sub();
        C().I64ExtendI32U();
    }

    // print's and str's arguments rendered at the top of stack stk: text as
    // it is, values structurally.
    void RenderArgs(g::Call *call, uint32_t stk, size_t first = 0, int64_t base = -1,
                    const std::function<void()> &after = nullptr) {
        auto saved = rstk;
        auto savedhooks = hooks;
        auto savedcall = hookcall;
        auto savedbase = rbase;
        rstk = stk;
        auto savedusehdr = usestrhdr;
        if (call->builtin != g::B_STR) usestrhdr = false;
        // A user `format` overload renders its type instead (§3.7).
        hooks = call->fmtspecs.empty() ? nullptr : &call->fmtspecs;
        hookcall = call;
        if (hooks) {
            // The builder's start: what is there already, or its top now.
            rbase = NewLocal(Repr::I64);
            if (base >= 0) {
                C().LocalGet(uint32_t(base));
            } else {
                C().LocalGet(stk);
                C().I32Const(1);
                C().I32Add();
                C().I64ExtendI32U();
                C().I64Const(32);
                C().Int(intop::Shl, true);
                TopAddr(stk);
                C().I32Load(mem0::Tops);
                C().I64ExtendI32U();
                C().Int(intop::Or, true);
            }
            C().LocalSet(rbase);
        }
        auto args = call->ArgNodes();
        for (size_t ai = first; ai < args.size(); ai++) {
            auto a = args[ai];
            auto t = a->exprtype;
            if (hooks && a->nkind != g::NK_StrLit && !IsPlainRef(t) && HookFor(t) &&
                (IsInt(t) || IsFlt(t) || (t && t->kind == g::TY_BOOL))) {
                // A scalar an overload renders: from the storage it lies
                // in, which an overload taking it by reference is given
                // (§3.7), or a place of its own for one that is no storage.
                Loc slot;
                auto varint = [](g::TypeExpr *x) { return x && x->kind == g::TY_INT && x->intstorage == g::IS_VARINT; };
                bool placed = MemLoc(a, slot);
                if (placed && IsPlainRef(slot.t) && !IsPlainRef(t)) slot = Deref(a, slot);
                // A varint lies in no i64 storage (§3.6): the overload is
                // given a temporary holding what it decodes to.
                if (placed && !varint(slot.t)) {
                } else {
                    slot = FrameSlot(t);
                    Store(a, slot, [&] { Expr(a, t); });
                }
                RenderAt(a, slot, t, false);
                if (after) after();
                continue;
            }
            if (hooks && a->nkind != g::NK_StrLit && !IsPlainRef(t) && HookFor(t)) {
                // A value an overload renders, taken by its place (§3.7):
                // a slice's slot, which the overload may re-point.
                RenderAt(a, ObjectLoc(a), t, false);
                if (after) after();
                continue;
            }
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
            if (after) after();
        }
        rstk = saved;
        hooks = savedhooks;
        hookcall = savedcall;
        rbase = savedbase;
        usestrhdr = savedusehdr;
    }
};

}  // namespace goosewasm
