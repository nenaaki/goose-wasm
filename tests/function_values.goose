// Function values (§7.6): a generic function parameter bound to a block or
// a named function; a call of it splices the body in, its parameters bound
// to the arguments; the block reads and writes the variables of the
// function it was written in.
struct P { x: i64, y: i64 }

fn apply<F>(n: i64) -> i64 { F(n) }
fn twice<F>(n: i64) -> i64 { F(F(n)) }
fn each<F>(xs: i64[:]) { for i in 0..xs.len { F(&xs[i]); } }
fn make<F>(k: i64) -> P { F(k) }
fn build<F>(n: i64) -> i64[>..] { var r: i64[>..] = []; for i in n { r.push(F(i)); } r }
fn sq(x: i64) -> i64 { x * x }

fn main() {
    var total = 0;
    let k = 3;
    print(apply(4) { it + k });
    print(twice(2) { total += it; it * 10 }, " ", total);
    var a: i64[4] = [1, 2, 3, 4];
    each(a) { it *= 2; total += it; };
    print(a, " ", total);
    print(make(5) { P { x: it, y: it + k } });
    print(build(4) { it * k });
    print(apply(7, sq));
}
