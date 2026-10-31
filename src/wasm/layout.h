// Byte sizes and field offsets of fixed-size values: upstream's packed
// layout (spec Appendix C), which this backend keeps byte for byte
// (docs/design/value-representation.md). The rules follow upstream's
// CodeGen::FixedSize, PadAlign and LayoutFields (codegen_types.h); upstream's
// own functions are members of its C code generator, which cannot run
// without generating the whole program, so they are restated here.
#pragma once

#include <map>

namespace goosewasm {

namespace g = goose;

struct Layouts {
    struct Layout {
        std::vector<int64_t> offs;  // aligned with the fields, pads included
        int64_t size = 0;
    };

    std::map<g::StructInst *, Layout> structs;

    // Bytes of an integer type (upstream CodeGen::IntSize).
    static int64_t IntSize(g::IntStorage s) { return g::IntBits(s) / 8; }

    // Whether a and b are the same type, as far as picking an
    // instantiation needs: upstream compares C representations (CodeGen::
    // TEq); this compares what determines one.
    static bool Same(g::TypeExpr *a, g::TypeExpr *b) {
        if (a == b) return true;
        if (!a || !b || a->kind != b->kind) return false;
        switch (a->kind) {
            case g::TY_INT: return a->intstorage == b->intstorage;
            case g::TY_FLT: return a->fltstorage == b->fltstorage;
            case g::TY_BOOL: return true;
            case g::TY_STRUCT: {
                if (a->struc->st != b->struc->st || a->struc->args.size() != b->struc->args.size())
                    return false;
                for (size_t i = 0; i < a->struc->args.size(); i++)
                    if (!Same(a->struc->args[i], b->struc->args[i])) return false;
                return true;
            }
            default: return false;
        }
    }

    // The instantiation of a struct type (upstream CodeGen::SI).
    static g::StructInst *SI(g::TypeExpr *t) {
        if (t->struc->inst) return t->struc->inst;
        for (auto inst : t->struc->st->insts) {
            if (inst->args.size() != t->struc->args.size()) continue;
            bool ok = true;
            for (size_t i = 0; i < inst->args.size(); i++) ok &= Same(inst->args[i], t->struc->args[i]);
            if (ok) return t->struc->inst = inst;
        }
        return nullptr;
    }

    // A resizable-class type (spec §1.1): a resizable array, or a struct
    // whose tail is one (upstream CodeGen::IsResz).
    static bool IsResizable(g::TypeExpr *t) {
        if (t->kind == g::TY_ARRAY) return t->arr->akind == g::A_GROW || t->arr->akind == g::A_GROWSHRINK;
        if (t->kind == g::TY_STRUCT) {
            auto si = SI(t);
            return si && si->sclass == g::SC_RESIZABLE;
        }
        return false;
    }
    // An address reference: plain (8 bytes) or, to a resizable value, fat
    // (16 bytes, { hdr, stk }); not a relative one.
    static bool IsAddressRef(g::TypeExpr *t) {
        return t && t->kind == g::TY_REF && t->ref->lenstorage < 0 && !t->ref->pool;
    }
    // A plain reference (T& or T?): an 8-byte handle. Relative references
    // and references to resizable values have other layouts.
    static bool IsPlainRef(g::TypeExpr *t) {
        return IsAddressRef(t) && !IsResizable(t->ref->sub);
    }
    // A fat reference gs_rref { hdr, stk } (spec Appendix C.2).
    static bool IsFatRef(g::TypeExpr *t) {
        return IsAddressRef(t) && IsResizable(t->ref->sub);
    }
    // A fixed-size array T[k] whose size the checker settled.
    static bool IsFixedArray(g::TypeExpr *t) {
        return t && t->kind == g::TY_ARRAY && t->arr->akind == g::A_FIXED && t->arr->size >= 0;
    }

    // Whether this file can lay t out yet; FixedSize asks only of these.
    static bool Known(g::TypeExpr *t) {
        switch (t->kind) {
            case g::TY_INT: return t->intstorage != g::IS_VARINT;
            case g::TY_FLT: case g::TY_BOOL: return true;
            case g::TY_REF: return IsAddressRef(t);
            case g::TY_SLICE: return true;
            case g::TY_ARRAY: return IsFixedArray(t) && Known(t->arr->sub);
            case g::TY_STRUCT: {
                auto si = SI(t);
                if (!si || si->sclass != g::SC_FIXED) return false;
                for (auto ft : si->ftypes)
                    if (ft && !Known(ft)) return false;
                return true;
            }
            default: return false;
        }
    }

    // The alignment a bare `pad` gives the field after it (PadAlign): its
    // scalar width, 8 for a plain reference or a slice, 1 for anything
    // composite.
    static int64_t PadAlign(g::TypeExpr *t) {
        switch (t->kind) {
            case g::TY_INT: return t->intstorage == g::IS_VARINT ? 1 : IntSize(t->intstorage);
            case g::TY_FLT: return t->fltstorage == g::FS_F32 ? 4 : 8;
            case g::TY_REF:
                return t->ref->lenstorage >= 0
                           ? (t->ref->lenstorage == g::IS_VARINT ? 1 : IntSize(g::IntStorage(t->ref->lenstorage)))
                           : 8;
            case g::TY_SLICE: return 8;
            default: return 1;
        }
    }

    // Packed offsets of a run of fields (LayoutFields): no implicit
    // padding; `pad n` adds n bytes, a bare `pad` aligns the next real
    // field to PadAlign of its type.
    Layout LayoutFields(const std::vector<g::Field> &fields, const std::vector<g::TypeExpr *> &ftypes) {
        Layout lo;
        int64_t off = 0;
        for (size_t i = 0; i < fields.size(); i++) {
            auto &f = fields[i];
            lo.offs.push_back(off);
            if (f.ispad) {
                if (f.padsize >= 0) {
                    off += f.padsize;
                } else {
                    g::TypeExpr *nt = nullptr;
                    for (auto j = i + 1; j < fields.size(); j++)
                        if (!fields[j].ispad) { nt = ftypes[j]; break; }
                    auto a = nt ? PadAlign(nt) : 1;
                    off = (off + a - 1) / a * a;
                }
                continue;
            }
            off += FixedSize(ftypes[i]);
        }
        lo.size = off;
        return lo;
    }

    const Layout &StructLayout(g::StructInst *si) {
        auto it = structs.find(si);
        if (it != structs.end()) return it->second;
        return structs[si] = LayoutFields(si->st->fields, si->ftypes);
    }

    // FixedSize, for the types Known accepts. An empty struct takes the
    // one byte C gives it.
    int64_t FixedSize(g::TypeExpr *t) {
        switch (t->kind) {
            case g::TY_INT: return IntSize(t->intstorage);
            case g::TY_FLT: return t->fltstorage == g::FS_F32 ? 4 : 8;
            case g::TY_BOOL: return 1;
            case g::TY_REF: return IsFatRef(t) ? 16 : 8;
            case g::TY_SLICE: return 16;
            case g::TY_ARRAY: return t->arr->size * FixedSize(t->arr->sub);
            case g::TY_STRUCT: {
                auto sz = StructLayout(SI(t)).size;
                return sz ? sz : 1;
            }
            default: return 0;
        }
    }
};

}  // namespace goosewasm
