// goose-wasm's slice_pool_internal: served in place of upstream's
// stdlib/slice_pool_internal.goose (lib/README.md). Upstream's three extern
// fns there are implemented by its C runtime; here they are that C ported
// to Goose, statement for statement, so that the Wasm backend compiles them
// as it does the rest of the library.
//
// Derived from upstream Goose at commit
// 2ed1a63dde2ddff777fbb7f4790412b4f00cd47d: stdlib/slice_pool_internal.goose
// (the node and tree types and the functions' signatures) and
// src/runtime_inline.h (gs_slice_pool_place, gs_slice_pool_free,
// gs_slice_pool_regrow and the gs_spc_* helpers and GS_SP* macros they use).
// Original work: Goose, https://github.com/aardappel/goose, by Wouter van
// Oortmerssen, licensed under the Apache License, Version 2.0
// (upstream/goose/LICENSE). Changes: the extern declarations replaced by
// Goose functions of the same signatures; the C translated to Goose, node
// fields reached through `data` at the offsets the C structs give them,
// pointers to nodes replaced by node numbers, the path through the tree
// kept in a struct passed by reference, unsigned comparisons and wrapping
// sums written with `as!` and u64; a corrupt tree aborts with the runtime's
// message (GS_E_SPANTREE) at the library call, which these functions pass
// on as the rest of the library does.

namespace slice_pool_detail;

// A 512-byte B+ node: leaves use 31 keys and 31 counts; inner nodes use
// 20 keys, 20 subtree maxima and 20 child indices in the same typed storage.
// Node 0 stays zero and terminates the chain of emptied nodes.
struct node { n: i64, link: i64, data: i64[62] }
struct tree { root: i64, height: i64, chain: i64 }

// Upstream's declarations, kept as they were (the C implemented them):
//   extern fn place(nodes: node[>..]&, tree: tree&, length: i64, cnt: i64) -> i64 = "gs_slice_pool_place";
//   extern fn release_run(nodes: node[>..]&, tree: tree&, idx: i64, cnt: i64) = "gs_slice_pool_free";
//   extern fn regrow(nodes: node[>..]&, tree: tree&, length: i64, idx: i64, old: i64,
//                    cnt: i64) -> i64 = "gs_slice_pool_regrow";

// GS_SPAN_LEAF, GS_SPAN_FAN, GS_SPAN_DEPTH. A leaf's idx[k] is data[k] and
// its cnt[k] data[31 + k]; an inner node's key[k] is data[k], its max[k]
// data[20 + k] and its child[k] data[40 + k].
// The path from the root to a leaf (gs_spcpath): the node and the entry
// taken at each level.
struct spc_path { h: i64, nd: i64[24], ps: i64[24] }
// gs_spcnear: the leaf entry at or before an index, and the one after it,
// in the same leaf or, through the second path, the next one.
struct spc_near { lf: i64, nl: i64, k: i64, nk: i64, useq: bool }

// GS_SPSUM: sums of stored indices and counts wrap.
fn spc_sum(a: i64, b: i64) -> i64 { return ((a as! u64) + (b as! u64)) as! i64; }

fn spc_corrupt() { abort("slice pool span tree is corrupt"); }

// GS_SPBADNUM, GS_SPBADIN, GS_SPBADLEAF, GS_SPBADH.
fn spc_badnum(nodes: node[>..]&, i: i64) -> bool {
    return (i as! u64) - 1 >= (nodes.len as! u64) - 1;
}
fn spc_badin(nodes: node[>..]&, i: i64) -> bool {
    return spc_badnum(nodes, i) || (nodes[i].n as! u64) - 1 >= 20;
}
fn spc_badleaf(nodes: node[>..]&, i: i64) -> bool {
    return spc_badnum(nodes, i) || (nodes[i].n as! u64) > 31;
}
fn spc_badh(h: i64) -> bool { return (h as! u64) >= 24; }

fn spc_newnode(nodes: node[>..]&, t: tree&) -> i64 {
    let i = t.chain;
    if i != 0 {
        if spc_badnum(nodes, i) { spc_corrupt(); }
        t.chain = nodes[i].link;
        return i;
    }
    let j = nodes.len;
    nodes.push(node { .. });
    return j;
}

fn spc_dropnode(nodes: node[>..]&, t: tree&, i: i64) {
    nodes[i].link = t.chain;
    t.chain = i;
}

