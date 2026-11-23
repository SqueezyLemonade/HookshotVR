// Eye-local reads of stereo render targets, without replaying a graphics draw.
// Deliberately conservative: unsupported opaque-image operations use the existing
// separate-eye route. Original material sampler types are never changed in place.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace mkwvk::vr {
inline bool spirv_eye_samples(const std::vector<uint32_t>& src, uint64_t bindings,
                              std::vector<uint32_t>& out, const char** why) {
    auto fail = [&](const char* s) { if (why) *why = s; out.clear(); return false; };
    if (src.size() < 5 || src[0] != 0x07230203) return fail("invalid module");
    if (!bindings) { out = src; return true; }
    using Words = std::vector<uint32_t>;
    std::vector<Words> ins;
    std::map<uint32_t, Words> types;
    std::map<uint32_t, uint32_t> value_type, binding, descriptor_set;
    uint32_t bound = src[3], view = 0, view_type = 0, entry_count = 0, model = ~0u;
    bool multi = false;
    for (size_t i = 5; i < src.size();) {
        uint32_t n = src[i] >> 16, op = src[i] & 65535;
        if (!n || i + n > src.size()) return fail("bad instruction");
        Words a(src.begin() + i, src.begin() + i + n); ins.push_back(a); i += n;
        if (op >= 19 && op <= 39 && n >= 2) types[a[1]] = a;
        if (op == 15) { ++entry_count; model = a[1]; }
        if (op == 17 && a[1] == 4439) multi = true;
        if (op == 71 && n == 4) {
            if (a[2] == 33) binding[a[1]] = a[3];
            if (a[2] == 34) descriptor_set[a[1]] = a[3];
            if (a[2] == 11 && a[3] == 4440) view = a[1];
        }
    }
    if (entry_count != 1 || !(model <= 4 || model == 5365)) return fail("unsupported stage");
    bool in_function = false;
    for (const auto& a : ins) {
        const uint32_t op = a[0] & 65535;
        if (op == 54) in_function = true;
        if ((in_function || op == 59 || (op >= 41 && op <= 52)) && a.size() >= 3 && types.count(a[1]))
            value_type[a[2]] = a[1];
    }
    if (view) {
        const auto it = value_type.find(view);
        if (it == value_type.end() || !types.count(it->second)) return fail("ViewIndex type");
        const Words& p = types.at(it->second);
        if ((p[0] & 65535) != 32 || p[2] != 1) return fail("ViewIndex pointer");
        view_type = p[3];
    }
    Words declarations, globals, body;
    auto emit = [](Words& v, uint32_t op, std::initializer_list<uint32_t> args) {
        v.push_back(uint32_t(args.size() + 1) << 16 | op); v.insert(v.end(), args);
    };
    auto add_type = [&](Words a) {
        // The translated game often already has array samplers, even when the
        // selected binding is 2D. SPIR-V forbids duplicate non-aggregate types.
        for (const auto& [id, t] : types)
            if (t.size() == a.size() && t[0] == a[0] && std::equal(t.begin() + 2, t.end(), a.begin() + 2))
                return id;
        const uint32_t id = bound++; a[1] = id; types[id] = a;
        declarations.insert(declarations.end(), a.begin(), a.end()); return id;
    };
    auto scalar_int = [&]() {
        for (const auto& [id, t] : types) if ((t[0] & 65535) == 21 && t[2] == 32 && t[3] == 1) return id;
        return add_type({4u << 16 | 21, 0, 32, 1});
    };
    auto vector3 = [&](uint32_t scalar) {
        for (const auto& [id, t] : types) if ((t[0] & 65535) == 23 && t[2] == scalar && t[3] == 3) return id;
        return add_type({4u << 16 | 23, 0, scalar, 3});
    };
    std::map<uint32_t, uint32_t> clones;
    std::function<uint32_t(uint32_t)> clone = [&](uint32_t id) -> uint32_t {
        if (clones.count(id)) return clones[id];
        if (!types.count(id)) return 0;
        Words t = types.at(id); const uint32_t op = t[0] & 65535;
        if (op == 25) { // Dim2D, non-arrayed, non-MS, sampled (not storage).
            if (t.size() < 9 || t[3] != 1 || t[5] != 0 || t[6] != 0 || t[7] != 1) return 0;
            t[5] = 1;
        } else if (op == 27 || op == 28 || op == 32) {
            const size_t elem = op == 32 ? 3 : 2;
            const uint32_t child = clone(t[elem]); if (!child) return 0; t[elem] = child;
        } else return 0;
        return clones[id] = add_type(t);
    };
    std::set<uint32_t> changed;
    uint64_t found = 0;
    std::vector<Words> selected_vars;
    for (const auto& a : ins) if ((a[0] & 65535) == 59 && a[3] == 0 && binding.count(a[2])) {
        const uint32_t b = binding[a[2]];
        if (b >= 64 || !(bindings >> b & 1)) continue;
        if (!descriptor_set.count(a[2]) || descriptor_set[a[2]] != 0) return fail("descriptor set");
        const uint32_t t = clone(a[1]); if (!t) return fail("unsupported sampled type");
        Words v = a; v[1] = t; selected_vars.push_back(v); changed.insert(a[2]); found |= uint64_t(1) << b;
    }
    if (found != bindings) return fail("binding not in module");
    const bool new_view = !view;
    if (!view_type) view_type = scalar_int();
    if (new_view) {
        const uint32_t ptr = add_type({4u << 16 | 32, 0, 1, view_type});
        view = bound++; emit(globals, 59, {ptr, view, 1});
    }
    auto coord3 = [&](uint32_t coord) -> uint32_t {
        if (!value_type.count(coord) || !types.count(value_type[coord])) return 0;
        const Words t = types.at(value_type[coord]);
        if ((t[0] & 65535) != 23 || t[3] < 2) return 0;
        const uint32_t scalar = t[2]; const Words s = types.at(scalar);
        if (!((s[0] & 65535) == 21 || (s[0] & 65535) == 22) || s[2] != 32) return 0;
        const uint32_t x = bound++, y = bound++, idx = bound++, c = bound++;
        emit(body, 81, {scalar, x, coord, 0}); emit(body, 81, {scalar, y, coord, 1});
        emit(body, 61, {view_type, idx, view});
        uint32_t layer = idx;
        if (scalar != view_type) {
            layer = bound++;
            emit(body, (s[0] & 65535) == 22 ? (types.at(view_type)[3] ? 111 : 112) : 124, {scalar, layer, idx});
        }
        emit(body, 80, {vector3(scalar), c, x, y, layer}); return c;
    };
    bool functions = false;
    for (const auto& original : ins) {
        Words a = original; const uint32_t op = a[0] & 65535;
        if (op == 54) functions = true;
        if (!functions) continue;
        if ((op == 61 || op == 65 || op == 66 || op == 83 || op == 100) && a.size() >= 4 && changed.count(a[3])) {
            const uint32_t t = clone(a[1]); if (!t) return fail("opaque type propagation");
            a[1] = t; changed.insert(a[2]);
        } else if ((op >= 87 && op <= 90 || op == 95 || op == 96 || op == 97) && changed.count(a[3])) {
            const uint32_t c = coord3(a[4]); if (!c) return fail("coordinate type"); a[4] = c;
        } else if ((op == 103 || op == 104) && changed.count(a[3])) {
            if (!types.count(a[1])) return fail("size result type");
            const Words t = types.at(a[1]);
            if ((t[0] & 65535) != 23 || t[3] != 2) return fail("size result dimensions");
            const uint32_t old_type = a[1], old_id = a[2], tmp = bound++;
            a[1] = vector3(t[2]); a[2] = tmp; body.insert(body.end(), a.begin(), a.end());
            emit(body, 79, {old_type, old_id, tmp, tmp, 0, 1}); continue;
        } else if ((op == 105 || op == 106 || op == 107) && changed.count(a[3])) {
            // QueryLod still takes spatial coordinates only; levels/samples are scalar.
        } else {
            // Fail closed for projected/sparse operations, opaque function calls,
            // phi/select image values, stores, and anything not explicitly handled.
            for (size_t j = 1; j < a.size(); ++j) if (changed.count(a[j])) return fail("unsupported image operation");
        }
        body.insert(body.end(), a.begin(), a.end());
    }
    if (!functions) return fail("no functions");
    out.assign(src.begin(), src.begin() + 5);
    bool caps_done = false, decoration_added = false;
    for (const auto& original : ins) {
        Words a = original; const uint32_t op = a[0] & 65535;
        if (!caps_done && op != 17) {
            if (!multi) emit(out, 17, {4439});
            if (!multi && src[1] < 0x10300) emit(out, 10, {0x5f565053,0x5f52484b,0x746c756d,0x65697669,0x77});
            caps_done = true;
        }
        if (op == 15 && new_view) { a.push_back(view); a[0] += 1u << 16; }
        if (!decoration_added && op >= 19 && op <= 39) {
            if (new_view) {
                emit(out, 71, {view, 11, 4440});
                if (model == 4) emit(out, 71, {view, 14}); // Fragment integer input: Flat.
            }
            decoration_added = true;
        }
        if (op == 54) break;
        if (op == 59 && changed.count(a[2])) continue; // Declared after cloned types.
        out.insert(out.end(), a.begin(), a.end());
    }
    out.insert(out.end(), declarations.begin(), declarations.end());
    for (const auto& a : selected_vars) out.insert(out.end(), a.begin(), a.end());
    out.insert(out.end(), globals.begin(), globals.end());
    out.insert(out.end(), body.begin(), body.end()); out[3] = bound;
    if (why) *why = "";
    return true;
}
} // namespace mkwvk::vr
