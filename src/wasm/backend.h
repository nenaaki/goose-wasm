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

#include <map>

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
enum class Repr { None, I32, I64 };

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
    Loc At(int64_t more, g::TypeExpr *ft) const { return Loc { frame, h, off + more, ft, mem0 }; }
    bool Direct() const { return frame || mem0; }
};

// A resizable array variable (spec §5): its header gs_rhdr { base handle,
// len i64 } in memory, and the stack its elements are on, as an absolute
// stack index in an i32 local.
struct RzVar {
    Loc hdr;
    uint32_t stk = 0;
};

struct Backend {
    g::Ast &ast;
    Module m;
    Runtime rt;
    Layouts lay;
    Handles hs;
    uint32_t ssp = 0;      // global: the shadow stack's top
    uint32_t sslimit = 0;  // global: its lowest address

    // Data stack use per specialization, for the module's stack count
    // (spec §10.3): the most indices its own body holds at once, and each
    // call with the indices in use there, which the callee's start above.
    struct StackUse {
        int own = 0;
        std::vector<std::pair<g::FnSpec *, int>> calls;
    };
    std::map<g::FnSpec *, StackUse> stackuse;
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
    uint32_t fp = 0;                             // local: the frame's base
    int64_t frame = 0;                           // the frame's size so far
    int64_t retdst = -1;                         // param: where a fixed aggregate result goes
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
        // Their initial values are set below, once static data's size is known.
        ssp = m.AddGlobal(ValType::I32, true, 0);
        sslimit = m.AddGlobal(ValType::I32, false, 0);
        auto mainfn = FuncOf(mainspec, mainspec->body);
        while (!pending.empty()) {
            auto sp = pending.back();
            pending.pop_back();
            Function(sp, funcs[sp]);
        }
        // A WASI command's entry point: main starts at stack index 0.
        auto start = m.AddFunc({});
        m.FuncAt(start).code.I32Const(0);
        m.FuncAt(start).code.Call(mainfn);

        // One memory per data stack the program can reach (spec §10.3).
        auto nstacks = StackCount(mainspec);
        if (nstacks > int(mem0::MaxStacks)) throw g::CompileError { "goose-wasm: too many data stacks" };

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

    static ValType ValOf(Repr r) { return r == Repr::I64 ? ValType::I64 : ValType::I32; }