fn spc_open(nodes: node[>..]&, lf: i64, s: i64) {
    let n = nodes[lf].n;
    if (n as! u64) >= 31 || (s as! u64) > (n as! u64) { spc_corrupt(); }
    var j = n;
    while j > s {
        nodes[lf].data[j] = nodes[lf].data[j - 1];
        nodes[lf].data[31 + j] = nodes[lf].data[31 + j - 1];
        j -= 1;
    }
    nodes[lf].n = n + 1;
}

fn spc_close(nodes: node[>..]&, lf: i64, s: i64) {
    if (s as! u64) >= (nodes[lf].n as! u64) { spc_corrupt(); }
    nodes[lf].n -= 1;
    var j = s;
    while j < nodes[lf].n {
        nodes[lf].data[j] = nodes[lf].data[j + 1];
        nodes[lf].data[31 + j] = nodes[lf].data[31 + j + 1];
        j += 1;
    }
}

fn spc_openin(nodes: node[>..]&, ni: i64, s: i64) {
    let n = nodes[ni].n;
    if (n as! u64) >= 20 || (s as! u64) > (n as! u64) { spc_corrupt(); }
    var j = n;
    while j > s {
        nodes[ni].data[j] = nodes[ni].data[j - 1];
        nodes[ni].data[20 + j] = nodes[ni].data[20 + j - 1];
        nodes[ni].data[40 + j] = nodes[ni].data[40 + j - 1];
        j -= 1;
    }
    nodes[ni].n = n + 1;
}

fn spc_closein(nodes: node[>..]&, ni: i64, s: i64) {
    if nodes[ni].n > 20 || (s as! u64) >= (nodes[ni].n as! u64) { spc_corrupt(); }
    nodes[ni].n -= 1;
    var j = s;
    while j < nodes[ni].n {
        nodes[ni].data[j] = nodes[ni].data[j + 1];
        nodes[ni].data[20 + j] = nodes[ni].data[20 + j + 1];
        nodes[ni].data[40 + j] = nodes[ni].data[40 + j + 1];
        j += 1;
    }
}

// gs_sp_most over a leaf's counts or an inner node's maxima: the largest,
// or `stop` as soon as one reaches it.
fn spc_max(nodes: node[>..]&, i: i64, leaf: bool, stop: i64) -> i64 {
    let off = if leaf { 31 } else { 20 };
    let w = nodes[i].n;
    var m = 0;
    var k = 0;
    while k < w && m < stop {
        let v = nodes[i].data[off + k];
        if v > m { m = v; }
        k += 1;
    }
    return m;
}

// A leaf's first index, an inner node's first key: both data[0].
fn spc_key(nodes: node[>..]&, i: i64) -> i64 { return nodes[i].data[0]; }

fn spc_fix(nodes: node[>..]&, p: spc_path&, from: i64, was0: i64, now0: i64) {
    var l = from;
    var was = was0;
    var now = now0;
    while l > 0 {
        let ni = p.nd[l - 1];
        let k = p.ps[l - 1];
        let c = p.nd[l];
        let m = nodes[ni].data[20 + k];
        let leaf = l == p.h;
        let key = spc_key(nodes, c);
        let nm = if now >= m { now } else if was < m { m } else { spc_max(nodes, c, leaf, was) };
        if nodes[ni].data[k] == key && nm == m { return; }
        nodes[ni].data[k] = key;
        nodes[ni].data[20 + k] = nm;
        was = m;
        now = nm;
        l -= 1;
    }
}

fn spc_find(nodes: node[>..]&, t: tree&, p: spc_path&, idx: i64) -> i64 {
    var i = t.root;
    p.h = t.height;
    if spc_badh(p.h) { spc_corrupt(); }
    var l = 0;
    while l < p.h {
        if spc_badin(nodes, i) { spc_corrupt(); }
        var k = 1;
        while k < nodes[i].n && nodes[i].data[k] <= idx { k += 1; }
        p.nd[l] = i;
        p.ps[l] = k - 1;
        i = nodes[i].data[40 + k - 1];
        l += 1;
    }
    p.nd[p.h] = i;
    if spc_badleaf(nodes, i) { spc_corrupt(); }
    var k = 0;
    while k < nodes[i].n && nodes[i].data[k] <= idx { k += 1; }
    p.ps[p.h] = k - 1;
    return k - 1;
}

