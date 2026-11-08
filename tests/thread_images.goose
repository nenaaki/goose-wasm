// Values crossing threads (spec §11.2) as images: through queues, as a
// worker's arguments, and as the zero value a missed qpoll gives, for each
// kind of flat value: scalars of every width, fixed structs and ADTs,
// variable-size values, resizable arrays (of variable-size elements too),
// frame objects, nested ones, and resizable structs with a variable-size
// prefix (as arguments only: upstream sends none through a queue).
// Deterministic: each receive follows the put it takes.

struct Pt { x: f32, y: f64, tag: u16, on: bool }
enum Shape { Dot, Box { w: i32, h: i32 } }
struct Bag { id: i64, data: i64[>..] }
struct Outer { tag: u8, bag: Bag }
struct Named { name: u8[], data: i32[>..] }
struct NamedBag { name: u8[], bag: Bag }

thread_fn echo(a: i8, b: u32, c: f64, p: Pt, s: Shape, t: u8[], var xs: i64[>..],
               var ws: u8[][>..], var o: Outer, var n: Named, var nb: NamedBag) {
    xs.push(4);
    ws.push("w");
    o.bag.data.push(5);
    n.data.push(6);
    nb.bag.data.push(7);
    // Values with a variable-size prefix cross as arguments, not through a
    // queue, which upstream does not support.
    print(n.name, " ", n.data, " ", nb.name, " ", nb.bag.id, " ", nb.bag.data);
    qput(a);
    qput(b);
    qput(c);
    qput(p);
    qput(s);
    qput(t);
    qput(xs);
    qput(ws);
    qput(o);
}

fn main() {
    let id = thread_spawn(echo, -5 as i8, 4000000000 as u32, 2.5, Pt { x: 1.5, y: -2.25, tag: 65535, on: true },
                          Shape.Box { w: 3, h: 4 }, "text", [1, 2, 3], ["a", "bc"],
                          Outer { tag: 9, bag: Bag { id: 8, data: [10] } },
                          Named { name: "nm", data: [11, 12] },
                          NamedBag { name: "nb", bag: Bag { id: 13, data: [] } });
    print(qget<i8>(), " ", qget<u32>(), " ", qget<f64>());
    let p = qget<Pt>();
    print(p.x, " ", p.y, " ", p.tag, " ", p.on);
    match qget<Shape>() {
        Dot => print("dot"),
        Box b => print("box ", b.w, "x", b.h),
    }
    print(qget<u8[]>());
    let xs = qget<i64[>..]>();
    print(xs, " ", xs.len);
    var ws = qget<u8[][>..]>();
    ws.push("main");
    print(ws);
    var o = qget<Outer>();
    o.bag.data.push(50);
    print(o.tag, " ", o.bag.id, " ", o.bag.data);
    print(thread_wait(id));

    // Misses: every kind's zero value.
    let a, gota = qpoll<i8>();
    let pp, gotp = qpoll<Pt>();
    let ss, gots = qpoll<Shape>();
    let tt, gott = qpoll<u8[]>();
    print(a, " ", gota, " ", pp.x, " ", pp.tag, " ", pp.on, " ", gotp, " ", gots, " ", tt.len, " ", gott);
    let ys, goty = qpoll<i64[>..]>();
    let oo, goto = qpoll<Outer>();
    print(ys.len, " ", goty, " ", oo.tag, " ", oo.bag.id, " ", oo.bag.data.len, " ", goto);
    // And a hit through the same calls, after values that were longer.
    qput(Bag { id: 1, data: [2, 3] });
    let hit, goth = qget<Bag>(0);
    print(hit.id, " ", hit.data, " ", goth);
}
