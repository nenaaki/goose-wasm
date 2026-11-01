// Encoding of the WebAssembly binary format (WebAssembly Core Specification
// 3.0, chapter 5): LEB128 integers, names, vectors and sections. Nothing
// here knows about Goose.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace goosewasm {

// A growing byte buffer with the format's primitive encodings (spec 5.1, 5.2).
struct Bytes {
    std::vector<uint8_t> b;

    void Byte(uint8_t v) { b.push_back(v); }
    void Raw(const Bytes &o) { b.insert(b.end(), o.b.begin(), o.b.end()); }
    void Raw(std::string_view s) { b.insert(b.end(), s.begin(), s.end()); }

    // Unsigned LEB128 (spec 5.2.2), for u32 and u64.
    void U(uint64_t v) {
        do {
            uint8_t byte = v & 0x7f;
            v >>= 7;
            if (v) byte |= 0x80;
            Byte(byte);
        } while (v);
    }
    // Signed LEB128, for s32, s64 and the i32/i64 constants.
    void S(int64_t v) {
        for (;;) {
            uint8_t byte = v & 0x7f;
            v >>= 7;  // arithmetic shift
            bool done = (v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40));
            if (!done) byte |= 0x80;
            Byte(byte);
            if (done) break;
        }
    }
    // A name: its UTF-8 bytes as a vector (spec 5.2.4).
    void Name(std::string_view s) {
        U(s.size());
        Raw(s);
    }
    size_t size() const { return b.size(); }
};

// Value and reference types (spec 5.3).
enum class ValType : uint8_t { I32 = 0x7f, I64 = 0x7e, F32 = 0x7d, F64 = 0x7c, V128 = 0x7b };

// Section ids (spec 5.5.2).
enum class SectionId : uint8_t {
    Custom = 0, Type = 1, Import = 2, Function = 3, Table = 4, Memory = 5, Global = 6,
    Export = 7, Start = 8, Element = 9, Code = 10, Data = 11, DataCount = 12, Tag = 13,
};

// Export descriptors (spec 5.5.10).
enum class ExternKind : uint8_t { Func = 0, Table = 1, Memory = 2, Global = 3, Tag = 4 };

// A section: its id, then its contents' size, then the contents (spec 5.5.2).
inline void AppendSection(Bytes &out, SectionId id, const Bytes &contents) {
    out.Byte(uint8_t(id));
    out.U(contents.size());
    out.Raw(contents);
}

// The module preamble: magic and version (spec 5.5.16).
inline void AppendPreamble(Bytes &out) {
    out.Raw(std::string_view("\0asm", 4));
    out.Raw(std::string_view("\x01\x00\x00\x00", 4));
}

}  // namespace goosewasm