fn spc_newroot(nodes: node[>..]&, t: tree&, a: i64, b: i64, leaf: bool) {
    let r = spc_newnode(nodes, t);
    nodes[r].n = 2;
    nodes[r].data[0] = spc_key(nodes, a);
    nodes[r].data[20] = spc_max(nodes, a, leaf, 9223372036854775807);
    nodes[r].data[40] = a;
    nodes[r].data[1] = spc_key(nodes, b);
    nodes[r].data[21] = spc_max(nodes, b, leaf, 9223372036854775807);
    nodes[r].data[41] = b;
    t.root = r;
    t.height += 1;
}

fn spc_addchild(nodes: node[>..]&, t: tree&, p: spc_path&, l0: i64, k0: i64, child0: i64, leaf0: bool) {
    var l = l0;
    var k = k0;
    var child = child0;
    var leaf = leaf0;
    loop {
        let ni = p.nd[l];
        var o = ni;
        let key = spc_key(nodes, child);
        let max = spc_max(nodes, child, leaf, 9223372036854775807);
        var s = k;
        var nn = 0;
        if nodes[ni].n == 20 {
            let half = 10;
            nn = spc_newnode(nodes, t);
            if nn == p.nd[l] { spc_corrupt(); }
            let rn = nodes[ni].n - half;
            nodes[nn].n = rn;
            for j in rn {
                nodes[nn].data[j] = nodes[ni].data[half + j];
                nodes[nn].data[20 + j] = nodes[ni].data[20 + half + j];
                nodes[nn].data[40 + j] = nodes[ni].data[40 + half + j];
            }
            nodes[ni].n = half;
            if k > half {
                o = nn;
                s = k - half;
            }
        }
        spc_openin(nodes, o, s);
        nodes[o].data[s] = key;
        nodes[o].data[20 + s] = max;
        nodes[o].data[40 + s] = child;
        if nn == 0 {
            spc_fix(nodes, p, l, 0, max);
            return;
        }
        if l == 0 {
            spc_newroot(nodes, t, p.nd[0], nn, false);
            return;
        }
        spc_fix(nodes, p, l, 9223372036854775807, 0);
        child = nn;
        leaf = false;
        k = p.ps[l - 1] + 1;
        l -= 1;
    }
}

fn spc_insert(nodes: node[>..]&, t: tree&, p: spc_path&, k: i64, idx: i64, cnt: i64) {
    let lf = p.nd[p.h];
    var o = lf;
    var s = k;
    var nn = 0;
    if nodes[lf].n == 31 {
        let half = 15;
        nn = spc_newnode(nodes, t);
        if nn == p.nd[p.h] { spc_corrupt(); }
        let rn = nodes[lf].n - half;
        nodes[nn].n = rn;
        for j in rn {
            nodes[nn].data[j] = nodes[lf].data[half + j];
            nodes[nn].data[31 + j] = nodes[lf].data[31 + half + j];
        }
        nodes[lf].n = half;
        if k > half {
            o = nn;
            s = k - half;
        }
    }
    spc_open(nodes, o, s);
    nodes[o].data[s] = idx;
    nodes[o].data[31 + s] = cnt;
    if nn == 0 {
        spc_fix(nodes, p, p.h, 0, cnt);
    } else if p.h == 0 {
        spc_newroot(nodes, t, p.nd[0], nn, true);
    } else {
        spc_fix(nodes, p, p.h, 9223372036854775807, 0);
        spc_addchild(nodes, t, p, p.h - 1, p.ps[p.h - 1] + 1, nn, true);
    }
}

fn spc_delete(nodes: node[>..]&, t: tree&, p: spc_path&, k: i64) {
    let lf = p.nd[p.h];
    let c = nodes[lf].data[31 + k];
    spc_close(nodes, lf, k);
    if nodes[lf].n > 0 || p.h == 0 {
        spc_fix(nodes, p, p.h, c, 0);
        return;
    }
    spc_dropnode(nodes, t, p.nd[p.h]);
    var l = p.h - 1;
    loop {
        let ni = p.nd[l];
        let e = p.ps[l];
        let m = nodes[ni].data[20 + e];
        spc_closein(nodes, ni, e);
        if nodes[ni].n > 0 {
            spc_fix(nodes, p, l, m, 0);
            break;
        }
        // A root of one child, which the functions never leave.
        if l == 0 { spc_corrupt(); }
        spc_dropnode(nodes, t, p.nd[l]);
        l -= 1;
    }
    while t.height > 0 {
        let r = t.root;
        if spc_badnum(nodes, r) { spc_corrupt(); }
        if nodes[r].n != 1 { break; }
        t.root = nodes[r].data[40];
        t.height -= 1;
        spc_dropnode(nodes, t, r);
    }
}

