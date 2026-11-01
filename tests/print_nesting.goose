// A print inside another print's argument has a builder of its own, so its
// line is written whole, before the outer one (spec §3.7); a line longer
// than any fixed buffer is written whole too.
fn noisy(v: i64) -> i64 {
    print("inner ", v);
    v * 2
}

fn main() {
    print("outer ", noisy(1), " and ", noisy(2), " done");
    var big: u8[>..] = [];
    for i in 0..100000 { big.push('a' + (i % 26) as u8); }
    print(big.len, " ", big[0..5], " ", big);
}
