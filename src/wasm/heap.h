// A heap of blocks in one memory: what upstream mallocs, here in the
// shared memory for spawn packets, queue nodes and abort messages
// (threads.h), and in memory 0 for what a host hands a module built with
// --header (backend.h, exports).
//
// A block is of the smallest size class whose 16 << c bytes hold the request
// after the block's 8-byte header, from the class's free list, else from the
// heap's end, growing the memory. Blocks go back on their class's list and
// the memory never shrinks.
#pragma once

#include "code.h"

namespace goosewasm {

// alloc(n i32) -> p i32, with locals cls i32, p i32, size i32, end i64: a
// block of n bytes at least, 0 when there is no room; the free lists are
// `classes` i32 words at `freebase` and the heap's end the i32 at `brk`, in
// memory `mem`.
inline void EmitAlloc(Code &c, uint32_t mem, uint32_t freebase, uint32_t brk, uint32_t classes) {
    enum { n, cls, p, size, end };
    c.I32Const(0);
    c.LocalSet(cls);
    c.Block();
    c.Loop();
    c.I64Const(16);
    c.LocalGet(cls);
    c.I64ExtendI32U();
    c.Int(intop::Shl, true);
    c.I64Const(8);
    c.I64Sub();
    c.LocalGet(n);
    c.I64ExtendI32U();
    c.Int(intop::GeU, true);
    c.BrIf(1);
    c.LocalGet(cls);
    c.I32Const(1);
    c.I32Add();
    c.LocalTee(cls);
    c.I32Const(int32_t(classes));
    c.Int(intop::GeU, false);
    c.If();
    c.I32Const(0);
    c.Return();
    c.End();
    c.Br(0);
    c.End();
    c.End();
    // From the free list.
    c.LocalGet(cls);
    c.I32Const(2);
    c.Int(intop::Shl, false);
    c.I32Load(freebase, mem);
    c.LocalTee(p);
    c.If();
    c.LocalGet(cls);
    c.I32Const(2);
    c.Int(intop::Shl, false);
    c.LocalGet(p);
    c.I32Load(4, mem);
    c.I32Store(freebase, mem);
    c.LocalGet(p);
    c.I32Const(8);
    c.I32Add();
    c.Return();
    c.End();
    // From the heap's end, below 4 GB.
    c.I32Const(16);
    c.LocalGet(cls);
    c.Int(intop::Shl, false);
    c.LocalSet(size);
    c.I32Const(0);
    c.I32Load(brk, mem);
    c.LocalTee(p);
    c.I64ExtendI32U();
    c.LocalGet(size);
    c.I64ExtendI32U();
    c.Int(intop::Add, true);
    c.LocalTee(end);
    c.I64Const(int64_t(1) << 32);
    c.Int(intop::GeU, true);
    c.If();
    c.I32Const(0);
    c.Return();
    c.End();
    c.LocalGet(end);
    c.MemorySize(mem);
    c.I64ExtendI32U();
    c.I64Const(16);
    c.Int(intop::Shl, true);
    c.Int(intop::GtU, true);
    c.If();
    c.LocalGet(end);
    c.I64Const(0xffff);
    c.Int(intop::Add, true);
    c.I64Const(16);
    c.Int(intop::ShrU, true);
    c.I32WrapI64();
    c.MemorySize(mem);
    c.I32Sub();
    c.MemoryGrow(mem);
    c.I32Const(-1);
    c.Int(intop::Eq, false);
    c.If();
    c.I32Const(0);
    c.Return();
    c.End();
    c.End();
    c.I32Const(0);
    c.LocalGet(end);
    c.I32WrapI64();
    c.I32Store(brk, mem);
    c.LocalGet(p);
    c.LocalGet(cls);
    c.I32Store(0, mem);
    c.LocalGet(p);
    c.I32Const(8);
    c.I32Add();
}

// release(p i32), with a local slot i32: p's block onto its class's free
// list; null is nothing.
inline void EmitRelease(Code &c, uint32_t mem, uint32_t freebase) {
    enum { p, slot };
    c.LocalGet(p);
    c.I32Eqz();
    c.If();
    c.Return();
    c.End();
    c.LocalGet(p);
    c.I32Const(8);
    c.I32Sub();
    c.LocalTee(p);
    c.I32Load(0, mem);
    c.I32Const(2);
    c.Int(intop::Shl, false);
    c.I32Const(int32_t(freebase));
    c.I32Add();
    c.LocalSet(slot);
    c.LocalGet(p);
    c.LocalGet(slot);
    c.I32Load(0, mem);
    c.I32Store(4, mem);
    c.LocalGet(slot);
    c.LocalGet(p);
    c.I32Store(0, mem);
}

}  // namespace goosewasm
