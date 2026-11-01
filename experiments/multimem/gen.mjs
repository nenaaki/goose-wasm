// Generates a benchmark module (WAT) comparing memory access strategies for
// references that carry their memory index: upper 32 bits = memory index,
// lower 32 bits = address within that memory.
//
//   node gen.mjs single          one linear memory, plain loads/stores
//   node gen.mjs brtable  <N>    N memories, dispatch with br_table
//   node gen.mjs indirect <N>    N memories, dispatch with call_indirect
//   node gen.mjs basetable <N>   one linear memory holding N stack regions; a
//                                reference decodes as bases[index] + address
//                                (upstream docs/design/wasm.md §4)
//
// Workloads (exported, each takes a repetition count):
//   walk  - follow a linked list whose nodes are spread round-robin over the
//           memories, in a scattered order (pointer chasing; the memory
//           index changes at every node, the worst case for dispatch)
//   walk1 - the same list shape with every node in one memory, as a pool's
//           nodes are: the index is only known at run time, but repeats
//   scan  - sum an i64 array reached through a reference (sequential reads)
//   copy  - copy 4 KiB blocks between memories (memory.copy)

const [variant, nArg] = process.argv.slice(2);
if (!['single', 'brtable', 'indirect', 'basetable'].includes(variant)) {
    console.error('usage: node gen.mjs single | brtable <N> | indirect <N> | basetable <N>');
    process.exit(2);
}
const N = variant === 'single' ? 1 : Number(nArg);
const L = 1 << 16;          // list nodes (16 bytes each: value, next)
const A = 1 << 16;          // array elements (i64)
const BLK = 4096;           // copy block size
const STEP = 40503;         // odd, so t*STEP mod L visits every node once
const NODE0 = 64;           // address 0 stays unused (null)
const ARR0 = 0x200000;
const COPY0 = 0x300000;
const LIST1 = 0x400000;     // walk1's list, all in the last memory
const PAGES = 96;           // 6 MiB per memory

// basetable: the table of region bases sits at BT, and region k starts at
// REGION0 + k * PAGES pages.
const BT = 1024;
const REGION0 = 0x10000;

const out = [];
const emit = s => out.push(s);
const range = n => [...Array(n).keys()];

emit('(module');
if (variant === 'basetable') {
    emit(`  (memory $m0 ${1 + N * PAGES})`);
} else {
    for (const k of range(N)) emit(`  (memory $m${k} ${PAGES})`);
}
emit('  (export "memory" (memory $m0))');

// The memory index and address of a reference.
const idx = '(i32.wrap_i64 (i64.shr_u (local.get $r) (i64.const 32)))';
const addr = '(i32.wrap_i64 (local.get $r))';

// A br_table over `n` cases; case k runs body(k), which returns.
function dispatch(sel, n, body) {
    const s = [];
    s.push('    block $bad');
    for (const k of range(n).reverse()) s.push(`    block $c${k}`);
    s.push(`      ${sel}`);
    s.push(`      br_table ${range(n).map(k => '$c' + k).join(' ')} $bad`);
    for (const k of range(n)) {
        s.push(`    end`);
        s.push(`      ${body(k)}`);
        s.push(`      return`);
    }
    s.push('    end');
    s.push('    unreachable');
    return s.join('\n');
}

