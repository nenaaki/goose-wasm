// goose-wasm's math: served in place of upstream's stdlib/math.goose
// (lib/README.md). Upstream's libm functions are extern fns its C runtime
// takes from the platform's C library (runtime_ext.h); a WebAssembly module
// has none, so here they are musl's libm ported to Goose, statement for
// statement, and compiled by the Wasm backend like the rest of the library.
// sqrt, floor, ceil, round and trunc stay extern fns, which the backend
// compiles to Wasm's own instructions; mul_add rounds twice, as upstream's
// does where the code is not built for FMA (runtime.h, GS_MUL_ADD_BASE).
//
// Derived from upstream Goose at commit
// 2ed1a63dde2ddff777fbb7f4790412b4f00cd47d: stdlib/math.goose (the
// declarations, constants and functions other than libm's). Original work:
// Goose, https://github.com/aardappel/goose, by Wouter van Oortmerssen,
// licensed under the Apache License, Version 2.0 (upstream/goose/LICENSE).
//
// The libm functions are ported from musl 1.2.5, src/math (named at each
// function), https://musl.libc.org/, Copyright © 2005-2020 Rich Felker, et
// al., licensed under the MIT license (NOTICE). musl's math code carries the
// notices of its own origins, which are kept here:
//
// From FreeBSD's msun (the trigonometric functions, their inverses,
// argument reduction, scalbn):
// ====================================================
// Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
//
// Developed at SunSoft, a Sun Microsystems, Inc. business.
// Permission to use, copy, modify, and distribute this
// software is freely granted, provided that this notice
// is preserved.
// ====================================================
// (e_rem_pio2.c and the float functions: optimized by Bruce D. Evans;
// the float kernels: Copyright (c) 2004 David Schultz and Bruce D. Evans.)
//
// From Arm's optimized-routines (exp, log, log2, pow and their float
// versions, and their data tables):
// Copyright (c) 2017-2018, Arm Limited.
// SPDX-License-Identifier: MIT
//
// Changes: musl's C translated to Goose; unions and GET_HIGH_WORD-style
// macros replaced by bit reinterpretations (libm::bits and friends, which
// the backend compiles to Wasm's reinterpret instructions); pointer
// out-parameters replaced by several results; goto replaced by loops and
// flags; FORCE_EVAL and floating-point exception raising dropped (Goose has
// no floating-point environment); the long-double-only part of the
// ipio2 table and the long-double precisions of __rem_pio2_large dropped;
// C's int and float conversions written with `as!` and explicit types, so
// that each operation has the width musl's has.

// The bits of a float, and the float of bits (the backend's reinterpret
// instructions).
extern fn libm::bits(x: f64) -> u64 = "gs_wasm_f64_bits";
extern fn libm::from_bits(b: u64) -> f64 = "gs_wasm_f64_from_bits";
extern fn libm::bits(x: f32) -> u32 = "gs_wasm_f32_bits";
extern fn libm::from_bits(b: u32) -> f32 = "gs_wasm_f32_from_bits";

extern fn sqrt(x: f64) -> f64 = "gs_sqrt";            extern fn sqrt(x: f32) -> f32 = "gs_sqrtf";
// x * y + z, rounded once where the code is built for FMA and twice where it
// is not: which is the implementation's, so results may differ between
// machines (§7.12). WebAssembly has no fused multiply-add: twice.
fn mul_add(x: f64, y: f64, z: f64) -> f64 { x * y + z }
fn mul_add(x: f32, y: f32, z: f32) -> f32 { x * y + z }
extern fn floor(x: f64) -> f64;                       extern fn floor(x: f32) -> f32 = "floorf";
extern fn ceil(x: f64) -> f64;                        extern fn ceil(x: f32) -> f32 = "ceilf";
extern fn round(x: f64) -> f64;                       extern fn round(x: f32) -> f32 = "roundf";
extern fn trunc(x: f64) -> f64;                       extern fn trunc(x: f32) -> f32 = "truncf";

let PI = 3.141592653589793;
let TAU = 6.283185307179586;

// Both widths, as libm's functions above. The f32 forms multiply by the
// ratio written out, which rounds to f32 once; 180.0 / PI worked out in
// f32 rounds PI first and comes out an ulp off.
fn radians(deg: f64) -> f64 { deg * (PI / 180.0) }
fn radians(deg: f32) -> f32 { deg * 0.017453292519943295 }
fn degrees(rad: f64) -> f64 { rad * (180.0 / PI) }
fn degrees(rad: f32) -> f32 { rad * 57.29577951308232 }

// x - x is 0 for every finite x, and NaN for an infinity or a NaN.
fn is_nan(x: f64) -> bool { x != x }
fn is_nan(x: f32) -> bool { x != x }
fn is_inf(x: f64) -> bool { x == x && (x - x) != (x - x) }
fn is_inf(x: f32) -> bool { x == x && (x - x) != (x - x) }
fn is_finite(x: f64) -> bool { x - x == 0.0 }
fn is_finite(x: f32) -> bool { x - x == 0.0 }

// ---- Bits ----

// GET_HIGH_WORD, GET_LOW_WORD, SET_LOW_WORD.
fn libm::high(x: f64) -> u32 { (libm::bits(x) >> 32) as! u32 }
fn libm::low(x: f64) -> u32 { libm::bits(x) as! u32 }
fn libm::zero_low(x: f64) -> f64 { libm::from_bits(libm::bits(x) & 0xffffffff00000000) }
fn libm::fabs(x: f64) -> f64 { libm::from_bits(libm::bits(x) & 0x7fffffffffffffff) }
fn libm::fabs(x: f32) -> f32 { libm::from_bits(libm::bits(x) & 0x7fffffff) }

// scalbn.c
fn libm::scalbn(x: f64, n0: i32) -> f64 {
    var n = n0;
    var y = x;
    if n > 1023 {
        y *= 0x1p1023;
        n -= 1023;
        if n > 1023 {
            y *= 0x1p1023;
            n -= 1023;
            if n > 1023 { n = 1023; }
        }
    } else if n < -1022 {
        // make sure final n < -53 to avoid double rounding in the
        // subnormal range
        y *= 0x1p-1022 * 0x1p53;
        n += 1022 - 53;
        if n < -1022 {
            y *= 0x1p-1022 * 0x1p53;
            n += 1022 - 53;
            if n < -1022 { n = -1022; }
        }
    }
    return y * libm::from_bits(((0x3ff + n) as! u64) << 52);
}

// ---- sin, cos, tan ----

// __sin.c: sin on [-pi/4, pi/4], x + y the argument, iy whether y is 0.
fn libm::k_sin(x: f64, y: f64, iy: i32) -> f64 {
    let S1: f64 = -1.66666666666666324348e-01;  // 0xBFC55555, 0x55555549
    let S2: f64 = 8.33333333332248946124e-03;   // 0x3F811111, 0x1110F8A6
    let S3: f64 = -1.98412698298579493134e-04;  // 0xBF2A01A0, 0x19C161D5
    let S4: f64 = 2.75573137070700676789e-06;   // 0x3EC71DE3, 0x57B1FE7D
    let S5: f64 = -2.50507602534068634195e-08;  // 0xBE5AE5E6, 0x8A2B9CEB
    let S6: f64 = 1.58969099521155010221e-10;   // 0x3DE5D93A, 0x5ACFD57C
    let z = x * x;
    let w = z * z;
    let r = S2 + z * (S3 + z * S4) + z * w * (S5 + z * S6);
    let v = z * x;
    if iy == 0 { return x + v * (S1 + z * r); }
    return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}

// __cos.c: cos on [-pi/4, pi/4].
fn libm::k_cos(x: f64, y: f64) -> f64 {
    let C1: f64 = 4.16666666666666019037e-02;   // 0x3FA55555, 0x5555554C
    let C2: f64 = -1.38888888888741095749e-03;  // 0xBF56C16C, 0x16C15177
    let C3: f64 = 2.48015872894767294178e-05;   // 0x3EFA01A0, 0x19CB1590
    let C4: f64 = -2.75573143513906633035e-07;  // 0xBE927E4F, 0x809C52AD
    let C5: f64 = 2.08757232129817482790e-09;   // 0x3E21EE9E, 0xBDB4B1C4
    let C6: f64 = -1.13596475577881948265e-11;  // 0xBDA8FAE9, 0xBE8838D4
    let z = x * x;
    var w = z * z;
    let r = z * (C1 + z * (C2 + z * C3)) + w * w * (C4 + z * (C5 + z * C6));
    let hz = 0.5 * z;
    w = 1.0 - hz;
    return w + (((1.0 - w) - hz) + (z * r - x * y));
}

// __tan.c's T[].
let libm::TAN_T: f64[13] = [
    3.33333333333334091986e-01,   // 3FD55555, 55555563
    1.33333333333201242699e-01,   // 3FC11111, 1110FE7A
    5.39682539762260521377e-02,   // 3FABA1BA, 1BB341FE
    2.18694882948595424599e-02,   // 3F9664F4, 8406D637
    8.86323982359930005737e-03,   // 3F8226E3, E96E8493
    3.59207910759131235356e-03,   // 3F6D6D22, C9560328
    1.45620945432529025516e-03,   // 3F57DBC8, FEE08315
    5.88041240820264096874e-04,   // 3F4344D8, F2F26501
    2.46463134818469906812e-04,   // 3F3026F7, 1A8D1068
    7.81794442939557092300e-05,   // 3F147E88, A03792A6
    7.14072491382608190305e-05,   // 3F12B80F, 32F0A7E9
    -1.85586374855275456654e-05,  // BEF375CB, DB605373
    2.59073051863633712884e-05,   // 3EFB2A70, 74BF7AD4
];

// __tan.c: tan on [-pi/4, pi/4], or -1/tan where odd.
fn libm::k_tan(x0: f64, y0: f64, odd: i32) -> f64 {
    let T = libm::TAN_T;
    let pio4: f64 = 7.85398163397448278999e-01;    // 3FE921FB, 54442D18
    let pio4lo: f64 = 3.06161699786838301793e-17;  // 3C81A626, 33145C07
    var x = x0;
    var y = y0;
    let hx = libm::high(x);
    let big = (hx & 0x7fffffff) >= 0x3FE59428;  // |x| >= 0.6744
    let sign = hx >> 31;
    if big {
        if sign != 0 {
            x = -x;
            y = -y;
        }
        x = (pio4 - x) + (pio4lo - y);
        y = 0.0;
    }
    let z = x * x;
    var w = z * z;
    // Break x^5*(T[1]+x^2*T[2]+...) into
    // x^5(T[1]+x^4*T[3]+...+x^20*T[11]) +
    // x^5(x^2*(T[2]+x^4*T[4]+...+x^22*[T12]))
    var r = T[1] + w * (T[3] + w * (T[5] + w * (T[7] + w * (T[9] + w * T[11]))));
    var v = z * (T[2] + w * (T[4] + w * (T[6] + w * (T[8] + w * (T[10] + w * T[12])))));
    var s = z * x;
    r = y + z * (s * (r + v) + y) + s * T[0];
    w = x + r;
    if big {
        s = 1 - 2 * odd;
        v = s - 2.0 * (x + (r - w * w / (w + s)));
        return if sign != 0 { -v } else { v };
    }
    if odd == 0 { return w; }
    // -1.0/(x+r) has up to 2ulp error, so compute it accurately
    let w0 = libm::zero_low(w);
    v = r - (w0 - x);  // w0+v = r+x
    let a = -1.0 / w;
    let a0 = libm::zero_low(a);
    return a0 + a * (1.0 + a0 * w0 + a0 * v);
}

// __rem_pio2_large.c's ipio2[]: 2/pi in 24-bit chunks, as far as double
// needs.
let libm::IPIO2: i32[66] = [
    0xA2F983, 0x6E4E44, 0x1529FC, 0x2757D1, 0xF534DD, 0xC0DB62,
    0x95993C, 0x439041, 0xFE5163, 0xABDEBB, 0xC561B7, 0x246E3A,
    0x424DD2, 0xE00649, 0x2EEA09, 0xD1921C, 0xFE1DEB, 0x1CB129,
    0xA73EE8, 0x8235F5, 0x2EBB44, 0x84E99C, 0x7026B4, 0x5F7E41,
    0x3991D6, 0x398353, 0x39F49C, 0x845F8B, 0xBDF928, 0x3B1FF8,
    0x97FFDE, 0x05980F, 0xEF2F11, 0x8B5A0A, 0x6D1F6D, 0x367ECF,
    0x27CB09, 0xB74F46, 0x3F669E, 0x5FEA2D, 0x7527BA, 0xC7EBE5,
    0xF17B3D, 0x0739F7, 0x8A5292, 0xEA6BFB, 0x5FB11F, 0x8D5D08,
    0x560330, 0x46FC7B, 0x6BABF0, 0xCFBC20, 0x9AF436, 0x1DA9E3,
    0x91615E, 0xE61B08, 0x659985, 0x5F14A0, 0x68408D, 0xFFD880,
    0x4D7327, 0x310606, 0x1556CA, 0x73A8C9, 0x60E27B, 0xC08C6B,
];

let libm::PIO2: f64[8] = [
    1.57079625129699707031e+00,  // 0x3FF921FB, 0x40000000
    7.54978941586159635335e-08,  // 0x3E74442D, 0x00000000
    5.39030252995776476554e-15,  // 0x3CF84698, 0x80000000
    3.28200341580791294123e-22,  // 0x3B78CC51, 0x60000000
    1.27065575308067607349e-29,  // 0x39F01B83, 0x80000000
    1.22933308981111328932e-36,  // 0x387A2520, 0x40000000
    2.73370053816464559624e-44,  // 0x36E38222, 0x80000000
    2.16741683877804819444e-51,  // 0x3569F31D, 0x00000000
];

// __rem_pio2_large.c, for the precisions double and float use (prec 1 and
// 0): x[0..nx] the argument's 24-bit chunks, e0 the exponent of x[0]; the
// last three bits of N and y = x - N*pi/2 (y0 + y1 at prec 1).
fn libm::rem_pio2_large(x: f64[3], e0: i32, nx: i32, prec: i32) -> i32, f64, f64 {
    let ipio2 = libm::IPIO2;
    let PIo2 = libm::PIO2;
    var iq: i32[20] = [0; 20];
    var f: f64[20] = [0.0; 20];
    var fq: f64[20] = [0.0; 20];
    var q: f64[20] = [0.0; 20];
    var y0 = 0.0;
    var y1 = 0.0;
    // initialize jk
    let jk: i32 = if prec == 0 { 3 } else { 4 };  // init_jk[prec]
    let jp = jk;
    // determine jx,jv,q0, note that 3>q0
    let jx = nx - 1;
    var jv = (e0 - 3) / 24;
    if jv < 0 { jv = 0; }
    var q0 = e0 - 24 * (jv + 1);
    // set up f[0] to f[jx+jk] where f[jx+jk] = ipio2[jv+jk]
    var j = jv - jx;
    let m = jx + jk;
    var i: i32 = 0;
    while i <= m {
        f[i] = if j < 0 { 0.0 } else { ipio2[j] };
        i++;
        j++;
    }
    // compute q[0],q[1],...q[jk]
    i = 0;
    while i <= jk {
        var fw = 0.0;
        j = 0;
        while j <= jx {
            fw += x[j] * f[jx + i - j];
            j++;
        }
        q[i] = fw;
        i++;
    }
    var jz = jk;
    var z = 0.0;
    var n: i32 = 0;
    var ih: i32 = 0;
    var carry: i32 = 0;
    while true {  // recompute:
        // distill q[] into iq[] reversingly
        i = 0;
        j = jz;
        z = q[jz];
        while j > 0 {
            let fw = ((0x1p-24 * z) as! i32) as f64;
            iq[i] = (z - 0x1p24 * fw) as! i32;
            z = q[j - 1] + fw;
            i++;
            j--;
        }
        // compute n
        z = libm::scalbn(z, q0);              // actual value of z
        z -= 8.0 * floor(z * 0.125);          // trim off integer >= 8
        n = z as! i32;
        z -= n;
        ih = 0;
        if q0 > 0 {  // need iq[jz-1] to determine n
            i = iq[jz - 1] >> (24 - q0);
            n += i;
            iq[jz - 1] -= i << (24 - q0);
            ih = iq[jz - 1] >> (23 - q0);
        } else if q0 == 0 {
            ih = iq[jz - 1] >> 23;
        } else if z >= 0.5 {
            ih = 2;
        }
        if ih > 0 {  // q > 0.5
            n += 1;
            carry = 0;
            i = 0;
            while i < jz {  // compute 1-q
                j = iq[i];
                if carry == 0 {
                    if j != 0 {
                        carry = 1;
                        iq[i] = 0x1000000 - j;
                    }
                } else {
                    iq[i] = 0xffffff - j;
                }
                i++;
            }
            if q0 > 0 {  // rare case: chance is 1 in 12
                if q0 == 1 { iq[jz - 1] &= 0x7fffff; }
                if q0 == 2 { iq[jz - 1] &= 0x3fffff; }
            }
            if ih == 2 {
                z = 1.0 - z;
                if carry != 0 { z -= libm::scalbn(1.0, q0); }
            }
        }
        // check if recomputation is needed
        if z == 0.0 {
            j = 0;
            i = jz - 1;
            while i >= jk {
                j |= iq[i];
                i--;
            }
            if j == 0 {  // need recomputation
                var k: i32 = 1;
                while iq[jk - k] == 0 { k++; }  // k = no. of terms needed
                i = jz + 1;
                while i <= jz + k {  // add q[jz+1] to q[jz+k]
                    f[jx + i] = ipio2[jv + i];
                    var fw = 0.0;
                    j = 0;
                    while j <= jx {
                        fw += x[j] * f[jx + i - j];
                        j++;
                    }
                    q[i] = fw;
                    i++;
                }
                jz += k;
                continue;
            }
        }
        break;
    }
    // chop off zero terms
    if z == 0.0 {
        jz -= 1;
        q0 -= 24;
        while iq[jz] == 0 {
            jz--;
            q0 -= 24;
        }
    } else {  // break z into 24-bit if necessary
        z = libm::scalbn(z, -q0);
        if z >= 0x1p24 {
            let fw = ((0x1p-24 * z) as! i32) as f64;
            iq[jz] = (z - 0x1p24 * fw) as! i32;
            jz += 1;
            q0 += 24;
            iq[jz] = fw as! i32;
        } else {
            iq[jz] = z as! i32;
        }
    }
    // convert integer "bit" chunk to floating-point value
    var fw = libm::scalbn(1.0, q0);
    i = jz;
    while i >= 0 {
        q[i] = fw * iq[i];
        fw *= 0x1p-24;
        i--;
    }
    // compute PIo2[0,...,jp]*q[jz,...,0]
    i = jz;
    while i >= 0 {
        fw = 0.0;
        var k: i32 = 0;
        while k <= jp && k <= jz - i {
            fw += PIo2[k] * q[i + k];
            k++;
        }
        fq[jz - i] = fw;
        i--;
    }
    // compress fq[] into y[]
    fw = 0.0;
    i = jz;
    while i >= 0 {
        fw += fq[i];
        i--;
    }
    y0 = if ih == 0 { fw } else { -fw };
    if prec != 0 {
        fw = fq[0] - fw;
        i = 1;
        while i <= jz {
            fw += fq[i];
            i++;
        }
        y1 = if ih == 0 { fw } else { -fw };
    }
    return n & 7, y0, y1;
}