fn spc_take(nodes: node[>..]&, t: tree&, p: spc_path&, k: i64, cnt: i64) {
    let lf = p.nd[p.h];
    let c = nodes[lf].data[31 + k];
    if c <= cnt {
        spc_delete(nodes, t, p, k);
        return;
    }
    nodes[lf].data[k] = spc_sum(nodes[lf].data[k], cnt);
    nodes[lf].data[31 + k] = c - cnt;
    spc_fix(nodes, p, p.h, c, c - cnt);
}

// gs_slice_pool_place: the start of a free run of cnt elements, first fit,
// taken from the tree; the array's length where none fits but the last run
// ending at it, which grows.
fn place(nodes: node[>..]&, tree: tree&, length: i64, cnt: i64) -> i64 {
    if cnt == 0 || nodes.len == 0 { return length; }
    var p = spc_path { .. };
    var before: i64[24] = [0; 24];
    p.h = tree.height;
    if spc_badh(p.h) { spc_corrupt(); }
    var i = tree.root;
    var l = 0;
    var k = 0;
    var m = 0;
    var lf = -1;
    while l < p.h {
        if spc_badin(nodes, i) { spc_corrupt(); }
        k = 0;
        m = 0;
        while k < nodes[i].n && nodes[i].data[20 + k] < cnt {
            if nodes[i].data[20 + k] > m { m = nodes[i].data[20 + k]; }
            k += 1;
        }
        if k == nodes[i].n { break; }
        p.nd[l] = i;
        p.ps[l] = k;
        before[l] = m;
        i = nodes[i].data[40 + k];
        l += 1;
    }
    if l == p.h {
        if spc_badleaf(nodes, i) { spc_corrupt(); }
        lf = i;
        k = 0;
        m = 0;
        while k < nodes[lf].n && nodes[lf].data[31 + k] < cnt {
            if nodes[lf].data[31 + k] > m { m = nodes[lf].data[31 + k]; }
            k += 1;
        }
        before[l] = m;
    }
    if l < p.h || k == nodes[lf].n {
        l = 0;
        i = tree.root;
        while l < p.h {
            if spc_badin(nodes, i) { spc_corrupt(); }
            p.nd[l] = i;
            p.ps[l] = nodes[i].n - 1;
            i = nodes[i].data[40 + nodes[i].n - 1];
            l += 1;
        }
        p.nd[p.h] = i;
        if spc_badleaf(nodes, i) { spc_corrupt(); }
        lf = i;
        k = nodes[lf].n - 1;
        if k < 0 || spc_sum(nodes[lf].data[k], nodes[lf].data[31 + k]) != length { return length; }
        let at = nodes[lf].data[k];
        spc_take(nodes, tree, p, k, cnt);
        return at;
    }
    p.nd[p.h] = i;
    p.ps[p.h] = k;
    let idx = nodes[lf].data[k];
    var c = nodes[lf].data[31 + k];
    var now = 0;
    var j = 0;
    if c > cnt {
        nodes[lf].data[k] = spc_sum(idx, cnt);
        now = c - cnt;
        nodes[lf].data[31 + k] = now;
        j = k + 1;
    } else if nodes[lf].n > 1 || p.h == 0 {
        spc_close(nodes, lf, k);
        now = 0;
        j = k;
    } else {
        spc_delete(nodes, tree, p, k);
        return idx;
    }
    l = p.h;
    while l > 0 {
        let ni = p.nd[l - 1];
        let e = p.ps[l - 1];
        let mx = nodes[ni].data[20 + e];
        var nm = mx;
        let key = spc_key(nodes, p.nd[l]);
        if c == mx {
            let vn = p.nd[l];
            let off = if l == p.h { 31 } else { 20 };
            let w = nodes[vn].n;
            nm = if before[l] > now { before[l] } else { now };
            while j < w && nm < c {
                if nodes[vn].data[off + j] > nm { nm = nodes[vn].data[off + j]; }
                j += 1;
            }
        }
        if nodes[ni].data[e] == key && nm == mx { break; }
        nodes[ni].data[e] = key;
        nodes[ni].data[20 + e] = nm;
        c = mx;
        now = nm;
        j = e + 1;
        l -= 1;
    }
    return idx;
}

