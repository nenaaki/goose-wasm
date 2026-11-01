// Structural rendering (spec §3.7): arrays and slices as [a, b], structs as
// their positional literal with the type's name, references as their
// pointee, a null optional as null, u8 arrays as their bytes at the top and
// quoted and escaped inside a value, a type reaching itself through
// references, and frame objects with their tail.
struct P { x: i32, y: i32 }
struct Pair<T> { a: T, b: T }
struct Named { name: const u8[:], ok: bool, pad 2, at: P }
struct Node { v: i64, next: Node? }
struct Empty {}
struct Bag { n: u8, items: i64[>..] }

fn make(n: i64) -> i64[>..] {
    var r: i64[>..] = [];
    for i in 0..n { r.push(i * 10); }
    r
}

fn main() {
    let p = P { x: 1, y: -2 };
    print(p);
    print(Pair<P> { a: p, b: P { x: 3, y: 4 } });
    var xs: i64[>..] = [1, 2, 3];
    print(xs, " ", xs[1..], " ", make(0), " ", make(3));
    let fixed: P[2] = [p, P { x: 5, y: 6 }];
    print(fixed);
    print(Named { name: "say \"hi\"\n\ttab\\", ok: true, at: p });
    var tail = Node { v: 2, next: null };
    var head = Node { v: 1, next: tail };
    print(head);
    print(Empty {});
    let r .= p;
    print(r, " ", r.x);
    var names: (const u8[:])[>..] = ["a", "b c"];
    print(names);
    var b = Bag { n: 3, items: [7, 8] };
    print(b);
    let words: const u8[:][2] = ["x", ""];
    print(words, "|", "plain");
}
