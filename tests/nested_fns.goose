// Nested functions (spec §7.5): free variables read and written by
// reference, a captured resizable array grown from inside, nested
// functions calling each other (and themselves), overloads, and a free
// variable reached only through a callee.
fn run(start: i64) -> i64 {
    var total = start;
    var log: i64[>..] = [];
    var r: i64& = total;
    fn add(k: i64) {
        total += k;
        log.push(k);
    }
    fn add(b: bool) { if b { add(100); } }
    fn twice(k: i64) {
        add(k);
        add(k);
    }
    recursive fn down(n: i64) -> i64 {
        if n == 0 { return total; }
        add(n);
        return down(n - 1);
    }
    twice(3);
    add(true);
    let d = down(4);
    r += 1000;
    print(total, " ", log, " ", d);
    total
}

fn main() {
    print(run(1));
    print(run(-5));
}
