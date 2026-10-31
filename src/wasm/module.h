// A WebAssembly module under construction, and its binary encoding. It
// holds only what the backend uses so far; sections it has nothing for are
// left out, as the format allows (spec 5.5.16).
#pragma once

#include <stdexcept>

#include "code.h"

namespace goosewasm {

struct FuncType {
    std::vector<ValType> params, results;
    bool operator==(const FuncType &) const = default;
};

// An imported function (spec 5.5.5). Imports come first in the function
// index space.
struct FuncImport {
    std::string module, name;
    uint32_t type = 0;
};

// A function defined in the module: its type, the locals after its
// parameters, and its body without the final `end`, which Encode adds.
struct Func {
    uint32_t type = 0;
    std::vector<ValType> locals;
    Code code;
};

struct Memory {
    uint64_t min = 0;
    bool hasmax = false;
    uint64_t max = 0;
};

struct Export {
    std::string name;
    ExternKind kind;
    uint32_t index;
};

// A global (spec 5.5.9) with a constant initializer.
struct Global {
    ValType type;
    bool mut;
    int64_t init;
};

// An active data segment (spec 5.5.14): bytes placed at a constant offset of
// a memory at instantiation.
struct DataSegment {
    uint32_t mem = 0;
    uint32_t offset = 0;
    std::string bytes;
};

struct Module {
    std::vector<FuncType> types;
    std::vector<FuncImport> imports;
    std::vector<Func> funcs;
    std::vector<Memory> memories;
    std::vector<Global> globals;
    std::vector<Export> exports;
    std::vector<DataSegment> data;

    uint32_t AddGlobal(ValType t, bool mut, int64_t init) {
        globals.push_back(Global { t, mut, init });
        return uint32_t(globals.size() - 1);
    }

    // The index of `t`, adding it once.
    uint32_t TypeIndex(const FuncType &t) {
        for (uint32_t i = 0; i < types.size(); i++)
            if (types[i] == t) return i;
        types.push_back(t);
        return uint32_t(types.size() - 1);
    }

    uint32_t ImportFunc(std::string module, std::string name, const FuncType &t) {
        if (!funcs.empty()) throw std::logic_error("imports must come before functions");
        imports.push_back(FuncImport { std::move(module), std::move(name), TypeIndex(t) });
        return uint32_t(imports.size() - 1);
    }

    // A new function's index in the function index space, imports included.
    uint32_t AddFunc(const FuncType &t, std::vector<ValType> locals = {}) {
        funcs.push_back(Func { TypeIndex(t), std::move(locals), {} });
        return uint32_t(imports.size() + funcs.size() - 1);
    }
    Func &FuncAt(uint32_t index) { return funcs[index - imports.size()]; }

    Bytes Encode() const {
        Bytes out;
        AppendPreamble(out);
        if (!types.empty()) {
            Bytes s;
            s.U(types.size());
            for (auto &t : types) {
                s.Byte(0x60);  // func type (spec 5.3.6)
                s.U(t.params.size());
                for (auto v : t.params) s.Byte(uint8_t(v));
                s.U(t.results.size());
                for (auto v : t.results) s.Byte(uint8_t(v));
            }
            AppendSection(out, SectionId::Type, s);
        }
        if (!imports.empty()) {
            Bytes s;
            s.U(imports.size());
            for (auto &i : imports) {
                s.Name(i.module);
                s.Name(i.name);
                s.Byte(uint8_t(ExternKind::Func));
                s.U(i.type);
            }
            AppendSection(out, SectionId::Import, s);
        }
        if (!funcs.empty()) {
            Bytes s;
            s.U(funcs.size());
            for (auto &f : funcs) s.U(f.type);
            AppendSection(out, SectionId::Function, s);
        }
        if (!memories.empty()) {
            Bytes s;
            s.U(memories.size());
            for (auto &m : memories) {
                s.Byte(m.hasmax ? 0x01 : 0x00);  // limits, 32-bit addresses (spec 5.3.7)
                s.U(m.min);
                if (m.hasmax) s.U(m.max);
            }
            AppendSection(out, SectionId::Memory, s);
        }
        if (!globals.empty()) {
            Bytes s;
            s.U(globals.size());
            for (auto &g : globals) {
                s.Byte(uint8_t(g.type));
                s.Byte(g.mut ? 0x01 : 0x00);
                if (g.type == ValType::I64) {
                    s.Byte(0x42);  // i64.const
                    s.S(g.init);
                } else {
                    s.Byte(0x41);  // i32.const
                    s.S(int32_t(g.init));
                }
                s.Byte(0x0b);
            }
            AppendSection(out, SectionId::Global, s);
        }
        if (!exports.empty()) {
            Bytes s;
            s.U(exports.size());
            for (auto &e : exports) {
                s.Name(e.name);
                s.Byte(uint8_t(e.kind));
                s.U(e.index);
            }
            AppendSection(out, SectionId::Export, s);
        }
        if (!funcs.empty()) {
            Bytes s;
            s.U(funcs.size());
            for (auto &f : funcs) {
                Bytes code;
                // Locals as runs of one type (spec 5.5.13).
                std::vector<std::pair<uint32_t, ValType>> runs;
                for (auto v : f.locals) {
                    if (!runs.empty() && runs.back().second == v) runs.back().first++;
                    else runs.push_back({ 1, v });
                }
                code.U(runs.size());
                for (auto &[n, v] : runs) {
                    code.U(n);
                    code.Byte(uint8_t(v));
                }
                code.Raw(f.code.b);
                code.Byte(0x0b);  // end
                s.U(code.size());
                s.Raw(code);
            }
            AppendSection(out, SectionId::Code, s);
        }
        if (!data.empty()) {
            Bytes s;
            s.U(data.size());
            for (auto &d : data) {
                if (d.mem == 0) {
                    s.U(0);  // active, memory 0
                } else {
                    s.U(2);  // active, explicit memory
                    s.U(d.mem);
                }
                s.Byte(0x41);  // i32.const offset, end
                s.S(int32_t(d.offset));
                s.Byte(0x0b);
                s.Name(d.bytes);
            }
            AppendSection(out, SectionId::Data, s);
        }
        return out;
    }
};

}  // namespace goosewasm
