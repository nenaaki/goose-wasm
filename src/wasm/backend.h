// The Wasm backend: turns a typechecked, optimized Goose program (the
// upstream Ast after BCE) into a module. For now it lays out memory 0 and an
// empty `_start`; it does not translate the program yet.
//
// Memory layout follows docs/design/memory-model.md: memory 0 holds static
// data, globals, the shadow stack and the WASI transfer area, and is exported
// as `memory`, the one memory WASI preview 1 reads and writes.
#pragma once

#include "module.h"

namespace goosewasm {

struct Backend {
    goose::Ast &ast;

    explicit Backend(goose::Ast &a) : ast(a) {}

    Module Build() {
        Module m;
        m.memories.push_back(Memory { 1 });
        m.exports.push_back(Export { "memory", ExternKind::Memory, 0 });
        Func start;
        start.type = m.TypeIndex(FuncType {});
        m.funcs.push_back(std::move(start));
        // A WASI command's entry point.
        m.exports.push_back(Export { "_start", ExternKind::Func, 0 });
        return m;
    }
};

}  // namespace goosewasm