// __rem_pio2.c: x rem pi/2, as n and y0 + y1. The caller handles
// |x| ~<= pi/4, where no reduction is needed.
fn libm::rem_pio2(x: f64) -> i32, f64, f64 {
    let toint: f64 = 1.5 / 2.22044604925031308085e-16;  // 1.5/DBL_EPSILON
    let pio4: f64 = 0x1.921fb54442d18p-1;
    let invpio2: f64 = 6.36619772367581382433e-01;  // 0x3FE45F30, 0x6DC9C883
    let pio2_1: f64 = 1.57079632673412561417e+00;   // 0x3FF921FB, 0x54400000
    let pio2_1t: f64 = 6.07710050650619224932e-11;  // 0x3DD0B461, 0x1A626331
    let pio2_2: f64 = 6.07710050630396597660e-11;   // 0x3DD0B461, 0x1A600000
    let pio2_2t: f64 = 2.02226624879595063154e-21;  // 0x3BA3198A, 0x2E037073
    let pio2_3: f64 = 2.02226624871116645580e-21;   // 0x3BA3198A, 0x2E000000
    let pio2_3t: f64 = 8.47842766036889956997e-32;  // 0x397B839A, 0x252049C1
    let ui = libm::bits(x);
    let sign = (ui >> 63) != 0;
    let ix = ((ui >> 32) as! u32) & 0x7fffffff;
    var medium = false;
    if ix <= 0x400f6a7a {  // |x| ~<= 5pi/4
        if (ix & 0xfffff) == 0x921fb {  // |x| ~= pi/2 or 2pi/2
            medium = true;  // cancellation -- use medium case
        } else if ix <= 0x4002d97c {  // |x| ~<= 3pi/4
            if !sign {
                let z = x - pio2_1;  // one round good to 85 bits
                let y0 = z - pio2_1t;
                return 1, y0, (z - y0) - pio2_1t;
            } else {
                let z = x + pio2_1;
                let y0 = z + pio2_1t;
                return -1, y0, (z - y0) + pio2_1t;
            }
        } else {
            if !sign {
                let z = x - 2.0 * pio2_1;
                let y0 = z - 2.0 * pio2_1t;
                return 2, y0, (z - y0) - 2.0 * pio2_1t;
            } else {
                let z = x + 2.0 * pio2_1;
                let y0 = z + 2.0 * pio2_1t;
                return -2, y0, (z - y0) + 2.0 * pio2_1t;
            }
        }
    }
    if !medium && ix <= 0x401c463b {  // |x| ~<= 9pi/4
        if ix <= 0x4015fdbc {  // |x| ~<= 7pi/4
            if ix == 0x4012d97c {  // |x| ~= 3pi/2
                medium = true;
            } else if !sign {
                let z = x - 3.0 * pio2_1;
                let y0 = z - 3.0 * pio2_1t;
                return 3, y0, (z - y0) - 3.0 * pio2_1t;
            } else {
                let z = x + 3.0 * pio2_1;
                let y0 = z + 3.0 * pio2_1t;
                return -3, y0, (z - y0) + 3.0 * pio2_1t;
            }
        } else {
            if ix == 0x401921fb {  // |x| ~= 4pi/2
                medium = true;
            } else if !sign {
                let z = x - 4.0 * pio2_1;
                let y0 = z - 4.0 * pio2_1t;
                return 4, y0, (z - y0) - 4.0 * pio2_1t;
            } else {
                let z = x + 4.0 * pio2_1;
                let y0 = z + 4.0 * pio2_1t;
                return -4, y0, (z - y0) + 4.0 * pio2_1t;
            }
        }
    }
    if medium || ix < 0x413921fb {  // |x| ~< 2^20*(pi/2), medium size
        // rint(x/(pi/2))
        var fnv = x * invpio2 + toint - toint;
        var n = fnv as! i32;
        var r = x - fnv * pio2_1;
        var w = fnv * pio2_1t;  // 1st round, good to 85 bits
        // Matters with directed rounding.
        if r - w < -pio4 {
            n--;
            fnv -= 1.0;
            r = x - fnv * pio2_1;
            w = fnv * pio2_1t;
        } else if r - w > pio4 {
            n++;
            fnv += 1.0;
            r = x - fnv * pio2_1;
            w = fnv * pio2_1t;
        }
        var y0 = r - w;
        var ey = ((libm::bits(y0) >> 52) & 0x7ff) as! i32;
        let ex = (ix >> 20) as! i32;
        if ex - ey > 16 {  // 2nd round, good to 118 bits
            var t = r;
            w = fnv * pio2_2;
            r = t - w;
            w = fnv * pio2_2t - ((t - r) - w);
            y0 = r - w;
            ey = ((libm::bits(y0) >> 52) & 0x7ff) as! i32;
            if ex - ey > 49 {  // 3rd round, good to 151 bits, covers all cases
                t = r;
                w = fnv * pio2_3;
                r = t - w;
                w = fnv * pio2_3t - ((t - r) - w);
                y0 = r - w;
            }
        }
        return n, y0, (r - y0) - w;
    }
    // all other (large) arguments
    if ix >= 0x7ff00000 {  // x is inf or NaN
        return 0, x - x, x - x;
    }
    // set z = scalbn(|x|,-ilogb(x)+23)
    var z = libm::from_bits((ui & (0xffffffffffffffff >> 12)) | ((0x3ff + 23) as u64) << 52);
    var tx: f64[3] = [0.0; 3];
    var i: i32 = 0;
    while i < 2 {
        tx[i] = z as! i32;
        z = (z - tx[i]) * 0x1p24;
        i++;
    }
    tx[i] = z;
    // skip zero terms, first term is non-zero
    while tx[i] == 0.0 { i--; }
    let n, ty0, ty1 = libm::rem_pio2_large(tx, ((ix >> 20) as! i32) - (0x3ff + 23), i + 1, 1);
    if sign { return -n, -ty0, -ty1; }
    return n, ty0, ty1;
}

// sin.c
fn sin(x: f64) -> f64 {
    let ix = libm::high(x) & 0x7fffffff;
    // |x| ~< pi/4
    if ix <= 0x3fe921fb {
        if ix < 0x3e500000 { return x; }  // |x| < 2**-26
        return libm::k_sin(x, 0.0, 0);
    }
    // sin(Inf or NaN) is NaN
    if ix >= 0x7ff00000 { return x - x; }
    // argument reduction needed
    let n, y0, y1 = libm::rem_pio2(x);
    let k = n & 3;
    if k == 0 { return libm::k_sin(y0, y1, 1); }
    if k == 1 { return libm::k_cos(y0, y1); }
    if k == 2 { return -libm::k_sin(y0, y1, 1); }
    return -libm::k_cos(y0, y1);
}

// cos.c
fn cos(x: f64) -> f64 {
    let ix = libm::high(x) & 0x7fffffff;
    // |x| ~< pi/4
    if ix <= 0x3fe921fb {
        if ix < 0x3e46a09e { return 1.0; }  // |x| < 2**-27 * sqrt(2)
        return libm::k_cos(x, 0.0);
    }
    // cos(Inf or NaN) is NaN
    if ix >= 0x7ff00000 { return x - x; }
    // argument reduction
    let n, y0, y1 = libm::rem_pio2(x);
    let k = n & 3;
    if k == 0 { return libm::k_cos(y0, y1); }
    if k == 1 { return -libm::k_sin(y0, y1, 1); }
    if k == 2 { return -libm::k_cos(y0, y1); }
    return libm::k_sin(y0, y1, 1);
}

// tan.c
fn tan(x: f64) -> f64 {
    let ix = libm::high(x) & 0x7fffffff;
    // |x| ~< pi/4
    if ix <= 0x3fe921fb {
        if ix < 0x3e400000 { return x; }  // |x| < 2**-27
        return libm::k_tan(x, 0.0, 0);
    }
    // tan(Inf or NaN) is NaN
    if ix >= 0x7ff00000 { return x - x; }
    // argument reduction
    let n, y0, y1 = libm::rem_pio2(x);
    return libm::k_tan(y0, y1, n & 1);
}

// ---- asin, acos, atan, atan2 ----

// asin.c and acos.c's R(): the rational approximation.
fn libm::asin_r(z: f64) -> f64 {
    let pS0: f64 = 1.66666666666666657415e-01;   // 0x3FC55555, 0x55555555
    let pS1: f64 = -3.25565818622400915405e-01;  // 0xBFD4D612, 0x03EB6F7D
    let pS2: f64 = 2.01212532134862925881e-01;   // 0x3FC9C155, 0x0E884455
    let pS3: f64 = -4.00555345006794114027e-02;  // 0xBFA48228, 0xB5688F3B
    let pS4: f64 = 7.91534994289814532176e-04;   // 0x3F49EFE0, 0x7501B288
    let pS5: f64 = 3.47933107596021167570e-05;   // 0x3F023DE1, 0x0DFDF709
    let qS1: f64 = -2.40339491173441421878e+00;  // 0xC0033A27, 0x1C8A2D4B
    let qS2: f64 = 2.02094576023350569471e+00;   // 0x40002AE5, 0x9C598AC8
    let qS3: f64 = -6.88283971605453293030e-01;  // 0xBFE6066C, 0x1B8D0159
    let qS4: f64 = 7.70381505559019352791e-02;   // 0x3FB3B8C5, 0xB12E9282
    let p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
    let q = 1.0 + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
    return p / q;
}

let libm::PIO2_HI: f64 = 1.57079632679489655800e+00;  // 0x3FF921FB, 0x54442D18
let libm::PIO2_LO: f64 = 6.12323399573676603587e-17;  // 0x3C91A626, 0x33145C07

// asin.c
fn asin(x0: f64) -> f64 {
    let pio2_hi = libm::PIO2_HI;
    let pio2_lo = libm::PIO2_LO;
    var x = x0;
    let hx = libm::high(x);
    let ix = hx & 0x7fffffff;
    // |x| >= 1 or nan
    if ix >= 0x3ff00000 {
        let lx = libm::low(x);
        // asin(1) = +-pi/2 with inexact
        if ((ix - 0x3ff00000) | lx) == 0 { return x * pio2_hi + 0x1p-120; }
        return 0.0 / (x - x);
    }
    // |x| < 0.5
    if ix < 0x3fe00000 {
        // if 0x1p-1022 <= |x| < 0x1p-26, avoid raising underflow
        if ix < 0x3e500000 && ix >= 0x00100000 { return x; }
        return x + x * libm::asin_r(x * x);
    }
    // 1 > |x| >= 0.5
    let z = (1.0 - libm::fabs(x)) * 0.5;
    let s = sqrt(z);
    let r = libm::asin_r(z);
    if ix >= 0x3fef3333 {  // if |x| > 0.975
        x = pio2_hi - (2.0 * (s + s * r) - pio2_lo);
    } else {
        // f+c = sqrt(z)
        let f = libm::zero_low(s);
        let c = (z - f * f) / (s + f);
        x = 0.5 * pio2_hi - (2.0 * s * r - (pio2_lo - 2.0 * c) - (0.5 * pio2_hi - 2.0 * f));
    }
    if (hx >> 31) != 0 { return -x; }
    return x;
}

// acos.c
fn acos(x: f64) -> f64 {
    let pio2_hi = libm::PIO2_HI;
    let pio2_lo = libm::PIO2_LO;
    let hx = libm::high(x);
    let ix = hx & 0x7fffffff;
    // |x| >= 1 or nan
    if ix >= 0x3ff00000 {
        let lx = libm::low(x);
        if ((ix - 0x3ff00000) | lx) == 0 {
            // acos(1)=0, acos(-1)=pi
            if (hx >> 31) != 0 { return 2.0 * pio2_hi + 0x1p-120; }
            return 0.0;
        }
        return 0.0 / (x - x);
    }
    // |x| < 0.5
    if ix < 0x3fe00000 {
        if ix <= 0x3c600000 { return pio2_hi + 0x1p-120; }  // |x| < 2**-57
        return pio2_hi - (x - (pio2_lo - x * libm::asin_r(x * x)));
    }
    // x < -0.5
    if (hx >> 31) != 0 {
        let z = (1.0 + x) * 0.5;
        let s = sqrt(z);
        let w = libm::asin_r(z) * s - pio2_lo;
        return 2.0 * (pio2_hi - (s + w));
    }
    // x > 0.5
    let z = (1.0 - x) * 0.5;
    let s = sqrt(z);
    let df = libm::zero_low(s);
    let c = (z - df * df) / (s + df);
    let w = libm::asin_r(z) * s + c;
    return 2.0 * (df + w);
}

// atan.c's tables.
let libm::ATANHI: f64[4] = [
    4.63647609000806093515e-01,  // atan(0.5)hi 0x3FDDAC67, 0x0561BB4F
    7.85398163397448278999e-01,  // atan(1.0)hi 0x3FE921FB, 0x54442D18
    9.82793723247329054082e-01,  // atan(1.5)hi 0x3FEF730B, 0xD281F69B
    1.57079632679489655800e+00,  // atan(inf)hi 0x3FF921FB, 0x54442D18
];
let libm::ATANLO: f64[4] = [
    2.26987774529616870924e-17,  // atan(0.5)lo 0x3C7A2B7F, 0x222F65E2
    3.06161699786838301793e-17,  // atan(1.0)lo 0x3C81A626, 0x33145C07
    1.39033110312309984516e-17,  // atan(1.5)lo 0x3C700788, 0x7AF0CBBD
    6.12323399573676603587e-17,  // atan(inf)lo 0x3C91A626, 0x33145C07
];
let libm::AT: f64[11] = [
    3.33333333333329318027e-01,   // 0x3FD55555, 0x5555550D
    -1.99999999998764832476e-01,  // 0xBFC99999, 0x9998EBC4
    1.42857142725034663711e-01,   // 0x3FC24924, 0x920083FF
    -1.11111104054623557880e-01,  // 0xBFBC71C6, 0xFE231671
    9.09088713343650656196e-02,   // 0x3FB745CD, 0xC54C206E
    -7.69187620504482999495e-02,  // 0xBFB3B0F2, 0xAF749A6D
    6.66107313738753120669e-02,   // 0x3FB10D66, 0xA0D03D51
    -5.83357013379057348645e-02,  // 0xBFADDE2D, 0x52DEFD9A
    4.97687799461593236017e-02,   // 0x3FA97B4B, 0x24760DEB
    -3.65315727442169155270e-02,  // 0xBFA2B444, 0x2C6A6C2F
    1.62858201153657823623e-02,   // 0x3F90AD3A, 0xE322DA11
];

// atan.c
fn atan(x0: f64) -> f64 {
    let aT = libm::AT;
    var x = x0;
    var ix = libm::high(x);
    let sign = ix >> 31;
    ix &= 0x7fffffff;
    var id: i32 = 0;
    if ix >= 0x44100000 {  // if |x| >= 2^66
        if x != x { return x; }
        let z = libm::ATANHI[3] + 0x1p-120;
        return if sign != 0 { -z } else { z };
    }
    if ix < 0x3fdc0000 {  // |x| < 0.4375
        if ix < 0x3e400000 { return x; }  // |x| < 2^-27
        id = -1;
    } else {
        x = libm::fabs(x);
        if ix < 0x3ff30000 {  // |x| < 1.1875
            if ix < 0x3fe60000 {  //  7/16 <= |x| < 11/16
                id = 0;
                x = (2.0 * x - 1.0) / (2.0 + x);
            } else {  // 11/16 <= |x| < 19/16
                id = 1;
                x = (x - 1.0) / (x + 1.0);
            }
        } else {
            if ix < 0x40038000 {  // |x| < 2.4375
                id = 2;
                x = (x - 1.5) / (1.0 + 1.5 * x);
            } else {  // 2.4375 <= |x| < 2^66
                id = 3;
                x = -1.0 / x;
            }
        }
    }
    // end of argument reduction
    var z = x * x;
    let w = z * z;
    // break sum from i=0 to 10 aT[i]z**(i+1) into odd and even poly
    let s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
    let s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
    if id < 0 { return x - x * (s1 + s2); }
    z = libm::ATANHI[id] - (x * (s1 + s2) - libm::ATANLO[id] - x);
    return if sign != 0 { -z } else { z };
}

// atan2.c
fn atan2(y: f64, x: f64) -> f64 {
    let pi: f64 = 3.1415926535897931160E+00;     // 0x400921FB, 0x54442D18
    let pi_lo: f64 = 1.2246467991473531772E-16;  // 0x3CA1A626, 0x33145C07
    if x != x || y != y { return x + y; }
    var ix = libm::high(x);
    let lx = libm::low(x);
    var iy = libm::high(y);
    let ly = libm::low(y);
    if ((ix - 0x3ff00000) | lx) == 0 { return atan(y); }  // x = 1.0
    let m = ((iy >> 31) & 1) | ((ix >> 30) & 2);  // 2*sign(x)+sign(y)
    ix = ix & 0x7fffffff;
    iy = iy & 0x7fffffff;
    // when y = 0
    if (iy | ly) == 0 {
        if m == 0 || m == 1 { return y; }  // atan(+-0,+anything)=+-0
        if m == 2 { return pi; }           // atan(+0,-anything) = pi
        return -pi;                        // atan(-0,-anything) =-pi
    }
    // when x = 0
    if (ix | lx) == 0 { return if (m & 1) != 0 { -pi / 2.0 } else { pi / 2.0 }; }
    // when x is INF
    if ix == 0x7ff00000 {
        if iy == 0x7ff00000 {
            if m == 0 { return pi / 4.0; }        // atan(+INF,+INF)
            if m == 1 { return -pi / 4.0; }       // atan(-INF,+INF)
            if m == 2 { return 3.0 * pi / 4.0; }  // atan(+INF,-INF)
            return -3.0 * pi / 4.0;               // atan(-INF,-INF)
        } else {
            if m == 0 { return 0.0; }   // atan(+...,+INF)
            if m == 1 { return -0.0; }  // atan(-...,+INF)
            if m == 2 { return pi; }    // atan(+...,-INF)
            return -pi;                 // atan(-...,-INF)
        }
    }
    // |y/x| > 0x1p64
    if ix + (64 << 20) < iy || iy == 0x7ff00000 {
        return if (m & 1) != 0 { -pi / 2.0 } else { pi / 2.0 };
    }
    // z = atan(|y/x|) without spurious underflow
    var z = 0.0;
    if (m & 2) != 0 && iy + (64 << 20) < ix {  // |y/x| < 0x1p-64, x<0
        z = 0.0;
    } else {
        z = atan(libm::fabs(y / x));
    }
    if m == 0 { return z; }                  // atan(+,+)
    if m == 1 { return -z; }                 // atan(-,+)
    if m == 2 { return pi - (z - pi_lo); }   // atan(+,-)
    return (z - pi_lo) - pi;                 // atan(-,-)
}

// ---- exp, log, log2, log10, pow ----

// Arm's tables (exp_data.c, log_data.c, log2_data.c, pow_data.c, at the end
// of this file) hold doubles and the bits of doubles; each is kept here as
// the bits of its fields, in the C struct's order, and read through these
// offsets. musl's builds have no fast FMA (__FP_FAST_FMA), so the tables
// carry their tab2 parts and the code takes its paths without fma.
let libm::EXP_INVLN2N = 0;
let libm::EXP_SHIFT = 1;
let libm::EXP_NEGLN2HIN = 2;
let libm::EXP_NEGLN2LON = 3;
let libm::EXP_POLY = 4;   // [4]: C2..C5
let libm::EXP_TAB = 14;   // [2 * 128]
fn libm::exp_d(i: i64) -> f64 { libm::from_bits(libm::EXP_DATA[i]) }

// exp.c's specialcase (and pow.c's, which tests |y| and fixes the sign of
// 0 for a negative scale; `signed` selects it).
fn libm::exp_special(tmp: f64, sbits0: u64, ki: u64, signed: bool) -> f64 {
    var sbits = sbits0;
    if (ki & 0x80000000) == 0 {
        // k > 0, the exponent of scale might have overflowed by <= 460.
        sbits -= 1009 << 52;
        let scale = libm::from_bits(sbits);
        return 0x1p1009 * (scale + scale * tmp);
    }
    // k < 0, need special care in the subnormal range.
    sbits += 1022 << 52;
    let scale = libm::from_bits(sbits);
    var y = scale + scale * tmp;
    if (if signed { libm::fabs(y) } else { y }) < 1.0 {
        // Round y to the right precision before scaling it into the
        // subnormal range to avoid double rounding that can cause 0.5+E/2
        // ulp error where E is the worst-case ulp error outside the
        // subnormal range.
        let one = if signed && y < 0.0 { -1.0 } else { 1.0 };
        var lo = scale - y + scale * tmp;
        let hi = one + y;
        lo = one - hi + y + lo;
        y = (hi + lo) - one;
        if y == 0.0 {
            // Avoid -0.0 with downward rounding (exp); fix the sign of 0 (pow).
            y = if signed { libm::from_bits(sbits & 0x8000000000000000) } else { 0.0 };
        }
    }
    return 0x1p-1022 * y;
}

