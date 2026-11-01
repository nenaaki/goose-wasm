// from_bytes (§12) verifies an image to_bytes made, or rejects one that is
// not: fixed and variable elements, booleans, tags, nested arrays, index
// types and self-relative links checked, into a resizable array or a T[];
// to_bytes into a limited builder; bytes_of over variable elements.
struct P { x: i32, y: i16 }
struct V { tag: u8, xs: varint[], k: varint }
enum Op { Push { v: i64 }, Pop, Jump { to: u16 } }
struct Node { v: i32, next: Node&<u32>? }
var items: i64[>..] = [];
type Item = u32 in items;

fn main() {
    var ps: P[>..] = [P { x: 1, y: 2 }, P { x: -3, y: 4 }];
    let a, aok = from_bytes<P[>..]>(to_bytes(ps));
    print(aok, " ", a);
    let flat, fok = from_bytes<P[]>(to_bytes(ps));
    print(fok, " ", flat.len, " ", flat[1].x);

    var vs: V[>..] = [V { tag: 1, xs: [1, -200, 3], k: 70000 }, V { tag: 2, xs: [], k: 0 }];
    var img = to_bytes(vs);
    let b, bok = from_bytes<V[>..]>(img);
    print(bok, " ", b, " ", vs.bytes_of().len);
    img.pop();
    let b2, bok2 = from_bytes<V[>..]>(img);
    print(bok2, " ", b2.len);

    var ops: Op[>..] = [Op.Push { v: 5 }, Op.Pop, Op.Jump { to: 1 }];
    var oimg = to_bytes(ops);
    let o, ook = from_bytes<Op[>..]>(oimg);
    print(ook, " ", o);
    oimg[1] = 9;
    let o2, ook2 = from_bytes<Op[>..]>(oimg);
    print(ook2, " ", o2.len);

    let bools: u8[4] = [3, 0, 1, 0];
    let bs, bsok = from_bytes<bool[>..]>(bools);
    print(bsok, " ", bs);
    let badb: u8[3] = [2, 1, 2];
    let bs2, bsok2 = from_bytes<bool[>..]>(badb);
    print(bsok2, " ", bs2.len);

    for i in 3 { items.push(i * 10); }
    var ids: Item[>..] = [];
    ids.push(2 as Item);
    ids.push(0 as Item);
    let ib, iok = from_bytes<Item[>..]>(to_bytes(ids));
    print(iok, " ", ib.len, " ", items[ib[0]]);
    var big: u32[>..] = [3];
    let ib2, iok2 = from_bytes<Item[>..]>(to_bytes(big));
    print(iok2, " ", ib2.len);

    var nodes: Node[>..] = [];
    let n0: Node& = nodes.push(Node { v: 1 });
    nodes.push(Node { v: 2, next: n0 });
    var nimg = to_bytes(nodes);
    let nb, nok = from_bytes<Node[>..]>(nimg);
    let l = nb[1].next;
    if l { print(nok, " ", l.v); }
    nimg[5 + 8] = 4;
    let nb2, nok2 = from_bytes<Node[>..]>(nimg);
    print(nok2, " ", nb2.len);

    var small: u8[..32] = [];
    to_bytes(ps, small);
    print(small.len, " ", small[0]);
    let empty: u8[0] = [];
    let e, eok = from_bytes<P[>..]>(empty);
    print(eok, " ", e.len);
}
