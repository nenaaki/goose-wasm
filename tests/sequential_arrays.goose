// Resizable arrays of variable-size elements are sequential (§3.3): pushed
// and appended images, walked when iterated, printed or compared.
struct Item { name: u8[], qty: varint }

fn names(xs: u8[][>..]&) -> i64 { var t = 0; for x in xs { t += x.len; } t }

fn main() {
    var words: u8[][>..] = ["alpha", "be"];
    words.push("gamma");
    let w: u8[] = "delta";
    words.push(copy(w));
    print(words, " ", words.len, " ", names(words));
    for x, i in words { print(i, " ", x, " ", x.len); }
    var items: Item[>..] = [];
    items.push(Item { name: "pen", qty: 300 });
    items.push(Item { name: "ink", qty: -2 });
    var more: Item[>..] = [Item { name: "pad", qty: 1 }];
    more.append(items);
    print(items, " ", more);
    print(items == more, " ", more.len);
    var copy2: u8[][>..] = [];
    copy2.append(words);
    print(copy2 == words);
    copy2.clear();
    print(copy2.len);
}