// exp.c
fn exp(x: f64) -> f64 {
    var abstop = ((libm::bits(x) >> 52) as! u32) & 0x7ff;
    // top12(0x1p-54) = 0x3c9, top12(512.0) = 0x408, top12(1024.0) = 0x409
    if abstop - 0x3c9 >= 0x408 - 0x3c9 {
        // Avoid spurious underflow for tiny x. Note: 0 is common input.
        if abstop - 0x3c9 >= 0x80000000 { return 1.0 + x; }
        if abstop >= 0x409 {
            if libm::bits(x) == 0xfff0000000000000 { return 0.0; }  // -inf
            if abstop >= 0x7ff { return 1.0 + x; }
            if (libm::bits(x) >> 63) != 0 { return libm::xflow(false, 0x1p-767); }  // __math_uflow(0)
            return libm::xflow(false, 0x1p769);  // __math_oflow(0)
        }
        // Large x is special cased below.
        abstop = 0;
    }
    // exp(x) = 2^(k/N) * exp(r), with exp(r) in [2^(-1/2N),2^(1/2N)].
    // x = ln2/N*k + r, with int k and r in [-ln2/2N, ln2/2N].
    let z = libm::exp_d(libm::EXP_INVLN2N) * x;
    // z - kd is in [-1, 1] in non-nearest rounding modes.
    let shift = libm::exp_d(libm::EXP_SHIFT);
    var kd = z + shift;
    let ki = libm::bits(kd);
    kd -= shift;
    let r = x + kd * libm::exp_d(libm::EXP_NEGLN2HIN) + kd * libm::exp_d(libm::EXP_NEGLN2LON);
    // 2^(k/N) ~= scale * (1 + tail).
    let idx = 2 * ((ki & 127) as! i64);
    let top = ki << 45;
    let tail = libm::exp_d(libm::EXP_TAB + idx);
    // This is only a valid scale when -1023*N < k < 1024*N.
    let sbits = libm::EXP_DATA[libm::EXP_TAB + idx + 1] + top;
    // exp(x) = 2^(k/N) * exp(r) ~= scale + scale * (tail + exp(r) - 1).
    let r2 = r * r;
    let P = libm::EXP_POLY;
    let tmp = tail + r + r2 * (libm::exp_d(P) + r * libm::exp_d(P + 1)) +
              r2 * r2 * (libm::exp_d(P + 2) + r * libm::exp_d(P + 3));
    if abstop == 0 { return libm::exp_special(tmp, sbits, ki, false); }
    let scale = libm::from_bits(sbits);
    return scale + scale * tmp;
}

// __math_xflow: y * y with the sign asked for, an overflow or underflow.
fn libm::xflow(negative: bool, y: f64) -> f64 { (if negative { -y } else { y }) * y }

let libm::LOG_LN2HI = 0;
let libm::LOG_LN2LO = 1;
let libm::LOG_A = 2;      // poly[5]
let libm::LOG_B = 7;      // poly1[11]
let libm::LOG_TAB = 18;   // [128] { invc, logc }
let libm::LOG_TAB2 = 274; // [128] { chi, clo }
fn libm::log_d(i: i64) -> f64 { libm::from_bits(libm::LOG_DATA[i]) }

// log.c
fn log(x: f64) -> f64 {
    var ix = libm::bits(x);
    let top = (ix >> 48) as! u32;
    let LO: u64 = 0x3fee000000000000;  // asuint64(1.0 - 0x1p-4)
    let HI: u64 = 0x3ff1090000000000;  // asuint64(1.0 + 0x1.09p-4)
    if ix - LO < HI - LO {
        // Handle close to 1.0 inputs separately.
        // Fix sign of zero with downward rounding when x==1.
        if ix == 0x3ff0000000000000 { return 0.0; }
        let B = libm::LOG_B;
        let r = x - 1.0;
        let r2 = r * r;
        let r3 = r * r2;
        var y = r3 * (libm::log_d(B + 1) + r * libm::log_d(B + 2) + r2 * libm::log_d(B + 3) +
                      r3 * (libm::log_d(B + 4) + r * libm::log_d(B + 5) + r2 * libm::log_d(B + 6) +
                            r3 * (libm::log_d(B + 7) + r * libm::log_d(B + 8) + r2 * libm::log_d(B + 9) +
                                  r3 * libm::log_d(B + 10))));
        // Worst-case error is around 0.507 ULP.
        var w = r * 0x1p27;
        let rhi = r + w - w;
        let rlo = r - rhi;
        w = rhi * rhi * libm::log_d(B);  // B[0] == -0.5.
        let hi = r + w;
        var lo = r - hi + w;
        lo += libm::log_d(B) * rlo * (rhi + r);
        y += lo;
        y += hi;
        return y;
    }
    if top - 0x0010 >= 0x7ff0 - 0x0010 {
        // x < 0x1p-1022 or inf or nan.
        if ix * 2 == 0 { return -1.0 / 0.0; }  // __math_divzero(1)
        if ix == 0x7ff0000000000000 { return x; }  // log(inf) == inf.
        if (top & 0x8000) != 0 || (top & 0x7ff0) == 0x7ff0 { return (x - x) / (x - x); }  // __math_invalid(x)
        // x is subnormal, normalize it.
        ix = libm::bits(x * 0x1p52);
        ix -= 52 << 52;
    }
    // x = 2^k z; where z is in range [OFF,2*OFF) and exact.
    // The range is split into N subintervals.
    // The ith subinterval contains z and c is near its center.
    let tmp = ix - 0x3fe6000000000000;  // OFF
    let i = ((tmp >> 45) & 127) as! i64;
    let k = (tmp as! i64) >> 52;  // arithmetic shift
    let iz = ix - (tmp & 0xfff0000000000000);
    let invc = libm::log_d(libm::LOG_TAB + 2 * i);
    let logc = libm::log_d(libm::LOG_TAB + 2 * i + 1);
    let z = libm::from_bits(iz);
    // log(x) = log1p(z/c-1) + log(c) + k*Ln2.
    // r ~= z/c - 1, |r| < 1/(2*N).
    // rounding error: 0x1p-55/N + 0x1p-66.
    let r = (z - libm::log_d(libm::LOG_TAB2 + 2 * i) - libm::log_d(libm::LOG_TAB2 + 2 * i + 1)) * invc;
    let kd: f64 = k;
    // hi + lo = r + log(c) + k*Ln2.
    let w = kd * libm::log_d(libm::LOG_LN2HI) + logc;
    let hi = w + r;
    let lo = w - hi + r + kd * libm::log_d(libm::LOG_LN2LO);
    // log(x) = lo + (log1p(r) - r) + hi.
    let r2 = r * r;  // rounding error: 0x1p-54/N^2.
    let A = libm::LOG_A;
    return lo + r2 * libm::log_d(A) +
           r * r2 * (libm::log_d(A + 1) + r * libm::log_d(A + 2) +
                     r2 * (libm::log_d(A + 3) + r * libm::log_d(A + 4))) + hi;
}

let libm::LOG2_INVLN2HI = 0;
let libm::LOG2_INVLN2LO = 1;
let libm::LOG2_A = 2;      // poly[6]
let libm::LOG2_B = 8;      // poly1[10]
let libm::LOG2_TAB = 18;   // [64] { invc, logc }
let libm::LOG2_TAB2 = 146; // [64] { chi, clo }
fn libm::log2_d(i: i64) -> f64 { libm::from_bits(libm::LOG2_DATA[i]) }

// log2.c
fn log2(x: f64) -> f64 {
    var ix = libm::bits(x);
    let top = (ix >> 48) as! u32;
    let LO: u64 = 0x3feea4af00000000;  // asuint64(1.0 - 0x1.5b51p-5)
    let HI: u64 = 0x3ff0b55900000000;  // asuint64(1.0 + 0x1.6ab2p-5)
    let invln2hi = libm::log2_d(libm::LOG2_INVLN2HI);
    let invln2lo = libm::log2_d(libm::LOG2_INVLN2LO);
    if ix - LO < HI - LO {
        // Handle close to 1.0 inputs separately.
        // Fix sign of zero with downward rounding when x==1.
        if ix == 0x3ff0000000000000 { return 0.0; }
        let B = libm::LOG2_B;
        let r = x - 1.0;
        let rhi = libm::from_bits(libm::bits(r) & 0xffffffff00000000);
        let rlo = r - rhi;
        let hi = rhi * invln2hi;
        var lo = rlo * invln2hi + r * invln2lo;
        let r2 = r * r;  // rounding error: 0x1p-62.
        let r4 = r2 * r2;
        // Worst-case error is less than 0.54 ULP (0.55 ULP without fma).
        let p = r2 * (libm::log2_d(B) + r * libm::log2_d(B + 1));
        var y = hi + p;
        lo += hi - y + p;
        lo += r4 * (libm::log2_d(B + 2) + r * libm::log2_d(B + 3) +
                    r2 * (libm::log2_d(B + 4) + r * libm::log2_d(B + 5)) +
                    r4 * (libm::log2_d(B + 6) + r * libm::log2_d(B + 7) +
                          r2 * (libm::log2_d(B + 8) + r * libm::log2_d(B + 9))));
        y += lo;
        return y;
    }
    if top - 0x0010 >= 0x7ff0 - 0x0010 {
        // x < 0x1p-1022 or inf or nan.
        if ix * 2 == 0 { return -1.0 / 0.0; }  // __math_divzero(1)
        if ix == 0x7ff0000000000000 { return x; }  // log(inf) == inf.
        if (top & 0x8000) != 0 || (top & 0x7ff0) == 0x7ff0 { return (x - x) / (x - x); }  // __math_invalid(x)
        // x is subnormal, normalize it.
        ix = libm::bits(x * 0x1p52);
        ix -= 52 << 52;
    }
    // x = 2^k z; where z is in range [OFF,2*OFF) and exact.
    let tmp = ix - 0x3fe6000000000000;  // OFF
    let i = ((tmp >> 46) & 63) as! i64;
    let k = (tmp as! i64) >> 52;  // arithmetic shift
    let iz = ix - (tmp & 0xfff0000000000000);
    let invc = libm::log2_d(libm::LOG2_TAB + 2 * i);
    let logc = libm::log2_d(libm::LOG2_TAB + 2 * i + 1);
    let z = libm::from_bits(iz);
    let kd: f64 = k;
    // log2(x) = log2(z/c) + log2(c) + k.
    // r ~= z/c - 1, |r| < 1/(2*N).
    // rounding error: 0x1p-55/N + 0x1p-65.
    let r = (z - libm::log2_d(libm::LOG2_TAB2 + 2 * i) - libm::log2_d(libm::LOG2_TAB2 + 2 * i + 1)) * invc;
    let rhi = libm::from_bits(libm::bits(r) & 0xffffffff00000000);
    let rlo = r - rhi;
    let t1 = rhi * invln2hi;
    let t2 = rlo * invln2hi + r * invln2lo;
    // hi + lo = r/ln2 + log2(c) + k.
    let t3 = kd + logc;
    let hi = t3 + t1;
    let lo = t3 - hi + t1 + t2;
    // log2(r+1) = r/ln2 + r^2*poly(r).
    let r2 = r * r;  // rounding error: 0x1p-54/N^2.
    let r4 = r2 * r2;
    let A = libm::LOG2_A;
    let p = libm::log2_d(A) + r * libm::log2_d(A + 1) + r2 * (libm::log2_d(A + 2) + r * libm::log2_d(A + 3)) +
            r4 * (libm::log2_d(A + 4) + r * libm::log2_d(A + 5));
    return lo + r2 * p + hi;
}

// log10.c (from FreeBSD's e_log10.c, Sun's notice above).
fn log10(x0: f64) -> f64 {
    let ivln10hi: f64 = 4.34294481878168880939e-01;   // 0x3fdbcb7b, 0x15200000
    let ivln10lo: f64 = 2.50829467116452752298e-11;   // 0x3dbb9438, 0xca9aadd5
    let log10_2hi: f64 = 3.01029995663611771306e-01;  // 0x3FD34413, 0x509F6000
    let log10_2lo: f64 = 3.69423907715893078616e-13;  // 0x3D59FEF3, 0x11F12B36
    let Lg1: f64 = 6.666666666666735130e-01;  // 3FE55555 55555593
    let Lg2: f64 = 3.999999999940941908e-01;  // 3FD99999 9997FA04
    let Lg3: f64 = 2.857142874366239149e-01;  // 3FD24924 94229359
    let Lg4: f64 = 2.222219843214978396e-01;  // 3FCC71C5 1D8E78AF
    let Lg5: f64 = 1.818357216161805012e-01;  // 3FC74664 96CB03DE
    let Lg6: f64 = 1.531383769920937332e-01;  // 3FC39A09 D078C69F
    let Lg7: f64 = 1.479819860511658591e-01;  // 3FC2F112 DF3E5244
    var x = x0;
    var ui = libm::bits(x);
    var hx = (ui >> 32) as! u32;
    var k: i32 = 0;
    if hx < 0x00100000 || (hx >> 31) != 0 {
        if ui << 1 == 0 { return -1.0 / (x * x); }  // log(+-0)=-inf
        if (hx >> 31) != 0 { return (x - x) / 0.0; }  // log(-#) = NaN
        // subnormal number, scale x up
        k -= 54;
        x *= 0x1p54;
        ui = libm::bits(x);
        hx = (ui >> 32) as! u32;
    } else if hx >= 0x7ff00000 {
        return x;
    } else if hx == 0x3ff00000 && ui << 32 == 0 {
        return 0.0;
    }
    // reduce x into [sqrt(2)/2, sqrt(2)]
    hx += 0x3ff00000 - 0x3fe6a09e;
    k += ((hx >> 20) as! i32) - 0x3ff;
    hx = (hx & 0x000fffff) + 0x3fe6a09e;
    ui = ((hx as u64) << 32) | (ui & 0xffffffff);
    x = libm::from_bits(ui);
    let f = x - 1.0;
    let hfsq = 0.5 * f * f;
    let s = f / (2.0 + f);
    let z = s * s;
    var w = z * z;
    let t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
    let t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
    let R = t2 + t1;
    // See log2.c for details.
    // hi+lo = f - hfsq + s*(hfsq+R) ~ log(1+f)
    let hi = libm::zero_low(f - hfsq);
    let lo = f - hi - hfsq + s * (hfsq + R);
    // val_hi+val_lo ~ log10(1+f) + k*log10(2)
    var val_hi = hi * ivln10hi;
    let dk: f64 = k;
    let y = dk * log10_2hi;
    var val_lo = dk * log10_2lo + (lo + hi) * ivln10lo + lo * ivln10hi;
    // Extra precision in for adding y is not strictly needed since there is
    // no very large cancellation near x = sqrt(2) or x = 1/sqrt(2), but we
    // do it anyway since it costs little on CPUs with some parallelism and
    // it reduces the error for many args.
    w = y + val_hi;
    val_lo += (y - w) + val_hi;
    val_hi = w;
    return val_lo + val_hi;
}

let libm::POW_LN2HI = 0;
let libm::POW_LN2LO = 1;
let libm::POW_A = 2;    // poly[7]
let libm::POW_TAB = 9;  // [128] { invc, pad, logc, logctail }
fn libm::pow_d(i: i64) -> f64 { libm::from_bits(libm::POW_LOG_DATA[i]) }

// pow.c's log_inline: log(x) as y + tail, about 15 bits more precise. ix
// is the bits of x, normalized in the subnormal range using the sign bit
// for the exponent.
fn libm::pow_log(ix: u64) -> f64, f64 {
    // x = 2^k z; where z is in range [OFF,2*OFF) and exact.
    let tmp = ix - 0x3fe6955500000000;  // OFF
    let i = ((tmp >> 45) & 127) as! i64;
    let k = (tmp as! i64) >> 52;  // arithmetic shift
    let iz = ix - (tmp & 0xfff0000000000000);
    let z = libm::from_bits(iz);
    let kd: f64 = k;
    // log(x) = k*Ln2 + log(c) + log1p(z/c-1).
    let invc = libm::pow_d(libm::POW_TAB + 4 * i);
    let logc = libm::pow_d(libm::POW_TAB + 4 * i + 2);
    let logctail = libm::pow_d(libm::POW_TAB + 4 * i + 3);
    // Note: 1/c is j/N or j/N/2 where j is an integer in [N,2N) and
    // |z/c - 1| < 1/N, so r = z/c - 1 is exactly representible.
    // Split z such that rhi, rlo and rhi*rhi are exact and |rlo| <= |r|.
    let zhi = libm::from_bits((iz + (1 << 31)) & 0xffffffff00000000);
    let zlo = z - zhi;
    let rhi = zhi * invc - 1.0;
    let rlo = zlo * invc;
    let r = rhi + rlo;
    // k*Ln2 + log(c) + r.
    let t1 = kd * libm::pow_d(libm::POW_LN2HI) + logc;
    let t2 = t1 + r;
    let lo1 = kd * libm::pow_d(libm::POW_LN2LO) + logctail;
    let lo2 = t1 - t2 + r;
    let A = libm::POW_A;
    let ar = libm::pow_d(A) * r;  // A[0] = -0.5.
    let ar2 = r * ar;
    let ar3 = r * ar2;
    // k*Ln2 + log(c) + r + A[0]*r*r.
    let arhi = libm::pow_d(A) * rhi;
    let arhi2 = rhi * arhi;
    let hi = t2 + arhi2;
    let lo3 = rlo * (ar + arhi);
    let lo4 = t2 - hi + arhi2;
    // p = log1p(r) - r - A[0]*r*r.
    let p = ar3 * (libm::pow_d(A + 1) + r * libm::pow_d(A + 2) +
                   ar2 * (libm::pow_d(A + 3) + r * libm::pow_d(A + 4) +
                          ar2 * (libm::pow_d(A + 5) + r * libm::pow_d(A + 6))));
    let lo = lo1 + lo2 + lo3 + lo4 + p;
    let y = hi + lo;
    return y, hi - y + lo;
}

// pow.c's exp_inline: sign*exp(x+xtail), where |xtail| < 2^-8/N and
// |xtail| <= |x|; sign_bias is SIGN_BIAS (0x800 << 7) or 0, the sign -1 or 1.
fn libm::pow_exp(x: f64, xtail: f64, sign_bias: u32) -> f64 {
    var abstop = ((libm::bits(x) >> 52) as! u32) & 0x7ff;
    if abstop - 0x3c9 >= 0x408 - 0x3c9 {
        if abstop - 0x3c9 >= 0x80000000 {
            // Avoid spurious underflow for tiny x. Note: 0 is common input.
            let one = 1.0 + x;
            return if sign_bias != 0 { -one } else { one };
        }
        if abstop >= 0x409 {
            // Note: inf and nan are already handled.
            if (libm::bits(x) >> 63) != 0 { return libm::xflow(sign_bias != 0, 0x1p-767); }  // __math_uflow
            return libm::xflow(sign_bias != 0, 0x1p769);  // __math_oflow
        }
        // Large x is special cased below.
        abstop = 0;
    }
    // exp(x) = 2^(k/N) * exp(r), with exp(r) in [2^(-1/2N),2^(1/2N)].
    let z = libm::exp_d(libm::EXP_INVLN2N) * x;
    let shift = libm::exp_d(libm::EXP_SHIFT);
    var kd = z + shift;
    let ki = libm::bits(kd);
    kd -= shift;
    var r = x + kd * libm::exp_d(libm::EXP_NEGLN2HIN) + kd * libm::exp_d(libm::EXP_NEGLN2LON);
    // The code assumes 2^-200 < |xtail| < 2^-8/N.
    r += xtail;
    // 2^(k/N) ~= scale * (1 + tail).
    let idx = 2 * ((ki & 127) as! i64);
    let top = (ki + sign_bias) << 45;
    let tail = libm::exp_d(libm::EXP_TAB + idx);
    // This is only a valid scale when -1023*N < k < 1024*N.
    let sbits = libm::EXP_DATA[libm::EXP_TAB + idx + 1] + top;
    let r2 = r * r;
    let P = libm::EXP_POLY;
    let tmp = tail + r + r2 * (libm::exp_d(P) + r * libm::exp_d(P + 1)) +
              r2 * r2 * (libm::exp_d(P + 2) + r * libm::exp_d(P + 3));
    if abstop == 0 { return libm::exp_special(tmp, sbits, ki, true); }
    let scale = libm::from_bits(sbits);
    return scale + scale * tmp;
}

