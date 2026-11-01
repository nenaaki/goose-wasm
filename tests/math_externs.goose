// The standard library's computing externs (§7.10): square roots,
// rounding, bit counts, frexp/ldexp and byte-set scans, against upstream's
// C library and runtime.
import std;
import math;

fn main() {
    print(sqrt(16.0), " ", sqrt(2.0), " ", sqrt(2.0 as f32), " ", sqrt(-1.0));
    for x in [2.5, -2.5, 0.49999999999999994, -0.3, 1.5, 3.7, -3.7, 1e300] {
        print(x, ": ", floor(x), " ", ceil(x), " ", trunc(x), " ", round(x));
    }
    for x in [2.5 as f32, -0.5 as f32, 7.25] {
        print(floor(x), " ", ceil(x), " ", trunc(x), " ", round(x));
    }
    for v in [0 as u64, 1, 0xff00, 0x8000000000000000] {
        print(popcount(v), " ", clz(v), " ", ctz(v));
    }
    let comma = byte_set(",");
    print(find_any("hello, world", comma), " ", find_any("abc", comma), " ", find_any("", comma));
    let vowels = byte_set("aeiou");
    let digits = byte_set("0123456789");
    print(find_pair("xa1b2e3", vowels, 1, digits), " ", find_pair("aa", vowels, 1, digits));
    print(parse_flt("0.000123"), " ", parse_flt("1e-310"), " ", parse_flt("123456789012345678901234567890"));
}
