// A float's text, as upstream's runtime spells it: the fewest significant
// digits that read back as the same value of its own type, the nearest of
// those, a tie going to the even digit, laid out as C's %g at a precision of
// max(15, digits), with a whole number ending in ".0" and inf, -inf and nan
// spelled out (spec §3.7).
//
// Derived from upstream Goose, src/runtime/runtime_impl.h at commit
// 2ed1a63dde2ddff777fbb7f4790412b4f00cd47d: gs_big_set, gs_big_mul,
// gs_big_mul_pow10, gs_big_cmp, gs_big_add, gs_big_sub, gs_float_digits,
// gs_fmt_float, gs_fmt_f64 and gs_fmt_f32. Original work: Goose,
// https://github.com/aardappel/goose, by Wouter van Oortmerssen, licensed
// under the Apache License, Version 2.0 (upstream/goose/LICENSE).
// Changed: restated as WebAssembly instructions, each function after the C
// one of the same name, whose lines the comments quote; the naturals live in
// a fixed scratch area of memory 0 rather than in C locals, which the
// formatter, calling nothing else, never needs two of at once.
#pragma once

#include <cmath>

#include "runtime.h"

namespace goosewasm {

struct FloatFmt {
    using V = ValType;

    // gs_big { int n; uint32_t d[36]; } at an address: n at 0, d[i] at 4 + 4i.
    static constexpr uint32_t BigSize = 4 + 36 * 4;
    static constexpr uint32_t R = mem0::FloatScratch, S = R + BigSize, MP = S + BigSize,
                              MLO = MP + BigSize, T = MLO + BigSize, Dig = T + BigSize,
                              KOut = Dig + 24;
    static_assert(KOut + 4 <= mem0::FloatScratch + mem0::FloatScratchSize);

    uint32_t big_set = 0, big_mul = 0, big_mul_pow10 = 0, big_cmp = 0, big_add = 0, big_sub = 0;
    uint32_t float_digits = 0, fmt_float = 0, fmt_f64 = 0, fmt_f32 = 0;

    // Code helpers over a function's locals.
    struct E {
        Code &c;
        void get(uint32_t l) { c.LocalGet(l); }
        void set(uint32_t l) { c.LocalSet(l); }
        void i32(int32_t v) { c.I32Const(v); }
        void i64(int64_t v) { c.I64Const(v); }
        // big->n and big->d[i] for a big whose address is in local a.
        void n(uint32_t a) { get(a); c.I32Load(0); }
        void setn(uint32_t a, auto value) { get(a); value(); c.I32Store(0); }
        void d(uint32_t a, auto index) { addr(a, index); c.I32Load(4); }
        void setd(uint32_t a, auto index, auto value) { addr(a, index); value(); c.I32Store(4); }
        void addr(uint32_t a, auto index) {
            get(a);
            index();
            i32(4);
            c.Op(0x6c);  // i32.mul
            c.I32Add();
        }
        void inc(uint32_t l, int32_t by = 1) { get(l); i32(by); c.I32Add(); set(l); }
        // *p++ = byte, for p a local address in memory 0.
        void put(uint32_t p, auto byte) { get(p); byte(); c.I32Store8(); inc(p); }
    };

