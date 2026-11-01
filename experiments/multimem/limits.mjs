// How many memories can one module declare and instantiate?
//   node limits.mjs gen <M> > m.wat     write a module with M one-page memories
//   node limits.mjs v8 <M>...           try them on V8
// Each memory gets a store and a load through an exported function, so an
// engine cannot skip setting it up.
const [mode, ...args] = process.argv.slice(2);

function wat(m) {
    const s = ['(module'];
    for (let k = 0; k < m; k++) s.push(`  (memory $m${k} 1)`);
    s.push('  (func (export "touch") (result i32) (local $s i32)');
    for (let k = 0; k < m; k++)
        s.push(`    (i32.store $m${k} (i32.const 0) (i32.const ${k}))` +
               ` (local.set $s (i32.add (local.get $s) (i32.load $m${k} (i32.const 0))))`);
    s.push('    (local.get $s))');
    s.push(')');
    return s.join('\n');
}

// Engines reserve several GB of address space per 32-bit memory (V8 and
// wasmtime both reserve the whole 4 GB plus guard pages). Instantiating
// 10,000+ memories reserved tens of TB and was followed by a Windows bugcheck
// 0x1A (MEMORY_MANAGEMENT, 0x3453) on 2026-10-31, so stay at 1000 or below.
const MAX_MEMORIES = 1000;

if (mode === 'gen') {
    const m = Number(args[0]);
    if (!(m <= MAX_MEMORIES)) {
        console.error(`refusing ${m} memories (max ${MAX_MEMORIES}, see the comment above)`);
        process.exit(2);
    }
    console.log(wat(m));
} else if (mode === 'v8') {
    const { readFileSync } = await import('node:fs');
    for (const file of args) {
        try {
            const { instance } = await WebAssembly.instantiate(readFileSync(file));
            console.log(`${file}: ok, touch() = ${instance.exports.touch()}`);
        } catch (e) {
            console.log(`${file}: ${e.constructor.name}: ${e.message}`);
        }
    }
}