    // Whether a type is a fixed aggregate this backend can hold in memory.
    bool IsAggregate(g::TypeExpr *t) {
        return t && t->kind == g::TY_STRUCT && Layouts::Known(t);
    }
    // A resizable array (spec §5) of elements this backend can lay out.
    static bool IsRz(g::TypeExpr *t) {
        return t && t->kind == g::TY_ARRAY &&
               (t->arr->akind == g::A_GROW || t->arr->akind == g::A_GROWSHRINK) &&
               Layouts::Known(t->arr->sub);
    }
    // A plain, non-optional reference to one: upstream's fat reference
    // gs_rref { hdr, stk } (spec Appendix C).
    static bool IsRzRef(g::TypeExpr *t) {
        return t && t->kind == g::TY_REF && t->ref->lenstorage < 0 && !t->ref->pool &&
               !t->ref->optional && IsRz(t->ref->sub);
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
        if (sp->lexparent || sp->isfunval) Unsupported(at, "a call of a nested function");
        if (sp->sf->isextern) Unsupported(at, "a call of an extern fn");
        FuncType t;
        t.params.push_back(ValType::I32);
        if (sp->rets.size() == 1 && IsAggregate(sp->rets[0])) {
            t.params.push_back(ValType::I64);
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
        fn = index;
        spec = sp;
        locals.clear();
        memvars.clear();
        rzvars.clear();
        labels.clear();
        scopes.clear();
        frame = 0;
        stknext = stkmax = 0;
        bool aggret = sp->rets.size() == 1 && IsAggregate(sp->rets[0]);
        uint32_t pi = 1;  // after SP
        retdst = aggret ? int64_t(pi++) : -1;
        for (auto p : sp->params) {
            if (IsAggregate(p->type)) {
                memvars[p] = Loc { false, pi++, 0, p->type, true };
            } else if (IsRzRef(p->type)) {
                rzvars[p] = RzVar { Loc { false, pi, 0, nullptr }, pi + 1 };
                pi += 2;
            } else {
                locals[p] = pi++;
            }
        }
        nparams = pi;
        stackuse[sp] = StackUse {};
        fp = NewLocal(Repr::I32);
        // A copy: TypeIndex may add to the types it lives in.
        auto results = m.types[F().type].results;
        if (results.size() > 1) C().BlockOfType(m.TypeIndex({ {}, results }));
        else C().Block(results.empty() ? BlockType::Empty : BlockOf(ReprOfVal(results[0])));
        labels.push_back(Label { LK_FUNC });
        if (aggret) {
            Loc dst { false, uint32_t(retdst), 0, sp->rets[0], false };
            BlockConstruct(sp->body, dst);
        } else if (sp->rets.size() > 1) {
            // Several results leave by return statements only.
            BlockValue(sp->body, nullptr);
            C().Unreachable();
        } else {
            BlockValue(sp->body, sp->rets.empty() ? nullptr : sp->rets[0]);
        }
        Close();
        stackuse[sp].own = stkmax;
        if (frame == 0) return;
        frame = (frame + 7) / 8 * 8;
        // Release: the results stay on the stack below.
        C().LocalGet(fp);
        C().I32Const(int32_t(frame));
        C().I32Add();
        C().GlobalSet(ssp);
        // Claim, checked against the shadow stack's end: running out of it is
        // upstream's native call stack overflow, which has no location.
        Code pro;
        pro.GlobalGet(ssp);
        pro.I32Const(int32_t(frame));
        pro.I32Sub();
        pro.LocalTee(fp);
        pro.GlobalGet(sslimit);
        pro.I32LtU();
        pro.If();
        static const std::string ovf = "native call stack overflow";
        pro.I32Const(int32_t(StaticString(ovf)));
        pro.I32Const(int32_t(ovf.size()));
        pro.Call(rt.panic);
        pro.End();
        pro.LocalGet(fp);
        pro.GlobalSet(ssp);
        auto &body = C().b.b;
        body.insert(body.begin(), pro.b.b.begin(), pro.b.b.end());
    }

    static Repr ReprOfVal(ValType v) { return v == ValType::I64 ? Repr::I64 : Repr::I32; }

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
    void CallSpec(g::Call *c, const Loc *dst = nullptr) {
        if (!c->dispatch.empty()) Unsupported(c, "a case-function dispatch");
        if (c->fvbody || c->trailing) Unsupported(c, "a call of a function value");
        if (!c->fvremap.empty()) Unsupported(c, "a call with free variables");
        auto sp = c->spec;
        auto index = FuncOf(sp, c);
        auto args = c->ValueArgNodes();
        if (args.size() != sp->params.size()) Unsupported(c, "a call's argument count");
        bool aggret = sp->rets.size() == 1 && IsAggregate(sp->rets[0]);
        // The callee's stacks start above those in use here (upstream
        // SpTop); a call into this function's own recursive cycle holds
        // none (§7.8), which the checker guarantees (NoStackAcrossCycleCall).
        auto cycle = [](g::FnSpec *s) { while (s->cyclelink) s = s->cyclelink; return s; };
        if (stknext && spec->incycle && sp->incycle && cycle(sp) == cycle(spec))
            Unsupported(c, "a data stack held across a call into a recursive cycle");
        stackuse[spec].calls.push_back({ sp, stknext });
        C().LocalGet(SP);
        C().I32Const(stknext);
        C().I32Add();
        if (aggret) {
            if (!dst) Unsupported(c, "an aggregate result here");
            Handle(*dst);
        } else if (dst) {
            Unsupported(c, "this call as an aggregate");
        }
        for (size_t i = 0; i < args.size(); i++) {
            auto pt = sp->params[i]->type;
            if (IsAggregate(pt)) {
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
        C().Call(index);
    }

    // The resizable array a node names: a variable of that type or a
    // reference to one, through any `&` the checker inserted.
    RzVar RzOf(g::Node *n) {
        if (n->nkind == g::NK_Unary && static_cast<g::Unary *>(n)->op == g::T_BITAND)
            n = static_cast<g::Unary *>(n)->child;
        if (n->nkind == g::NK_Ident) {
            auto v = static_cast<g::Ident *>(n)->vdef;
            auto it = v ? rzvars.find(v) : rzvars.end();
            if (it != rzvars.end()) return it->second;
        }
        Unsupported(n, g::cat("a resizable array ", KindName(n->nkind)));
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
        F().locals.push_back(r == Repr::I64 ? ValType::I64 : ValType::I32);
        return nparams + uint32_t(F().locals.size() - 1);
    }

    uint32_t LocalOf(const g::Node *n, g::VarDef *v) {
        auto it = locals.find(v);
        if (it == locals.end()) Unsupported(n, g::cat("variable ", v->name));
        return it->second;
    }

    BlockType BlockOf(Repr r) {
        return r == Repr::None ? BlockType::Empty : r == Repr::I64 ? BlockType::I64 : BlockType::I32;
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
        Unsupported(n, g::cat("a value of type ", TypeKindName(t)));
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
        auto fr = ReprOf(n, from), tr = ReprOf(n, to);
        bool fromint = IsInt(from), toint = IsInt(to);
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

    // The type both operands of a binary operation are computed at: the
    // operands' when they agree, else the one the other widens to
    // implicitly (spec §6.1, §6.3).
    g::TypeExpr *Common(const g::Node *n, g::TypeExpr *a, g::TypeExpr *b) {
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
        if (l.frame) {
            C().LocalGet(fp);
        } else {
            C().LocalGet(l.h);
            C().I32WrapI64();
        }
    }
    // The handle of a place, its offset included.
    void HandleOf(const Loc &l) {
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
        if (!IsInt(t)) Unsupported(n, g::cat("a store of type ", TypeKindName(t)));
        switch (Bits(t)) {
            case 8: return 0x3a;
            case 16: return 0x3b;
            case 32: return 0x36;
            default: return 0x37;  // i64.store
        }
    }

    void Load(const g::Node *n, const Loc &l) {
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

    // The place in memory a node names: a variable held in memory, or a
    // field of one. False for anything else.
    bool MemLoc(g::Node *n, Loc &out) {
        if (n->nkind == g::NK_Index) {
            out = IndexLoc(static_cast<g::Index *>(n));
            return true;
        }
        if (n->nkind == g::NK_Ident) {
            auto v = static_cast<g::Ident *>(n)->vdef;
            auto it = v ? memvars.find(v) : memvars.end();
            if (it == memvars.end()) return false;
            out = it->second;
            return true;
        }
        if (n->nkind == g::NK_Dot) {
            auto d = static_cast<g::Dot *>(n);
            if (!d->IsField()) return false;
            auto ot = d->obj->exprtype;
            if (!ot || ot->kind != g::TY_STRUCT) Unsupported(n, "a field of this kind of object");
            Loc base;
            if (!MemLoc(d->obj, base)) {
                // A field of a value with no place of its own (f().x): the
                // value is built in a temporary first.
                if (!IsAggregate(ot)) Unsupported(n, "a field of this expression");
                base = FrameSlot(ot);
                Construct(d->obj, base);
            }
            auto si = Layouts::SI(ot);
            out = base.At(lay.StructLayout(si).offs[d->fieldidx], si->ftypes[d->fieldidx]);
            return true;
        }
        return false;
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
                    if (IsAggregate(ft)) Construct(val, floc);
                    else Store(val, floc, [&] { Expr(val, ft); });
                }
                for (size_t fi = 0; fi < si->ftypes.size(); fi++)
                    if (!given[fi] && !si->st->fields[fi].ispad)
                        Unsupported(n, "an omitted optional field");
                return;
            }
            case g::NK_Ident:
            case g::NK_Dot:
            case g::NK_Index: {
                Loc src;
                if (!MemLoc(n, src)) Unsupported(n, g::cat("an aggregate ", KindName(n->nkind)));
                Copy(dst, src);
                return;
            }
            case g::NK_Call: {
                auto c = static_cast<g::Call *>(n);
                if (c->builtin >= 0 || !c->spec) Unsupported(n, "this call as an aggregate");
                CallSpec(c, &dst);
                return;
            }
            case g::NK_Block: return BlockConstruct(static_cast<g::Block *>(n), dst);
            case g::NK_IfExpr: {
                auto i = static_cast<g::IfExpr *>(n);
                if (!i->elseb) Unsupported(n, "an if of a value without else");
                Expr(i->cond, i->cond->exprtype);
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

    // A block whose value, an aggregate, is built at dst.
    void BlockConstruct(g::Block *b, const Loc &dst) {
        PushScope();
        for (auto s : b->stmts) Stmt(s);
        if (b->tail) Construct(b->tail, dst);
        PopScope();
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
        if (!init || init->nkind != g::NK_ArrayLit) Unsupported(d, "this resizable array's initializer");
        auto lit = static_cast<g::ArrayLit *>(init);
        if (lit->fillval || lit->capexpr) Unsupported(d, "this resizable array's initializer");
        auto rel = stknext++;
        stkmax = std::max(stkmax, stknext);
        RzVar rz;
        rz.stk = NewLocal(Repr::I32);
        C().LocalGet(SP);
        C().I32Const(rel);
        C().I32Add();
        C().LocalSet(rz.stk);
        auto base = NewLocal(Repr::I32);
        TopAddr(rz.stk);
        C().I32Load(mem0::Tops);
        C().LocalSet(base);
        scopes.back().saves.push_back({ rz.stk, base });
        rz.hdr = FrameBytes(16);
        Store(d, rz.hdr.At(0, I64T()), [&] {
            C().LocalGet(rz.stk);
            C().I32Const(1);
            C().I32Add();
            C().I64ExtendI32U();
            C().I64Const(32);
            C().Int(intop::Shl, true);
            C().LocalGet(base);
            C().I64ExtendI32U();
            C().Int(intop::Or, true);
        });
        Store(d, rz.hdr.At(8, I64T()), [&] { C().I64Const(0); });
        rzvars[v] = rz;
        for (auto e : lit->elems) Push(d, rz, v->type->arr->sub, e);
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
        if (agg) {
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
        if (agg) Copy(slot, tmploc);
        else Store(at, slot, [&] { C().LocalGet(tmp); });
        auto len = rz.hdr.At(8, I64T());
        Store(at, len, [&] {
            Load(at, len);
            C().I64Const(1);
            C().Int(intop::Add, true);
        });
        if (ref) *ref = h;
    }

    // The handle of element i (a local) of an array whose header is at hdr.
    void ElemHandle(const g::Node *at, const Loc &hdr, uint32_t i, g::TypeExpr *et) {
        Load(at, hdr.At(0, I64T()));
        C().LocalGet(i);
        C().I64Const(lay.FixedSize(et));
        C().Int(intop::Mul, true);
        C().Int(intop::Add, true);
    }

    // The place of xs[i]: the index checked against the length unless BCE
    // proved it (Index::nobc), with upstream's GS_IDX: an unsigned compare,
    // so a negative index fails too.
    Loc IndexLoc(g::Index *x) {
        auto rz = RzOf(x->obj);
        auto et = x->obj->exprtype;
        if (et->kind == g::TY_REF) et = et->ref->sub;
        et = et->arr->sub;
        auto i = NewLocal(Repr::I64);
        Expr(x->idx, I64T());
        C().LocalSet(i);
        if (!x->nobc) {
            auto len = rz.hdr.At(8, I64T());
            C().LocalGet(i);
            Load(x, len);
            C().Int(intop::GeU, true);
            C().If();
            C().LocalGet(i);
            Load(x, len);
            Location(x);
            C().Call(rt.idxfail);
            C().End();
        }
        auto h = NewLocal(Repr::I64);
        ElemHandle(x, rz.hdr, i, et);
        C().LocalSet(h);
        return Loc { false, h, 0, et, false };
    }

    // A runtime check's location arguments: the file's name and length in
    // static data, and the line. A check in a standard library function
    // reports where the program called it, through a location parameter
    // (upstream CodeGen::AtCaller, SpecInfo::located); inlined library code
    // is anchored at the call already.
    void Location(const g::Node *at) {
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
            default: {
                // A call for its effects drops every result it has.
                if (n->nkind == g::NK_Call) {
                    auto call = static_cast<g::Call *>(n);
                    if (call->builtin == g::B_PUSH) {
                        auto an = call->ArgNodes();
                        auto rz = RzOf(an[0]);
                        auto at = an[0]->exprtype;
                        if (at->kind == g::TY_REF) at = at->ref->sub;
                        Push(call, rz, at->arr->sub, an[1]);
                        return;
                    }
                    if (call->builtin < 0 && call->spec && call->rettypes.size() > 1) {
                        CallSpec(call);
                        for (size_t i = 0; i < call->rettypes.size(); i++) C().Drop();
                        return;
                    }
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
        if (d->isglobal || d->byref || d->reusable) Unsupported(d, "this kind of declaration");
        // `let a, b = f();`, and the synthetic `result#1, result#2` the
        // parser declares for the statement `a, b = f();` (results).
        if (d->results || (d->defs.size() > 1 && d->inits.size() == 1)) return VarDeclResults(d);
        for (size_t i = 0; i < d->defs.size(); i++) {
            auto v = d->defs[i];
            if (IsRz(v->type)) {
                DeclareRz(d, v, i < d->inits.size() ? d->inits[i] : nullptr);
                continue;
            }
            if (IsRzRef(v->type)) {
                // A reference to a resizable array, as an inlined call binds
                // its parameter: the same header and stack. A `let` cannot
                // be rebound, so the alias holds for the whole scope.
                if (i >= d->inits.size() || v->isvar) Unsupported(d, "this reference declaration");
                rzvars[v] = RzOf(d->inits[i]);
                continue;
            }
            if (IsAggregate(v->type)) {
                // Built in its own slot, which its initializer cannot read.
                if (i >= d->inits.size()) Unsupported(d, "an aggregate without an initializer");
                auto slot = FrameSlot(v->type);
                Construct(d->inits[i], slot);
                memvars[v] = slot;
                continue;
            }
            auto local = NewLocal(ReprOf(d, v->type));
            if (i < d->inits.size()) {
                Expr(d->inits[i], v->type);
            } else {
                // A declaration without an initializer holds its type's
                // default, zero, each time it runs.
                C().IntConst(ReprOf(d, v->type) == Repr::I64, 0);
            }
            C().LocalSet(local);
            locals[v] = local;
        }
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

    void Assign(g::Assign *a) {
        if (a->pointee) Unsupported(a, "assignment through a reference");
        auto t = a->lval->exprtype;
        // The value an assignment stores, the old one on the stack for a
        // compound operator.
        auto value = [&](auto load) {
            if (a->op == g::T_ASSIGN) {
                Expr(a->rhs, t);
                return;
            }
            auto op = OpOfCompound(a->op);
            if (op == g::T_ASSIGN) Unsupported(a, "this assignment operator");
            load();
            Arith(a, op, t, a->rhs);
        };
        Loc l;
        if (MemLoc(a->lval, l)) {
            if (IsAggregate(t)) {
                if (a->op != g::T_ASSIGN) Unsupported(a, "this assignment operator on an aggregate");
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
        if (!IsInt(t)) Unsupported(n, "++ or -- of this type");
        auto wide = IsWide(t);
        auto step = [&] {
            C().IntConst(wide, 1);
            C().Int(n->op == g::T_INC ? intop::Add : intop::Sub, wide);
            if (!wide) Normalize(t);
        };
        Loc l;
        if (MemLoc(n->lval, l)) {
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
        Expr(w->cond, w->cond->exprtype);
        C().I32Eqz();
        C().BrIf(1);
        Stmt(w->body);
        C().Br(0);
        Close();
        Close();
    }

    // for over a range or a count (spec §6.5): the bounds are evaluated
    // once, and the counter runs in 64 bits so that an end one past a
    // narrow type's largest value (`for i: u8 in 0..256`) never wraps it.
    // for x in xs over a resizable array (spec §6.5): the current length is
    // tested before every iteration, so elements the body pushes are
    // visited too; x is a copy of a fixed-size element; the index is an i64.
    void ForArray(g::ForLoop *f) {
        if (f->byref || !f->vdef->copybind) Unsupported(f, "for over elements by reference");
        auto rz = RzOf(f->iter);
        auto at = f->iter->exprtype;
        if (at->kind == g::TY_REF) at = at->ref->sub;
        auto et = at->arr->sub;
        auto i = NewLocal(Repr::I64);
        C().I64Const(0);
        C().LocalSet(i);
        // The binding lives in the loop's own place; each iteration fills it.
        bool agg = IsAggregate(et);
        Loc xslot;
        uint32_t xlocal = 0;
        if (agg) {
            xslot = FrameSlot(et);
            memvars[f->vdef] = xslot;
        } else {
            xlocal = NewLocal(ReprOf(f, et));
            locals[f->vdef] = xlocal;
        }
        if (f->idxdef) locals[f->idxdef] = i;
        Open(LK_BREAK, Repr::None);
        Open(LK_PLAIN, Repr::None, nullptr, nullptr, true);
        C().LocalGet(i);
        Load(f, rz.hdr.At(8, I64T()));
        C().Int(intop::GeU, true);
        C().BrIf(1);
        auto h = NewLocal(Repr::I64);
        ElemHandle(f, rz.hdr, i, et);
        C().LocalSet(h);
        Loc elem { false, h, 0, et, false };
        if (agg) {
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

    void ForLoop(g::ForLoop *f) {
        if (f->iterkind == g::IK_ARRAY) return ForArray(f);
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
            if (l.dst) Construct(b->val, *l.dst);
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
                if (l.dst) Construct(r->vals[0], *l.dst);
                else Expr(r->vals[0], l.type);
            }
            ExitRestores(l.scopes);
            C().Br(uint32_t(Depth(LK_INLINE, r->target)));
            return;
        }
        if (r->target != spec->sf) Unsupported(r, "return from an enclosing function");
        if (r->vals.size() > spec->rets.size()) Unsupported(r, "a return of more values");
        auto out = uint32_t(Depth(LK_FUNC));
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
        Value(n);
        if (IsVoid(to)) {
            if (!IsVoid(t)) C().Drop();
            return;
        }
        if (IsVoid(t)) return;  // control left: nothing follows here
        Convert(n, t, to);
    }

    // Emits n's value at its own type.
    void Value(g::Node *n) {
        switch (n->nkind) {
            case g::NK_IntLit: {
                auto lit = static_cast<g::IntLit *>(n);
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
                if (!id->vdef) Unsupported(n, "a name that is not a variable");
                if (id->vdef->isglobal) Unsupported(n, "a global");
                Loc l;
                if (MemLoc(n, l)) Load(n, l);
                else C().LocalGet(LocalOf(n, id->vdef));
                Convert(n, id->vdef->type, n->exprtype);
                return;
            }
            case g::NK_Index: {
                auto l = IndexLoc(static_cast<g::Index *>(n));
                Load(n, l);
                Convert(n, l.t, n->exprtype);
                return;
            }
            case g::NK_Dot: {
                auto d = static_cast<g::Dot *>(n);
                if (d->member == g::B_LEN) {
                    auto rz = RzOf(d->obj);
                    Load(n, rz.hdr.At(8, I64T()));
                    Convert(n, I64T(), n->exprtype);
                    return;
                }
                if (!d->IsField()) Unsupported(n, g::cat("property .", d->name));
                Loc l;
                if (!MemLoc(n, l)) Unsupported(n, "this field access");
                Load(n, l);
                Convert(n, l.t, n->exprtype);
                return;
            }
            case g::NK_Unary: return Unary(static_cast<g::Unary *>(n));
            case g::NK_Binary: return Binary(static_cast<g::Binary *>(n));
            case g::NK_AsCast: {
                auto c = static_cast<g::AsCast *>(n);
                // Release semantics: `as` and `as!` both truncate or wrap
                // (spec §6.3); the debug build's range check is not emitted.
                Expr(c->child, c->child->exprtype);
                Convert(n, c->child->exprtype, n->exprtype);
                return;
            }
            case g::NK_Block: return BlockValue(static_cast<g::Block *>(n), n->exprtype);
            case g::NK_IfExpr: return If(static_cast<g::IfExpr *>(n));
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
                auto r = ReprOf(n);
                Open(LK_INLINE, r, n->exprtype, ib->sf);
                BlockValue(ib->body, n->exprtype);
                Close();
                return;
            }
            case g::NK_Call: {
                auto call = static_cast<g::Call *>(n);
                if (call->builtin == g::B_PRINT) return Print(call);
                if (call->builtin >= 0)
                    Unsupported(n, g::cat("builtin ", g::builtindefs[call->builtin].name));
                if (!call->spec) Unsupported(n, "call");
                CallSpec(call);
                // A call of several results used as a value is its first.
                for (size_t i = 1; i < call->rettypes.size(); i++) C().Drop();
                if (!call->rettypes.empty()) Convert(n, call->rettypes[0], n->exprtype);
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
        Expr(i->cond, i->cond->exprtype);
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

    void Unary(g::Unary *u) {
        auto t = u->exprtype;
        switch (u->op) {
            case g::T_NOT:
                Expr(u->child, u->child->exprtype);
                C().I32Eqz();
                return;
            case g::T_MINUS: {
                if (!IsInt(t)) Unsupported(u, "negation of this type");
                auto wide = IsWide(t);
                C().IntConst(wide, 0);
                Expr(u->child, t);
                C().Int(intop::Sub, wide);
                if (!wide) Normalize(t);
                return;
            }
            case g::T_BITNOT: {
                if (!IsInt(t)) Unsupported(u, "~ of this type");
                auto wide = IsWide(t);
                Expr(u->child, t);
                C().IntConst(wide, -1);
                C().Int(intop::Xor, wide);
                if (!wide) Normalize(t);
                return;
            }
            default:
                Unsupported(u, "this unary operator");
        }
    }

    void Binary(g::Binary *b) {
        auto lt = b->left->exprtype, rt_ = b->right->exprtype;
        switch (b->op) {
            case g::T_ANDAND:
            case g::T_OROR: {
                // Short-circuit: the right operand runs only when it decides.
                Expr(b->left, lt);
                OpenIf(Repr::I32);
                if (b->op == g::T_ANDAND) {
                    Expr(b->right, rt_);
                    C().Else();
                    C().I32Const(0);
                } else {
                    C().I32Const(1);
                    C().Else();
                    Expr(b->right, rt_);
                }
                Close();
                return;
            }
            case g::T_EQ: case g::T_NEQ: case g::T_LT: case g::T_GT: case g::T_LTEQ:
            case g::T_GTEQ: {
                auto ct = Common(b, lt, rt_);
                auto r = ReprOf(b, ct);
                Expr(b->left, ct);
                Expr(b->right, ct);
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
                auto t = b->exprtype;
                if (t->kind == g::TY_BOOL && (b->op == g::T_BITAND || b->op == g::T_BITOR ||
                                              b->op == g::T_XOR)) {
                    Expr(b->left, t);
                    Expr(b->right, t);
                    C().Int(b->op == g::T_BITAND ? intop::And : b->op == g::T_BITOR ? intop::Or
                                                                                    : intop::Xor,
                            false);
                    return;
                }
                if (!IsInt(t)) Unsupported(b, "arithmetic on this type");
                // A shift computes at its left operand's type; its count
                // has a type of its own.
                Expr(b->left, t);
                Arith(b, b->op, t, b->right);
            }
        }
    }

    // The integer operation `op` at type t, its left operand on the stack
    // and its right operand `right` still to emit. Release semantics (spec
    // §6.2, upstream runtime.h): signed arithmetic wraps, `/` and `%` check
    // for zero always, `%` is Euclidean, i64.min / -1 aborts, shift counts
    // are masked to the width.
    void Arith(g::Node *at, g::TType op, g::TypeExpr *t, g::Node *right) {
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

    void DivFail(g::Node *at, const char *msg) {
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
        DivFail(at, "division by zero");
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
                DivFail(at, "integer overflow in division");
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

    // ---- print ----

    // print(args...): renders the whole line, newline included, then writes
    // it at once (spec §3.7; upstream codegen_builtins.h, B_PRINT).
    void Print(g::Call *call) {
        uint64_t maxlen = 1;  // the newline
        auto line = NewLocal(Repr::I32);
        C().I32Const(mem0::LineBuf);
        C().LocalSet(line);
        auto appendstatic = [&](const std::string &s) {
            C().LocalGet(line);
            C().I32Const(int32_t(StaticString(s)));
            C().I32Const(int32_t(s.size()));
            C().Call(rt.append);
            C().LocalSet(line);
        };
        for (auto a : call->ArgNodes()) {
            auto t = a->exprtype;
            if (a->nkind == g::NK_StrLit) {
                auto &s = static_cast<g::StrLit *>(a)->val;
                maxlen += s.size();
                if (!s.empty()) appendstatic(s);
            } else if (IsInt(t)) {
                maxlen += 20;  // u64.max, or i64.min with its sign
                Expr(a, t);
                if (!IsWide(t)) {
                    if (IsSigned(t)) C().I64ExtendI32S();
                    else C().I64ExtendI32U();
                }
                C().LocalGet(line);
                C().Call(IsSigned(t) ? rt.fmt_i64 : rt.fmt_u64);
                C().LocalSet(line);
            } else if (t && t->kind == g::TY_BOOL) {
                maxlen += 5;
                Expr(a, t);
                OpenIf(Repr::None);
                appendstatic("true");
                C().Else();
                appendstatic("false");
                Close();
            } else {
                Unsupported(a, "print of a value of this type");
            }
        }
        if (maxlen > mem0::LineBufSize) Unsupported(call, "print of a line this long");
        C().LocalGet(line);
        C().I32Const('\n');
        C().I32Store8();
        C().I32Const(1);  // stdout
        C().I32Const(mem0::LineBuf);
        C().LocalGet(line);
        C().I32Const(1);
        C().I32Add();
        C().I32Const(mem0::LineBuf);
        C().I32Sub();
        C().Call(rt.write);
    }
};

}  // namespace goosewasm
