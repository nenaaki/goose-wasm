// Long-distance returns (`return E from f`) of results built where f's
// caller wants them: a resizable array, a struct ending in one, a fixed
// struct, several results mixing them, and a target active more than once.
struct Bag { tag: i64, items: i64[>..] }
struct P { x: i64, y: i64 }

fn leave_arr(k: i64) { return [k, k + 1, k + 2] from arr; }
fn arr(k: i64) -> i64[>..] {
    var scratch: i64[>..] = [1, 2];
    scratch.push(3);
    if k > 0 { leave_arr(k); }
    return [0];
}

fn leave_p(k: i64) { return P { x: k, y: -k } from point; }
fn point(k: i64) -> P {
    if k > 0 { leave_p(k); }
    return P { x: 0, y: 0 };
}

fn leave_many(k: i64) { return [k], Bag { tag: k, items: [k * 2] }, k * 3 from many; }
fn many(k: i64) -> i64[>..], Bag, i64 {
    var tmp: i64[>..] = [5];
    if k > 0 { leave_many(k); }
    return tmp, Bag { tag: 0, items: [] }, 0;
}

// The innermost activation of the target takes the return.
fn leave_pt(k: i64) { return P { x: k, y: 0 } from pt; }
recursive fn pt(k: i64) -> P {
    if k > 1 {
        let q = pt(k - 1);
        leave_pt(q.x * 10 + k);
    }
    leave_pt(1);
    return P { x: -1, y: -1 };
}

fn main() {
    var a = arr(4);
    a.push(9);
    print(a, " ", arr(0));
    print(point(3), " ", point(0));
    var xs, b, n = many(2);
    xs.push(1);
    b.items.push(1);
    print(xs, " ", b, " ", n);
    let ys, c, m = many(0);
    print(ys, " ", c, " ", m);
    print(pt(3), " ", pt(1));
}
