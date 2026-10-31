// A WebAssembly module under construction, and its binary encoding. It
// holds only what the backend uses so far; sections it has nothing for are
// left out, as the format allows (spec 5.5.16).
#pragma once

#include "binary.h"

namespace goosewasm {

struct FuncType {
    std::vector<ValType> params, results;
    bool operator==(const FuncType &) const = default;
};

// A function defined in the module: its type, locals and body expression.
// `body` holds the instructions without the final `end`, which Encode adds.
struct Func {
    uint32_t type = 0;
    std::vector<ValType> locals;
    Bytes body;
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

struct Module {
    std::vector<FuncType> types;
    std::vector<Func> funcs;
    std::vector<Memory> memories;
    std::vector<Export> exports;

    // The index of `t`, adding it once.
    uint32_t TypeIndex(const FuncType &t) {
        for (uint32_t i = 0; i < types.size(); i++)
            if (types[i] == t) return i;
        types.push_back(t);
        return uint32_t(types.size() - 1);
    }

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
                code.Raw(f.body);
                code.Byte(0x0b);  // end
                s.U(code.size());
                s.Raw(code);
            }
            AppendSection(out, SectionId::Code, s);
        }
        return out;
    }
};

}  // namespace goosewasm
