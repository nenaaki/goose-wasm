// Calls of specializations with scalar parameters and results: recursion
// (not inlined), generics, several results, and a call's results dropped.
recursive fn fib(n: i64) -> i64 {
    if n < 2 { return n; }
    return fib(n - 1) + fib(n - 2);
}

fn divmod(a: i32, b: i32) -> i32, i32 {
    return a / b, a % b;
}

fn twice<T>(x: T) -> T { x + x }

fn gcd(a: u64, b: u64) -> u64 {
    var x = a;
    var y = b;
    while y != 0 {
        let t = x % y;
        x = y;
        y = t;
    }
    x
}

fn collatz(n: i64) -> i64 {
    var steps: i64 = 0;
    var k = n;
    while k != 1 {
        k = if k % 2 == 0 { k / 2 } else { 3 * k + 1 };
        steps++;
    }
    steps
}

fn main() {
    print(fib(25));
    let q, r = divmod(-17, 5);
    print(q, " ", r);
    divmod(1, 1);
    print(twice(21), " ", twice(250 as u8), " ", twice(-4 as i8));
    print(gcd(1071, 462), " ", collatz(27));
    swap_test();
}

// The statement form, into existing variables.
fn swap_test() {
    var x: i32 = 0;
    var y: i32 = 0;
    x, y = divmod(23, -4);
    print(x, " ", y);
}
