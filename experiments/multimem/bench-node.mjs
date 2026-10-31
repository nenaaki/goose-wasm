// Times the workloads of each module on V8: node bench-node.mjs a.wasm b.wasm ...
import { readFileSync } from 'node:fs';
import { basename } from 'node:path';

// Repetitions per workload, and the operations each repetition does.
const WORK = {
    walk: { reps: 100, ops: 1 << 16, unit: 'node' },
    walk1: { reps: 100, ops: 1 << 16, unit: 'node' },
    scan: { reps: 100, ops: 1 << 16, unit: 'load' },
    copy: { reps: 200000, ops: 1, unit: '4KiB copy' },
};

for (const file of process.argv.slice(2)) {
    const { instance } = await WebAssembly.instantiate(readFileSync(file));
    const row = [basename(file, '.wasm').padEnd(14)];
    for (const [name, w] of Object.entries(WORK)) {
        const f = instance.exports[name];
        f(Math.max(1, w.reps / 10));            // warm up (tier-up)
        let best = Infinity, result;
        for (let i = 0; i < 5; i++) {
            const t0 = process.hrtime.bigint();
            result = f(w.reps);
            const dt = Number(process.hrtime.bigint() - t0);
            best = Math.min(best, dt);
        }
        const ns = best / (w.reps * w.ops);
        row.push(`${name} ${ns.toFixed(2).padStart(7)} ns/${w.unit} (${result})`);
    }
    console.log(row.join('  '));
}
