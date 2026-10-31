// An array literal where a slice is expected (§3.10) is a temporary array
// the slice views: elements, a fill, or none.
struct P { x: i64, y: i64 }
fn sum(xs: i64[:]) -> i64 { var t = 0; for x in xs { t += x; } t }
fn names(ns: const u8[:][:]) -> i64 { var t = 0; for n in ns { t += n.len; } t }
fn first(ps: P[:]) -> i64 { if ps.len > 0 { ps[0].x + ps[0].y } else { -1 } }
fn main() {
    print(sum([1, 2, 3]), " ", sum([]), " ", sum([5; 4]));
    print(names(["ab", "cde"]), " ", first([P { x: 1, y: 2 }]), " ", first([]));
    let n = 3;
    print(sum([n * 2; 3]));
}
