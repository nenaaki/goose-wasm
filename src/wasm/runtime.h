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
// TempBuilder does (codegen_builtins.h, B_PRINT), and where a runtime error
// is assembled. The backend only emits a print whose longest line it can
// bound and that bound fits.
constexpr uint32_t LineBuf = 64;
constexpr uint32_t LineBufSize = 1 << 16;
// Where a runtime error's formatted message is assembled before fail()
// copies it into the line buffer.
constexpr uint32_t MsgBuf = LineBuf + LineBufSize;
constexpr uint32_t MsgBufSize = 256;
// The data stacks' state, one 16-byte entry per stack index: its top (an
// i32 address in its memory) at 0 and its memory's size in bytes (an i64)
// at 8. Upstream's GS_MAX_STACKS bounds the indices.
constexpr uint32_t Tops = MsgBuf + MsgBufSize;
constexpr uint32_t MaxStacks = 1024;
// The float formatter's naturals and digits (floatfmt.h).
constexpr uint32_t FloatScratch = Tops + MaxStacks * 16;
constexpr uint32_t FloatScratchSize = 1024;
// A module built with --header: the heap its host allocates from (heap.h,
// gs_alloc), whose end is the i32 at HeapBrk and whose free lists are
// HeapClasses i32 words at HeapFree. Its blocks follow the shadow stack.
constexpr uint32_t HeapBrk = FloatScratch + FloatScratchSize;
constexpr uint32_t HeapFree = HeapBrk + 4;
constexpr uint32_t HeapClasses = 28;
constexpr uint32_t StaticData = HeapBrk + 128;  // String literals and other constants.
static_assert(HeapFree + 4 * HeapClasses <= StaticData);
// The shadow stack follows static data and grows down from its end
// (docs/design/value-representation.md §2).
constexpr uint32_t ShadowStackSize = 8 << 20;
}  // namespace mem0

struct Runtime {
    uint32_t fd_write = 0;
    uint32_t proc_exit = 0;
    uint32_t write = 0;     // (fd i32, ptr i32, len i32): all of it
    uint32_t fmt_u64 = 0;   // (v i64, dst i32) -> i32: v in decimal at dst, returns its end
    uint32_t fmt_i64 = 0;   // (v i64, dst i32) -> i32: as fmt_u64, signed
    uint32_t append = 0;    // (dst i32, src i32, len i32) -> i32: copies, returns dst + len
    uint32_t fail = 0;      // (msg i32, len i32, file i32, filelen i32, line i32): never returns
    uint32_t panic = 0;     // (msg i32, len i32): never returns
    uint32_t idxfail = 0;   // (i i64, n i64, file i32, filelen i32, line i32): never returns

    // With threads (threads.h), WASI reads and writes the shared memory:
    // write stages its bytes in the thread's area there, whose address is
    // in Wasm global `xfer`; and a failure in a contained worker, while
    // global `contained` is set, hands its message to `contain`, which
    // does not return.
    bool threaded = false;
    uint32_t xfer = 0, contained = 0, contain = 0;

    using V = ValType;

    // Imports go first in the function index space, so this runs before any
    // function is added.
    void Import(Module &m) {
        // WASI preview 1: fd_write(fd, iovs, iovs_len, nwritten) -> errno.
        fd_write = m.ImportFunc("wasi_snapshot_preview1", "fd_write",
                                { { V::I32, V::I32, V::I32, V::I32 }, { V::I32 } });
        proc_exit = m.ImportFunc("wasi_snapshot_preview1", "proc_exit", { { V::I32 }, {} });
    }