// pow.c's checkint: 0 if not an integer, 1 if an odd one, 2 if an even
// one; iy the bits of a non-zero finite value.
fn libm::checkint(iy: u64) -> i32 {
    let e = ((iy >> 52) & 0x7ff) as! i32;
    if e < 0x3ff { return 0; }
    if e > 0x3ff + 52 { return 2; }
    let s = (0x3ff + 52 - e) as! u64;
    if (iy & ((1 << s) - 1)) != 0 { return 0; }
    if (iy & (1 << s)) != 0 { return 1; }
    return 2;
}

// pow.c's zeroinfnan: whether i is the bits of 0, infinity or nan.
fn libm::zeroinfnan(i: u64) -> bool { 2 * i - 1 >= 0xffdfffffffffffff }

// pow.c
fn pow(x: f64, y: f64) -> f64 {
    var sign_bias: u32 = 0;
    var ix = libm::bits(x);
    let iy = libm::bits(y);
    var topx = (ix >> 52) as! u32;
    let topy = (iy >> 52) as! u32;
    if topx - 0x001 >= 0x7ff - 0x001 || (topy & 0x7ff) - 0x3be >= 0x43e - 0x3be {
        // Note: if |y| > 1075 * ln2 * 2^53 ~= 0x1.749p62 then pow(x,y) =
        // inf/0 and if |y| < 2^-54 / 1075 ~= 0x1.e7b6p-65 then pow(x,y) = +-1.
        // Special cases: (x < 0x1p-126 or inf or nan) or
        // (|y| < 0x1p-65 or |y| >= 0x1p63 or nan).
        if libm::zeroinfnan(iy) {
            if 2 * iy == 0 { return 1.0; }
            if ix == 0x3ff0000000000000 { return 1.0; }
            if 2 * ix > 0xffe0000000000000 || 2 * iy > 0xffe0000000000000 { return x + y; }
            if 2 * ix == 0x7fe0000000000000 { return 1.0; }
            // |x|<1 && y==inf or |x|>1 && y==-inf.
            if (2 * ix < 0x7fe0000000000000) == ((iy >> 63) == 0) { return 0.0; }
            return y * y;
        }
        if libm::zeroinfnan(ix) {
            var x2 = x * x;
            if (ix >> 63) != 0 && libm::checkint(iy) == 1 { x2 = -x2; }
            return if (iy >> 63) != 0 { 1.0 / x2 } else { x2 };
        }
        // Here x and y are non-zero finite.
        if (ix >> 63) != 0 {
            // Finite x < 0.
            let yint = libm::checkint(iy);
            if yint == 0 { return (x - x) / (x - x); }  // __math_invalid(x)
            if yint == 1 { sign_bias = 0x800 << 7; }  // SIGN_BIAS
            ix &= 0x7fffffffffffffff;
            topx &= 0x7ff;
        }
        if (topy & 0x7ff) - 0x3be >= 0x43e - 0x3be {
            // Note: sign_bias == 0 here because y is not odd.
            if ix == 0x3ff0000000000000 { return 1.0; }
            if (topy & 0x7ff) < 0x3be {
                // |y| < 2^-65, x^y ~= 1 + y*log(x).
                return if ix > 0x3ff0000000000000 { 1.0 + y } else { 1.0 - y };
            }
            if (ix > 0x3ff0000000000000) == (topy < 0x800) { return libm::xflow(false, 0x1p769); }  // __math_oflow(0)
            return libm::xflow(false, 0x1p-767);  // __math_uflow(0)
        }
        if topx == 0 {
            // Normalize subnormal x so exponent becomes negative.
            ix = libm::bits(x * 0x1p52);
            ix &= 0x7fffffffffffffff;
            ix -= 52 << 52;
        }
    }
    let hi, lo = libm::pow_log(ix);
    let yhi = libm::from_bits(iy & 0xfffffffff8000000);  // -1ULL << 27
    let ylo = y - yhi;
    let lhi = libm::from_bits(libm::bits(hi) & 0xfffffffff8000000);
    let llo = hi - lhi + lo;
    let ehi = yhi * lhi;
    let elo = ylo * lhi + y * llo;  // |elo| < |ehi| * 2^-25.
    return libm::pow_exp(ehi, elo, sign_bias);
}

// ---- The f32 functions ----
//
// musl computes these in double where its C does (double, double_t, a
// float promoted by a double operand), and so do these: f64 values, a float
// widened with `as f64` where C promotes it, rounded to f32 where C
// converts back. float_t is float (FLT_EVAL_METHOD 0).

// __sindf.c: sin on [-pi/4, pi/4], in double.
fn libm::k_sinf(x: f64) -> f32 {
    let S1: f64 = -0x15555554cbac77.0p-55;  // -0.166666666416265235595
    let S2: f64 = 0x111110896efbb2.0p-59;   //  0.0083333293858894631756
    let S3: f64 = -0x1a00f9e2cae774.0p-65;  // -0.000198393348360966317347
    let S4: f64 = 0x16cd878c3b46a7.0p-71;   //  0.0000027183114939898219064
    // Try to optimize for parallel evaluation as in __tandf.c.
    let z = x * x;
    let w = z * z;
    let r = S3 + z * S4;
    let s = z * x;
    return ((x + s * (S1 + z * S2)) + s * w * r) as f32;
}

// __cosdf.c: cos on [-pi/4, pi/4], in double.
fn libm::k_cosf(x: f64) -> f32 {
    let C0: f64 = -0x1ffffffd0c5e81.0p-54;  // -0.499999997251031003120
    let C1: f64 = 0x155553e1053a42.0p-57;   //  0.0416666233237390631894
    let C2: f64 = -0x16c087e80f1e27.0p-62;  // -0.00138867637746099294692
    let C3: f64 = 0x199342e0ee5069.0p-68;   //  0.0000243904487962774090654
    // Try to optimize for parallel evaluation as in __tandf.c.
    let z = x * x;
    let w = z * z;
    let r = C2 + z * C3;
    return (((1.0 + z * C0) + w * C1) + (w * z) * r) as f32;
}

// __tandf.c: tan on [-pi/4, pi/4] in double, or -1/tan where odd.
fn libm::k_tanf(x: f64, odd: i32) -> f32 {
    let T0: f64 = 0x15554d3418c99f.0p-54;  // 0.333331395030791399758
    let T1: f64 = 0x1112fd38999f72.0p-55;  // 0.133392002712976742718
    let T2: f64 = 0x1b54c91d865afe.0p-57;  // 0.0533812378445670393523
    let T3: f64 = 0x191df3908c33ce.0p-58;  // 0.0245283181166547278873
    let T4: f64 = 0x185dadfcecf44e.0p-61;  // 0.00297435743359967304927
    let T5: f64 = 0x1362b9bf971bcd.0p-59;  // 0.00946564784943673166728
    let z = x * x;
    // Split up the polynomial into small independent terms to give
    // opportunities for parallel evaluation (see musl's __tandf.c).
    var r = T4 + z * T5;
    let t = T2 + z * T3;
    let w = z * z;
    let s = z * x;
    let u = T0 + z * T1;
    r = (x + s * u) + (s * w) * (t + w * r);
    return (if odd != 0 { -1.0 / r } else { r }) as f32;
}

// __rem_pio2f.c: x rem pi/2, as n and y (a double). The caller handles
// |x| ~<= pi/4.
fn libm::rem_pio2f(x: f32) -> i32, f64 {
    let toint: f64 = 1.5 / 2.22044604925031308085e-16;  // 1.5/DBL_EPSILON
    let pio4: f64 = 0x1.921fb6p-1;
    let invpio2: f64 = 6.36619772367581382433e-01;  // 0x3FE45F30, 0x6DC9C883
    let pio2_1: f64 = 1.57079631090164184570e+00;   // 0x3FF921FB, 0x50000000
    let pio2_1t: f64 = 1.58932547735281966916e-08;  // 0x3E5110b4, 0x611A6263
    let ui = libm::bits(x);
    let ix = ui & 0x7fffffff;
    let xd = x as f64;
    // 25+53 bit pi is good enough for medium size
    if ix < 0x4dc90fdb {  // |x| ~< 2^28*(pi/2), medium size
        // Use a specialized rint() to get fn.
        var fnv = xd * invpio2 + toint - toint;
        var n = fnv as! i32;
        var y = xd - fnv * pio2_1 - fnv * pio2_1t;
        // Matters with directed rounding.
        if y < -pio4 {
            n--;
            fnv -= 1.0;
            y = xd - fnv * pio2_1 - fnv * pio2_1t;
        } else if y > pio4 {
            n++;
            fnv += 1.0;
            y = xd - fnv * pio2_1 - fnv * pio2_1t;
        }
        return n, y;
    }
    if ix >= 0x7f800000 {  // x is inf or NaN
        return 0, x - x;
    }
    // scale x into [2^23, 2^24-1]
    let sign = (ui >> 31) != 0;
    let e0 = ((ix >> 23) as! i32) - (0x7f + 23);  // e0 = ilogb(|x|)-23, positive
    var tx: f64[3] = [0.0; 3];
    tx[0] = libm::from_bits(ix - ((e0 as! u32) << 23));
    let n, ty0, ty1 = libm::rem_pio2_large(tx, e0, 1, 0);
    if sign { return -n, -ty0; }
    return n, ty0;
}

// 1..4 times M_PI_2, in double (sinf.c, cosf.c, tanf.c).
let libm::PIO2_1X: f64 = 1.0 * 1.57079632679489661923;  // 0x3FF921FB, 0x54442D18
let libm::PIO2_2X: f64 = 2.0 * 1.57079632679489661923;  // 0x400921FB, 0x54442D18
let libm::PIO2_3X: f64 = 3.0 * 1.57079632679489661923;  // 0x4012D97C, 0x7F3321D2
let libm::PIO2_4X: f64 = 4.0 * 1.57079632679489661923;  // 0x401921FB, 0x54442D18

// sinf.c
fn sin(x: f32) -> f32 {
    let s1pio2 = libm::PIO2_1X;
    let s2pio2 = libm::PIO2_2X;
    let s3pio2 = libm::PIO2_3X;
    let s4pio2 = libm::PIO2_4X;
    var ix = libm::bits(x);
    let sign = (ix >> 31) != 0;
    ix &= 0x7fffffff;
    let xd = x as f64;
    if ix <= 0x3f490fda {  // |x| ~<= pi/4
        if ix < 0x39800000 { return x; }  // |x| < 2**-12
        return libm::k_sinf(xd);
    }
    if ix <= 0x407b53d1 {  // |x| ~<= 5*pi/4
        if ix <= 0x4016cbe3 {  // |x| ~<= 3pi/4
            if sign { return -libm::k_cosf(xd + s1pio2); }
            return libm::k_cosf(xd - s1pio2);
        }
        return libm::k_sinf(if sign { -(xd + s2pio2) } else { -(xd - s2pio2) });
    }
    if ix <= 0x40e231d5 {  // |x| ~<= 9*pi/4
        if ix <= 0x40afeddf {  // |x| ~<= 7*pi/4
            if sign { return libm::k_cosf(xd + s3pio2); }
            return -libm::k_cosf(xd - s3pio2);
        }
        return libm::k_sinf(if sign { xd + s4pio2 } else { xd - s4pio2 });
    }
    // sin(Inf or NaN) is NaN
    if ix >= 0x7f800000 { return x - x; }
    // general argument reduction needed
    let n, y = libm::rem_pio2f(x);
    let k = n & 3;
    if k == 0 { return libm::k_sinf(y); }
    if k == 1 { return libm::k_cosf(y); }
    if k == 2 { return libm::k_sinf(-y); }
    return -libm::k_cosf(y);
}

// cosf.c
fn cos(x: f32) -> f32 {
    let c1pio2 = libm::PIO2_1X;
    let c2pio2 = libm::PIO2_2X;
    let c3pio2 = libm::PIO2_3X;
    let c4pio2 = libm::PIO2_4X;
    var ix = libm::bits(x);
    let sign = (ix >> 31) != 0;
    ix &= 0x7fffffff;
    let xd = x as f64;
    if ix <= 0x3f490fda {  // |x| ~<= pi/4
        if ix < 0x39800000 { return 1.0; }  // |x| < 2**-12
        return libm::k_cosf(xd);
    }
    if ix <= 0x407b53d1 {  // |x| ~<= 5*pi/4
        if ix > 0x4016cbe3 {  // |x|  ~> 3*pi/4
            return -libm::k_cosf(if sign { xd + c2pio2 } else { xd - c2pio2 });
        }
        if sign { return libm::k_sinf(xd + c1pio2); }
        return libm::k_sinf(c1pio2 - xd);
    }
    if ix <= 0x40e231d5 {  // |x| ~<= 9*pi/4
        if ix > 0x40afeddf {  // |x| ~> 7*pi/4
            return libm::k_cosf(if sign { xd + c4pio2 } else { xd - c4pio2 });
        }
        if sign { return libm::k_sinf(-xd - c3pio2); }
        return libm::k_sinf(xd - c3pio2);
    }
    // cos(Inf or NaN) is NaN
    if ix >= 0x7f800000 { return x - x; }
    // general argument reduction needed
    let n, y = libm::rem_pio2f(x);
    let k = n & 3;
    if k == 0 { return libm::k_cosf(y); }
    if k == 1 { return libm::k_sinf(-y); }
    if k == 2 { return -libm::k_cosf(y); }
    return libm::k_sinf(y);
}

// tanf.c
fn tan(x: f32) -> f32 {
    let t1pio2 = libm::PIO2_1X;
    let t2pio2 = libm::PIO2_2X;
    let t3pio2 = libm::PIO2_3X;
    let t4pio2 = libm::PIO2_4X;
    var ix = libm::bits(x);
    let sign = (ix >> 31) != 0;
    ix &= 0x7fffffff;
    let xd = x as f64;
    if ix <= 0x3f490fda {  // |x| ~<= pi/4
        if ix < 0x39800000 { return x; }  // |x| < 2**-12
        return libm::k_tanf(xd, 0);
    }
    if ix <= 0x407b53d1 {  // |x| ~<= 5*pi/4
        if ix <= 0x4016cbe3 {  // |x| ~<= 3pi/4
            return libm::k_tanf(if sign { xd + t1pio2 } else { xd - t1pio2 }, 1);
        }
        return libm::k_tanf(if sign { xd + t2pio2 } else { xd - t2pio2 }, 0);
    }
    if ix <= 0x40e231d5 {  // |x| ~<= 9*pi/4
        if ix <= 0x40afeddf {  // |x| ~<= 7*pi/4
            return libm::k_tanf(if sign { xd + t3pio2 } else { xd - t3pio2 }, 1);
        }
        return libm::k_tanf(if sign { xd + t4pio2 } else { xd - t4pio2 }, 0);
    }
    // tan(Inf or NaN) is NaN
    if ix >= 0x7f800000 { return x - x; }
    // argument reduction
    let n, y = libm::rem_pio2f(x);
    return libm::k_tanf(y, n & 1);
}

// asinf.c and acosf.c's R(), in float.
fn libm::asinf_r(z: f32) -> f32 {
    let pS0: f32 = 1.6666586697e-01;
    let pS1: f32 = -4.2743422091e-02;
    let pS2: f32 = -8.6563630030e-03;
    let qS1: f32 = -7.0662963390e-01;
    let p = z * (pS0 + z * (pS1 + z * pS2));
    let q: f32 = 1.0 + z * qS1;
    return p / q;
}

// asinf.c
fn asin(x: f32) -> f32 {
    let pio2: f64 = 1.570796326794896558e+00;
    let hx = libm::bits(x);
    let ix = hx & 0x7fffffff;
    if ix >= 0x3f800000 {  // |x| >= 1
        // asin(+-1) = +-pi/2 with inexact
        if ix == 0x3f800000 { return (x * pio2 + 0x1p-120) as f32; }
        return 0.0 / (x - x);  // asin(|x|>1) is NaN
    }
    if ix < 0x3f000000 {  // |x| < 0.5
        // if 0x1p-126 <= |x| < 0x1p-12, avoid raising underflow
        if ix < 0x39800000 && ix >= 0x00800000 { return x; }
        return x + x * libm::asinf_r(x * x);
    }
    // 1 > |x| >= 0.5
    let z: f32 = (1.0 - libm::fabs(x)) * 0.5;
    let s = sqrt(z as f64);
    let r = (pio2 - 2.0 * (s + s * libm::asinf_r(z))) as f32;
    if (hx >> 31) != 0 { return -r; }
    return r;
}

// acosf.c
fn acos(x: f32) -> f32 {
    let pio2_hi: f32 = 1.5707962513e+00;  // 0x3fc90fda
    let pio2_lo: f32 = 7.5497894159e-08;  // 0x33a22168
    let hx = libm::bits(x);
    let ix = hx & 0x7fffffff;
    // |x| >= 1 or nan
    if ix >= 0x3f800000 {
        if ix == 0x3f800000 {
            if (hx >> 31) != 0 { return 2.0 * pio2_hi + 0x1p-120; }
            return 0.0;
        }
        return 0.0 / (x - x);
    }
    // |x| < 0.5
    if ix < 0x3f000000 {
        if ix <= 0x32800000 { return pio2_hi + 0x1p-120; }  // |x| < 2**-26
        return pio2_hi - (x - (pio2_lo - x * libm::asinf_r(x * x)));
    }
    // x < -0.5
    if (hx >> 31) != 0 {
        let z: f32 = (1.0 + x) * 0.5;
        let s = sqrt(z);
        let w = libm::asinf_r(z) * s - pio2_lo;
        return 2.0 * (pio2_hi - (s + w));
    }
    // x > 0.5
    let z: f32 = (1.0 - x) * 0.5;
    let s = sqrt(z);
    let df = libm::from_bits(libm::bits(s) & 0xfffff000);
    let c = (z - df * df) / (s + df);
    let w = libm::asinf_r(z) * s + c;
    return 2.0 * (df + w);
}

// atanf.c
fn atan(x0: f32) -> f32 {
    let atanhi: f32[4] = [
        4.6364760399e-01,  // atan(0.5)hi 0x3eed6338
        7.8539812565e-01,  // atan(1.0)hi 0x3f490fda
        9.8279368877e-01,  // atan(1.5)hi 0x3f7b985e
        1.5707962513e+00,  // atan(inf)hi 0x3fc90fda
    ];
    let atanlo: f32[4] = [
        5.0121582440e-09,  // atan(0.5)lo 0x31ac3769
        3.7748947079e-08,  // atan(1.0)lo 0x33222168
        3.4473217170e-08,  // atan(1.5)lo 0x33140fb4
        7.5497894159e-08,  // atan(inf)lo 0x33a22168
    ];
    let aT: f32[5] = [3.3333328366e-01, -1.9999158382e-01, 1.4253635705e-01, -1.0648017377e-01, 6.1687607318e-02];
    var x = x0;
    var ix = libm::bits(x);
    let sign = ix >> 31;
    ix &= 0x7fffffff;
    var id: i32 = 0;
    if ix >= 0x4c800000 {  // if |x| >= 2**26
        if x != x { return x; }
        let z = atanhi[3] + 0x1p-120;
        return if sign != 0 { -z } else { z };
    }
    if ix < 0x3ee00000 {  // |x| < 0.4375
        if ix < 0x39800000 { return x; }  // |x| < 2**-12
        id = -1;
    } else {
        x = libm::fabs(x);
        if ix < 0x3f980000 {  // |x| < 1.1875
            if ix < 0x3f300000 {  //  7/16 <= |x| < 11/16
                id = 0;
                x = (2.0 * x - 1.0) / (2.0 + x);
            } else {  // 11/16 <= |x| < 19/16
                id = 1;
                x = (x - 1.0) / (x + 1.0);
            }
        } else {
            if ix < 0x401c0000 {  // |x| < 2.4375
                id = 2;
                x = (x - 1.5) / (1.0 + 1.5 * x);
            } else {  // 2.4375 <= |x| < 2**26
                id = 3;
                x = -1.0 / x;
            }
        }
    }
    // end of argument reduction
    var z = x * x;
    let w = z * z;
    // break sum from i=0 to 10 aT[i]z**(i+1) into odd and even poly
    let s1 = z * (aT[0] + w * (aT[2] + w * aT[4]));
    let s2 = w * (aT[1] + w * aT[3]);
    if id < 0 { return x - x * (s1 + s2); }
    z = atanhi[id] - ((x * (s1 + s2) - atanlo[id]) - x);
    return if sign != 0 { -z } else { z };
}

