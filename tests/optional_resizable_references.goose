// An optional reference to a resizable value (§3.8, C.2): null or the
// array (or frame object) it names, tested, narrowed, rebound with `.=`
// to another one and back to null.
struct Bag { tag: i64, items: i64[>..] }

fn main() {
    var a: i64[>..] = [1, 2];
    var b: i64[>..] = [];
    var o: i64[>..]? = null;
    print(o == null, " ", null != o);
    o .= a;
    if o { o.push(3); }
    print(o != null, " ", a);
    o .= b;
    if o != null { o.push(9); print(o.len); }
    o .= null;
    if o { print("unreached"); } else { print("null again"); }
    print(a, " ", b);

    var bag = Bag { tag: 5, items: [] };
    var ob: Bag? = null;
    if !ob { print("no bag"); }
    ob .= bag;
    if ob { ob.items.push(ob.tag); }
    print(bag);
}
