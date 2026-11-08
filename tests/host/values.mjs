import { load, g } from './host.mjs';

const h = await load();
const x = h.x;
const v = () => h.view();
x.goose_init();

// Upstream's packed layouts: i32[4] is 16 bytes, V3 three f32s, Color its
// tag byte, Shape its tag byte then the variant's fields, Item a u16, a
// Color and a V3 (15 bytes).
const a = h.alloc(16), r = h.alloc(16);
[1, 2, 3, 4].forEach((n, i) => v().setInt32(a + 4 * i, n, true));
x.rev(r, a);
console.log([0, 1, 2, 3].map(i => v().getInt32(r + 4 * i, true)).join(' '));
const p = h.alloc(12), q = h.alloc(12);
[1.5, -2.0, 0.25].forEach((n, i) => v().setFloat32(p + 4 * i, n, true));
x.scale(q, p, 2.0);
console.log([0, 1, 2].map(i => g(v().getFloat32(q + 4 * i, true))).join(' '));
const c = h.alloc(1);
v().setUint8(c, 2);
x.next(c, c);
console.log(v().getUint8(c));
x.next(c, c);
x.next(c, c);
console.log(v().getUint8(c));
const s = h.alloc(9);
const shape = () => {
    const t = v().getUint8(s);
    if (t === 1) console.log(`circle ${g(v().getFloat64(s + 1, true))}`);
    else if (t === 2) console.log(`rect ${v().getInt32(s + 1, true)} ${v().getInt32(s + 5, true)}`);
    else console.log('dot');
};
console.log(g(x.area(s)));
x.grow(s, s);
shape();
console.log(g(x.area(s)));
v().setFloat64(s + 1, 2.5, true);
x.grow(s, s);
shape();
console.log(g(x.area(s)));
x.grow(s, s);
shape();
const items = h.alloc(45);
[[7, 0, 0], [8, 1, 1], [9, 2, 2]].forEach(([id, color, y], i) => {
    v().setUint16(items + 15 * i, id, true);
    v().setUint8(items + 15 * i + 2, color);
    v().setFloat32(items + 15 * i + 7, y, true);
});
v().setUint8(c, 2);
x.paint(items + 15, c);
console.log(`${v().getUint8(items + 17)} ${g(v().getFloat32(items + 22, true))}`);
console.log(x.ids(items, 3n).toString());
const xs = h.alloc(20);
x.fill(xs + 4, 3n, 10);
console.log([0, 1, 2, 3, 4].map(i => v().getInt32(xs + 4 * i, true)).join(' '));