// atan2f.c
fn atan2(y: f32, x: f32) -> f32 {
    let pi: f32 = 3.1415927410e+00;      // 0x40490fdb
    let pi_lo: f32 = -8.7422776573e-08;  // 0xb3bbbd2e
    if x != x || y != y { return x + y; }
    var ix = libm::bits(x);
    var iy = libm::bits(y);
    if ix == 0x3f800000 { return atan(y); }  // x=1.0
    let m = ((iy >> 31) & 1) | ((ix >> 30) & 2);  // 2*sign(x)+sign(y)
    ix &= 0x7fffffff;
    iy &= 0x7fffffff;
    // when y = 0
    if iy == 0 {
        if m == 0 || m == 1 { return y; }  // atan(+-0,+anything)=+-0
        if m == 2 { return pi; }           // atan(+0,-anything) = pi
        return -pi;                        // atan(-0,-anything) =-pi
    }
    // when x = 0
    if ix == 0 { return if (m & 1) != 0 { -pi / 2.0 } else { pi / 2.0 }; }
    // when x is INF
    if ix == 0x7f800000 {
        if iy == 0x7f800000 {
            if m == 0 { return pi / 4.0; }        // atan(+INF,+INF)
            if m == 1 { return -pi / 4.0; }       // atan(-INF,+INF)
            if m == 2 { return 3.0 * pi / 4.0; }  // atan(+INF,-INF)
            return -3.0 * pi / 4.0;               // atan(-INF,-INF)
        } else {
            if m == 0 { return 0.0; }   // atan(+...,+INF)
            if m == 1 { return -0.0; }  // atan(-...,+INF)
            if m == 2 { return pi; }    // atan(+...,-INF)
            return -pi;                 // atan(-...,-INF)
        }
    }
    // |y/x| > 0x1p26
    if ix + (26 << 23) < iy || iy == 0x7f800000 {
        return if (m & 1) != 0 { -pi / 2.0 } else { pi / 2.0 };
    }
    // z = atan(|y/x|) with correct underflow
    var z: f32 = 0.0;
    if (m & 2) != 0 && iy + (26 << 23) < ix {  // |y/x| < 0x1p-26, x < 0
        z = 0.0;
    } else {
        z = atan(libm::fabs(y / x));
    }
    if m == 0 { return z; }                  // atan(+,+)
    if m == 1 { return -z; }                 // atan(-,+)
    if m == 2 { return pi - (z - pi_lo); }   // atan(+,-)
    return (z - pi_lo) - pi;                 // atan(-,-)
}

// exp2f_data.c's fields, as offsets (the table is at the end of this file).
let libm::EXP2F_TAB = 0;            // [32]
let libm::EXP2F_SHIFT_SCALED = 32;
let libm::EXP2F_POLY = 33;          // [3]
let libm::EXP2F_SHIFT = 36;
let libm::EXP2F_INVLN2_SCALED = 37;
let libm::EXP2F_POLY_SCALED = 38;   // [3]
fn libm::exp2f_d(i: i64) -> f64 { libm::from_bits(libm::EXP2F_DATA[i]) }

// __math_xflowf: y * y in float with the sign asked for.
fn libm::xflowf(negative: bool, y: f32) -> f32 { (if negative { -y } else { y }) * y }

// expf.c
fn exp(x: f32) -> f32 {
    let xd = x as f64;
    let abstop = (libm::bits(x) >> 20) & 0x7ff;
    if abstop >= 0x42b {  // top12(88.0f)
        // |x| >= 88 or x is nan.
        if libm::bits(x) == 0xff800000 { return 0.0; }  // -inf
        if abstop >= 0x7f8 { return x + x; }  // top12(INFINITY)
        if x > 0x1.62e42ep6 { return libm::xflowf(false, 0x1p97); }    // x > log(0x1p128) ~= 88.72: __math_oflowf(0)
        if x < -0x1.9fe368p6 { return libm::xflowf(false, 0x1p-95); }  // x < log(0x1p-150) ~= -103.97: __math_uflowf(0)
    }
    // x*N/Ln2 = k + r with r in [-1/2, 1/2] and int k.
    var z = libm::exp2f_d(libm::EXP2F_INVLN2_SCALED) * xd;
    // Round and convert z to int, the result is in [-150*N, 128*N] and
    // ideally ties-to-even rule is used, otherwise the magnitude of r can
    // be bigger which gives larger approximation error.
    let shift = libm::exp2f_d(libm::EXP2F_SHIFT);
    var kd = z + shift;
    let ki = libm::bits(kd);
    kd -= shift;
    let r = z - kd;
    // exp(x) = 2^(k/N) * 2^(r/N) ~= s * (C0*r^3 + C1*r^2 + C2*r + 1)
    var t = libm::EXP2F_DATA[libm::EXP2F_TAB + ((ki & 31) as! i64)];
    t += ki << 47;
    let s = libm::from_bits(t);
    let C = libm::EXP2F_POLY_SCALED;
    z = libm::exp2f_d(C) * r + libm::exp2f_d(C + 1);
    let r2 = r * r;
    var y = libm::exp2f_d(C + 2) * r + 1.0;
    y = z * r2 + y;
    y = y * s;
    return y as f32;
}

// logf_data.c's fields: tab[16] { invc, logc }, ln2, poly[3].
fn libm::logf_d(i: i64) -> f64 { libm::from_bits(libm::LOGF_DATA[i]) }

// logf.c
fn log(x: f32) -> f32 {
    var ix = libm::bits(x);
    // Fix sign of zero with downward rounding when x==1.
    if ix == 0x3f800000 { return 0.0; }
    if ix - 0x00800000 >= 0x7f800000 - 0x00800000 {
        // x < 0x1p-126 or inf or nan.
        if ix * 2 == 0 { return -1.0 / 0.0; }  // __math_divzerof(1)
        if ix == 0x7f800000 { return x; }  // log(inf) == inf.
        if (ix & 0x80000000) != 0 || ix * 2 >= 0xff000000 { return (x - x) / (x - x); }  // __math_invalidf(x)
        // x is subnormal, normalize it.
        ix = libm::bits(x * 0x1p23);
        ix -= 23 << 23;
    }
    // x = 2^k z; where z is in range [OFF,2*OFF] and exact.
    // The range is split into N subintervals.
    // The ith subinterval contains z and c is near its center.
    let tmp = ix - 0x3f330000;  // OFF
    let i = ((tmp >> 19) & 15) as! i64;
    let k = (tmp as! i32) >> 23;  // arithmetic shift
    let iz = ix - (tmp & 0xff800000);
    let invc = libm::logf_d(2 * i);
    let logc = libm::logf_d(2 * i + 1);
    let z = libm::from_bits(iz) as f64;
    // log(x) = log1p(z/c-1) + log(c) + k*Ln2
    let r = z * invc - 1.0;
    let kd: f64 = k;
    let y0 = logc + kd * libm::logf_d(32);
    // Pipelined polynomial evaluation to approximate log1p(r).
    let r2 = r * r;
    var y = libm::logf_d(34) * r + libm::logf_d(35);
    y = libm::logf_d(33) * r2 + y;
    y = y * r2 + (y0 + r);
    return y as f32;
}

// log2f_data.c's fields: tab[16] { invc, logc }, poly[4].
fn libm::log2f_d(i: i64) -> f64 { libm::from_bits(libm::LOG2F_DATA[i]) }

// log2f.c
fn log2(x: f32) -> f32 {
    var ix = libm::bits(x);
    // Fix sign of zero with downward rounding when x==1.
    if ix == 0x3f800000 { return 0.0; }
    if ix - 0x00800000 >= 0x7f800000 - 0x00800000 {
        // x < 0x1p-126 or inf or nan.
        if ix * 2 == 0 { return -1.0 / 0.0; }  // __math_divzerof(1)
        if ix == 0x7f800000 { return x; }  // log2(inf) == inf.
        if (ix & 0x80000000) != 0 || ix * 2 >= 0xff000000 { return (x - x) / (x - x); }  // __math_invalidf(x)
        // x is subnormal, normalize it.
        ix = libm::bits(x * 0x1p23);
        ix -= 23 << 23;
    }
    // x = 2^k z; where z is in range [OFF,2*OFF] and exact.
    let tmp = ix - 0x3f330000;  // OFF
    let i = ((tmp >> 19) & 15) as! i64;
    let top = tmp & 0xff800000;
    let iz = ix - top;
    let k = (tmp as! i32) >> 23;  // arithmetic shift
    let invc = libm::log2f_d(2 * i);
    let logc = libm::log2f_d(2 * i + 1);
    let z = libm::from_bits(iz) as f64;
    // log2(x) = log1p(z/c-1)/ln2 + log2(c) + k
    let r = z * invc - 1.0;
    let kd: f64 = k;
    let y0 = logc + kd;
    // Pipelined polynomial evaluation to approximate log1p(r)/ln2.
    let r2 = r * r;
    var y = libm::log2f_d(33) * r + libm::log2f_d(34);
    y = libm::log2f_d(32) * r2 + y;
    let p = libm::log2f_d(35) * r + y0;
    y = y * r2 + p;
    return y as f32;
}

// log10f.c (from FreeBSD's e_log10f.c, Sun's notice above).
fn log10(x0: f32) -> f32 {
    let ivln10hi: f32 = 4.3432617188e-01;    // 0x3ede6000
    let ivln10lo: f32 = -3.1689971365e-05;   // 0xb804ead9
    let log10_2hi: f32 = 3.0102920532e-01;   // 0x3e9a2080
    let log10_2lo: f32 = 7.9034151668e-07;   // 0x355427db
    // |(log(1+s)-log(1-s))/s - Lg(s)| < 2**-34.24 (~[-4.95e-11, 4.97e-11]).
    let Lg1: f32 = 0xaaaaaa.0p-24;  // 0.66666662693
    let Lg2: f32 = 0xccce13.0p-25;  // 0.40000972152
    let Lg3: f32 = 0x91e9ee.0p-25;  // 0.28498786688
    let Lg4: f32 = 0xf89e26.0p-26;  // 0.24279078841
    var x = x0;
    var ix = libm::bits(x);
    var k: i32 = 0;
    if ix < 0x00800000 || (ix >> 31) != 0 {  // x < 2**-126
        if ix << 1 == 0 { return -1.0 / (x * x); }  // log(+-0)=-inf
        if (ix >> 31) != 0 { return (x - x) / 0.0; }  // log(-#) = NaN
        // subnormal number, scale up x
        k -= 25;
        x *= 0x1p25;
        ix = libm::bits(x);
    } else if ix >= 0x7f800000 {
        return x;
    } else if ix == 0x3f800000 {
        return 0.0;
    }
    // reduce x into [sqrt(2)/2, sqrt(2)]
    ix += 0x3f800000 - 0x3f3504f3;
    k += ((ix >> 23) as! i32) - 0x7f;
    ix = (ix & 0x007fffff) + 0x3f3504f3;
    x = libm::from_bits(ix);
    let f: f32 = x - 1.0;
    let s = f / (2.0 + f);
    let z = s * s;
    let w = z * z;
    let t1 = w * (Lg2 + w * Lg4);
    let t2 = z * (Lg1 + w * Lg3);
    let R = t2 + t1;
    let hfsq: f32 = 0.5 * f * f;
    let hi = libm::from_bits(libm::bits(f - hfsq) & 0xfffff000);
    let lo = f - hi - hfsq + s * (hfsq + R);
    let dk = k as f32;
    return dk * log10_2lo + (lo + hi) * ivln10lo + lo * ivln10hi + hi * ivln10hi + dk * log10_2hi;
}

// powf_data.c's fields: tab[16] { invc, logc }, poly[5].
fn libm::powf_d(i: i64) -> f64 { libm::from_bits(libm::POWF_LOG2_DATA[i]) }

// powf.c's log2_inline: log2(x), ix normalized so a subnormal x has a
// negative biased exponent (POWF_SCALE_BITS is 0: no scaling).
fn libm::powf_log2(ix: u32) -> f64 {
    // x = 2^k z; where z is in range [OFF,2*OFF] and exact.
    let tmp = ix - 0x3f330000;  // OFF
    let i = ((tmp >> 19) & 15) as! i64;
    let top = tmp & 0xff800000;
    let iz = ix - top;
    let k = (top as! i32) >> 23;  // arithmetic shift
    let invc = libm::powf_d(2 * i);
    let logc = libm::powf_d(2 * i + 1);
    let z = libm::from_bits(iz) as f64;
    // log2(x) = log1p(z/c-1)/ln2 + log2(c) + k
    let r = z * invc - 1.0;
    let kd: f64 = k;
    let y0 = logc + kd;
    // Pipelined polynomial evaluation to approximate log1p(r)/ln2.
    let r2 = r * r;
    var y = libm::powf_d(32) * r + libm::powf_d(33);
    let p = libm::powf_d(34) * r + libm::powf_d(35);
    let r4 = r2 * r2;
    var q = libm::powf_d(36) * r + y0;
    q = p * r2 + q;
    y = y * r4 + q;
    return y;
}

// powf.c's exp2_inline: 2^xd, xd in [-1021,1023]; sign_bias sets the sign.
fn libm::powf_exp2(xd: f64, sign_bias: u32) -> f32 {
    // x = k/N + r with r in [-1/(2N), 1/(2N)]
    let shift = libm::exp2f_d(libm::EXP2F_SHIFT_SCALED);
    var kd = xd + shift;
    let ki = libm::bits(kd);
    kd -= shift;  // k/N
    let r = xd - kd;
    // exp2(x) = 2^(k/N) * 2^r ~= s * (C0*r^3 + C1*r^2 + C2*r + 1)
    var t = libm::EXP2F_DATA[libm::EXP2F_TAB + ((ki & 31) as! i64)];
    let ski = ki + sign_bias;
    t += ski << 47;
    let s = libm::from_bits(t);
    let C = libm::EXP2F_POLY;
    let z = libm::exp2f_d(C) * r + libm::exp2f_d(C + 1);
    let r2 = r * r;
    var y = libm::exp2f_d(C + 2) * r + 1.0;
    y = z * r2 + y;
    y = y * s;
    return y as f32;
}

// powf.c's checkint, for a float's bits.
fn libm::checkintf(iy: u32) -> i32 {
    let e = ((iy >> 23) & 0xff) as! i32;
    if e < 0x7f { return 0; }
    if e > 0x7f + 23 { return 2; }
    let s = (0x7f + 23 - e) as! u32;
    if (iy & ((1 << s) - 1)) != 0 { return 0; }
    if (iy & (1 << s)) != 0 { return 1; }
    return 2;
}

// powf.c's zeroinfnan.
fn libm::zeroinfnanf(ix: u32) -> bool { 2 * ix - 1 >= 0xfeffffff }  // 2u * 0x7f800000 - 1

// powf.c
fn pow(x: f32, y: f32) -> f32 {
    var sign_bias: u32 = 0;
    var ix = libm::bits(x);
    let iy = libm::bits(y);
    if ix - 0x00800000 >= 0x7f800000 - 0x00800000 || libm::zeroinfnanf(iy) {
        // Either (x < 0x1p-126 or inf or nan) or (y is 0 or inf or nan).
        if libm::zeroinfnanf(iy) {
            if 2 * iy == 0 { return 1.0; }
            if ix == 0x3f800000 { return 1.0; }
            if 2 * ix > 0xff000000 || 2 * iy > 0xff000000 { return x + y; }  // 2u * 0x7f800000
            if 2 * ix == 2 * 0x3f800000 { return 1.0; }
            // |x|<1 && y==inf or |x|>1 && y==-inf.
            if (2 * ix < 2 * 0x3f800000) == ((iy & 0x80000000) == 0) { return 0.0; }
            return y * y;
        }
        if libm::zeroinfnanf(ix) {
            var x2 = x * x;
            if (ix & 0x80000000) != 0 && libm::checkintf(iy) == 1 { x2 = -x2; }
            return if (iy & 0x80000000) != 0 { 1.0 / x2 } else { x2 };
        }
        // x and y are non-zero finite.
        if (ix & 0x80000000) != 0 {
            // Finite x < 0.
            let yint = libm::checkintf(iy);
            if yint == 0 { return (x - x) / (x - x); }  // __math_invalidf(x)
            if yint == 1 { sign_bias = 1 << (5 + 11); }  // SIGN_BIAS
            ix &= 0x7fffffff;
        }
        if ix < 0x00800000 {
            // Normalize subnormal x so exponent becomes negative.
            ix = libm::bits(x * 0x1p23);
            ix &= 0x7fffffff;
            ix -= 23 << 23;
        }
    }
    let logx = libm::powf_log2(ix);
    let ylogx = y * logx;  // cannot overflow, y is single prec.
    let big: f64 = 126.0;
    if ((libm::bits(ylogx) >> 47) & 0xffff) >= libm::bits(big) >> 47 {
        // |y*log(x)| >= 126.
        if ylogx > 0x1.fffffffd1d571p+6 { return libm::xflowf(sign_bias != 0, 0x1p97); }   // __math_oflowf
        if ylogx <= -150.0 { return libm::xflowf(sign_bias != 0, 0x1p-95); }              // __math_uflowf
    }
    return libm::powf_exp2(ylogx, sign_bias);
}

// ---- Tables ----