    void Define(Module &m, uint32_t nanat, uint32_t infat, uint32_t ninfat) {
        // gs_big_set(a, v, sh): a = v << sh, for v > 0.
        {
            big_set = m.AddFunc({ { V::I32, V::I64, V::I32 }, {} }, { V::I32, V::I32, V::I32, V::I64, V::I64 });
            E e { m.FuncAt(big_set).code };
            auto &c = e.c;
            enum { a, v, sh, w, b, i, lo, hi };
            e.get(sh); e.i32(5); c.Op(0x76); e.set(w);   // int w = sh >> 5 (sh >= 0)
            e.get(sh); e.i32(31); c.I32And(); e.set(b);  // b = sh & 31
            // uint64_t lo = v << b, hi = b ? v >> (64 - b) : 0;
            e.get(v); e.get(b); c.I64ExtendI32U(); c.Int(intop::Shl, true); e.set(lo);
            e.get(v); e.i64(64); e.get(b); c.I64ExtendI32U(); c.I64Sub(); c.Int(intop::ShrU, true);
            e.i64(0); e.get(b); c.Select(); e.set(hi);
            // for (i = 0; i < w; i++) a->d[i] = 0;
            e.i32(0); e.set(i);
            c.Block(); c.Loop();
            e.get(i); e.get(w); c.Int(intop::GeS, false); c.BrIf(1);
            e.setd(a, [&] { e.get(i); }, [&] { e.i32(0); });
            e.inc(i); c.Br(0);
            c.End(); c.End();
            // a->d[w] = (uint32_t)lo; a->d[w + 1] = lo >> 32; a->d[w + 2] = hi;
            e.setd(a, [&] { e.get(w); }, [&] { e.get(lo); c.I32WrapI64(); });
            e.setd(a, [&] { e.get(w); e.i32(1); c.I32Add(); },
                   [&] { e.get(lo); e.i64(32); c.Int(intop::ShrU, true); c.I32WrapI64(); });
            e.setd(a, [&] { e.get(w); e.i32(2); c.I32Add(); }, [&] { e.get(hi); c.I32WrapI64(); });
            // a->n = w + 3; while (!a->d[a->n - 1]) a->n--;
            e.setn(a, [&] { e.get(w); e.i32(3); c.I32Add(); });
            c.Block(); c.Loop();
            e.d(a, [&] { e.n(a); e.i32(1); c.I32Sub(); });
            c.BrIf(1);
            e.setn(a, [&] { e.n(a); e.i32(1); c.I32Sub(); });
            c.Br(0);
            c.End(); c.End();
        }
        // gs_big_mul(a, m): a *= m.
        {
            big_mul = m.AddFunc({ { V::I32, V::I32 }, {} }, { V::I64, V::I32 });
            E e { m.FuncAt(big_mul).code };
            auto &c = e.c;
            enum { a, mm, cy, i };
            c.Block(); c.Loop();
            e.get(i); e.n(a); c.Int(intop::GeS, false); c.BrIf(1);
            // c += (uint64_t)a->d[i] * m; a->d[i] = (uint32_t)c; c >>= 32;
            e.get(cy);
            e.d(a, [&] { e.get(i); }); c.I64ExtendI32U();
            e.get(mm); c.I64ExtendI32U();
            c.Int(intop::Mul, true);
            c.Int(intop::Add, true);
            e.set(cy);
            e.setd(a, [&] { e.get(i); }, [&] { e.get(cy); c.I32WrapI64(); });
            e.get(cy); e.i64(32); c.Int(intop::ShrU, true); e.set(cy);
            e.inc(i); c.Br(0);
            c.End(); c.End();
            // if (c) a->d[a->n++] = (uint32_t)c;
            e.get(cy); c.I64Eqz(); c.I32Eqz();
            c.If();
            e.setd(a, [&] { e.n(a); }, [&] { e.get(cy); c.I32WrapI64(); });
            e.setn(a, [&] { e.n(a); e.i32(1); c.I32Add(); });
            c.End();
        }
        // gs_big_mul_pow10(a, k): a *= 10^k.
        {
            big_mul_pow10 = m.AddFunc({ { V::I32, V::I32 }, {} });
            E e { m.FuncAt(big_mul_pow10).code };
            auto &c = e.c;
            enum { a, k };
            for (auto [step, factor] : { std::pair { 9, 1000000000 }, { 1, 10 } }) {
                // for (; k >= step; k -= step) gs_big_mul(a, factor);
                c.Block(); c.Loop();
                e.get(k); e.i32(step); c.Int(intop::LtS, false); c.BrIf(1);
                e.get(a); e.i32(factor); c.Call(big_mul);
                e.get(k); e.i32(step); c.I32Sub(); e.set(k);
                c.Br(0);
                c.End(); c.End();
            }
        }
        // gs_big_cmp(a, b): -1, 0 or 1.
        {
            big_cmp = m.AddFunc({ { V::I32, V::I32 }, { V::I32 } }, { V::I32 });
            E e { m.FuncAt(big_cmp).code };
            auto &c = e.c;
            enum { a, b, i };
            // if (a->n != b->n) return a->n < b->n ? -1 : 1;
            e.n(a); e.n(b); c.I32Ne();
            c.If();
            e.i32(-1); e.i32(1); e.n(a); e.n(b); c.Int(intop::LtS, false); c.Select(); c.Return();
            c.End();
            // for (i = a->n - 1; i >= 0; i--) if (a->d[i] != b->d[i]) return ... ;
            e.n(a); e.i32(1); c.I32Sub(); e.set(i);
            c.Block(); c.Loop();
            e.get(i); e.i32(0); c.Int(intop::LtS, false); c.BrIf(1);
            e.d(a, [&] { e.get(i); }); e.d(b, [&] { e.get(i); }); c.I32Ne();
            c.If();
            e.i32(-1); e.i32(1);
            e.d(a, [&] { e.get(i); }); e.d(b, [&] { e.get(i); }); c.I32LtU();
            c.Select(); c.Return();
            c.End();
            e.inc(i, -1); c.Br(0);
            c.End(); c.End();
            e.i32(0);
        }
        // gs_big_add(t, a, b): t = a + b.
        {
            big_add = m.AddFunc({ { V::I32, V::I32, V::I32 }, {} }, { V::I32, V::I64, V::I32 });
            E e { m.FuncAt(big_add).code };
            auto &c = e.c;
            enum { t, a, b, nn, cy, i };
            // int n = a->n > b->n ? a->n : b->n;
            e.n(a); e.n(b); e.n(a); e.n(b); c.Int(intop::GtS, false); c.Select(); e.set(nn);
            auto limb = [&](uint32_t x) {
                // (i < x->n ? x->d[i] : 0)
                e.get(i); e.n(x); c.Int(intop::LtS, false);
                c.If(BlockType::I64);
                e.d(x, [&] { e.get(i); }); c.I64ExtendI32U();
                c.Else();
                e.i64(0);
                c.End();
            };
            c.Block(); c.Loop();
            e.get(i); e.get(nn); c.Int(intop::GeS, false); c.BrIf(1);
            e.get(cy); limb(a); c.Int(intop::Add, true); limb(b); c.Int(intop::Add, true); e.set(cy);
            e.setd(t, [&] { e.get(i); }, [&] { e.get(cy); c.I32WrapI64(); });
            e.get(cy); e.i64(32); c.Int(intop::ShrU, true); e.set(cy);
            e.inc(i); c.Br(0);
            c.End(); c.End();
            e.setn(t, [&] { e.get(nn); });
            e.get(cy); c.I64Eqz(); c.I32Eqz();
            c.If();
            e.setd(t, [&] { e.n(t); }, [&] { e.get(cy); c.I32WrapI64(); });
            e.setn(t, [&] { e.n(t); e.i32(1); c.I32Add(); });
            c.End();
        }
        // gs_big_sub(a, b): a -= b, for a >= b.
        {
            big_sub = m.AddFunc({ { V::I32, V::I32 }, {} }, { V::I64, V::I32, V::I64 });
            E e { m.FuncAt(big_sub).code };
            auto &c = e.c;
            enum { a, b, borrow, i, x };
            c.Block(); c.Loop();
            e.get(i); e.n(a); c.Int(intop::GeS, false); c.BrIf(1);
            // x = (uint64_t)a->d[i] - (i < b->n ? b->d[i] : 0) - borrow;
            e.d(a, [&] { e.get(i); }); c.I64ExtendI32U();
            e.get(i); e.n(b); c.Int(intop::LtS, false);
            c.If(BlockType::I64);
            e.d(b, [&] { e.get(i); }); c.I64ExtendI32U();
            c.Else();
            e.i64(0);
            c.End();
            c.I64Sub();
            e.get(borrow); c.I64Sub();
            e.set(x);
            e.setd(a, [&] { e.get(i); }, [&] { e.get(x); c.I32WrapI64(); });
            e.get(x); e.i64(63); c.Int(intop::ShrU, true); e.set(borrow);
            e.inc(i); c.Br(0);
            c.End(); c.End();
            // while (a->n && !a->d[a->n - 1]) a->n--;
            c.Block(); c.Loop();
            e.n(a); c.I32Eqz(); c.BrIf(1);
            e.d(a, [&] { e.n(a); e.i32(1); c.I32Sub(); }); c.BrIf(1);
            e.setn(a, [&] { e.n(a); e.i32(1); c.I32Sub(); });
            c.Br(0);
            c.End(); c.End();
        }
        // gs_float_digits(f, e, bits, emin): the shortest digits of f * 2^e
        // (f > 0) into Dig, their count returned, k at KOut, so that they
        // spell 0.d1d2... * 10^k.
        {
            float_digits = m.AddFunc({ { V::I64, V::I32, V::I32, V::I32 }, { V::I32 } },
                                     { V::I32, V::I32, V::I32, V::I32, V::I32, V::I32, V::I64,
                                       V::I32, V::I32, V::I32, V::I32, V::I32 });
            E e { m.FuncAt(float_digits).code };
            auto &c = e.c;
            enum { f, ex, bits, emin, even, asym, up, lg, nn, cc, g, k, mm, d, low, high };
            auto big = [&](uint32_t addr) { e.i32(int32_t(addr)); };
            // int even = !(f & 1);
            e.get(f); e.i64(1); c.Int(intop::And, true); c.I64Eqz(); e.set(even);
            // int asym = f == (uint64_t)1 << (bits - 1) && e > emin;
            e.get(f); e.i64(1); e.get(bits); e.i32(1); c.I32Sub(); c.I64ExtendI32U();
            c.Int(intop::Shl, true); c.Int(intop::Eq, true);
            e.get(ex); e.get(emin); c.Int(intop::GtS, false);
            c.I32And(); e.set(asym);
            // int up = e > 0 ? e : 0, lg = e - 1, n = 0;
            e.get(ex); e.i32(0); e.get(ex); e.i32(0); c.Int(intop::GtS, false); c.Select(); e.set(up);
            e.get(ex); e.i32(1); c.I32Sub(); e.set(lg);
            // gs_big *mm = asym ? &mlo : &mp;
            big(MLO); big(MP); e.get(asym); c.Select(); e.set(mm);
            // gs_big_set(&r, f, up + 1 + asym); (&s, 1, up - e + 1 + asym);
            // (&mp, 1, up + asym); (&mlo, 1, up);
            big(R); e.get(f); e.get(up); e.i32(1); c.I32Add(); e.get(asym); c.I32Add(); c.Call(big_set);
            big(S); e.i64(1); e.get(up); e.get(ex); c.I32Sub(); e.i32(1); c.I32Add(); e.get(asym);
            c.I32Add(); c.Call(big_set);
            big(MP); e.i64(1); e.get(up); e.get(asym); c.I32Add(); c.Call(big_set);
            big(MLO); e.i64(1); e.get(up); c.Call(big_set);
            // for (uint64_t g = f; g; g >>= 1) lg++;
            e.get(f); e.set(g);
            c.Block(); c.Loop();
            e.get(g); c.I64Eqz(); c.BrIf(1);
            e.inc(lg);
            e.get(g); e.i64(1); c.Int(intop::ShrU, true); e.set(g);
            c.Br(0);
            c.End(); c.End();
            // *k = lg > 0 ? ((lg * 78913) >> 18) + 1 : -((-lg * 78913) >> 18);
            e.get(lg); e.i32(0); c.Int(intop::GtS, false);
            c.If(BlockType::I32);
            e.get(lg); e.i32(78913); c.Op(0x6c); e.i32(18); c.Op(0x75); e.i32(1); c.I32Add();
            c.Else();
            e.i32(0);
            e.i32(0); e.get(lg); c.I32Sub(); e.i32(78913); c.Op(0x6c); e.i32(18); c.Op(0x75);
            c.I32Sub();
            c.End();
            e.set(k);
            // if (*k >= 0) gs_big_mul_pow10(&s, *k); else { r, mp, and mlo if asym, by -*k }
            e.get(k); e.i32(0); c.Int(intop::GeS, false);
            c.If();
            big(S); e.get(k); c.Call(big_mul_pow10);
            c.Else();
            big(R); e.i32(0); e.get(k); c.I32Sub(); c.Call(big_mul_pow10);
            big(MP); e.i32(0); e.get(k); c.I32Sub(); c.Call(big_mul_pow10);
            e.get(asym);
            c.If();
            big(MLO); e.i32(0); e.get(k); c.I32Sub(); c.Call(big_mul_pow10);
            c.End();
            c.End();
            // gs_big_add(&t, &r, &mp); c = gs_big_cmp(&t, &s);
            // if (even ? c >= 0 : c > 0) { gs_big_mul(&s, 10); ++*k; }
            auto reaches = [&](uint32_t out) {
                // even ? c >= 0 : c > 0, from cc
                e.get(cc); e.i32(0); c.Int(intop::GeS, false);
                e.get(cc); e.i32(0); c.Int(intop::GtS, false);
                e.get(even); c.Select();
                e.set(out);
            };
            big(T); big(R); big(MP); c.Call(big_add);
            big(T); big(S); c.Call(big_cmp); e.set(cc);
            reaches(high);
            e.get(high);
            c.If();
            big(S); e.i32(10); c.Call(big_mul);
            e.inc(k);
            c.End();
            // for (;;) { ... }
            c.Loop();
            e.i32(0); e.set(d);
            big(R); e.i32(10); c.Call(big_mul);
            big(MP); e.i32(10); c.Call(big_mul);
            e.get(asym);
            c.If();
            big(MLO); e.i32(10); c.Call(big_mul);
            c.End();
            // while (gs_big_cmp(&r, &s) >= 0) { gs_big_sub(&r, &s); d++; }
            c.Block(); c.Loop();
            big(R); big(S); c.Call(big_cmp); e.i32(0); c.Int(intop::LtS, false); c.BrIf(1);
            big(R); big(S); c.Call(big_sub);
            e.inc(d);
            c.Br(0);
            c.End(); c.End();
            // c = gs_big_cmp(&r, mm); low = even ? c <= 0 : c < 0;
            big(R); e.get(mm); c.Call(big_cmp); e.set(cc);
            e.get(cc); e.i32(0); c.Int(intop::LeS, false);
            e.get(cc); e.i32(0); c.Int(intop::LtS, false);
            e.get(even); c.Select(); e.set(low);
            // gs_big_add(&t, &r, &mp); c = gs_big_cmp(&t, &s); high = even ? c >= 0 : c > 0;
            big(T); big(R); big(MP); c.Call(big_add);
            big(T); big(S); c.Call(big_cmp); e.set(cc);
            reaches(high);
            // if (!low && !high) { dig[n++] = '0' + d; continue; }
            e.get(low); e.get(high); c.Int(intop::Or, false); c.I32Eqz();
            c.If();
            e.i32(int32_t(Dig)); e.get(nn); c.I32Add(); e.get(d); e.i32('0'); c.I32Add(); c.I32Store8();
            e.inc(nn);
            c.Br(1);  // the loop
            c.End();
            // if (low && high) { t = r + r; c = cmp(t, s); high = c > 0 || (c == 0 && (d & 1)); }
            e.get(low); e.get(high); c.I32And();
            c.If();
            big(T); big(R); big(R); c.Call(big_add);
            big(T); big(S); c.Call(big_cmp); e.set(cc);
            e.get(cc); e.i32(0); c.Int(intop::GtS, false);
            e.get(cc); c.I32Eqz(); e.get(d); e.i32(1); c.I32And(); c.I32And();
            c.Int(intop::Or, false);
            e.set(high);
            c.End();
            // dig[n++] = '0' + d + high; return n;
            e.i32(int32_t(Dig)); e.get(nn); c.I32Add();
            e.get(d); e.i32('0'); c.I32Add(); e.get(high); c.I32Add(); c.I32Store8();
            e.inc(nn);
            e.i32(int32_t(KOut)); e.get(k); c.I32Store(0);
            e.get(nn);
            c.Return();
            c.End();  // loop
            c.Unreachable();
        }
        // gs_fmt_float(dst, neg, frac, bexp, bits, emin): a finite float's
        // text from its fields, its length returned.
        {
            fmt_float = m.AddFunc({ { V::I32, V::I32, V::I64, V::I32, V::I32, V::I32 }, { V::I32 } },
                                  { V::I32, V::I32, V::I32, V::I32, V::I32 });
            E e { m.FuncAt(fmt_float).code };
            auto &c = e.c;
            enum { dst, neg, frac, bexp, bits, emin, p, n, k, x, i };
            auto dig = [&](auto index) {
                e.i32(int32_t(Dig)); index(); c.I32Add(); c.LoadOp(0x2d, 0);  // i32.load8_u
            };
            auto ch = [&](char v) { e.put(p, [&] { e.i32(v); }); };
            e.get(dst); e.set(p);
            // if (neg) *p++ = '-';
            e.get(neg);
            c.If();
            ch('-');
            c.End();
            // if (!frac && !bexp) { "0.0" }
            e.get(frac); c.I64Eqz(); e.get(bexp); c.I32Eqz(); c.I32And();
            c.If();
            ch('0'); ch('.'); ch('0');
            e.get(p); e.get(dst); c.I32Sub(); c.Return();
            c.End();
            // n = gs_float_digits(bexp ? frac | 1 << (bits - 1) : frac,
            //                     (bexp ? bexp - 1 : 0) + emin, bits, emin, dig, &k);
            e.get(frac); e.i64(1); e.get(bits); e.i32(1); c.I32Sub(); c.I64ExtendI32U();
            c.Int(intop::Shl, true); c.Int(intop::Or, true);
            e.get(frac);
            e.get(bexp);
            c.Select();
            e.get(bexp); e.i32(1); c.I32Sub(); e.i32(0); e.get(bexp); c.Select();
            e.get(emin); c.I32Add();
            e.get(bits); e.get(emin);
            c.Call(float_digits);
            e.set(n);
            e.i32(int32_t(KOut)); c.I32Load(0); e.set(k);
            // x = k - 1;
            e.get(k); e.i32(1); c.I32Sub(); e.set(x);
            // if (x < -4 || x >= (n > 15 ? n : 15)) { exponent form }
            e.get(x); e.i32(-4); c.Int(intop::LtS, false);
            e.get(x);
            e.get(n); e.i32(15); e.get(n); e.i32(15); c.Int(intop::GtS, false); c.Select();
            c.Int(intop::GeS, false);
            c.Int(intop::Or, false);
            c.If();
            {
                // *p++ = dig[0]; if (n > 1) *p++ = '.';
                e.put(p, [&] { dig([&] { e.i32(0); }); });
                e.get(n); e.i32(1); c.Int(intop::GtS, false);
                c.If();
                ch('.');
                c.End();
                // for (i = 1; i < n; i++) *p++ = dig[i];
                e.i32(1); e.set(i);
                c.Block(); c.Loop();
                e.get(i); e.get(n); c.Int(intop::GeS, false); c.BrIf(1);
                e.put(p, [&] { dig([&] { e.get(i); }); });
                e.inc(i); c.Br(0);
                c.End(); c.End();
                // *p++ = 'e'; *p++ = x < 0 ? '-' : '+'; if (x < 0) x = -x;
                ch('e');
                e.put(p, [&] { e.i32('-'); e.i32('+'); e.get(x); e.i32(0); c.Int(intop::LtS, false); c.Select(); });
                e.get(x); e.i32(0); c.Int(intop::LtS, false);
                c.If();
                e.i32(0); e.get(x); c.I32Sub(); e.set(x);
                c.End();
                // if (x >= 100) *p++ = '0' + x / 100;
                e.get(x); e.i32(100); c.Int(intop::GeS, false);
                c.If();
                e.put(p, [&] { e.i32('0'); e.get(x); e.i32(100); c.Op(0x6d); c.I32Add(); });
                c.End();
                // *p++ = '0' + x / 10 % 10; *p++ = '0' + x % 10;
                e.put(p, [&] { e.i32('0'); e.get(x); e.i32(10); c.Op(0x6d); e.i32(10); c.Op(0x6f); c.I32Add(); });
                e.put(p, [&] { e.i32('0'); e.get(x); e.i32(10); c.Op(0x6f); c.I32Add(); });
            }
            c.Else();
            e.get(x); e.i32(0); c.Int(intop::LtS, false);
            c.If();
            {
                // *p++ = '0'; *p++ = '.'; for (i = -1; i > x; i--) *p++ = '0';
                ch('0'); ch('.');
                e.i32(-1); e.set(i);
                c.Block(); c.Loop();
                e.get(i); e.get(x); c.Int(intop::LeS, false); c.BrIf(1);
                ch('0');
                e.inc(i, -1); c.Br(0);
                c.End(); c.End();
                // for (i = 0; i < n; i++) *p++ = dig[i];
                e.i32(0); e.set(i);
                c.Block(); c.Loop();
                e.get(i); e.get(n); c.Int(intop::GeS, false); c.BrIf(1);
                e.put(p, [&] { dig([&] { e.get(i); }); });
                e.inc(i); c.Br(0);
                c.End(); c.End();
            }
            c.Else();
            {
                // for (i = 0; i < n || i <= x; i++) {
                //     if (i == x + 1) *p++ = '.';
                //     *p++ = i < n ? dig[i] : '0';
                // }
                e.i32(0); e.set(i);
                c.Block(); c.Loop();
                e.get(i); e.get(n); c.Int(intop::LtS, false);
                e.get(i); e.get(x); c.Int(intop::LeS, false);
                c.Int(intop::Or, false); c.I32Eqz(); c.BrIf(1);
                e.get(i); e.get(x); e.i32(1); c.I32Add(); c.Int(intop::Eq, false);
                c.If();
                ch('.');
                c.End();
                e.put(p, [&] {
                    e.get(i); e.get(n); c.Int(intop::LtS, false);
                    c.If(BlockType::I32);
                    dig([&] { e.get(i); });
                    c.Else();
                    e.i32('0');
                    c.End();
                });
                e.inc(i); c.Br(0);
                c.End(); c.End();
                // if (n <= x + 1) { *p++ = '.'; *p++ = '0'; }
                e.get(n); e.get(x); e.i32(1); c.I32Add(); c.Int(intop::LeS, false);
                c.If();
                ch('.'); ch('0');
                c.End();
            }
            c.End();
            c.End();
            e.get(p); e.get(dst); c.I32Sub();
        }
        // The bytes of a spelled-out value at `at`, n long, copied to dst.
        auto spell = [&](Code &c, uint32_t dst, uint32_t at, int32_t n) {
            c.LocalGet(dst);
            c.I32Const(int32_t(at));
            c.I32Const(n);
            c.MemoryCopy();
            c.I32Const(n);
            c.Return();
        };
        // gs_fmt_f64(dst, v): nan, inf and -inf spelled by the runtime, as
        // C libraries disagree on them; else the fields of v.
        {
            fmt_f64 = m.AddFunc({ { V::I32, V::F64 }, { V::I32 } }, { V::I64 });
            auto &c = m.FuncAt(fmt_f64).code;
            enum { dst, v, b };
            c.LocalGet(v); c.LocalGet(v); c.Op(0x62);  // f64.ne: v != v
            c.If();
            spell(c, dst, nanat, 3);
            c.End();
            c.LocalGet(v); c.Op(0x99); c.F64Const(INFINITY); c.Op(0x61);  // |v| == inf
            c.If();
            c.LocalGet(v); c.F64Const(0.0); c.Op(0x64);  // v > 0
            c.If();
            spell(c, dst, infat, 3);
            c.End();
            spell(c, dst, ninfat, 4);
            c.End();
            c.LocalGet(v); c.Op(0xbd); c.LocalSet(b);  // i64.reinterpret_f64
            c.LocalGet(dst);
            c.LocalGet(b); c.I64Const(63); c.Int(intop::ShrU, true); c.I32WrapI64();
            c.LocalGet(b); c.I64Const((int64_t(1) << 52) - 1); c.Int(intop::And, true);
            c.LocalGet(b); c.I64Const(52); c.Int(intop::ShrU, true); c.I32WrapI64();
            c.I32Const(0x7ff); c.I32And();
            c.I32Const(53); c.I32Const(-1074);
            c.Call(fmt_float);
        }
        // gs_fmt_f32(dst, v): an f32's own digits, laid out as an f64's.
        {
            fmt_f32 = m.AddFunc({ { V::I32, V::F32 }, { V::I32 } }, { V::I32 });
            auto &c = m.FuncAt(fmt_f32).code;
            enum { dst, v, b };
            c.LocalGet(v); c.LocalGet(v); c.Op(0x5c);  // f32.ne: v != v
            c.LocalGet(v); c.Op(0x8b); c.F32Const(INFINITY); c.Op(0x5b);  // |v| == inf
            c.Int(intop::Or, false);
            c.If();
            c.LocalGet(dst); c.LocalGet(v); c.Op(0xbb); c.Call(fmt_f64); c.Return();  // f64.promote_f32
            c.End();
            c.LocalGet(v); c.Op(0xbc); c.LocalSet(b);  // i32.reinterpret_f32
            c.LocalGet(dst);
            c.LocalGet(b); c.I32Const(31); c.Op(0x76); // i32.shr_u
            c.LocalGet(b); c.I32Const(0x7fffff); c.I32And(); c.I64ExtendI32U();
            c.LocalGet(b); c.I32Const(23); c.Op(0x76); c.I32Const(0xff); c.I32And();
            c.I32Const(24); c.I32Const(-149);
            c.Call(fmt_float);
        }
    }
};

}  // namespace goosewasm
