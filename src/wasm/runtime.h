// The runtime functions the backend puts into every module, written as Wasm
// instructions: what upstream's C runtime (src/runtime/) provides, as far as
// the backend needs it so far.
//
// Memory 0 (docs/design/memory-model.md) starts with the runtime's fixed
// area; static data follows it.
#pragma once

#include "module.h"

namespace goosewasm {

namespace mem0 {
// Address 0 is null and never handed out.
constexpr uint32_t Iovec = 16;      // fd_write's one iovec: buf, len.
constexpr uint32_t Nwritten = 24;   // fd_write's result count.
constexpr uint32_t Digits = 32;     // Scratch for rendering an integer, 20 digits and a sign.
constexpr uint32_t DigitsEnd = 64;
// What print renders a line into before writing it, as upstream's
// TempBuilder does (codegen_builtins.h, B_PRINT). The backend only emits a
// print whose longest line it can bound and that bound fits.
constexpr uint32_t LineBuf = 64;
constexpr uint32_t LineBufSize = 1 << 16;
constexpr uint32_t StaticData = LineBuf + LineBufSize;  // String literals and other constants.
}  // namespace mem0

struct Runtime {
    uint32_t fd_write = 0;
    uint32_t write = 0;     // (ptr i32, len i32): all of it to stdout
    uint32_t fmt_u64 = 0;   // (v i64, dst i32) -> i32: v in decimal at dst, returns its end
    uint32_t fmt_i64 = 0;   // (v i64, dst i32) -> i32: as fmt_u64, signed

    using V = ValType;

    // Imports go first in the function index space, so this runs before any
    // function is added.
    void Import(Module &m) {
        // WASI preview 1: fd_write(fd, iovs, iovs_len, nwritten) -> errno.
        fd_write = m.ImportFunc("wasi_snapshot_preview1", "fd_write",
                                { { V::I32, V::I32, V::I32, V::I32 }, { V::I32 } });
    }

    void Define(Module &m) {
        // write(ptr, len): calls fd_write until every byte is out; stops on
        // an error, as fwrite to stdout would leave it.
        {
            write = m.AddFunc({ { V::I32, V::I32 }, {} });
            auto &c = m.FuncAt(write).code;
            enum { ptr, len };
            c.Block();
            c.Loop();
            c.LocalGet(len);
            c.I32Eqz();
            c.BrIf(1);
            c.I32Const(mem0::Iovec);
            c.LocalGet(ptr);
            c.I32Store(0);
            c.I32Const(mem0::Iovec);
            c.LocalGet(len);
            c.I32Store(4);
            c.I32Const(1);  // stdout
            c.I32Const(mem0::Iovec);
            c.I32Const(1);
            c.I32Const(mem0::Nwritten);
            c.Call(fd_write);
            c.BrIf(1);  // nonzero errno
            c.LocalGet(ptr);
            c.I32Const(mem0::Nwritten);
            c.I32Load();
            c.I32Add();
            c.LocalSet(ptr);
            c.LocalGet(len);
            c.I32Const(mem0::Nwritten);
            c.I32Load();
            c.I32Sub();
            c.LocalSet(len);
            c.Br(0);
            c.End();
            c.End();
        }
        // fmt_u64(v, dst) -> end: the digits are made backwards in the
        // scratch area, then copied to dst.
        {
            fmt_u64 = m.AddFunc({ { V::I64, V::I32 }, { V::I32 } }, { V::I32, V::I32 });
            auto &c = m.FuncAt(fmt_u64).code;
            enum { v, dst, p, n };
            c.I32Const(mem0::DigitsEnd);
            c.LocalSet(p);
            c.Loop();
            c.LocalGet(p);
            c.I32Const(1);
            c.I32Sub();
            c.LocalTee(p);
            c.LocalGet(v);
            c.I64Const(10);
            c.I64RemU();
            c.I32WrapI64();
            c.I32Const('0');
            c.I32Add();
            c.I32Store8();
            c.LocalGet(v);
            c.I64Const(10);
            c.I64DivU();
            c.LocalTee(v);
            c.I64Eqz();
            c.I32Eqz();
            c.BrIf(0);
            c.End();
            c.I32Const(mem0::DigitsEnd);
            c.LocalGet(p);
            c.I32Sub();
            c.LocalSet(n);
            c.LocalGet(dst);
            c.LocalGet(p);
            c.LocalGet(n);
            c.MemoryCopy();
            c.LocalGet(dst);
            c.LocalGet(n);
            c.I32Add();
        }
        // fmt_i64(v, dst) -> end: a sign, then the magnitude. Negating
        // i64.min wraps to itself, whose bits are its magnitude unsigned.
        {
            fmt_i64 = m.AddFunc({ { V::I64, V::I32 }, { V::I32 } });
            auto &c = m.FuncAt(fmt_i64).code;
            enum { v, dst };
            c.LocalGet(v);
            c.I64Const(0);
            c.I64LtS();
            c.If();
            c.LocalGet(dst);
            c.I32Const('-');
            c.I32Store8();
            c.I64Const(0);
            c.LocalGet(v);
            c.I64Sub();
            c.LocalSet(v);
            c.LocalGet(dst);
            c.I32Const(1);
            c.I32Add();
            c.LocalSet(dst);
            c.End();
            c.LocalGet(v);
            c.LocalGet(dst);
            c.Call(fmt_u64);
        }
    }
};

}  // namespace goosewasm