// exp_data.c: the struct's fields as bits, in order.
let libm::EXP_DATA: u64[270] = [
    0x40671547652b82fe, 0x4338000000000000, 0xbf762e42fefa0000, 0xbd0cf79abc9e3b3a,
    0x3fdffffffffffdbd, 0x3fc555555555543c, 0x3fa55555cf172b91, 0x3f81111167a4d017,
    0x42c8000000000000, 0x3fe62e42fefa39ef, 0x3fcebfbdff82c424, 0x3fac6b08d70cf4b5,
    0x3f83b2abd24650cc, 0x3f55d7e09b4e3a84, 0x0000000000000000, 0x3ff0000000000000,
    0x3c9b3b4f1a88bf6e, 0x3feff63da9fb3335, 0xbc7160139cd8dc5d, 0x3fefec9a3e778061,
    0xbc905e7a108766d1, 0x3fefe315e86e7f85, 0x3c8cd2523567f613, 0x3fefd9b0d3158574,
    0xbc8bce8023f98efa, 0x3fefd06b29ddf6de, 0x3c60f74e61e6c861, 0x3fefc74518759bc8,
    0x3c90a3e45b33d399, 0x3fefbe3ecac6f383, 0x3c979aa65d837b6d, 0x3fefb5586cf9890f,
    0x3c8eb51a92fdeffc, 0x3fefac922b7247f7, 0x3c3ebe3d702f9cd1, 0x3fefa3ec32d3d1a2,
    0xbc6a033489906e0b, 0x3fef9b66affed31b, 0xbc9556522a2fbd0e, 0x3fef9301d0125b51,
    0xbc5080ef8c4eea55, 0x3fef8abdc06c31cc, 0xbc91c923b9d5f416, 0x3fef829aaea92de0,
    0x3c80d3e3e95c55af, 0x3fef7a98c8a58e51, 0xbc801b15eaa59348, 0x3fef72b83c7d517b,
    0xbc8f1ff055de323d, 0x3fef6af9388c8dea, 0x3c8b898c3f1353bf, 0x3fef635beb6fcb75,
    0xbc96d99c7611eb26, 0x3fef5be084045cd4, 0x3c9aecf73e3a2f60, 0x3fef54873168b9aa,
    0xbc8fe782cb86389d, 0x3fef4d5022fcd91d, 0x3c8a6f4144a6c38d, 0x3fef463b88628cd6,
    0x3c807a05b0e4047d, 0x3fef3f49917ddc96, 0x3c968efde3a8a894, 0x3fef387a6e756238,
    0x3c875e18f274487d, 0x3fef31ce4fb2a63f, 0x3c80472b981fe7f2, 0x3fef2b4565e27cdd,
    0xbc96b87b3f71085e, 0x3fef24dfe1f56381, 0x3c82f7e16d09ab31, 0x3fef1e9df51fdee1,
    0xbc3d219b1a6fbffa, 0x3fef187fd0dad990, 0x3c8b3782720c0ab4, 0x3fef1285a6e4030b,
    0x3c6e149289cecb8f, 0x3fef0cafa93e2f56, 0x3c834d754db0abb6, 0x3fef06fe0a31b715,
    0x3c864201e2ac744c, 0x3fef0170fc4cd831, 0x3c8fdd395dd3f84a, 0x3feefc08b26416ff,
    0xbc86a3803b8e5b04, 0x3feef6c55f929ff1, 0xbc924aedcc4b5068, 0x3feef1a7373aa9cb,
    0xbc9907f81b512d8e, 0x3feeecae6d05d866, 0xbc71d1e83e9436d2, 0x3feee7db34e59ff7,
    0xbc991919b3ce1b15, 0x3feee32dc313a8e5, 0x3c859f48a72a4c6d, 0x3feedea64c123422,
    0xbc9312607a28698a, 0x3feeda4504ac801c, 0xbc58a78f4817895b, 0x3feed60a21f72e2a,
    0xbc7c2c9b67499a1b, 0x3feed1f5d950a897, 0x3c4363ed60c2ac11, 0x3feece086061892d,
    0x3c9666093b0664ef, 0x3feeca41ed1d0057, 0x3c6ecce1daa10379, 0x3feec6a2b5c13cd0,
    0x3c93ff8e3f0f1230, 0x3feec32af0d7d3de, 0x3c7690cebb7aafb0, 0x3feebfdad5362a27,
    0x3c931dbdeb54e077, 0x3feebcb299fddd0d, 0xbc8f94340071a38e, 0x3feeb9b2769d2ca7,
    0xbc87deccdc93a349, 0x3feeb6daa2cf6642, 0xbc78dec6bd0f385f, 0x3feeb42b569d4f82,
    0xbc861246ec7b5cf6, 0x3feeb1a4ca5d920f, 0x3c93350518fdd78e, 0x3feeaf4736b527da,
    0x3c7b98b72f8a9b05, 0x3feead12d497c7fd, 0x3c9063e1e21c5409, 0x3feeab07dd485429,
    0x3c34c7855019c6ea, 0x3feea9268a5946b7, 0x3c9432e62b64c035, 0x3feea76f15ad2148,
    0xbc8ce44a6199769f, 0x3feea5e1b976dc09, 0xbc8c33c53bef4da8, 0x3feea47eb03a5585,
    0xbc845378892be9ae, 0x3feea34634ccc320, 0xbc93cedd78565858, 0x3feea23882552225,
    0x3c5710aa807e1964, 0x3feea155d44ca973, 0xbc93b3efbf5e2228, 0x3feea09e667f3bcd,
    0xbc6a12ad8734b982, 0x3feea012750bdabf, 0xbc6367efb86da9ee, 0x3fee9fb23c651a2f,
    0xbc80dc3d54e08851, 0x3fee9f7df9519484, 0xbc781f647e5a3ecf, 0x3fee9f75e8ec5f74,
    0xbc86ee4ac08b7db0, 0x3fee9f9a48a58174, 0xbc8619321e55e68a, 0x3fee9feb564267c9,
    0x3c909ccb5e09d4d3, 0x3feea0694fde5d3f, 0xbc7b32dcb94da51d, 0x3feea11473eb0187,
    0x3c94ecfd5467c06b, 0x3feea1ed0130c132, 0x3c65ebe1abd66c55, 0x3feea2f336cf4e62,
    0xbc88a1c52fb3cf42, 0x3feea427543e1a12, 0xbc9369b6f13b3734, 0x3feea589994cce13,
    0xbc805e843a19ff1e, 0x3feea71a4623c7ad, 0xbc94d450d872576e, 0x3feea8d99b4492ed,
    0x3c90ad675b0e8a00, 0x3feeaac7d98a6699, 0x3c8db72fc1f0eab4, 0x3feeace5422aa0db,
    0xbc65b6609cc5e7ff, 0x3feeaf3216b5448c, 0x3c7bf68359f35f44, 0x3feeb1ae99157736,
    0xbc93091fa71e3d83, 0x3feeb45b0b91ffc6, 0xbc5da9b88b6c1e29, 0x3feeb737b0cdc5e5,
    0xbc6c23f97c90b959, 0x3feeba44cbc8520f, 0xbc92434322f4f9aa, 0x3feebd829fde4e50,
    0xbc85ca6cd7668e4b, 0x3feec0f170ca07ba, 0x3c71affc2b91ce27, 0x3feec49182a3f090,
    0x3c6dd235e10a73bb, 0x3feec86319e32323, 0xbc87c50422622263, 0x3feecc667b5de565,
    0x3c8b1c86e3e231d5, 0x3feed09bec4a2d33, 0xbc91bbd1d3bcbb15, 0x3feed503b23e255d,
    0x3c90cc319cee31d2, 0x3feed99e1330b358, 0x3c8469846e735ab3, 0x3feede6b5579fdbf,
    0xbc82dfcd978e9db4, 0x3feee36bbfd3f37a, 0x3c8c1a7792cb3387, 0x3feee89f995ad3ad,
    0xbc907b8f4ad1d9fa, 0x3feeee07298db666, 0xbc55c3d956dcaeba, 0x3feef3a2b84f15fb,
    0xbc90a40e3da6f640, 0x3feef9728de5593a, 0xbc68d6f438ad9334, 0x3feeff76f2fb5e47,
    0xbc91eee26b588a35, 0x3fef05b030a1064a, 0x3c74ffd70a5fddcd, 0x3fef0c1e904bc1d2,
    0xbc91bdfbfa9298ac, 0x3fef12c25bd71e09, 0x3c736eae30af0cb3, 0x3fef199bdd85529c,
    0x3c8ee3325c9ffd94, 0x3fef20ab5fffd07a, 0x3c84e08fd10959ac, 0x3fef27f12e57d14b,
    0x3c63cdaf384e1a67, 0x3fef2f6d9406e7b5, 0x3c676b2c6c921968, 0x3fef3720dcef9069,
    0xbc808a1883ccb5d2, 0x3fef3f0b555dc3fa, 0xbc8fad5d3ffffa6f, 0x3fef472d4a07897c,
    0xbc900dae3875a949, 0x3fef4f87080d89f2, 0x3c74a385a63d07a7, 0x3fef5818dcfba487,
    0xbc82919e2040220f, 0x3fef60e316c98398, 0x3c8e5a50d5c192ac, 0x3fef69e603db3285,
    0x3c843a59ac016b4b, 0x3fef7321f301b460, 0xbc82d52107b43e1f, 0x3fef7c97337b9b5f,
    0xbc892ab93b470dc9, 0x3fef864614f5a129, 0x3c74b604603a88d3, 0x3fef902ee78b3ff6,
    0x3c83c5ec519d7271, 0x3fef9a51fbc74c83, 0xbc8ff7128fd391f0, 0x3fefa4afa2a490da,
    0xbc8dae98e223747d, 0x3fefaf482d8e67f1, 0x3c8ec3bc41aa2008, 0x3fefba1bee615a27,
    0x3c842b94c3a9eb32, 0x3fefc52b376bba97, 0x3c8a64a931d185ee, 0x3fefd0765b6e4540,
    0xbc8e37bae43be3ed, 0x3fefdbfdad9cbe14, 0x3c77893b4d91cd9d, 0x3fefe7c1819e90d8,
    0x3c5305c14160cc89, 0x3feff3c22b8f71f1,
];

// log_data.c: the struct's fields as bits, in order.
let libm::LOG_DATA: u64[530] = [
    0x3fe62e42fefa3800, 0x3d2ef35793c76730, 0xbfe0000000000001, 0x3fd555555551305b,
    0xbfcfffffffeb4590, 0x3fc999b324f10111, 0xbfc55575e506c89f, 0xbfe0000000000000,
    0x3fd5555555555577, 0xbfcffffffffffdcb, 0x3fc999999995dd0c, 0xbfc55555556745a7,
    0x3fc24924a344de30, 0xbfbfffffa4423d65, 0x3fbc7184282ad6ca, 0xbfb999eb43b068ff,
    0x3fb78182f7afd085, 0xbfb5521375d145cd, 0x3ff734f0c3e0de9f, 0xbfd7cc7f79e69000,
    0x3ff713786a2ce91f, 0xbfd76feec20d0000, 0x3ff6f26008fab5a0, 0xbfd713e31351e000,
    0x3ff6d1a61f138c7d, 0xbfd6b85b38287800, 0x3ff6b1490bc5b4d1, 0xbfd65d5590807800,
    0x3ff69147332f0cba, 0xbfd602d076180000, 0x3ff6719f18224223, 0xbfd5a8ca86909000,
    0x3ff6524f99a51ed9, 0xbfd54f4356035000, 0x3ff63356aa8f24c4, 0xbfd4f637c36b4000,
    0x3ff614b36b9ddc14, 0xbfd49da7fda85000, 0x3ff5f66452c65c4c, 0xbfd445923989a800,
    0x3ff5d867b5912c4f, 0xbfd3edf439b0b800, 0x3ff5babccb5b90de, 0xbfd396ce448f7000,
    0x3ff59d61f2d91a78, 0xbfd3401e17bda000, 0x3ff5805612465687, 0xbfd2e9e2ef468000,
    0x3ff56397cee76bd3, 0xbfd2941b3830e000, 0x3ff54725e2a77f93, 0xbfd23ec58cda8800,
    0x3ff52aff42064583, 0xbfd1e9e129279000, 0x3ff50f22dbb2bddf, 0xbfd1956d2b48f800,
    0x3ff4f38f4734ded7, 0xbfd141679ab9f800, 0x3ff4d843cfde2840, 0xbfd0edd094ef9800,
    0x3ff4bd3ec078a3c8, 0xbfd09aa518db1000, 0x3ff4a27fc3e0258a, 0xbfd047e65263b800,
    0x3ff4880524d48434, 0xbfcfeb224586f000, 0x3ff46dce1b192d0b, 0xbfcf474a7517b000,
    0x3ff453d9d3391854, 0xbfcea4443d103000, 0x3ff43a2744b4845a, 0xbfce020d44e9b000,
    0x3ff420b54115f8fb, 0xbfcd60a22977f000, 0x3ff40782da3ef4b1, 0xbfccc00104959000,
    0x3ff3ee8f5d57fe8f, 0xbfcc202956891000, 0x3ff3d5d9a00b4ce9, 0xbfcb81178d811000,
    0x3ff3bd60c010c12b, 0xbfcae2c9ccd3d000, 0x3ff3a5242b75dab8, 0xbfca45402e129000,
    0x3ff38d22cd9fd002, 0xbfc9a877681df000, 0x3ff3755bc5847a1c, 0xbfc90c6d69483000,
    0x3ff35dce49ad36e2, 0xbfc87120a645c000, 0x3ff34679984dd440, 0xbfc7d68fb4143000,
    0x3ff32f5cceffcb24, 0xbfc73cb83c627000, 0x3ff3187775a10d49, 0xbfc6a39a9b376000,
    0x3ff301c8373e3990, 0xbfc60b3154b7a000, 0x3ff2eb4ebb95f841, 0xbfc5737d76243000,
    0x3ff2d50a0219a9d1, 0xbfc4dc7b8fc23000, 0x3ff2bef9a8b7fd2a, 0xbfc4462c51d20000,
    0x3ff2a91c7a0c1bab, 0xbfc3b08abc830000, 0x3ff293726014b530, 0xbfc31b996b490000,
    0x3ff27dfa5757a1f5, 0xbfc2875490a44000, 0x3ff268b39b1d3bbf, 0xbfc1f3b9f879a000,
    0x3ff2539d838ff5bd, 0xbfc160c8252ca000, 0x3ff23eb7aac9083b, 0xbfc0ce7f57f72000,
    0x3ff22a012ba940b6, 0xbfc03cdc49fea000, 0x3ff2157996cc4132, 0xbfbf57bdbc4b8000,
    0x3ff201201dd2fc9b, 0xbfbe370896404000, 0x3ff1ecf4494d480b, 0xbfbd17983ef94000,
    0x3ff1d8f5528f6569, 0xbfbbf9674ed8a000, 0x3ff1c52311577e7c, 0xbfbadc79202f6000,
    0x3ff1b17c74cb26e9, 0xbfb9c0c3e7288000, 0x3ff19e010c2c1ab6, 0xbfb8a646b372c000,
    0x3ff18ab07bb670bd, 0xbfb78d01b3ac0000, 0x3ff1778a25efbcb6, 0xbfb674f145380000,
    0x3ff1648d354c31da, 0xbfb55e0e6d878000, 0x3ff151b990275fdd, 0xbfb4485cdea1e000,
    0x3ff13f0ea432d24c, 0xbfb333d94d6aa000, 0x3ff12c8b7210f9da, 0xbfb22079f8c56000,
    0x3ff11a3028ecb531, 0xbfb10e4698622000, 0x3ff107fbda8434af, 0xbfaffa6c6ad20000,
    0x3ff0f5ee0f4e6bb3, 0xbfadda8d4a774000, 0x3ff0e4065d2a9fce, 0xbfabbcece4850000,
    0x3ff0d244632ca521, 0xbfa9a1894012c000, 0x3ff0c0a77ce2981a, 0xbfa788583302c000,
    0x3ff0af2f83c636d1, 0xbfa5715e67d68000, 0x3ff09ddb98a01339, 0xbfa35c8a49658000,
    0x3ff08cabaf52e7df, 0xbfa149e364154000, 0x3ff07b9f2f4e28fb, 0xbf9e72c082eb8000,
    0x3ff06ab58c358f19, 0xbf9a55f152528000, 0x3ff059eea5ecf92c, 0xbf963d62cf818000,
    0x3ff04949cdd12c90, 0xbf9228fb8caa0000, 0x3ff038c6c6f0ada9, 0xbf8c317b20f90000,
    0x3ff02865137932a9, 0xbf8419355daa0000, 0x3ff0182427ea7348, 0xbf781203c2ec0000,
    0x3ff008040614b195, 0xbf60040979240000, 0x3fefe01ff726fa1a, 0x3f6feff384900000,
    0x3fefa11cc261ea74, 0x3f87dc41353d0000, 0x3fef6310b081992e, 0x3f93cea3c4c28000,
    0x3fef25f63ceeadcd, 0x3f9b9fc114890000, 0x3feee9c8039113e7, 0x3fa1b0d8ce110000,
    0x3feeae8078cbb1ab, 0x3fa58a5bd001c000, 0x3fee741aa29d0c9b, 0x3fa95c8340d88000,
    0x3fee3a91830a99b5, 0x3fad276aef578000, 0x3fee01e009609a56, 0x3fb07598e598c000,
    0x3fedca01e577bb98, 0x3fb253f5e30d2000, 0x3fed92f20b7c9103, 0x3fb42edd8b380000,
    0x3fed5cac66fb5cce, 0x3fb606598757c000, 0x3fed272caa5ede9d, 0x3fb7da76356a0000,
    0x3fecf26e3e6b2ccd, 0x3fb9ab434e1c6000, 0x3fecbe6da2a77902, 0x3fbb78c7bb0d6000,
    0x3fec8b266d37086d, 0x3fbd431332e72000, 0x3fec5894bd5d5804, 0x3fbf0a3171de6000,
    0x3fec26b533bb9f8c, 0x3fc067152b914000, 0x3febf583eeece73f, 0x3fc147858292b000,
    0x3febc4fd75db96c1, 0x3fc2266ecdca3000, 0x3feb951e0c864a28, 0x3fc303d7a6c55000,
    0x3feb65e2c5ef3e2c, 0x3fc3dfc33c331000, 0x3feb374867c9888b, 0x3fc4ba366b7a8000,
    0x3feb094b211d304a, 0x3fc5933928d1f000, 0x3feadbe885f2ef7e, 0x3fc66acd2418f000,
    0x3feaaf1d31603da2, 0x3fc740f8ec669000, 0x3fea82e63fd358a7, 0x3fc815c0f51af000,
    0x3fea5740ef09738b, 0x3fc8e92954f68000, 0x3fea2c2a90ab4b27, 0x3fc9bb3602f84000,
    0x3fea01a01393f2d1, 0x3fca8bed1c2c0000, 0x3fe9d79f24db3c1b, 0x3fcb5b515c01d000,
    0x3fe9ae2505c7b190, 0x3fcc2967ccbcc000, 0x3fe9852ef297ce2f, 0x3fccf635d5486000,
    0x3fe95cbaeea44b75, 0x3fcdc1bd3446c000, 0x3fe934c69de74838, 0x3fce8c01b8cfe000,
    0x3fe90d4f2f6752e6, 0x3fcf5509c0179000, 0x3fe8e6528effd79d, 0x3fd00e6c121fb800,
    0x3fe8bfce9fcc007c, 0x3fd071b80e93d000, 0x3fe899c0dabec30e, 0x3fd0d46b9e867000,
    0x3fe87427aa2317fb, 0x3fd13687334bd000, 0x3fe84f00acb39a08, 0x3fd1980d67234800,
    0x3fe82a49e8653e55, 0x3fd1f8ffe0cc8000, 0x3fe8060195f40260, 0x3fd2595fd7636800,
    0x3fe7e22563e0a329, 0x3fd2b9300914a800, 0x3fe7beb377dcb5ad, 0x3fd3187210436000,
    0x3fe79baa679725c2, 0x3fd377266dec1800, 0x3fe77907f2170657, 0x3fd3d54ffbaf3000,
    0x3fe756cadbd6130c, 0x3fd432eee32fe000, 0x3fe61000014fb66b, 0x3c7e026c91425b3c,
    0x3fe63000034db495, 0x3c8dbfea48005d41, 0x3fe650000d94d478, 0x3c8e7fa786d6a5b7,
    0x3fe67000074e6fad, 0x3c61fcea6b54254c, 0x3fe68ffffedf0fae, 0xbc7c7e274c590efd,
    0x3fe6b0000763c5bc, 0xbc8ac16848dcda01, 0x3fe6d0001e5cc1f6, 0x3c833f1c9d499311,
    0x3fe6efffeb05f63e, 0xbc7e80041ae22d53, 0x3fe710000e869780, 0x3c7bff6671097952,
    0x3fe72ffffc67e912, 0x3c8c00e226bd8724, 0x3fe74fffdf81116a, 0xbc6e02916ef101d2,
    0x3fe770000f679c90, 0xbc67fc71cd549c74, 0x3fe78ffffa7ec835, 0x3c81bec19ef50483,
    0x3fe7affffe20c2e6, 0xbc707e1729cc6465, 0x3fe7cfffed3fc900, 0xbc808072087b8b1c,
    0x3fe7efffe9261a76, 0x3c8dc0286d9df9ae, 0x3fe81000049ca3e8, 0x3c897fd251e54c33,
    0x3fe8300017932c8f, 0xbc8afee9b630f381, 0x3fe850000633739c, 0x3c89bfbf6b6535bc,
    0x3fe87000204289c6, 0xbc8bbf65f3117b75, 0x3fe88fffebf57904, 0xbc89006ea23dcb57,
    0x3fe8b00022bc04df, 0xbc7d00df38e04b0a, 0x3fe8cfffe50c1b8a, 0xbc88007146ff9f05,
    0x3fe8effffc918e43, 0x3c83817bd07a7038, 0x3fe910001efa5fc7, 0x3c893e9176dfb403,
    0x3fe9300013467bb9, 0x3c7f804e4b980276, 0x3fe94fffe6ee076f, 0xbc8f7ef0d9ff622e,
    0x3fe96fffde3c12d1, 0xbc7082aa962638ba, 0x3fe98ffff4458a0d, 0xbc87801b9164a8ef,
    0x3fe9afffdd982e3e, 0xbc8740e08a5a9337, 0x3fe9cfffed49fb66, 0x3c3fce08c19be000,
    0x3fe9f00020f19c51, 0xbc8a3faa27885b0a, 0x3fea10001145b006, 0x3c74ff489958da56,
    0x3fea300007bbf6fa, 0x3c8cbeab8a2b6d18, 0x3fea500010971d79, 0x3c88fecadd787930,
    0x3fea70001df52e48, 0xbc8f41763dd8abdb, 0x3fea90001c593352, 0xbc8ebf0284c27612,
    0x3feab0002a4f3e4b, 0xbc69fd043cff3f5f, 0x3feacfffd7ae1ed1, 0xbc823ee7129070b4,
    0x3feaefffee510478, 0x3c6a063ee00edea3, 0x3feb0fffdb650d5b, 0x3c5a06c8381f0ab9,
    0x3feb2ffffeaaca57, 0xbc79011e74233c1d, 0x3feb4fffd995badc, 0xbc79ff1068862a9f,
    0x3feb7000249e659c, 0x3c8aff45d0864f3e, 0x3feb8ffff9871640, 0x3c7cfe7796c2c3f9,
    0x3febafffd204cb4f, 0xbc63ff27eef22bc4, 0x3febcfffd2415c45, 0xbc6cffb7ee3bea21,
    0x3febeffff86309df, 0xbc814103972e0b5c, 0x3fec0fffe1b57653, 0x3c8bc16494b76a19,
    0x3fec2ffff1fa57e3, 0xbc64feef8d30c6ed, 0x3fec4fffdcbfe424, 0xbc843f68bcec4775,
    0x3fec6fffed54b9f7, 0x3c847ea3f053e0ec, 0x3fec8fffeb998fd5, 0x3c7383068df992f1,
    0x3fecb0002125219a, 0xbc68fd8e64180e04, 0x3feccfffdd94469c, 0x3c8e7ebe1cc7ea72,
    0x3fecefffeafdc476, 0x3c8ebe39ad9f88fe, 0x3fed1000169af82b, 0x3c757d91a8b95a71,
    0x3fed30000d0ff71d, 0x3c89c1906970c7da, 0x3fed4fffea790fc4, 0xbc580e37c558fe0c,
    0x3fed70002edc87e5, 0xbc7f80d64dc10f44, 0x3fed900021dc82aa, 0xbc747c8f94fd5c5c,
    0x3fedafffd86b0283, 0x3c8c7f1dc521617e, 0x3fedd000296c4739, 0x3c88019eb2ffb153,
    0x3fedefffe54490f5, 0x3c6e00d2c652cc89, 0x3fee0fffcdabf694, 0xbc7f8340202d69d2,
    0x3fee2fffdb52c8dd, 0x3c7b00c1ca1b0864, 0x3fee4ffff24216ef, 0x3c72ffa8b094ab51,
    0x3fee6fffe88a5e11, 0xbc57f673b1efbe59, 0x3fee9000119eff0d, 0xbc84808d5e0bc801,
    0x3feeafffdfa51744, 0x3c780006d54320b5, 0x3feed0001a127fa1, 0xbc5002f860565c92,
    0x3feef00007babcc4, 0xbc8540445d35e611, 0x3fef0ffff57a8d02, 0xbc4ffb3139ef9105,
    0x3fef30001ee58ac7, 0x3c8a81acf2731155, 0x3fef4ffff5823494, 0x3c8a3f41d4d7c743,
    0x3fef6ffffca94c6b, 0xbc6202f41c987875, 0x3fef8fffe1f9c441, 0x3c777dd1f477e74b,
    0x3fefafffd2e0e37e, 0xbc6f01199a7ca331, 0x3fefd0001c77e49e, 0x3c7181ee4bceacb1,
    0x3fefeffff7e0c331, 0xbc6e05370170875a, 0x3ff00ffff465606e, 0xbc8a7ead491c0ada,
    0x3ff02ffff3867a58, 0xbc977f69c3fcb2e0, 0x3ff04ffffdfc0d17, 0x3c97bffe34cb945b,
    0x3ff0700003cd4d82, 0x3c820083c0e456cb, 0x3ff08ffff9f2cbe8, 0xbc6dffdfbe37751a,
    0x3ff0b000010cda65, 0xbc913f7faee626eb, 0x3ff0d00001a4d338, 0x3c807dfa79489ff7,
    0x3ff0effffadafdfd, 0xbc77040570d66bc0, 0x3ff110000bbafd96, 0x3c8e80d4846d0b62,
    0x3ff12ffffae5f45d, 0x3c9dbffa64fd36ef, 0x3ff150000dd59ad9, 0x3c9a0077701250ae,
    0x3ff170000f21559a, 0x3c8dfdf9e2e3deee, 0x3ff18ffffc275426, 0x3c910030dc3b7273,
    0x3ff1b000123d3c59, 0x3c997f7980030188, 0x3ff1cffff8299eb7, 0xbc65f932ab9f8c67,
    0x3ff1effff48ad400, 0x3c937fbf9da75beb, 0x3ff210000c8b86a4, 0x3c9f806b91fd5b22,
    0x3ff2300003854303, 0x3c93ffc2eb9fbf33, 0x3ff24fffffbcf684, 0x3c7601e77e2e2e72,
    0x3ff26ffff52921d9, 0x3c7ffcbb767f0c61, 0x3ff2900014933a3c, 0xbc7202ca3c02412b,
    0x3ff2b00014556313, 0xbc92808233f21f02, 0x3ff2cfffebfe523b, 0xbc88ff7e384fdcf2,
    0x3ff2f0000bb8ad96, 0xbc85ff51503041c5, 0x3ff30ffffb7ae2af, 0xbc810071885e289d,
    0x3ff32ffffeac5f7f, 0xbc91ff5d3fb7b715, 0x3ff350000ca66756, 0x3c957f82228b82bd,
    0x3ff3700011fbf721, 0x3c8000bac40dd5cc, 0x3ff38ffff9592fb9, 0xbc943f9d2db2a751,
    0x3ff3b00004ddd242, 0x3c857f6b707638e1, 0x3ff3cffff5b2c957, 0x3c7a023a10bf1231,
    0x3ff3efffeab0b418, 0x3c987f6d66b152b0, 0x3ff410001532aff4, 0x3c67f8375f198524,
    0x3ff4300017478b29, 0x3c8301e672dc5143, 0x3ff44fffe795b463, 0x3c89ff69b8b2895a,
    0x3ff46fffe80475e0, 0xbc95c0b19bc2f254, 0x3ff48fffef6fc1e7, 0x3c9b4009f23a2a72,
    0x3ff4afffe5bea704, 0xbc94ffb7bf0d7d45, 0x3ff4d000171027de, 0xbc99c06471dc6a3d,
    0x3ff4f0000ff03ee2, 0x3c977f890b85531c, 0x3ff5100012dc4bd1, 0x3c6004657166a436,
    0x3ff530001605277a, 0xbc96bfcece233209, 0x3ff54fffecdb704c, 0xbc8902720505a1d7,
    0x3ff56fffef5f54a9, 0x3c9bbfe60ec96412, 0x3ff5900017e61012, 0x3c887ec581afef90,
    0x3ff5b00003c93e92, 0xbc9f41080abf0cc0, 0x3ff5d0001d4919bc, 0xbc98812afb254729,
    0x3ff5efffe7b87a89, 0xbc947eb780ed6904,
];

