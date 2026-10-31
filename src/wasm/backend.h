// The Wasm backend: turns a typechecked, optimized Goose program (the
// upstream Ast after BCE) into a module.
//
// It translates a growing subset of the language. Anything outside it stops
// the compile with "goose-wasm: unsupported: <what> at <file>:<line>", which
// scripts/difftest.py counts apart from failures.
//
// Memory layout follows docs/design/memory-model.md: memory 0 holds the
// runtime's fixed area and static data, and is exported as `memory`, the one
// memory WASI preview 1 reads and writes.
#pragma once

#include <map>

#include "runtime.h"

namespace goosewasm {

using goose::Ast;
using goose::Call;
using goose::FnSpec;
using goose::IntLit;
using goose::Node;
using goose::StrLit;

inline const char *KindName(goose::NodeKind k) {
    static const char *const names[] = {
        "IntLit", "FltLit", "BoolLit", "StrLit", "Ident", "ArrayLit", "StructLit", "Unary",
        "Binary", "Dot", "Call", "Index", "SliceExpr", "AsCast", "NullLit", "SelfRef",
        "RangeExpr", "Block", "IfExpr", "MatchExpr", "EarlyBlock", "While", "LoopExpr",
        "ForLoop", "ConstFor", "Return", "Break", "Continue", "Defer", "FunVal", "VarDecl",
        "Assign", "IncDec", "InlineBlock", "FnDecl", "StructDecl", "EnumDecl", "AliasDecl",
    };
    static_assert(sizeof(names) / sizeof(names[0]) == goose::NK_AliasDecl + 1);
    return names[k];
}

struct Backend {
    Ast &ast;
    Module m;
    Runtime rt;
    std::map<std::string, uint32_t> strings;  // literal bytes -> address in memory 0
    std::string staticdata;                   // memory 0 from mem0::StaticData

    explicit Backend(Ast &a) : ast(a) {}

    [[noreturn]] void Unsupported(const Node *n, const std::string &what) {
        auto &file = ast.sources[n->line.fileidx].first;
        throw goose::CompileError {
            goose::cat("goose-wasm: unsupported: ", what, " at ", file, ":", n->line.line) };
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
        FnSpec *mainspec = nullptr;
        for (auto sp : ast.fnspecs)
            if (sp->live && !sp->isfunval && !sp->lexparent && sp->sf->qname == "main")
                mainspec = sp;
        if (!mainspec) throw goose::CompileError { "goose-wasm: no live main" };

        rt.Import(m);
        rt.Define(m);
        auto mainfn = m.AddFunc({}, { ValType::I32 });
        FunctionBody(mainspec, m.FuncAt(mainfn).code);
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

    // The locals of the function being emitted.
    enum : uint32_t { L_LINE = 0 };  // print's cursor into the line buffer

    void FunctionBody(FnSpec *sp, Code &c) {
        if (!sp->params.empty() || !sp->rets.empty()) Unsupported(sp->body, "parameters or results");
        Stmt(sp->body, c);
    }

    void Stmt(Node *n, Code &c) {
        switch (n->nkind) {
            case goose::NK_Block: {
                auto b = static_cast<goose::Block *>(n);
                for (auto s : b->stmts) Stmt(s, c);
                if (b->tail) Stmt(b->tail, c);
                return;
            }
            case goose::NK_Call: {
                auto call = static_cast<Call *>(n);
                if (call->builtin == goose::B_PRINT) return Print(call, c);
                Unsupported(n, call->builtin >= 0
                                   ? goose::cat("builtin ", goose::builtindefs[call->builtin].name)
                                   : std::string("call"));
            }
            default:
                Unsupported(n, goose::cat("statement ", KindName(n->nkind)));
        }
    }

    // print(args...): renders the whole line, newline included, then writes
    // it at once (spec §3.7; upstream codegen_builtins.h, B_PRINT).
    void Print(Call *call, Code &c) {
        uint64_t maxlen = 1;  // the newline
        c.I32Const(mem0::LineBuf);
        c.LocalSet(L_LINE);
        for (auto a : call->ArgNodes()) {
            switch (a->nkind) {
                case goose::NK_StrLit: {
                    auto &s = static_cast<StrLit *>(a)->val;
                    maxlen += s.size();
                    if (s.empty()) break;
                    c.LocalGet(L_LINE);
                    c.I32Const(int32_t(StaticString(s)));
                    c.I32Const(int32_t(s.size()));
                    c.MemoryCopy();
                    c.LocalGet(L_LINE);
                    c.I32Const(int32_t(s.size()));
                    c.I32Add();
                    c.LocalSet(L_LINE);
                    break;
                }
                case goose::NK_IntLit: {
                    auto lit = static_cast<IntLit *>(a);
                    auto t = a->exprtype;
                    if (!t || t->kind != goose::TY_INT || t->intstorage == goose::IS_VARINT)
                        Unsupported(a, "print of this literal's type");
                    maxlen += 20;  // u64.max, or i64.min with its sign
                    c.I64Const(lit->val);
                    c.LocalGet(L_LINE);
                    c.Call(goose::IsUnsigned(t->intstorage) ? rt.fmt_u64 : rt.fmt_i64);
                    c.LocalSet(L_LINE);
                    break;
                }
                default:
                    Unsupported(a, goose::cat("print argument ", KindName(a->nkind)));
            }
        }
        if (maxlen > mem0::LineBufSize) Unsupported(call, "print of a line this long");
        c.LocalGet(L_LINE);
        c.I32Const('\n');
        c.I32Store8();
        c.I32Const(mem0::LineBuf);
        c.LocalGet(L_LINE);
        c.I32Const(1);
        c.I32Add();
        c.I32Const(mem0::LineBuf);
        c.I32Sub();
        c.Call(rt.write);
    }
};

}  // namespace goosewasm
