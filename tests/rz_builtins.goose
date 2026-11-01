// clear, pop, pop_n, resize and assert on resizable arrays, including the
// aborts their misuse ends in.
struct P { a: i32, b: i32 }

fn main() {
    var xs: i64[>..<] = [1, 2, 3, 4, 5];
    let last = xs.pop();
    xs.pop_n(2);
    print(last, " ", xs.len, " ", xs);
    xs.resize(5, 9);
    print(xs);
    xs.resize(1);
    xs.push(7);
    print(xs);
    xs.clear();
    xs.push(8);
    print(xs, " ", xs.len);
    var ps: P[>..<] = [P { a: 1, b: 2 }, P { a: 3, b: 4 }];
    let p = ps.pop();
    ps.resize(3, P { a: 0, b: -1 });
    print(p, " ", ps);
    assert(ps.len == 3);
    var empty: i64[>..<] = [];
    for round in 0..3 {
        var tmp: i64[>..<] = [round];
        tmp.clear();
        empty.push(round);
    }
    print(empty);
    empty.clear();
    print(empty.pop());
}