// log2_data.c: the struct's fields as bits, in order.
let libm::LOG2_DATA: u64[274] = [
    0x3ff7154765200000, 0x3de705fc2eefa200, 0xbfe71547652b8339, 0x3fdec709dc3a04be,
    0xbfd7154764702ffb, 0x3fd2776c50034c48, 0xbfcec7b328ea92bc, 0x3fca6225e117f92e,
    0xbfe71547652b82fe, 0x3fdec709dc3a03f7, 0xbfd71547652b7c3f, 0x3fd2776c50f05be4,
    0xbfcec709dd768fe5, 0x3fca61761ec4e736, 0xbfc7153fbc64a79b, 0x3fc484d154f01b4a,
    0xbfc289e4a72c383c, 0x3fc0b32f285aee66, 0x3ff724286bb1acf8, 0xbfe1095feecdb000,
    0x3ff6e1f766d2cca1, 0xbfe08494bd76d000, 0x3ff6a13d0e30d48a, 0xbfe00143aee8f800,
    0x3ff661ec32d06c85, 0xbfdefec5360b4000, 0x3ff623fa951198f8, 0xbfddfdd91ab7e000,
    0x3ff5e75ba4cf026c, 0xbfdcffae0cc79000, 0x3ff5ac055a214fb8, 0xbfdc043811fda000,
    0x3ff571ed0f166e1e, 0xbfdb0b67323ae000, 0x3ff53909590bf835, 0xbfda152f5a2db000,
    0x3ff5014fed61addd, 0xbfd9217f5af86000, 0x3ff4cab88e487bd0, 0xbfd8304db0719000,
    0x3ff49539b4334fee, 0xbfd74189f9a9e000, 0x3ff460cbdfafd569, 0xbfd6552bb5199000,
    0x3ff42d664ee4b953, 0xbfd56b23a29b1000, 0x3ff3fb01111dd8a6, 0xbfd483650f5fa000,
    0x3ff3c995b70c5836, 0xbfd39de937f6a000, 0x3ff3991c4ab6fd4a, 0xbfd2baa1538d6000,
    0x3ff3698e0ce099b5, 0xbfd1d98340ca4000, 0x3ff33ae48213e7b2, 0xbfd0fa853a40e000,
    0x3ff30d191985bdb1, 0xbfd01d9c32e73000, 0x3ff2e025cab271d7, 0xbfce857da2fa6000,
    0x3ff2b404cf13cd82, 0xbfccd3c8633d8000, 0x3ff288b02c7ccb50, 0xbfcb26034c14a000,
    0x3ff25e2263944de5, 0xbfc97c1c2f4fe000, 0x3ff234563d8615b1, 0xbfc7d6023f800000,
    0x3ff20b46e33eaf38, 0xbfc633a71a05e000, 0x3ff1e2eefdcda3dd, 0xbfc494f5e9570000,
    0x3ff1bb4a580b3930, 0xbfc2f9e424e0a000, 0x3ff19453847f2200, 0xbfc162595afdc000,
    0x3ff16e06c0d5d73c, 0xbfbf9c9a75bd8000, 0x3ff1485f47b7e4c2, 0xbfbc7b575bf9c000,
    0x3ff12358ad0085d1, 0xbfb960c60ff48000, 0x3ff0fef00f532227, 0xbfb64ce247b60000,
    0x3ff0db2077d03a8f, 0xbfb33f78b2014000, 0x3ff0b7e6d65980d9, 0xbfb0387d1a42c000,
    0x3ff0953efe7b408d, 0xbfaa6f9208b50000, 0x3ff07325cac53b83, 0xbfa47a954f770000,
    0x3ff05197e40d1b5c, 0xbf9d23a8c50c0000, 0x3ff03091c1208ea2, 0xbf916a2629780000,
    0x3ff0101025b37e21, 0xbf7720f8d8e80000, 0x3fefc07ef9caa76b, 0x3f86fe53b1500000,
    0x3fef4465d3f6f184, 0x3fa11ccce10f8000, 0x3feecc079f84107f, 0x3fac4dfc8c8b8000,
    0x3fee573a99975ae8, 0x3fb3aa321e574000, 0x3fede5d6f0bd3de6, 0x3fb918a0d08b8000,
    0x3fed77b681ff38b3, 0x3fbe72e9da044000, 0x3fed0cb5724de943, 0x3fc1dcd2507f6000,
    0x3feca4b2dc0e7563, 0x3fc476ab03dea000, 0x3fec3f8ee8d6cb51, 0x3fc7074377e22000,
    0x3febdd2b4f020c4c, 0x3fc98ede8ba94000, 0x3feb7d6c006015ca, 0x3fcc0db86ad2e000,
    0x3feb20366e2e338f, 0x3fce840aafcee000, 0x3feac57026295039, 0x3fd0790ab4678000,
    0x3fea6d01bc2731dd, 0x3fd1ac056801c000, 0x3fea16d3bc3ff18b, 0x3fd2db11d4fee000,
    0x3fe9c2d14967fead, 0x3fd406464ec58000, 0x3fe970e4f47c9902, 0x3fd52dbe093af000,
    0x3fe920fb3982bcf2, 0x3fd651902050d000, 0x3fe8d30187f759f1, 0x3fd771d2cdeaf000,
    0x3fe886e5ebb9f66d, 0x3fd88e9c857d9000, 0x3fe83c97b658b994, 0x3fd9a80155e16000,
    0x3fe7f405ffc61022, 0x3fdabe186ed3d000, 0x3fe7ad22181415ca, 0x3fdbd0f2aea0e000,
    0x3fe767dcf99eff8c, 0x3fdce0a43dbf4000, 0x3fe6200012b90a8e, 0x3c8904ab0644b605,
    0x3fe66000045734a6, 0x3c61ff9bea62f7a9, 0x3fe69fffc325f2c5, 0x3c827ecfcb3c90ba,
    0x3fe6e00038b95a04, 0x3c88ff8856739326, 0x3fe71fffe09994e3, 0x3c8afd40275f82b1,
    0x3fe7600015590e10, 0xbc72fd75b4238341, 0x3fe7a00012655bd5, 0x3c7808e67c242b76,
    0x3fe7e0003259e9a6, 0xbc6208e426f622b7, 0x3fe81fffedb4b2d2, 0xbc8402461ea5c92f,
    0x3fe860002dfafcc3, 0x3c6df7f4a2f29a1f, 0x3fe89ffff78c6b50, 0xbc8e0453094995fd,
    0x3fe8e00039671566, 0xbc8a04f3bec77b45, 0x3fe91fffe2bf1745, 0xbc77fa34400e203c,
    0x3fe95fffcc5c9fd1, 0xbc76ff8005a0695d, 0x3fe9a0003bba4767, 0x3c70f8c4c4ec7e03,
    0x3fe9dfffe7b92da5, 0x3c8e7fd9478c4602, 0x3fea1fffd72efdaf, 0xbc6a0c554dcdae7e,
    0x3fea5fffde04ff95, 0x3c867da98ce9b26b, 0x3fea9fffca5e8d2b, 0xbc8284c9b54c13de,
    0x3feadfffddad03ea, 0x3c5812c8ea602e3c, 0x3feb1ffff10d3d4d, 0xbc8efaddad27789c,
    0x3feb5fffce21165a, 0x3c53cb1719c61237, 0x3feb9fffd950e674, 0x3c73f7d94194ce00,
    0x3febe000139ca8af, 0x3c750ac4215d9bc0, 0x3fec20005b46df99, 0x3c6beea653e9c1c9,
    0x3fec600040b9f7ae, 0xbc7c079f274a70d6, 0x3feca0006255fd8a, 0xbc7a0b4076e84c1f,
    0x3fecdfffd94c095d, 0x3c88f933f99ab5d7, 0x3fed1ffff975d6cf, 0xbc582c08665fe1be,
    0x3fed5fffa2561c93, 0xbc7b04289bd295f3, 0x3fed9fff9d228b0c, 0x3c870251340fa236,
    0x3fede00065bc7e16, 0xbc75011e16a4d80c, 0x3fee200002f64791, 0x3c89802f09ef62e0,
    0x3fee600057d7a6d8, 0xbc7e0b75580cf7fa, 0x3feea00027edc00c, 0xbc8c848309459811,
    0x3feee0006cf5cb7c, 0xbc8f8027951576f4, 0x3fef2000782b7dcc, 0xbc8f81d97274538f,
    0x3fef6000260c450a, 0xbc4071002727ffdc, 0x3fef9fffe88cd533, 0xbc581bdce1fda8b0,
    0x3fefdfffd50f8689, 0x3c87f91acb918e6e, 0x3ff0200004292367, 0x3c9b7ff365324681,
    0x3ff05fffe3e3d668, 0x3c86fa08ddae957b, 0x3ff0a0000a85a757, 0xbc57e2de80d3fb91,
    0x3ff0e0001a5f3fcc, 0xbc91823305c5f014, 0x3ff11ffff8afbaf5, 0xbc8bfabb6680bac2,
    0x3ff15fffe54d91ad, 0xbc9d7f121737e7ef, 0x3ff1a00011ac36e1, 0x3c9c000a0516f5ff,
    0x3ff1e00019c84248, 0xbc9082fbe4da5da0, 0x3ff220000ffe5e6e, 0xbc88fdd04c9cfb43,
    0x3ff26000269fd891, 0x3c8cfe2a7994d182, 0x3ff2a00029a6e6da, 0xbc700273715e8bc5,
    0x3ff2dfffe0293e39, 0x3c9b7c39dab2a6f9, 0x3ff31ffff7dcf082, 0x3c7df1336edc5254,
    0x3ff35ffff05a8b60, 0xbc9e03564ccd31eb, 0x3ff3a0002e0eaecc, 0x3c75f0e74bd3a477,
    0x3ff3e000043bb236, 0x3c9c7dcb149d8833, 0x3ff4200002d187ff, 0x3c7e08afcf2d3d28,
    0x3ff460000d387cb1, 0x3c820837856599a6, 0x3ff4a00004569f89, 0xbc89fa5c904fbcd2,
    0x3ff4e000043543f3, 0xbc781125ed175329, 0x3ff51fffcc027f0f, 0x3c9883d8847754dc,
    0x3ff55ffffd87b36f, 0xbc8709e731d02807, 0x3ff59ffff21df7ba, 0x3c87f79f68727b02,
    0x3ff5dfffebfc3481, 0xbc9180902e30e93e,
];