if (variant === 'single') {
    emit(`  (func $ld (param $r i64) (result i64) (i64.load ${addr}))`);
    emit(`  (func $st (param $r i64) (param $v i64) (i64.store ${addr} (local.get $v)))`);
    emit(`  (func $cp (param $d i64) (param $s i64) (param $n i32)
    (memory.copy (i32.wrap_i64 (local.get $d)) (i32.wrap_i64 (local.get $s)) (local.get $n)))`);
} else if (variant === 'basetable') {
    // No range check on the index, as upstream's decode has none.
    const decode = v => `(i32.add (i32.load (i32.add (i32.const ${BT}) ` +
        `(i32.shl (i32.wrap_i64 (i64.shr_u (local.get ${v}) (i64.const 32))) (i32.const 2)))) ` +
        `(i32.wrap_i64 (local.get ${v})))`;
    emit(`  (func $ld (param $r i64) (result i64) (i64.load ${decode('$r')}))`);
    emit(`  (func $st (param $r i64) (param $v i64) (i64.store ${decode('$r')} (local.get $v)))`);
    emit(`  (func $cp (param $d i64) (param $s i64) (param $n i32)
    (memory.copy ${decode('$d')} ${decode('$s')} (local.get $n)))`);
} else if (variant === 'brtable') {
    emit(`  (func $ld (param $r i64) (result i64)
${dispatch(idx, N, k => `(i64.load $m${k} ${addr})`)}
  )`);
    emit(`  (func $st (param $r i64) (param $v i64)
${dispatch(idx, N, k => `(i64.store $m${k} ${addr} (local.get $v))`)}
  )`);
    const pair = '(i32.add (i32.mul (i32.wrap_i64 (i64.shr_u (local.get $d) (i64.const 32))) ' +
        `(i32.const ${N})) (i32.wrap_i64 (i64.shr_u (local.get $s) (i64.const 32))))`;
    emit(`  (func $cp (param $d i64) (param $s i64) (param $n i32)
${dispatch(pair, N * N, c => `(memory.copy $m${Math.floor(c / N)} $m${c % N} ` +
        '(i32.wrap_i64 (local.get $d)) (i32.wrap_i64 (local.get $s)) (local.get $n))')}
  )`);
} else {
    emit('  (type $ld_t (func (param i32) (result i64)))');
    emit('  (type $st_t (func (param i32) (param i64)))');
    emit('  (type $cp_t (func (param i32) (param i32) (param i32)))');
    for (const k of range(N)) {
        emit(`  (func $ld${k} (type $ld_t) (i64.load $m${k} (local.get 0)))`);
        emit(`  (func $st${k} (type $st_t) (i64.store $m${k} (local.get 0) (local.get 1)))`);
    }
    for (const c of range(N * N))
        emit(`  (func $cp${c} (type $cp_t) (memory.copy $m${Math.floor(c / N)} $m${c % N} ` +
            '(local.get 0) (local.get 1) (local.get 2)))');
    emit(`  (table $tld funcref (elem ${range(N).map(k => '$ld' + k).join(' ')}))`);
    emit(`  (table $tst funcref (elem ${range(N).map(k => '$st' + k).join(' ')}))`);
    emit(`  (table $tcp funcref (elem ${range(N * N).map(c => '$cp' + c).join(' ')}))`);
    emit(`  (func $ld (param $r i64) (result i64)
    (call_indirect $tld (type $ld_t) ${addr} ${idx}))`);
    emit(`  (func $st (param $r i64) (param $v i64)
    (call_indirect $tst (type $st_t) ${addr} (local.get $v) ${idx}))`);
    emit(`  (func $cp (param $d i64) (param $s i64) (param $n i32)
    (call_indirect $tcp (type $cp_t)
      (i32.wrap_i64 (local.get $d)) (i32.wrap_i64 (local.get $s)) (local.get $n)
      (i32.add (i32.mul (i32.wrap_i64 (i64.shr_u (local.get $d) (i64.const 32))) (i32.const ${N}))
               (i32.wrap_i64 (i64.shr_u (local.get $s) (i64.const 32))))))`);
}

emit(`
  (func $ref (param $m i32) (param $a i32) (result i64)
    (i64.or (i64.shl (i64.extend_i32_u (local.get $m)) (i64.const 32))
            (i64.extend_i32_u (local.get $a))))
  ;; Node j lives in memory j % N.
  (func $node (param $j i32) (result i64)
    (call $ref (i32.rem_u (local.get $j) (i32.const ${N}))
               (i32.add (i32.const ${NODE0})
                        (i32.mul (i32.div_u (local.get $j) (i32.const ${N})) (i32.const 16)))))

  ;; walk1's node j: all in the last memory.
  (func $node1 (param $j i32) (result i64)
    (call $ref (i32.const ${N - 1})
               (i32.add (i32.const ${LIST1}) (i32.mul (local.get $j) (i32.const 16)))))

  (global $head (mut i64) (i64.const 0))
  (global $head1 (mut i64) (i64.const 0))
  (global $arr (mut i64) (i64.const 0))

  (func $build
    (local $t i32) (local $j i32) (local $jn i32) (local $n i64) (local $i i32)
${variant === 'basetable' ? range(N).map(k =>
    `    (i32.store (i32.const ${BT + 4 * k}) (i32.const ${REGION0 + k * PAGES * 65536}))`).join('\n') : ''}
    (loop $l
      (local.set $j (i32.and (i32.mul (local.get $t) (i32.const ${STEP})) (i32.const ${L - 1})))
      (local.set $jn (i32.and (i32.mul (i32.add (local.get $t) (i32.const 1)) (i32.const ${STEP}))
                              (i32.const ${L - 1})))
      (local.set $n (call $node (local.get $j)))
      (call $st (local.get $n) (i64.extend_i32_u (local.get $j)))
      (call $st (i64.add (local.get $n) (i64.const 8)) (call $node (local.get $jn)))
      (local.set $n (call $node1 (local.get $j)))
      (call $st (local.get $n) (i64.extend_i32_u (local.get $j)))
      (call $st (i64.add (local.get $n) (i64.const 8)) (call $node1 (local.get $jn)))
      (local.set $t (i32.add (local.get $t) (i32.const 1)))
      (br_if $l (i32.lt_u (local.get $t) (i32.const ${L}))))
    (global.set $head (call $node (i32.const 0)))
    (global.set $head1 (call $node1 (i32.const 0)))
    (global.set $arr (call $ref (i32.const ${N - 1}) (i32.const ${ARR0})))
    (loop $a
      (call $st (i64.add (global.get $arr) (i64.extend_i32_u (i32.shl (local.get $i) (i32.const 3))))
                (i64.extend_i32_u (local.get $i)))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $a (i32.lt_u (local.get $i) (i32.const ${A})))))
  (start $build)

  (func (export "walk") (param $reps i32) (result i64)
    (call $walk (global.get $head) (local.get $reps)))
  (func (export "walk1") (param $reps i32) (result i64)
    (call $walk (global.get $head1) (local.get $reps)))

  (func $walk (param $r i64) (param $reps i32) (result i64)
    (local $sum i64) (local $c i64)
    (local.set $c (i64.mul (i64.extend_i32_u (local.get $reps)) (i64.const ${L})))
    (block $done
      (br_if $done (i64.eqz (local.get $c)))
      (loop $l
        (local.set $sum (i64.add (local.get $sum) (call $ld (local.get $r))))
        (local.set $r (call $ld (i64.add (local.get $r) (i64.const 8))))
        (local.set $c (i64.sub (local.get $c) (i64.const 1)))
        (br_if $l (i64.ne (local.get $c) (i64.const 0)))))
    (local.get $sum))

  (func (export "scan") (param $reps i32) (result i64)
    (local $sum i64) (local $i i32) (local $k i32)
    (block $done
      (br_if $done (i32.eqz (local.get $reps)))
      (loop $outer
        (local.set $i (i32.const 0))
        (loop $l
          (local.set $sum (i64.add (local.get $sum)
            (call $ld (i64.add (global.get $arr)
                               (i64.extend_i32_u (i32.shl (local.get $i) (i32.const 3)))))))
          (local.set $i (i32.add (local.get $i) (i32.const 1)))
          (br_if $l (i32.lt_u (local.get $i) (i32.const ${A}))))
        (local.set $k (i32.add (local.get $k) (i32.const 1)))
        (br_if $outer (i32.lt_u (local.get $k) (local.get $reps)))))
    (local.get $sum))

  (func (export "copy") (param $reps i32) (result i64)
    (local $k i32)
    (block $done
      (br_if $done (i32.eqz (local.get $reps)))
      (loop $l
        (call $cp
          (call $ref (i32.rem_u (i32.add (local.get $k) (i32.const 1)) (i32.const ${N}))
                     (i32.const ${COPY0 + BLK}))
          (call $ref (i32.rem_u (local.get $k) (i32.const ${N})) (i32.const ${COPY0}))
          (i32.const ${BLK}))
        (local.set $k (i32.add (local.get $k) (i32.const 1)))
        (br_if $l (i32.lt_u (local.get $k) (local.get $reps)))))
    (i64.extend_i32_u (local.get $k)))
)`);

console.log(out.join('\n'));
