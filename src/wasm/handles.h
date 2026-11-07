// Memory access through handles, and the data stacks' growth
// (docs/design/memory-model.md, value-representation.md).
//
// A handle is an i64: the memory index in the upper 32 bits, the address
// within that memory in the lower 32. Memory 0 is the program's own
// (static data, globals, the shadow stack); memory k + 1 is data stack k. A
// Wasm load or store names its memory as an immediate, so an access through
// a handle whose memory is not known when it is emitted calls a helper here
// that dispatches on the index with br_table.
//
// The helpers' bodies depend on how many memories the module has, which is
// known only once every function is emitted: Reserve gives them their
// function indices first, Define their bodies at the end.
#pragma once

#include <map>

#include "runtime.h"

namespace goosewasm {

struct Handles {
    using V = ValType;

    // Load and store helpers by opcode: load (h i64) -> value, store (h i64,
    // value).
    std::map<uint8_t, uint32_t> loads, stores;
    uint32_t copy = 0;      // (dst i64, src i64, n i32)
    uint32_t fill = 0;      // (dst i64, byte i32, n i32)
    uint32_t pushslot = 0;  // (k i32, n i32) -> i64: n bytes claimed at the top of stack k
    uint32_t grow = 0;      // (k i32, need i64): stack k's memory made at least need bytes
    std::string overflow;   // the overflow message, placed by the backend
    uint32_t overflowat = 0;

    // The value type a load opcode produces or a store opcode takes (spec
    // 5.4.7): i64.load/store, f32 and f64 ones, else i32.
    static ValType LoadType(uint8_t op) {
        switch (op) {
            case 0x29: return V::I64;
            case 0x2a: return V::F32;
            case 0x2b: return V::F64;
            default: return V::I32;
        }
    }
    static ValType StoreType(uint8_t op) {
        switch (op) {
            case 0x37: return V::I64;
            case 0x38: return V::F32;
            case 0x39: return V::F64;
            default: return V::I32;
        }
    }

    uint32_t Load(Module &m, uint8_t op) {
        auto it = loads.find(op);
        if (it != loads.end()) return it->second;
        return loads[op] = m.AddFunc({ { V::I64 }, { LoadType(op) } }, { V::I32 });
    }
    uint32_t Store(Module &m, uint8_t op) {
        auto it = stores.find(op);
        if (it != stores.end()) return it->second;
        return stores[op] = m.AddFunc({ { V::I64, StoreType(op) }, {} }, { V::I32 });
    }
    void Reserve(Module &m) {
        copy = m.AddFunc({ { V::I64, V::I64, V::I32 }, {} });
        fill = m.AddFunc({ { V::I64, V::I32, V::I32 }, {} });
        pushslot = m.AddFunc({ { V::I32, V::I32 }, { V::I64 } }, { V::I32 });
        grow = m.AddFunc({ { V::I32, V::I64 }, {} }, { V::I64, V::I64, V::I64 });
    }

    // A br_table over `n` cases on the i32 that `sel` pushes; case k runs
    // body(k), which leaves nothing on the stack and does not fall out. An
    // index past the cases traps.
    template <typename S, typename B> static void Dispatch(Code &c, uint32_t n, S sel, B body) {
        for (uint32_t k = 0; k <= n; k++) c.Block();
        sel();
        c.Op(0x0e);  // br_table
        c.b.U(n);
        for (uint32_t k = 0; k < n; k++) c.b.U(k);
        c.b.U(n);
        for (uint32_t k = 0; k < n; k++) {
            c.End();
            body(k);
        }
        c.End();
        c.Unreachable();
    }

    // The memory index and address of the handle in local `h`.
    static void Index(Code &c, uint32_t h) {
        c.LocalGet(h);
        c.I64Const(32);
        c.Op(0x88);  // i64.shr_u
        c.I32WrapI64();
    }
    static void Addr(Code &c, uint32_t h) {
        c.LocalGet(h);
        c.I32WrapI64();
    }