// pow_data.c: the struct's fields as bits, in order.
let libm::POW_LOG_DATA: u64[521] = [
    0x3fe62e42fefa3800, 0x3d2ef35793c76730, 0xbfe0000000000000, 0xbfe5555555555560,
    0x3fe0000000000006, 0x3fe999999959554e, 0xbfe555555529a47a, 0xbff2495b9b4845e9,
    0x3ff0002b8b263fc3, 0x3ff6a00000000000, 0x0000000000000000, 0xbfd62c82f2b9c800,
    0x3cfab42428375680, 0x3ff6800000000000, 0x0000000000000000, 0xbfd5d1bdbf580800,
    0xbd1ca508d8e0f720, 0x3ff6600000000000, 0x0000000000000000, 0xbfd5767717455800,
    0xbd2362a4d5b6506d, 0x3ff6400000000000, 0x0000000000000000, 0xbfd51aad872df800,
    0xbce684e49eb067d5, 0x3ff6200000000000, 0x0000000000000000, 0xbfd4be5f95777800,
    0xbd041b6993293ee0, 0x3ff6000000000000, 0x0000000000000000, 0xbfd4618bc21c6000,
    0x3d13d82f484c84cc, 0x3ff5e00000000000, 0x0000000000000000, 0xbfd404308686a800,
    0x3cdc42f3ed820b3a, 0x3ff5c00000000000, 0x0000000000000000, 0xbfd3a64c55694800,
    0x3d20b1c686519460, 0x3ff5a00000000000, 0x0000000000000000, 0xbfd347dd9a988000,
    0x3d25594dd4c58092, 0x3ff5800000000000, 0x0000000000000000, 0xbfd2e8e2bae12000,
    0x3d267b1e99b72bd8, 0x3ff5600000000000, 0x0000000000000000, 0xbfd2895a13de8800,
    0x3d15ca14b6cfb03f, 0x3ff5600000000000, 0x0000000000000000, 0xbfd2895a13de8800,
    0x3d15ca14b6cfb03f, 0x3ff5400000000000, 0x0000000000000000, 0xbfd22941fbcf7800,
    0xbd165a242853da76, 0x3ff5200000000000, 0x0000000000000000, 0xbfd1c898c1699800,
    0xbd1fafbc68e75404, 0x3ff5000000000000, 0x0000000000000000, 0xbfd1675cababa800,
    0x3d1f1fc63382a8f0, 0x3ff4e00000000000, 0x0000000000000000, 0xbfd1058bf9ae4800,
    0xbd26a8c4fd055a66, 0x3ff4c00000000000, 0x0000000000000000, 0xbfd0a324e2739000,
    0xbd0c6bee7ef4030e, 0x3ff4a00000000000, 0x0000000000000000, 0xbfd0402594b4d000,
    0xbcf036b89ef42d7f, 0x3ff4a00000000000, 0x0000000000000000, 0xbfd0402594b4d000,
    0xbcf036b89ef42d7f, 0x3ff4800000000000, 0x0000000000000000, 0xbfcfb9186d5e4000,
    0x3d0d572aab993c87, 0x3ff4600000000000, 0x0000000000000000, 0xbfcef0adcbdc6000,
    0x3d2b26b79c86af24, 0x3ff4400000000000, 0x0000000000000000, 0xbfce27076e2af000,
    0xbd172f4f543fff10, 0x3ff4200000000000, 0x0000000000000000, 0xbfcd5c216b4fc000,
    0x3d21ba91bbca681b, 0x3ff4000000000000, 0x0000000000000000, 0xbfcc8ff7c79aa000,
    0x3d27794f689f8434, 0x3ff4000000000000, 0x0000000000000000, 0xbfcc8ff7c79aa000,
    0x3d27794f689f8434, 0x3ff3e00000000000, 0x0000000000000000, 0xbfcbc286742d9000,
    0x3d194eb0318bb78f, 0x3ff3c00000000000, 0x0000000000000000, 0xbfcaf3c94e80c000,
    0x3cba4e633fcd9066, 0x3ff3a00000000000, 0x0000000000000000, 0xbfca23bc1fe2b000,
    0xbd258c64dc46c1ea, 0x3ff3a00000000000, 0x0000000000000000, 0xbfca23bc1fe2b000,
    0xbd258c64dc46c1ea, 0x3ff3800000000000, 0x0000000000000000, 0xbfc9525a9cf45000,
    0xbd2ad1d904c1d4e3, 0x3ff3600000000000, 0x0000000000000000, 0xbfc87fa06520d000,
    0x3d2bbdbf7fdbfa09, 0x3ff3400000000000, 0x0000000000000000, 0xbfc7ab890210e000,
    0x3d2bdb9072534a58, 0x3ff3400000000000, 0x0000000000000000, 0xbfc7ab890210e000,
    0x3d2bdb9072534a58, 0x3ff3200000000000, 0x0000000000000000, 0xbfc6d60fe719d000,
    0xbd10e46aa3b2e266, 0x3ff3000000000000, 0x0000000000000000, 0xbfc5ff3070a79000,
    0xbd1e9e439f105039, 0x3ff3000000000000, 0x0000000000000000, 0xbfc5ff3070a79000,
    0xbd1e9e439f105039, 0x3ff2e00000000000, 0x0000000000000000, 0xbfc526e5e3a1b000,
    0xbd20de8b90075b8f, 0x3ff2c00000000000, 0x0000000000000000, 0xbfc44d2b6ccb8000,
    0x3d170cc16135783c, 0x3ff2c00000000000, 0x0000000000000000, 0xbfc44d2b6ccb8000,
    0x3d170cc16135783c, 0x3ff2a00000000000, 0x0000000000000000, 0xbfc371fc201e9000,
    0x3cf178864d27543a, 0x3ff2800000000000, 0x0000000000000000, 0xbfc29552f81ff000,
    0xbd248d301771c408, 0x3ff2600000000000, 0x0000000000000000, 0xbfc1b72ad52f6000,
    0xbd2e80a41811a396, 0x3ff2600000000000, 0x0000000000000000, 0xbfc1b72ad52f6000,
    0xbd2e80a41811a396, 0x3ff2400000000000, 0x0000000000000000, 0xbfc0d77e7cd09000,
    0x3d0a699688e85bf4, 0x3ff2400000000000, 0x0000000000000000, 0xbfc0d77e7cd09000,
    0x3d0a699688e85bf4, 0x3ff2200000000000, 0x0000000000000000, 0xbfbfec9131dbe000,
    0xbd2575545ca333f2, 0x3ff2000000000000, 0x0000000000000000, 0xbfbe27076e2b0000,
    0x3d2a342c2af0003c, 0x3ff2000000000000, 0x0000000000000000, 0xbfbe27076e2b0000,
    0x3d2a342c2af0003c, 0x3ff1e00000000000, 0x0000000000000000, 0xbfbc5e548f5bc000,
    0xbd1d0c57585fbe06, 0x3ff1c00000000000, 0x0000000000000000, 0xbfba926d3a4ae000,
    0x3d253935e85baac8, 0x3ff1c00000000000, 0x0000000000000000, 0xbfba926d3a4ae000,
    0x3d253935e85baac8, 0x3ff1a00000000000, 0x0000000000000000, 0xbfb8c345d631a000,
    0x3d137c294d2f5668, 0x3ff1a00000000000, 0x0000000000000000, 0xbfb8c345d631a000,
    0x3d137c294d2f5668, 0x3ff1800000000000, 0x0000000000000000, 0xbfb6f0d28ae56000,
    0xbd269737c93373da, 0x3ff1600000000000, 0x0000000000000000, 0xbfb51b073f062000,
    0x3d1f025b61c65e57, 0x3ff1600000000000, 0x0000000000000000, 0xbfb51b073f062000,
    0x3d1f025b61c65e57, 0x3ff1400000000000, 0x0000000000000000, 0xbfb341d7961be000,
    0x3d2c5edaccf913df, 0x3ff1400000000000, 0x0000000000000000, 0xbfb341d7961be000,
    0x3d2c5edaccf913df, 0x3ff1200000000000, 0x0000000000000000, 0xbfb16536eea38000,
    0x3d147c5e768fa309, 0x3ff1000000000000, 0x0000000000000000, 0xbfaf0a30c0118000,
    0x3d2d599e83368e91, 0x3ff1000000000000, 0x0000000000000000, 0xbfaf0a30c0118000,
    0x3d2d599e83368e91, 0x3ff0e00000000000, 0x0000000000000000, 0xbfab42dd71198000,
    0x3d1c827ae5d6704c, 0x3ff0e00000000000, 0x0000000000000000, 0xbfab42dd71198000,
    0x3d1c827ae5d6704c, 0x3ff0c00000000000, 0x0000000000000000, 0xbfa77458f632c000,
    0xbd2cfc4634f2a1ee, 0x3ff0c00000000000, 0x0000000000000000, 0xbfa77458f632c000,
    0xbd2cfc4634f2a1ee, 0x3ff0a00000000000, 0x0000000000000000, 0xbfa39e87b9fec000,
    0x3cf502b7f526feaa, 0x3ff0a00000000000, 0x0000000000000000, 0xbfa39e87b9fec000,
    0x3cf502b7f526feaa, 0x3ff0800000000000, 0x0000000000000000, 0xbf9f829b0e780000,
    0xbd2980267c7e09e4, 0x3ff0800000000000, 0x0000000000000000, 0xbf9f829b0e780000,
    0xbd2980267c7e09e4, 0x3ff0600000000000, 0x0000000000000000, 0xbf97b91b07d58000,
    0xbd288d5493faa639, 0x3ff0400000000000, 0x0000000000000000, 0xbf8fc0a8b0fc0000,
    0xbcdf1e7cf6d3a69c, 0x3ff0400000000000, 0x0000000000000000, 0xbf8fc0a8b0fc0000,
    0xbcdf1e7cf6d3a69c, 0x3ff0200000000000, 0x0000000000000000, 0xbf7fe02a6b100000,
    0xbd19e23f0dda40e4, 0x3ff0200000000000, 0x0000000000000000, 0xbf7fe02a6b100000,
    0xbd19e23f0dda40e4, 0x3ff0000000000000, 0x0000000000000000, 0x0000000000000000,
    0x0000000000000000, 0x3ff0000000000000, 0x0000000000000000, 0x0000000000000000,
    0x0000000000000000, 0x3fefc00000000000, 0x0000000000000000, 0x3f80101575890000,
    0xbd10c76b999d2be8, 0x3fef800000000000, 0x0000000000000000, 0x3f90205658938000,
    0xbd23dc5b06e2f7d2, 0x3fef400000000000, 0x0000000000000000, 0x3f98492528c90000,
    0xbd2aa0ba325a0c34, 0x3fef000000000000, 0x0000000000000000, 0x3fa0415d89e74000,
    0x3d0111c05cf1d753, 0x3feec00000000000, 0x0000000000000000, 0x3fa466aed42e0000,
    0xbd2c167375bdfd28, 0x3fee800000000000, 0x0000000000000000, 0x3fa894aa149fc000,
    0xbd197995d05a267d, 0x3fee400000000000, 0x0000000000000000, 0x3faccb73cdddc000,
    0xbd1a68f247d82807, 0x3fee200000000000, 0x0000000000000000, 0x3faeea31c006c000,
    0xbd0e113e4fc93b7b, 0x3fede00000000000, 0x0000000000000000, 0x3fb1973bd1466000,
    0xbd25325d560d9e9b, 0x3feda00000000000, 0x0000000000000000, 0x3fb3bdf5a7d1e000,
    0x3d2cc85ea5db4ed7, 0x3fed600000000000, 0x0000000000000000, 0x3fb5e95a4d97a000,
    0xbd2c69063c5d1d1e, 0x3fed400000000000, 0x0000000000000000, 0x3fb700d30aeac000,
    0x3cec1e8da99ded32, 0x3fed000000000000, 0x0000000000000000, 0x3fb9335e5d594000,
    0x3d23115c3abd47da, 0x3fecc00000000000, 0x0000000000000000, 0x3fbb6ac88dad6000,
    0xbd1390802bf768e5, 0x3feca00000000000, 0x0000000000000000, 0x3fbc885801bc4000,
    0x3d2646d1c65aacd3, 0x3fec600000000000, 0x0000000000000000, 0x3fbec739830a2000,
    0xbd2dc068afe645e0, 0x3fec400000000000, 0x0000000000000000, 0x3fbfe89139dbe000,
    0xbd2534d64fa10afd, 0x3fec000000000000, 0x0000000000000000, 0x3fc1178e8227e000,
    0x3d21ef78ce2d07f2, 0x3febe00000000000, 0x0000000000000000, 0x3fc1aa2b7e23f000,
    0x3d2ca78e44389934, 0x3feba00000000000, 0x0000000000000000, 0x3fc2d1610c868000,
    0x3d039d6ccb81b4a1, 0x3feb800000000000, 0x0000000000000000, 0x3fc365fcb0159000,
    0x3cc62fa8234b7289, 0x3feb400000000000, 0x0000000000000000, 0x3fc4913d8333b000,
    0x3d25837954fdb678, 0x3feb200000000000, 0x0000000000000000, 0x3fc527e5e4a1b000,
    0x3d2633e8e5697dc7, 0x3feae00000000000, 0x0000000000000000, 0x3fc6574ebe8c1000,
    0x3d19cf8b2c3c2e78, 0x3feac00000000000, 0x0000000000000000, 0x3fc6f0128b757000,
    0xbd25118de59c21e1, 0x3feaa00000000000, 0x0000000000000000, 0x3fc7898d85445000,
    0xbd1c661070914305, 0x3fea600000000000, 0x0000000000000000, 0x3fc8beafeb390000,
    0xbd073d54aae92cd1, 0x3fea400000000000, 0x0000000000000000, 0x3fc95a5adcf70000,
    0x3d07f22858a0ff6f, 0x3fea000000000000, 0x0000000000000000, 0x3fca93ed3c8ae000,
    0xbd28724350562169, 0x3fe9e00000000000, 0x0000000000000000, 0x3fcb31d8575bd000,
    0xbd0c358d4eace1aa, 0x3fe9c00000000000, 0x0000000000000000, 0x3fcbd087383be000,
    0xbd2d4bc4595412b6, 0x3fe9a00000000000, 0x0000000000000000, 0x3fcc6ffbc6f01000,
    0xbcf1ec72c5962bd2, 0x3fe9600000000000, 0x0000000000000000, 0x3fcdb13db0d49000,
    0xbd2aff2af715b035, 0x3fe9400000000000, 0x0000000000000000, 0x3fce530effe71000,
    0x3cc212276041f430, 0x3fe9200000000000, 0x0000000000000000, 0x3fcef5ade4dd0000,
    0xbcca211565bb8e11, 0x3fe9000000000000, 0x0000000000000000, 0x3fcf991c6cb3b000,
    0x3d1bcbecca0cdf30, 0x3fe8c00000000000, 0x0000000000000000, 0x3fd07138604d5800,
    0x3cf89cdb16ed4e91, 0x3fe8a00000000000, 0x0000000000000000, 0x3fd0c42d67616000,
    0x3d27188b163ceae9, 0x3fe8800000000000, 0x0000000000000000, 0x3fd1178e8227e800,
    0xbd2c210e63a5f01c, 0x3fe8600000000000, 0x0000000000000000, 0x3fd16b5ccbacf800,
    0x3d2b9acdf7a51681, 0x3fe8400000000000, 0x0000000000000000, 0x3fd1bf99635a6800,
    0x3d2ca6ed5147bdb7, 0x3fe8200000000000, 0x0000000000000000, 0x3fd214456d0eb800,
    0x3d0a87deba46baea, 0x3fe7e00000000000, 0x0000000000000000, 0x3fd2bef07cdc9000,
    0x3d2a9cfa4a5004f4, 0x3fe7c00000000000, 0x0000000000000000, 0x3fd314f1e1d36000,
    0xbd28e27ad3213cb8, 0x3fe7a00000000000, 0x0000000000000000, 0x3fd36b6776be1000,
    0x3d116ecdb0f177c8, 0x3fe7800000000000, 0x0000000000000000, 0x3fd3c25277333000,
    0x3d183b54b606bd5c, 0x3fe7600000000000, 0x0000000000000000, 0x3fd419b423d5e800,
    0x3d08e436ec90e09d, 0x3fe7400000000000, 0x0000000000000000, 0x3fd4718dc271c800,
    0xbd2f27ce0967d675, 0x3fe7200000000000, 0x0000000000000000, 0x3fd4c9e09e173000,
    0xbd2e20891b0ad8a4, 0x3fe7000000000000, 0x0000000000000000, 0x3fd522ae0738a000,
    0x3d2ebe708164c759, 0x3fe6e00000000000, 0x0000000000000000, 0x3fd57bf753c8d000,
    0x3d1fadedee5d40ef, 0x3fe6c00000000000, 0x0000000000000000, 0x3fd5d5bddf596000,
    0xbd0a0b2a08a465dc,
];

// exp2f_data.c: the struct's fields as bits, in order.
let libm::EXP2F_DATA: u64[41] = [
    0x3ff0000000000000, 0x3fefd9b0d3158574, 0x3fefb5586cf9890f, 0x3fef9301d0125b51,
    0x3fef72b83c7d517b, 0x3fef54873168b9aa, 0x3fef387a6e756238, 0x3fef1e9df51fdee1,
    0x3fef06fe0a31b715, 0x3feef1a7373aa9cb, 0x3feedea64c123422, 0x3feece086061892d,
    0x3feebfdad5362a27, 0x3feeb42b569d4f82, 0x3feeab07dd485429, 0x3feea47eb03a5585,
    0x3feea09e667f3bcd, 0x3fee9f75e8ec5f74, 0x3feea11473eb0187, 0x3feea589994cce13,
    0x3feeace5422aa0db, 0x3feeb737b0cdc5e5, 0x3feec49182a3f090, 0x3feed503b23e255d,
    0x3feee89f995ad3ad, 0x3feeff76f2fb5e47, 0x3fef199bdd85529c, 0x3fef3720dcef9069,
    0x3fef5818dcfba487, 0x3fef7c97337b9b5f, 0x3fefa4afa2a490da, 0x3fefd0765b6e4540,
    0x42e8000000000000, 0x3fac6af84b912394, 0x3fcebfce50fac4f3, 0x3fe62e42ff0c52d6,
    0x4338000000000000, 0x40471547652b82fe, 0x3ebc6af84b912394, 0x3f2ebfce50fac4f3,
    0x3f962e42ff0c52d6,
];

// logf_data.c: the struct's fields as bits, in order.
let libm::LOGF_DATA: u64[36] = [
    0x3ff661ec79f8f3be, 0xbfd57bf7808caade, 0x3ff571ed4aaf883d, 0xbfd2bef0a7c06ddb,
    0x3ff49539f0f010b0, 0xbfd01eae7f513a67, 0x3ff3c995b0b80385, 0xbfcb31d8a68224e9,
    0x3ff30d190c8864a5, 0xbfc6574f0ac07758, 0x3ff25e227b0b8ea0, 0xbfc1aa2bc79c8100,
    0x3ff1bb4a4a1a343f, 0xbfba4e76ce8c0e5e, 0x3ff12358f08ae5ba, 0xbfb1973c5a611ccc,
    0x3ff0953f419900a7, 0xbfa252f438e10c1e, 0x3ff0000000000000, 0x0000000000000000,
    0x3fee608cfd9a47ac, 0x3faaa5aa5df25984, 0x3feca4b31f026aa0, 0x3fbc5e53aa362eb4,
    0x3feb2036576afce6, 0x3fc526e57720db08, 0x3fe9c2d163a1aa2d, 0x3fcbc2860d224770,
    0x3fe886e6037841ed, 0x3fd1058bc8a07ee1, 0x3fe767dcf5534862, 0x3fd4043057b6ee09,
    0x3fe62e42fefa39ef, 0xbfd00ea348b88334, 0x3fd5575b0be00b6a, 0xbfdffffef20a4123,
];

// log2f_data.c: the struct's fields as bits, in order.
let libm::LOG2F_DATA: u64[36] = [
    0x3ff661ec79f8f3be, 0xbfdefec65b963019, 0x3ff571ed4aaf883d, 0xbfdb0b6832d4fca4,
    0x3ff49539f0f010b0, 0xbfd7418b0a1fb77b, 0x3ff3c995b0b80385, 0xbfd39de91a6dcf7b,
    0x3ff30d190c8864a5, 0xbfd01d9bf3f2b631, 0x3ff25e227b0b8ea0, 0xbfc97c1d1b3b7af0,
    0x3ff1bb4a4a1a343f, 0xbfc2f9e393af3c9f, 0x3ff12358f08ae5ba, 0xbfb960cbbf788d5c,
    0x3ff0953f419900a7, 0xbfaa6f9db6475fce, 0x3ff0000000000000, 0x0000000000000000,
    0x3fee608cfd9a47ac, 0x3fb338ca9f24f53d, 0x3feca4b31f026aa0, 0x3fc476a9543891ba,
    0x3feb2036576afce6, 0x3fce840b4ac4e4d2, 0x3fe9c2d163a1aa2d, 0x3fd40645f0c6651c,
    0x3fe886e6037841ed, 0x3fd88e9c2c1b9ff8, 0x3fe767dcf5534862, 0x3fdce0a44eb17bcc,
    0xbfd712b6f70a7e4d, 0x3fdecabf496832e0, 0xbfe715479ffae3de, 0x3ff715475f35c8b8,
];

// powf_data.c: the struct's fields as bits, in order.
let libm::POWF_LOG2_DATA: u64[37] = [
    0x3ff661ec79f8f3be, 0xbfdefec65b963019, 0x3ff571ed4aaf883d, 0xbfdb0b6832d4fca4,
    0x3ff49539f0f010b0, 0xbfd7418b0a1fb77b, 0x3ff3c995b0b80385, 0xbfd39de91a6dcf7b,
    0x3ff30d190c8864a5, 0xbfd01d9bf3f2b631, 0x3ff25e227b0b8ea0, 0xbfc97c1d1b3b7af0,
    0x3ff1bb4a4a1a343f, 0xbfc2f9e393af3c9f, 0x3ff12358f08ae5ba, 0xbfb960cbbf788d5c,
    0x3ff0953f419900a7, 0xbfaa6f9db6475fce, 0x3ff0000000000000, 0x0000000000000000,
    0x3fee608cfd9a47ac, 0x3fb338ca9f24f53d, 0x3feca4b31f026aa0, 0x3fc476a9543891ba,
    0x3feb2036576afce6, 0x3fce840b4ac4e4d2, 0x3fe9c2d163a1aa2d, 0x3fd40645f0c6651c,
    0x3fe886e6037841ed, 0x3fd88e9c2c1b9ff8, 0x3fe767dcf5534862, 0x3fdce0a44eb17bcc,
    0x3fd27616c9496e0b, 0xbfd71969a075c67a, 0x3fdec70a6ca7badd, 0xbfe7154748bef6c8,
    0x3ff71547652ab82b,
];
