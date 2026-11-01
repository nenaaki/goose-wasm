// Relative references of varint width (§3.6, §3.9): held as the reference
// they encode, stored as a zigzag offset, which makes what holds them
// variable-size. As locals and parameters, struct and variant fields,
// elements iterated (bound to the reference they hold), omitted (null),
// printed through, and round-tripped through to_bytes / from_bytes.
struct Cell { v: i64 }
var cells: Cell[>..] = [];
struct Link { name: u8[], to: Cell&<varint in cells>, back: Cell&<varint in cells>? }
enum Node { Leaf { v: i64 }, Ref { to: Node..&<varint>?, w: i64 } }

fn via(r: Cell&<varint in cells>?) -> i64 {
    if r { return r.v; }
    return -1;
}

fn main() {
    while cells.len < 6 { cells.push(Cell { v: 10 * cells.len }); }
    let a: Cell&<varint in cells> = cells[3];
    let n: Cell&<varint in cells>? = null;
    print(a.v, " ", via(a), " ", via(n), " ", via(cells[5]), " ", a .== cells[3]);

    var links: Link[>..] = [];
    links.push(Link { name: "one", to: cells[1], back: cells[2] });
    links.push(Link { name: "two", to: cells[4] });
    for l in links {
        let b = l.back;
        print(l.name, " ", l.to.v, " ", if b { b.v } else { -1 });
    }
    var rs: (Cell&<varint in cells>?)[>..] = [cells[5], null, cells[0]];
    for r in rs { print(via(r)); }
    for r in rs { if r { r.v += 1; } }
    print(cells[5].v, " ", cells[0].v);

    var nodes: Node..[>..] = [];
    let leaf: Node..& = nodes.push(Node.Leaf { v: 7 });
    nodes.push(Node.Ref { to: leaf, w: 2 });
    nodes.push(Node.Ref { w: 3 });
    var img = to_bytes(nodes);
    let back, ok = from_bytes<Node..[>..]>(img);
    var sum = 0;
    for x in back {
        match x {
            Leaf &l => { sum += l.v; },
            Ref &r => {
                let t = r.to;
                if t { match t { Leaf &l => { sum += 100 * l.v * r.w; }, Ref &_ => {} } }
                sum += r.w;
            }
        }
    }
    print(ok, " ", back.len, " ", sum);
    img.push(0);
    let bad, bok = from_bytes<Node..[>..]>(img);
    print(bok, " ", bad.len);
}
