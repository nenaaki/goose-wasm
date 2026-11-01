// A nested function reaching a loop's variables, and a resizable reached
// through a reference, takes them by reference like any free variable
// (§7.5); the optimizer's copies of such calls pass the copies.
fn weigh(a: i64[:]) -> i64 {
    var total = 0;
    for x, i in a {
        fn add() { total += x * i; }
        add();
    }
    for k in 3 {
        fn bump() { total += k; }
        bump();
    }
    total
}

fn fill(out: i64[>..]&, n: i64) {
    fn put(v: i64) { out.push(v); }
    for i in n { put(i * i); }
}

fn main() {
    print(weigh([1, 2, 3]));
    var xs: i64[>..] = [];
    fill(xs, 4);
    print(xs);
}
