// math's libm functions (lib/math.goose, ported from musl) at both widths:
// their special values, which every libm gives exactly, and results rounded
// to nine digits, which the platform libm upstream's oracle runs on agrees
// with too (scripts/libm_check.py checks the port against musl bit for bit).
// The arguments come from an array, so no value is known to the optimizer.
import math;
import std;

fn r9(x: f64) -> f64 { round(x * 1e9) / 1e9 }
fn r5(x: f32) -> f32 { round(x * 100000.0) / 100000.0 }

fn main() {
    var a: f64[>..] = [0.5, -0.5, 1.0, 2.0, 3.0, 10.0, 100.0, 1e22, -7.25, 0.001];
    for x in a {
        print(x, ": ", r9(sin(x)), " ", r9(cos(x)), " ", r9(tan(x)), " ", r9(atan(x)), " ",
              r9(exp(x / 10.0)), " ", r9(log(abs(x))), " ", r9(log2(abs(x))), " ", r9(log10(abs(x))));
    }
    var u: f64[>..] = [0.0, 0.25, -0.75, 1.0];
    for x in u { print(r9(asin(x)), " ", r9(acos(x))); }
    var p: f64[>..] = [2.0, 10.0, -2.0, 3.0, 0.5, -1.5, 9.0, 0.5, -8.0, 3.0, 1.0001, 10000.0];
    var i = 0;
    while i < p.len {
        print(r9(pow(p[i], p[i + 1])), " ", r9(atan2(p[i], p[i + 1])));
        i += 2;
    }
    // Special values.
    var s: f64[>..] = [0.0, -0.0, 1.0, -1.0];
    let z = s[0];
    let inf = 1.0 / z;
    print(sin(s[1]), " ", tan(s[1]), " ", atan(s[1]), " ", asin(s[1]), " ", cos(s[0]), " ", exp(s[0]));
    print(log(s[0]), " ", log(s[2]), " ", log2(s[0]), " ", log10(s[2]), " ", exp(-inf), " ", exp(inf), " ", exp(1000.0 * s[2]));
    print(is_nan(log(s[3])), " ", is_nan(sin(inf)), " ", is_nan(acos(2.0 * s[2])), " ", is_nan(pow(-8.0 * s[2], 1.0 / 3.0)));
    print(pow(s[0], s[0]), " ", pow(-2.0 * s[2], 3.0), " ", pow(s[0], -s[2]), " ", pow(s[1], -s[2]), " ", pow(-s[2], inf), " ", pow(0.5 * s[2], -inf));
    print(atan2(s[0], s[3]), " ", atan2(s[1], s[3]), " ", atan2(s[2], s[0]), " ", atan2(inf, inf), " ", atan2(s[0], s[2]));

    // f32.
    var b: f32[>..] = [0.5, -0.5, 1.0, 2.0, 3.0, 10.0, 100.0, 1e20, -7.25, 0.001];
    for x in b {
        print(x, ": ", r5(sin(x)), " ", r5(cos(x)), " ", r5(tan(x)), " ", r5(atan(x)), " ",
              r5(exp(x / 10.0)), " ", r5(log(abs(x))), " ", r5(log2(abs(x))), " ", r5(log10(abs(x))));
    }
    var v: f32[>..] = [0.0, 0.25, -0.75, 1.0];
    for x in v { print(r5(asin(x)), " ", r5(acos(x))); }
    var q: f32[>..] = [2.0, 10.0, -2.0, 3.0, 0.5, -1.5, 9.0, 0.5, -8.0, 3.0, 1.0001, 10000.0];
    i = 0;
    while i < q.len {
        print(r5(pow(q[i], q[i + 1])), " ", r5(atan2(q[i], q[i + 1])));
        i += 2;
    }
    var t: f32[>..] = [0.0, -0.0, 1.0, -1.0];
    let finf = 1.0 / t[0];
    print(sin(t[1]), " ", log(t[0]), " ", log2(t[2]), " ", exp(-finf), " ", exp(200.0 * t[2]), " ", is_nan(log(t[3])), " ", pow(t[0], t[0]), " ", pow(t[1], -t[2]));
}