    void Define(Module &m) {
        // write(fd, ptr, len): calls fd_write until every byte is out; stops
        // on an error, as fwrite would leave it.
        if (threaded) {
            write = m.AddFunc({ { V::I32, V::I32, V::I32 }, {} }, { V::I32 });
            auto &c = m.FuncAt(write).code;
            enum { fd, ptr, len, n };
            constexpr int32_t chunk = 1 << 16;  // threads.h, shm::XferSize
            c.Block();
            c.Loop();
            c.LocalGet(len);
            c.I32Eqz();
            c.BrIf(1);
            c.LocalGet(len);
            c.I32Const(chunk);
            c.LocalGet(len);
            c.I32Const(chunk);
            c.I32LtU();
            c.Select();
            c.LocalSet(n);
            c.GlobalGet(xfer);
            c.I32Const(16);
            c.I32Add();
            c.LocalGet(ptr);
            c.LocalGet(n);
            c.MemoryCopy(SharedMem, 0);
            c.GlobalGet(xfer);
            c.GlobalGet(xfer);
            c.I32Const(16);
            c.I32Add();
            c.I32Store(0, SharedMem);
            c.GlobalGet(xfer);
            c.LocalGet(n);
            c.I32Store(4, SharedMem);
            c.LocalGet(fd);
            c.GlobalGet(xfer);
            c.I32Const(1);
            c.GlobalGet(xfer);
            c.I32Const(8);
            c.I32Add();
            c.Call(fd_write);
            c.BrIf(1);  // nonzero errno
            c.LocalGet(ptr);
            c.GlobalGet(xfer);
            c.I32Load(8, SharedMem);
            c.I32Add();
            c.LocalSet(ptr);
            c.LocalGet(len);
            c.GlobalGet(xfer);
            c.I32Load(8, SharedMem);
            c.I32Sub();
            c.LocalSet(len);
            c.Br(0);
            c.End();
            c.End();
        } else {
            write = m.AddFunc({ { V::I32, V::I32, V::I32 }, {} });
            auto &c = m.FuncAt(write).code;
            enum { fd, ptr, len };
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
            c.LocalGet(fd);
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
        // append(dst, src, len) -> dst + len.
        {
            append = m.AddFunc({ { V::I32, V::I32, V::I32 }, { V::I32 } });
            auto &c = m.FuncAt(append).code;
            enum { dst, src, len };
            c.LocalGet(dst);
            c.LocalGet(src);
            c.LocalGet(len);
            c.MemoryCopy();
            c.LocalGet(dst);
            c.LocalGet(len);
            c.I32Add();
        }
    }

    // idxfail(i, n, file, filelen, line): upstream's gs_idxfail, "index %lld
    // out of bounds (length %lld)" at the location. `pieces` are the fixed
    // texts' addresses in static data: "index ", " out of bounds (length ",
    // ")".
    void DefineIdxFail(Module &m, uint32_t p0, uint32_t n0, uint32_t p1, uint32_t n1,
                       uint32_t p2, uint32_t n2) {
        idxfail = m.AddFunc({ { V::I64, V::I64, V::I32, V::I32, V::I32 }, {} }, { V::I32 });
        auto &c = m.FuncAt(idxfail).code;
        enum { i, n, file, filelen, line, p };
        auto text = [&](uint32_t addr, uint32_t len) {
            c.LocalGet(p);
            c.I32Const(int32_t(addr));
            c.I32Const(int32_t(len));
            c.Call(append);
            c.LocalSet(p);
        };
        c.I32Const(mem0::MsgBuf);
        c.LocalSet(p);
        text(p0, n0);
        c.LocalGet(i);
        c.LocalGet(p);
        c.Call(fmt_i64);
        c.LocalSet(p);
        text(p1, n1);
        c.LocalGet(n);
        c.LocalGet(p);
        c.Call(fmt_i64);
        c.LocalSet(p);
        text(p2, n2);
        c.I32Const(mem0::MsgBuf);
        c.LocalGet(p);
        c.I32Const(mem0::MsgBuf);
        c.I32Sub();
        c.LocalGet(file);
        c.LocalGet(filelen);
        c.LocalGet(line);
        c.Call(fail);
        c.Unreachable();
    }

    // panic(msg, len): a runtime error with no Goose location, as upstream's
    // gs_panic and its fault handler's reports print it: "goose runtime
    // error: ", the message and a newline, then exit code 1.
    void DefinePanic(Module &m, uint32_t lead, uint32_t leadlen) {
        panic = m.AddFunc({ { V::I32, V::I32 }, {} }, { V::I32 });
        auto &c = m.FuncAt(panic).code;
        enum { msg, len, p };
        c.I32Const(mem0::LineBuf);
        c.I32Const(int32_t(lead));
        c.I32Const(int32_t(leadlen));
        c.Call(append);
        c.LocalGet(msg);
        c.LocalGet(len);
        c.Call(append);
        c.LocalTee(p);
        c.I32Const('\n');
        c.I32Store8();
        c.I32Const(2);  // stderr
        c.I32Const(mem0::LineBuf);
        c.LocalGet(p);
        c.I32Const(1 - int32_t(mem0::LineBuf));
        c.I32Add();
        c.Call(write);
        c.I32Const(1);
        c.Call(proc_exit);
        c.Unreachable();
    }

    // fail(msg, len, file, filelen, line): upstream's gs_fail
    // (runtime_impl.h) for a check with a location: "goose runtime error: ",
    // the message, " (file:line)" and a newline, written to stderr at once,
    // then exit code 1. `lead` and `sep` are the fixed texts' addresses in
    // static data, which the backend places.
    void DefineFail(Module &m, uint32_t lead, uint32_t leadlen, uint32_t paren) {
        fail = m.AddFunc({ { V::I32, V::I32, V::I32, V::I32, V::I32 }, {} }, { V::I32 });
        auto &c = m.FuncAt(fail).code;
        enum { msg, len, file, filelen, line, p };
        auto put = [&](auto addr, auto n) {
            c.LocalGet(p);
            addr();
            n();
            c.Call(append);
            c.LocalSet(p);
        };
        c.I32Const(mem0::LineBuf);
        c.LocalSet(p);
        put([&] { c.I32Const(int32_t(lead)); }, [&] { c.I32Const(int32_t(leadlen)); });
        put([&] { c.LocalGet(msg); }, [&] { c.LocalGet(len); });
        put([&] { c.I32Const(int32_t(paren)); }, [&] { c.I32Const(2); });  // " ("
        put([&] { c.LocalGet(file); }, [&] { c.LocalGet(filelen); });
        c.LocalGet(p);
        c.I32Const(':');
        c.I32Store8();
        c.LocalGet(line);
        c.I64ExtendI32U();
        c.LocalGet(p);
        c.I32Const(1);
        c.I32Add();
        c.Call(fmt_u64);
        c.LocalTee(p);
        c.I32Const(')' | '\n' << 8);
        c.Op(0x3b);  // i32.store16
        c.MemArg(0, 0, 0);
        // A contained worker keeps the message, without the lead and the
        // newline (upstream gs_fail).
        if (threaded) {
            c.GlobalGet(contained);
            c.If();
            c.I32Const(int32_t(mem0::LineBuf + leadlen));
            c.LocalGet(p);
            c.I32Const(1 - int32_t(mem0::LineBuf + leadlen));
            c.I32Add();
            c.Call(contain);
            c.End();
        }
        c.I32Const(2);  // stderr
        c.I32Const(mem0::LineBuf);
        c.LocalGet(p);
        c.I32Const(2);
        c.I32Add();
        c.I32Const(mem0::LineBuf);
        c.I32Sub();
        c.Call(write);
        c.I32Const(1);
        c.Call(proc_exit);
        c.Unreachable();
    }
};

}  // namespace goosewasm
