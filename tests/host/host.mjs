// What the JS hosts share: the module named on the command line,
// instantiated as a WASI reactor by scripts/wasihost.mjs (with its threads,
// if it has workers or queues), and views of the memory its addresses are
// in (gs_memory).
import { argv } from 'node:process';
import { loadReactor } from '../../scripts/wasihost.mjs';

export async function load() {
    const { exports: x, memory } = loadReactor(argv[2]);
    const view = () => new DataView(memory.buffer);
    return {
        x,
        view,
        // A block of n bytes, zeroed.
        alloc(n) {
            const p = x.gs_alloc(n);
            new Uint8Array(memory.buffer, p, n).fill(0);
            return p;
        },
        // n bytes at p as text.
        text(p, n) { return Buffer.from(new Uint8Array(memory.buffer, p, Number(n))).toString('latin1'); },
        // A copy of a string's bytes, and its length.
        put(s) {
            const b = Buffer.from(s, 'latin1');
            const p = x.gs_alloc(b.length || 1);
            new Uint8Array(memory.buffer, p, b.length).set(b);
            return [p, BigInt(b.length)];
        },
        // A returned slice: the bytes at the address it returned, as many
        // as *lenp says.
        slice(p, lenp) { return this.text(p, view().getBigInt64(lenp, true)); },
    };
}

// What printf("%g") would print for the f64s and f32s the hosts show.
export function g(v) {
    if (Number.isInteger(v) && Math.abs(v) < 1e6) return String(v);
    let s = v.toPrecision(6);
    if (s.includes('e')) {
        let [m, e] = s.split('e');
        if (m.includes('.')) m = m.replace(/0+$/, '').replace(/\.$/, '');
        const n = parseInt(e, 10);
        return `${m}e${n < 0 ? '-' : '+'}${String(Math.abs(n)).padStart(2, '0')}`;
    }
    if (s.includes('.')) s = s.replace(/0+$/, '').replace(/\.$/, '');
    return s;
}