    void Define(Module &m, Runtime &rt, uint32_t nmem) {
        for (auto [op, f] : loads) {
            auto &c = m.FuncAt(f).code;
            Dispatch(c, nmem, [&] { Index(c, 0); }, [&](uint32_t k) {
                Addr(c, 0);
                c.LoadOp(op, 0, k);
                c.Return();
            });
        }
        for (auto [op, f] : stores) {
            auto &c = m.FuncAt(f).code;
            Dispatch(c, nmem, [&] { Index(c, 0); }, [&](uint32_t k) {
                Addr(c, 0);
                c.LocalGet(1);
                c.StoreOp(op, 0, k);
                c.Return();
            });
        }
        // copy(dst, src, n): one case per pair of memories, as memory.copy
        // names both.
        {
            auto &c = m.FuncAt(copy).code;
            Dispatch(c, nmem * nmem, [&] {
                Index(c, 0);
                c.I32Const(int32_t(nmem));
                c.Op(0x6c);  // i32.mul
                Index(c, 1);
                c.I32Add();
            }, [&](uint32_t k) {
                Addr(c, 0);
                Addr(c, 1);
                c.LocalGet(2);
                c.MemoryCopy(k / nmem, k % nmem);
                c.Return();
            });
        }
        // fill(dst, byte, n): memory.fill in the handle's memory.
        {
            auto &c = m.FuncAt(fill).code;
            Dispatch(c, nmem, [&] { Index(c, 0); }, [&](uint32_t k) {
                Addr(c, 0);
                c.LocalGet(1);
                c.LocalGet(2);
                c.MemoryFill(k);
                c.Return();
            });
        }
        // pushslot(k, n): n bytes at the top of stack k, its memory grown
        // first if they do not fit; returns their handle.
        {
            auto &c = m.FuncAt(pushslot).code;
            enum { k, n, top };
            c.LocalGet(k);
            c.I32Const(16);
            c.Op(0x6c);  // i32.mul
            c.I32Load(mem0::Tops);
            c.LocalTee(top);
            c.I64ExtendI32U();
            c.LocalGet(n);
            c.I64ExtendI32U();
            c.Int(intop::Add, true);
            c.LocalGet(k);
            c.I32Const(16);
            c.Op(0x6c);
            c.I64Load(mem0::Tops + 8);
            c.Int(intop::GtU, true);
            c.If();
            c.LocalGet(k);
            c.LocalGet(top);
            c.I64ExtendI32U();
            c.LocalGet(n);
            c.I64ExtendI32U();
            c.Int(intop::Add, true);
            c.Call(grow);
            c.End();
            c.LocalGet(k);
            c.I32Const(16);
            c.Op(0x6c);
            c.LocalGet(top);
            c.LocalGet(n);
            c.I32Add();
            c.I32Store(mem0::Tops);
            c.LocalGet(k);
            c.I32Const(1);
            c.I32Add();
            c.I64ExtendI32U();
            c.I64Const(32);
            c.Int(intop::Shl, true);
            c.LocalGet(top);
            c.I64ExtendI32U();
            c.Int(intop::Or, true);
        }
        // grow(k, need): at least doubles stack k's memory, and to `need`
        // bytes, up to the 4 GB a 32-bit memory can hold; past that, the
        // data stack overflow upstream reports when a stack outgrows its
        // reservation (gs_stack_overflow).
        {
            auto &c = m.FuncAt(grow).code;
            enum { k, need, cur, want, delta };
            constexpr int64_t maxpages = 65536;
            c.LocalGet(need);
            c.I64Const(65535);
            c.Int(intop::Add, true);
            c.I64Const(16);
            c.Op(0x88);  // i64.shr_u
            c.LocalTee(want);
            c.I64Const(maxpages);
            c.Int(intop::GtU, true);
            c.If();
            c.I32Const(int32_t(overflowat));
            c.I32Const(int32_t(overflow.size()));
            c.Call(rt.panic);
            c.End();
            // Stack j is memory j + 1.
            Dispatch(c, nmem - 1, [&] { c.LocalGet(k); }, [&](uint32_t j) {
                auto mem = j + 1;
                auto memgrow = [&] {
                    c.Op(0x40);  // memory.grow
                    c.b.U(mem);
                };
                c.Op(0x3f);  // memory.size
                c.b.U(mem);
                c.I64ExtendI32U();
                c.LocalSet(cur);
                // delta: what `need` asks, at least the current size
                // (doubling), at most what is left of 4 GB.
                c.LocalGet(want);
                c.LocalGet(cur);
                c.Int(intop::Sub, true);
                c.LocalSet(delta);
                c.LocalGet(cur);
                c.LocalGet(delta);
                c.Int(intop::GtU, true);
                c.If();
                c.LocalGet(cur);
                c.LocalSet(delta);
                c.End();
                c.LocalGet(cur);
                c.LocalGet(delta);
                c.Int(intop::Add, true);
                c.I64Const(maxpages);
                c.Int(intop::GtU, true);
                c.If();
                c.I64Const(maxpages);
                c.LocalGet(cur);
                c.Int(intop::Sub, true);
                c.LocalSet(delta);
                c.End();
                // If the doubled size is refused, the exact one; if that is
                // too, the overflow.
                c.LocalGet(delta);
                c.I32WrapI64();
                memgrow();
                c.I32Const(-1);
                c.Int(intop::Eq, false);
                c.If();
                c.LocalGet(want);
                c.LocalGet(cur);
                c.Int(intop::Sub, true);
                c.LocalTee(delta);
                c.I32WrapI64();
                memgrow();
                c.I32Const(-1);
                c.Int(intop::Eq, false);
                c.If();
                c.I32Const(int32_t(overflowat));
                c.I32Const(int32_t(overflow.size()));
                c.Call(rt.panic);
                c.End();
                c.End();
                // The stack's new size in bytes.
                c.LocalGet(k);
                c.I32Const(16);
                c.Op(0x6c);  // i32.mul
                c.LocalGet(cur);
                c.LocalGet(delta);
                c.Int(intop::Add, true);
                c.I64Const(16);
                c.Int(intop::Shl, true);
                c.I64Store(mem0::Tops + 8);
                c.Return();
            });
        }
    }
};

}  // namespace goosewasm
