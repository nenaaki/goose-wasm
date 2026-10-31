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

struct Backend {
    g::Ast &ast;
    Module m;
    Runtime rt;
    std::map<std::string, uint32_t> strings;  // literal bytes -> address in memory 0
    std::string staticdata;                   // memory 0 from mem0::StaticData

    // The function being emitted.
    Func *fn = nullptr;
    uint32_t nparams = 0;
    g::FnSpec *spec = nullptr;
    std::map<g::VarDef *, uint32_t> locals;

    // The structured instructions open around the code being emitted,
    // innermost last: what a br's depth counts.
    enum LabelKind { LK_PLAIN, LK_BREAK, LK_CONTINUE, LK_INLINE };
    struct Label {
        LabelKind kind;
        g::SFunction *sf = nullptr;  // LK_INLINE: whose body
        g::TypeExpr *type = nullptr; // LK_BREAK, LK_INLINE: the value a branch out carries
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
        auto mainfn = m.AddFunc({});
        Function(mainspec, m.FuncAt(mainfn));
        // A WASI command's entry point.
        auto start = m.AddFunc({});
        m.FuncAt(start).code.Call(mainfn);

        auto end = uint64_t(mem0::StaticData) + staticdata.size();
        m.memories.push_back(Memory { (end + 0xffff) / 0x10000 });
        if (!staticdata.empty()) m.data.push_back(DataSegment { 0, mem0::StaticData, staticdata });
        m.exports.push_back(Export { "memory", ExternKind::Memory, 0 });
        m.exports.push_back(Export { "_start", ExternKind::Func, start });
        return std::move(m);
    }

    // ---- Functions, locals and labels ----

    void Function(g::FnSpec *sp, Func &f) {
        if (!sp->params.empty() || !sp->rets.empty()) Unsupported(sp->body, "parameters or results");
        fn = &f;
        nparams = 0;
        spec = sp;
        locals.clear();
        labels.clear();
        Stmt(sp->body);
    }

    Code &C() { return fn->code; }

    uint32_t NewLocal(Repr r) {
        fn->locals.push_back(r == Repr::I64 ? ValType::I64 : ValType::I32);
        return nparams + uint32_t(fn->locals.size() - 1);
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
        labels.push_back(Label { k, sf, t });
    }
    void OpenIf(Repr r) {
        C().If(BlockOf(r));
        labels.push_back(Label { LK_PLAIN });
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
                C().Br(uint32_t(d));
                return;
            }
            case g::NK_Return: return Return(static_cast<g::Return *>(n));
            default: {
                auto r = ReprOf(n);
                Expr(n, n->exprtype);
                if (r != Repr::None) C().Drop();
            }
        }
    }

    void VarDecl(g::VarDecl *d) {
        if (d->isglobal || d->byref || d->reusable || d->results)
            Unsupported(d, "this kind of declaration");
        for (size_t i = 0; i < d->defs.size(); i++) {
            auto v = d->defs[i];
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
        auto local = LvalLocal(a->lval);
        auto t = a->lval->exprtype;
        if (a->op == g::T_ASSIGN) {
            Expr(a->rhs, t);
        } else {
            auto op = OpOfCompound(a->op);
            if (op == g::T_ASSIGN) Unsupported(a, "this assignment operator");
            C().LocalGet(local);
            Arith(a, op, t, a->rhs);
        }
        C().LocalSet(local);
    }

    void IncDec(g::IncDec *n) {
        auto local = LvalLocal(n->lval);
        auto t = n->lval->exprtype;
        if (!IsInt(t)) Unsupported(n, "++ or -- of this type");
        auto wide = IsWide(t);
        C().LocalGet(local);
        C().IntConst(wide, 1);
        C().Int(n->op == g::T_INC ? intop::Add : intop::Sub, wide);
        if (!wide) Normalize(t);
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
    void ForLoop(g::ForLoop *f) {
        if (f->iterkind != g::IK_RANGE && f->iterkind != g::IK_COUNT)
            Unsupported(f, "for over an array or slice");
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
        auto &l = labels[labels.size() - 1 - d];
        if (b->val) Expr(b->val, l.type);
        C().Br(uint32_t(d));
    }

    // return: out of the inlined body of the function it returns from, or
    // out of the function itself.
    void Return(g::Return *r) {
        if (r->vals.size() > 1) Unsupported(r, "a return of several values");
        auto d = Depth(LK_INLINE, r->target);
        if (d >= 0) {
            auto &l = labels[labels.size() - 1 - d];
            if (!r->vals.empty()) Expr(r->vals[0], l.type);
            C().Br(uint32_t(d));
            return;
        }
        if (r->target != spec->sf) Unsupported(r, "return from an enclosing function");
        if (!r->vals.empty()) Unsupported(r, "a return value");
        C().Return();
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
                C().LocalGet(LocalOf(n, id->vdef));
                Convert(n, id->vdef->type, n->exprtype);
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
                Unsupported(n, call->builtin >= 0
                                   ? g::cat("builtin ", g::builtindefs[call->builtin].name)
                                   : std::string("call"));
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
        for (auto s : b->stmts) Stmt(s);
        if (b->tail) Expr(b->tail, t);
        else if (!IsVoid(t)) C().Unreachable();
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
        auto file = FileName(at->line);
        C().I32Const(int32_t(StaticString(s)));
        C().I32Const(int32_t(s.size()));
        C().I32Const(int32_t(StaticString(file)));
        C().I32Const(int32_t(file.size()));
        C().I32Const(at->line.line);
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