fn spc_near(nodes: node[>..]&, t: tree&, p: spc_path&, q: spc_path&, idx: i64, end: i64) -> spc_near {
    var s = spc_near { lf: 0, nl: -1, k: 0, nk: -1, useq: false };
    s.k = spc_find(nodes, t, p, idx);
    s.lf = p.nd[p.h];
    if s.k + 1 < nodes[s.lf].n {
        s.nl = s.lf;
        s.nk = s.k + 1;
        return s;
    }
    var l = p.h - 1;
    while l >= 0 && p.ps[l] + 1 >= nodes[p.nd[l]].n { l -= 1; }
    if l < 0 || nodes[p.nd[l]].data[p.ps[l] + 1] != end { return s; }
    q.h = p.h;
    for x in l + 1 {
        q.nd[x] = p.nd[x];
        q.ps[x] = p.ps[x];
    }
    q.ps[l] += 1;
    while l < q.h {
        let c = nodes[q.nd[l]].data[40 + q.ps[l]];
        let bad = if l + 1 < q.h { spc_badin(nodes, c) } else { spc_badleaf(nodes, c) };
        if bad { spc_corrupt(); }
        q.nd[l + 1] = c;
        q.ps[l + 1] = 0;
        l += 1;
    }
    s.useq = true;
    s.nl = q.nd[q.h];
    s.nk = 0;
    return s;
}

fn spc_release(nodes: node[>..]&, t: tree&, p: spc_path&, q: spc_path&, s: spc_near, idx: i64, cnt: i64) {
    let lf = s.lf;
    let nl = s.nl;
    let k = s.k;
    let nk = s.nk;
    let prev = k >= 0 && spc_sum(nodes[lf].data[k], nodes[lf].data[31 + k]) == idx;
    let next = nl >= 0 && spc_sum(idx, cnt) == nodes[nl].data[nk];
    if prev {
        let c = nodes[lf].data[31 + k];
        let m = spc_sum(spc_sum(c, cnt), if next { nodes[nl].data[31 + nk] } else { 0 });
        nodes[lf].data[31 + k] = m;
        if next && nl == lf { spc_close(nodes, lf, nk); }
        spc_fix(nodes, p, p.h, c, m);
        if next && nl != lf {
            if s.useq { spc_delete(nodes, t, q, nk); } else { spc_delete(nodes, t, p, nk); }
        }
    } else if next {
        let c = nodes[nl].data[31 + nk];
        nodes[nl].data[nk] = idx;
        nodes[nl].data[31 + nk] = spc_sum(c, cnt);
        if s.useq { spc_fix(nodes, q, q.h, c, nodes[nl].data[31 + nk]); }
        else { spc_fix(nodes, p, p.h, c, nodes[nl].data[31 + nk]); }
    } else {
        spc_insert(nodes, t, p, k + 1, idx, cnt);
    }
}

// gs_slice_pool_free: the run back into the tree, merged with its
// neighbours.
fn release_run(nodes: node[>..]&, tree: tree&, idx: i64, cnt: i64) {
    if cnt <= 0 { return; }
    if nodes.len == 0 {
        // Node 0 stays zero, the chain's end.
        nodes.push(node { .. });
        tree.chain = 0;
        tree.height = 0;
        tree.root = spc_newnode(nodes, tree);
    }
    var p = spc_path { .. };
    var q = spc_path { .. };
    let s = spc_near(nodes, tree, p, q, idx, spc_sum(idx, cnt));
    spc_release(nodes, tree, p, q, s, idx, cnt);
}

// gs_slice_pool_regrow: a run grown in place where the free run after it
// is long enough (or ends the array), else freed and placed afresh.
fn regrow(nodes: node[>..]&, tree: tree&, length: i64, idx: i64, old: i64, cnt: i64) -> i64 {
    let end = spc_sum(idx, old);
    if nodes.len == 0 {
        release_run(nodes, tree, idx, old);
        return place(nodes, tree, length, cnt);
    }
    var p = spc_path { .. };
    var q = spc_path { .. };
    let s = spc_near(nodes, tree, p, q, idx, end);
    if s.nl >= 0 && nodes[s.nl].data[s.nk] == end &&
       (nodes[s.nl].data[31 + s.nk] >= cnt - old || spc_sum(end, nodes[s.nl].data[31 + s.nk]) == length) {
        if s.useq { spc_take(nodes, tree, q, s.nk, cnt - old); } else { spc_take(nodes, tree, p, s.nk, cnt - old); }
        return idx;
    }
    spc_release(nodes, tree, p, q, s, idx, old);
    return place(nodes, tree, length, cnt);
}
