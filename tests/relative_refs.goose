// Relative references (§3.9): self-relative links measured from their own
// slot, built in place; `in pool` links measured from a global pool's
// base; optional ones with null; `self`; links read as plain references.
struct Node { id: i64, next: Node&<u32>? }
struct Cell { v: i64, neighbors: (Cell&<u16>)[..4] }
struct Loop { id: i64, me: Loop&<u32> }
struct PNode { id: i32, next: PNode&<u16 in pool>? }

var pool: PNode[>..] = [];

fn chain(ns: Node[>..]&) -> i64 {
    var t = 0;
    var cur: Node? = ns[0];
    while cur {
        t += cur.id;
        cur .= cur.next;
    }
    t
}

fn main() {
    var ns: Node[>..] = [];
    ns.push(Node { id: 1, next: null });
    ns.push(Node { id: 2, next: ns[0] });
    ns.push(Node { id: 3, next: ns[1] });
    ns[0].next .= ns[2];
    ns[2].next .= null;
    print(chain(ns), " ", ns[1].next, " ", ns[0].next);
    var cells: Cell[>..] = [];
    for i in 4 { cells.push(Cell { v: i * 10, neighbors: [] }); }
    for i in 4 { cells[i].neighbors.push(cells[(i + 1) % 4]); cells[i].neighbors.push(cells[(i + 3) % 4]); }
    var s = 0;
    for &c in cells { for n in c.neighbors { s += n.v; } }
    print(s, " ", cells[2].neighbors[0].v, " ", cells[0].neighbors[1].v);
    var loops: Loop[>..] = [];
    loops.push(Loop { id: 7, me: self });
    print(loops[0].me.id);
    pool.push(PNode { id: 1, next: null });
    pool.push(PNode { id: 2, next: pool[0] });
    pool.push(PNode { id: 3, next: pool[1] });
    var t = 0;
    var p: PNode? = pool[2];
    while p { t += p.id; p .= p.next; }
    print(t, " ", pool[1].next);
    let copied = pool[2];
    let cn: PNode? = copied.next;
    if cn { print(cn.id); }
}
