// (OoT VR, true stereo, STE-31) A fragment stage drawn once for both eyes that reads copies of the camera: each of its reads of
// a camera number corrected for the view it is drawing (gl_ViewIndex), so its lighting and shadow lookups are worked out
// from each eye's own place, with no second draw.
//
// The game keeps copies of the camera in its constant buffers (vk_vr_eyes.cpp eyes_move_camera_copies learns where). An eye
// e metres along the camera's right changes only a few numbers of each copy: the view's x translation (row 0, w) by -e, each
// inverse-view row's w by +e times that row's x, and the view-projection's row 0 w by -P00 * e. So each such float is read as
//     value + d[ViewIndex] * (from < 0 ? 1 : the float at `from` in the same buffer)
// The translator names each constant buffer's variables "c<binding>" (one per element type, all aliasing the buffer), and
// reads them through an access chain (0, constant index): only those reads are corrected; a read at a computed place is left
// as it is (counted, not refused). Header only, so the tester can build it alone (vr/tools/test.py stereo_camera_patch).
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mkwvk::vr {

/// One float of a constant buffer to correct per view: value + d[view] * (from < 0 ? 1 : buffer[from]) (offsets in floats)
struct CameraFix {
    uint32_t binding;
    uint32_t at;
    int32_t from;
    float d[2];
};

/// The fragment stage's SPIR-V with its reads of the camera corrected per view; false (why) when nothing could be corrected.
/// edits: the reads corrected; dynamic: reads of those buffers at a computed place (not corrected)
inline bool spirv_camera_patch(const std::vector<uint32_t>& src, const std::vector<CameraFix>& fixes, std::vector<uint32_t>& out,
                               int* edits_out, int* dynamic_out, const char** why) {
    enum : uint32_t {
        OpName = 5, OpExtension = 10, OpEntryPoint = 15, OpCapability = 17, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22,
        OpTypeVector = 23, OpTypeArray = 28, OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43, OpFunction = 54,
        OpVariable = 59, OpLoad = 61, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71, OpMemberDecorate = 72,
        OpDecorationGroup = 73, OpGroupDecorate = 74, OpGroupMemberDecorate = 75, OpCompositeExtract = 81,
        OpCompositeInsert = 82, OpFAdd = 129, OpFMul = 133, OpBitcast = 124, OpSelect = 169, OpIEqual = 170,
        OpDecorateId = 332, OpDecorateString = 5632, OpMemberDecorateString = 5633,
    };
    constexpr uint32_t kBuiltIn = 11, kViewIndex = 4440, kMultiView = 4439, kInput = 1, kUniform = 2;
    const char* dummy;
    if (!why) why = &dummy;
    *why = "";
    int edits = 0, dynamic = 0;
    if (edits_out) *edits_out = 0;
    if (dynamic_out) *dynamic_out = 0;
    out.clear();
    if (src.size() < 5 || src[0] != 0x07230203u) return *why = "not SPIR-V", false;
    if (fixes.empty()) return *why = "no camera copies", false;
    const uint32_t* w = src.data();
    const size_t count = src.size();
    const uint32_t version = w[1];
    uint32_t bound = w[3];
    struct Inst { size_t at; uint32_t op, n; };
    std::vector<Inst> ins;
    for (size_t i = 5; i < count;) {
        const uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (!n || i + n > count) return *why = "malformed", false;
        ins.push_back({i, op, n});
        i += n;
    }
    std::unordered_map<uint32_t, std::vector<const CameraFix*>> fixes_of;   // binding -> its fixes
    for (const CameraFix& f : fixes) fixes_of[f.binding].push_back(&f);

    // what the module has
    std::unordered_map<uint32_t, std::string> names;
    std::unordered_map<uint32_t, uint32_t> int_width, vec_comp, vec_n, array_elem, struct_first, var_ptr, stride;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> ptr_of;   // pointer type -> (storage, pointee)
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> constant;   // id -> (type, value) of 32-bit integers
    std::unordered_set<uint32_t> float_types;
    uint32_t t_float = 0, t_bool = 0, t_int = 0, view_var = 0;
    bool multiview = false;
    size_t entry_at = 0, first_function = 0, last_decoration = 0, last_capability = 0;
    int entries = 0;
    for (const Inst& in : ins) {
        const uint32_t* o = w + in.at;
        switch (in.op) {
        case OpName: names[o[1]] = std::string((const char*)(o + 2), strnlen((const char*)(o + 2), (in.n - 2) * 4)); break;
        case OpCapability:
            last_capability = in.at + in.n;
            if (o[1] == kMultiView) multiview = true;
            break;
        case OpEntryPoint:
            if (!entries++) entry_at = in.at;
            break;
        case OpDecorate: case OpMemberDecorate: case OpDecorateId: case OpDecorationGroup: case OpGroupDecorate:
        case OpGroupMemberDecorate: case OpDecorateString: case OpMemberDecorateString:
            last_decoration = in.at + in.n;
            if (in.op == OpDecorate && in.n >= 4 && o[2] == kBuiltIn && o[3] == kViewIndex) view_var = o[1];
            if (in.op == OpDecorate && in.n >= 4 && o[2] == 6) stride[o[1]] = o[3];   // (ArrayStride)
            break;
        case OpTypeBool: t_bool = o[1]; break;
        case OpTypeInt:
            int_width[o[1]] = o[2];
            if (o[2] == 32 && !t_int) t_int = o[1];
            break;
        case OpTypeFloat:
            if (o[2] == 32) t_float = o[1], float_types.insert(o[1]);
            break;
        case OpTypeVector: vec_comp[o[1]] = o[2], vec_n[o[1]] = o[3]; break;
        case OpTypeArray: array_elem[o[1]] = o[2]; break;
        case OpTypeStruct:
            if (in.n >= 3) struct_first[o[1]] = o[2];
            break;
        case OpTypePointer: ptr_of[o[1]] = {o[2], o[3]}; break;
        case OpConstant:
            if (in.n == 4 && int_width.count(o[1]) && int_width[o[1]] == 32) constant[o[2]] = {o[1], o[3]};
            break;
        case OpVariable:
            if (!first_function) var_ptr[o[2]] = o[1];
            break;
        case OpFunction:
            if (!first_function) first_function = in.at;
            break;
        default: break;
        }
    }
    if (entries != 1 || !first_function) return *why = "not one entry point", false;
    if (!t_float) return *why = "no float type", false;

    // the camera buffers' variables: "c<binding>" in Uniform storage, a struct of one array of 32-bit scalars or vectors
    struct CbufVar { uint32_t binding, elem, comp, n, ptr_elem; bool is_float; };
    std::unordered_map<uint32_t, CbufVar> cbuf;
    for (auto& [id, ptr] : var_ptr) {
        auto nm = names.find(id);
        if (nm == names.end() || nm->second.size() < 2 || nm->second[0] != 'c') continue;
        char* end = nullptr;
        const unsigned long b = strtoul(nm->second.c_str() + 1, &end, 10);
        if (!end || *end || !fixes_of.count((uint32_t)b)) continue;
        auto p = ptr_of.find(ptr);
        if (p == ptr_of.end() || p->second.first != kUniform || !struct_first.count(p->second.second)) continue;
        auto a = array_elem.find(struct_first[p->second.second]);
        if (a == array_elem.end()) continue;
        const uint32_t elem = a->second;
        const uint32_t comp = vec_comp.count(elem) ? vec_comp[elem] : elem, n = vec_n.count(elem) ? vec_n[elem] : 1;
        const bool is_float = float_types.count(comp) != 0;
        if (!is_float && !(int_width.count(comp) && int_width[comp] == 32)) continue;   // (the 8- and 16-bit views: not read here)
        if (n != 1 && n != 2 && n != 4) continue;
        if (!stride.count(a->first) || stride[a->first] != 4 * n) continue;   // (the translator's tight arrays only)
        uint32_t ptr_elem = 0;
        for (auto& [pid, pt] : ptr_of)
            if (pt.first == kUniform && pt.second == elem) ptr_elem = pid;
        cbuf[id] = {(uint32_t)b, elem, comp, n, ptr_elem, is_float};
    }
    if (cbuf.empty()) return *why = "no buffer of the camera's binding read", false;

    // new declarations
    std::vector<uint32_t> types;
    auto add = [&](std::vector<uint32_t>& v, uint32_t op, std::initializer_list<uint32_t> operands) {
        v.push_back(((uint32_t)(operands.size() + 1) << 16) | op);
        v.insert(v.end(), operands.begin(), operands.end());
    };
    if (!t_bool) t_bool = bound++, add(types, OpTypeBool, {t_bool});
    bool new_view = false;
    uint32_t view_type = 0;
    if (view_var) {
        auto v = var_ptr.find(view_var);
        if (v == var_ptr.end() || !ptr_of.count(v->second)) return *why = "ViewIndex not understood", false;
        view_type = ptr_of[v->second].second;
    } else {
        if (!t_int) t_int = bound++, add(types, OpTypeInt, {t_int, 32, 1}), int_width[t_int] = 32;
        view_type = t_int;
        uint32_t vp = 0;
        for (auto& [pid, pt] : ptr_of)
            if (pt.first == kInput && pt.second == view_type) vp = pid;
        if (!vp) vp = bound++, add(types, OpTypePointer, {vp, kInput, view_type});
        view_var = bound++, new_view = true;
        add(types, OpVariable, {vp, view_var, kInput});
    }
    std::unordered_map<uint64_t, uint32_t> made_int;
    auto int_const = [&](uint32_t type, uint32_t value) {
        for (auto& [id, c] : constant)
            if (c.first == type && c.second == value) return id;
        const uint64_t key = (uint64_t)type << 32 | value;
        if (auto it = made_int.find(key); it != made_int.end()) return it->second;
        const uint32_t id = bound++;
        add(types, OpConstant, {type, id, value});
        return made_int[key] = id;
    };
    std::unordered_map<uint32_t, uint32_t> made_float;
    auto float_const = [&](float f) {
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        if (auto it = made_float.find(bits); it != made_float.end()) return it->second;
        const uint32_t id = bound++;
        add(types, OpConstant, {t_float, id, bits});
        return made_float[bits] = id;
    };
    const uint32_t view_one = int_const(view_type, 1);

    // the functions, with each corrected read
    std::vector<uint32_t> body;
    struct Chain { uint32_t var, index, index_type; };
    std::unordered_map<uint32_t, Chain> chains;
    auto as_float = [&](uint32_t value, const CbufVar& c) {
        if (c.is_float) return value;
        const uint32_t f = bound++;
        add(body, OpBitcast, {t_float, f, value});
        return f;
    };
    auto component = [&](uint32_t vec, const CbufVar& c, uint32_t k) {   // (component k of a loaded element)
        if (c.n == 1) return vec;
        const uint32_t x = bound++;
        add(body, OpCompositeExtract, {c.comp, x, vec, k});
        return x;
    };
    for (const Inst& in : ins) {
        const uint32_t* o = w + in.at;
        if (in.at < first_function) continue;
        // (result type, result, base, the struct's member 0, the array's index)
        if ((in.op == OpAccessChain || in.op == OpInBoundsAccessChain) && in.n == 6 && cbuf.count(o[3])) {
            auto i0 = constant.find(o[4]), i1 = constant.find(o[5]);
            if (i0 != constant.end() && i0->second.second == 0) {
                if (i1 != constant.end()) chains[o[2]] = {o[3], i1->second.second, i1->second.first};
                else dynamic++;
            }
        }
        if (in.op == OpLoad && in.n >= 4) {
            auto ch = chains.find(o[3]);
            if (ch != chains.end()) {
                const CbufVar& c = cbuf[ch->second.var];
                const uint32_t first = ch->second.index * c.n;   // (the first float this read covers)
                std::vector<const CameraFix*> here;
                for (const CameraFix* f : fixes_of[c.binding])
                    if (f->at >= first && f->at < first + c.n) here.push_back(f);
                if (!here.empty()) {
                    const uint32_t raw = bound++;
                    body.push_back(w[in.at]);
                    body.push_back(o[1]), body.push_back(raw);
                    body.insert(body.end(), o + 3, o + in.n);
                    const uint32_t vi = bound++, is1 = bound++;
                    add(body, OpLoad, {view_type, vi, view_var});
                    add(body, OpIEqual, {t_bool, is1, vi, view_one});
                    uint32_t cur = raw;
                    for (size_t h = 0; h < here.size(); h++) {
                        const CameraFix& f = *here[h];
                        const uint32_t k = f.at - first;
                        const uint32_t value = as_float(component(raw, c, k), c);
                        const uint32_t d = bound++;
                        add(body, OpSelect, {t_float, d, is1, float_const(f.d[1]), float_const(f.d[0])});
                        uint32_t delta = d;
                        if (f.from >= 0) {
                            uint32_t x;
                            if ((uint32_t)f.from >= first && (uint32_t)f.from < first + c.n) {
                                x = as_float(component(raw, c, (uint32_t)f.from - first), c);
                            } else {   // (another element of the same buffer)
                                if (!c.ptr_elem) return *why = "no pointer type for a second read", false;
                                const uint32_t p = bound++, e = bound++;
                                add(body, OpAccessChain, {c.ptr_elem, p, ch->second.var, int_const(ch->second.index_type, 0),
                                                          int_const(ch->second.index_type, (uint32_t)f.from / c.n)});
                                add(body, OpLoad, {c.elem, e, p});
                                x = as_float(component(e, c, (uint32_t)f.from % c.n), c);
                            }
                            delta = bound++;
                            add(body, OpFMul, {t_float, delta, d, x});
                        }
                        if (c.n == 1) {   // (a scalar read holds one float: one fix, its result the read's own id)
                            if (c.is_float) {
                                add(body, OpFAdd, {t_float, o[2], value, delta});
                            } else {
                                const uint32_t sum = bound++;
                                add(body, OpFAdd, {t_float, sum, value, delta});
                                add(body, OpBitcast, {c.comp, o[2], sum});
                            }
                            break;
                        }
                        const uint32_t sum = bound++;
                        add(body, OpFAdd, {t_float, sum, value, delta});
                        uint32_t back = sum;
                        if (!c.is_float) back = bound++, add(body, OpBitcast, {c.comp, back, sum});
                        const uint32_t next = h + 1 == here.size() ? o[2] : bound++;
                        add(body, OpCompositeInsert, {o[1], next, back, cur, k});
                        cur = next;
                    }
                    edits++;
                    continue;
                }
            }
        }
        body.insert(body.end(), w + in.at, w + in.at + in.n);
    }
    if (edits_out) *edits_out = edits;
    if (dynamic_out) *dynamic_out = dynamic;
    if (!edits) return *why = "no read of the camera at a constant place", false;

    // the module: header, capabilities (+ MultiView), the extension, the entry point (+ ViewIndex), the decorations (+ its
    // BuiltIn), the type section (+ the new declarations), the functions as edited
    out.insert(out.end(), w, w + 5);
    out[3] = bound;
    size_t at = 5;
    auto copy_to = [&](size_t end) {
        if (end > at) out.insert(out.end(), w + at, w + end), at = end;
    };
    copy_to(last_capability ? last_capability : 5);
    if (!multiview) {
        out.push_back((2u << 16) | OpCapability), out.push_back(kMultiView);
        if (version < 0x00010300u) {
            static const char name[] = "SPV_KHR_multiview";
            const uint32_t nwords = (uint32_t)((sizeof name + 3) / 4);
            out.push_back(((1 + nwords) << 16) | OpExtension);
            uint32_t buf[8]{};
            std::memcpy(buf, name, sizeof name);
            out.insert(out.end(), buf, buf + nwords);
        }
    }
    copy_to(entry_at);
    {
        const uint32_t n = w[entry_at] >> 16;
        out.push_back(((n + (new_view ? 1 : 0)) << 16) | OpEntryPoint);
        out.insert(out.end(), w + entry_at + 1, w + entry_at + n);
        if (new_view) out.push_back(view_var);
        at = entry_at + n;
    }
    if (last_decoration < at) last_decoration = at;
    copy_to(last_decoration);
    if (new_view) {   // (a fragment stage's integer input is Flat, a built-in too: VUID-StandaloneSpirv-Flat-04744)
        out.push_back((4u << 16) | OpDecorate), out.push_back(view_var), out.push_back(kBuiltIn), out.push_back(kViewIndex);
        out.push_back((3u << 16) | OpDecorate), out.push_back(view_var), out.push_back(14);   // (Flat)
    }
    copy_to(first_function);
    out.insert(out.end(), types.begin(), types.end());
    out.insert(out.end(), body.begin(), body.end());
    return true;
}

}  // namespace mkwvk::vr
