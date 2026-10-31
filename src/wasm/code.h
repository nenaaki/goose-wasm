// Emitting instructions (WebAssembly Core Specification 3.0, 5.4) into a
// function body. Only the instructions the backend uses are here; each is
// named after its text-format mnemonic.
#pragma once

#include "binary.h"

namespace goosewasm {

// The block type of a structured instruction (spec 5.4.1): empty, or one
// value type.
enum class BlockType : uint8_t { Empty = 0x40, I32 = 0x7f, I64 = 0x7e };

// The opcodes of the integer instructions that come in an i32 and an i64
// form (spec 5.4.8), indexed by width: [0] for i32, [1] for i64.
namespace intop {
constexpr uint8_t Eqz[2] = { 0x45, 0x50 }, Eq[2] = { 0x46, 0x51 }, Ne[2] = { 0x47, 0x52 },
    LtS[2] = { 0x48, 0x53 }, LtU[2] = { 0x49, 0x54 }, GtS[2] = { 0x4a, 0x55 },
    GtU[2] = { 0x4b, 0x56 }, LeS[2] = { 0x4c, 0x57 }, LeU[2] = { 0x4d, 0x58 },
    GeS[2] = { 0x4e, 0x59 }, GeU[2] = { 0x4f, 0x5a }, Add[2] = { 0x6a, 0x7c },
    Sub[2] = { 0x6b, 0x7d }, Mul[2] = { 0x6c, 0x7e }, DivS[2] = { 0x6d, 0x7f },
    DivU[2] = { 0x6e, 0x80 }, RemS[2] = { 0x6f, 0x81 }, RemU[2] = { 0x70, 0x82 },
    And[2] = { 0x71, 0x83 }, Or[2] = { 0x72, 0x84 }, Xor[2] = { 0x73, 0x85 },
    Shl[2] = { 0x74, 0x86 }, ShrS[2] = { 0x75, 0x87 }, ShrU[2] = { 0x76, 0x88 };
}  // namespace intop

struct Code {
    Bytes b;

    void Op(uint8_t op) { b.Byte(op); }
    // An instruction under the 0xFC prefix (spec 5.4.6, 5.4.7).
    void OpFC(uint32_t sub) { b.Byte(0xfc); b.U(sub); }

    // A memory argument (spec 5.4.7). With multi-memory, bit 6 of the
    // alignment field says a memory index follows; memory 0 omits it.
    void MemArg(uint32_t alignlog2, uint64_t offset, uint32_t mem) {
        if (mem == 0) {
            b.U(alignlog2);
        } else {
            b.U(alignlog2 | 0x40);
            b.U(mem);
        }
        b.U(offset);
    }

    // Control (spec 5.4.1).
    void Unreachable() { Op(0x00); }
    void Block(BlockType t = BlockType::Empty) { Op(0x02); b.Byte(uint8_t(t)); }
    void Loop(BlockType t = BlockType::Empty) { Op(0x03); b.Byte(uint8_t(t)); }
    void If(BlockType t = BlockType::Empty) { Op(0x04); b.Byte(uint8_t(t)); }
    void Else() { Op(0x05); }
    void End() { Op(0x0b); }
    void Br(uint32_t depth) { Op(0x0c); b.U(depth); }
    void BrIf(uint32_t depth) { Op(0x0d); b.U(depth); }
    void Return() { Op(0x0f); }
    void Call(uint32_t func) { Op(0x10); b.U(func); }

    // Parametric (spec 5.4.3).
    void Drop() { Op(0x1a); }

    // Variables (spec 5.4.4).
    void LocalGet(uint32_t i) { Op(0x20); b.U(i); }
    void LocalSet(uint32_t i) { Op(0x21); b.U(i); }
    void LocalTee(uint32_t i) { Op(0x22); b.U(i); }

    // Memory (spec 5.4.7).
    void I32Load(uint64_t off = 0, uint32_t mem = 0) { Op(0x28); MemArg(2, off, mem); }
    void I64Load(uint64_t off = 0, uint32_t mem = 0) { Op(0x29); MemArg(3, off, mem); }
    void I32Store(uint64_t off = 0, uint32_t mem = 0) { Op(0x36); MemArg(2, off, mem); }
    void I64Store(uint64_t off = 0, uint32_t mem = 0) { Op(0x37); MemArg(3, off, mem); }
    void I32Store8(uint64_t off = 0, uint32_t mem = 0) { Op(0x3a); MemArg(0, off, mem); }
    void MemoryCopy(uint32_t dst = 0, uint32_t src = 0) { OpFC(10); b.U(dst); b.U(src); }

    // Numeric (spec 5.4.8).
    void I32Const(int32_t v) { Op(0x41); b.S(v); }
    void I64Const(int64_t v) { Op(0x42); b.S(v); }
    void I32Eqz() { Op(0x45); }
    void I32Ne() { Op(0x47); }
    void I32LtU() { Op(0x49); }
    void I64Eqz() { Op(0x50); }
    void I64Ne() { Op(0x52); }
    void I64LtS() { Op(0x53); }
    void I32Add() { Op(0x6a); }
    void I32Sub() { Op(0x6b); }
    void I64Sub() { Op(0x7d); }
    void I64DivU() { Op(0x80); }
    void I64RemU() { Op(0x82); }
    void I32WrapI64() { Op(0xa7); }
    void I64ExtendI32S() { Op(0xac); }
    void I64ExtendI32U() { Op(0xad); }
    void I32Extend8S() { Op(0xc0); }
    void I32Extend16S() { Op(0xc1); }
    void Select() { Op(0x1b); }
    void I32And() { Op(0x71); }
    // The i32 or i64 form of an integer instruction (intop).
    void Int(const uint8_t (&op)[2], bool wide) { Op(op[wide]); }
    void IntConst(bool wide, int64_t v) {
        if (wide) I64Const(v);
        else I32Const(int32_t(v));
    }
};

}  // namespace goosewasm
