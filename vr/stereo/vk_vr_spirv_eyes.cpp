// (OoT VR, true stereo) vk_vr_spirv_eyes.h: the multiview patch of a translated stage's SPIR-V.
#include "vk_vr_spirv_eyes.h"

#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace mkwvk::vr {
namespace {

// (SPIR-V opcodes, decorations and enums used here: the specification's numbers)
enum : uint32_t {
    OpExtension = 10, OpEntryPoint = 15, OpCapability = 17, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22,
    OpTypeVector = 23, OpTypeArray = 28, OpTypePointer = 32, OpConstant = 43, OpFunction = 54, OpFunctionEnd = 56,
    OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71,
    OpCompositeExtract = 81, OpCompositeInsert = 82, OpFSub = 131, OpFMul = 133, OpFOrdEqual = 180, OpFOrdGreaterThanEqual = 190, OpIEqual = 170, OpSelect = 169, OpLabel = 248,
    OpReturn = 253, OpEmitVertex = 218, OpEmitStreamVertex = 220, OpExtInstImport = 11, OpMemoryModel = 14,
    OpExecutionMode = 16, OpString = 7, OpSource = 3, OpSourceExtension = 4, OpName = 5, OpMemberName = 6,
    OpModuleProcessed = 330, OpLine = 8, OpNoLine = 317, OpExecutionModeId = 331, OpSourceContinued = 2,
    OpDecorateId = 332, OpMemberDecorate = 72, OpDecorationGroup = 73, OpGroupDecorate = 74, OpGroupMemberDecorate = 75,
    OpDecorateString = 5632, OpMemberDecorateString = 5633,
};
constexpr uint32_t kDecorationBuiltIn = 11, kBuiltInPosition = 0, kBuiltInViewIndex = 4440, kCapabilityMultiView = 4439;
constexpr uint32_t kStorageInput = 1, kStorageOutput = 3;

struct Inst {
    size_t at;       // word offset in the module
    uint32_t op, n;  // opcode and word count
};

}  // namespace

int spirv_execution_model(const uint32_t* w, size_t count) {
    if (!w || count < 5 || w[0] != 0x07230203u) return -1;
    for (size_t i = 5; i < count;) {
        const uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (!n || i + n > count) return -1;
        if (op == OpEntryPoint && n >= 3) return (int)w[i + 1];
        i += n;
    }
    return -1;
}

bool spirv_eyes_patch(const uint32_t* w, size_t count, SpirvEyesStage stage, const float kx[2], std::vector<uint32_t>& out,
                      const char** why) {
    const char* dummy;
    if (!why) why = &dummy;
    *why = "";
    if (!w || count < 5 || w[0] != 0x07230203u) return *why = "not SPIR-V", false;
    const uint32_t version = w[1];
    uint32_t bound = w[3];
    std::vector<Inst> ins;
    for (size_t i = 5; i < count;) {
        const uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (!n || i + n > count) return *why = "malformed", false;
        ins.push_back({i, op, n});
        i += n;
    }
    // what the module already has
    bool has_multiview = false;
    uint32_t t_float = 0, t_int = 0, t_uint = 0, t_bool = 0;
    std::unordered_map<uint32_t, uint32_t> vec4_of_float;   // (vec4 type id -> 1)
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> pointer_types;   // id -> (storage, pointee)
    std::unordered_map<uint32_t, uint32_t> array_elem;      // array type -> element type
    std::unordered_map<uint32_t, uint32_t> var_type;        // variable -> pointer type
    std::unordered_set<uint32_t> position_decorated;
    std::unordered_map<uint64_t, uint32_t> float_constants, int_constants;
    size_t entry_at = 0, first_function = 0, last_decoration = 0, last_capability = 0;
    uint32_t entry_function = 0;
    int entry_count = 0;
    for (const Inst& in : ins) {
        const uint32_t* o = w + in.at;
        switch (in.op) {
        case OpCapability:
            last_capability = in.at + in.n;
            if (o[1] == kCapabilityMultiView) has_multiview = true;
            break;
        case OpEntryPoint:
            if (!entry_count++) entry_at = in.at, entry_function = o[2];
            break;
        case OpDecorate:
        case OpMemberDecorate:
        case OpDecorateId:
        case OpDecorationGroup:
        case OpGroupDecorate:
        case OpGroupMemberDecorate:
        case OpDecorateString:
        case OpMemberDecorateString:
            last_decoration = in.at + in.n;
            if (in.op == OpDecorate && in.n >= 4 && o[2] == kDecorationBuiltIn) {
                if (o[3] == kBuiltInPosition) position_decorated.insert(o[1]);
                if (o[3] == kBuiltInViewIndex) return *why = "already has ViewIndex", false;
            }
            break;
        case OpTypeFloat:
            if (o[2] == 32 && in.n == 3) t_float = o[1];
            break;
        case OpTypeInt:
            if (o[2] == 32 && o[3] == 1) t_int = o[1];
            if (o[2] == 32 && o[3] == 0) t_uint = o[1];
            break;
        case OpTypeBool: t_bool = o[1]; break;
        case OpTypeVector:
            if (o[3] == 4 && o[2] && o[2] == t_float) vec4_of_float[o[1]] = 1;
            break;
        case OpTypeArray: array_elem[o[1]] = o[2]; break;
        case OpTypePointer: pointer_types[o[1]] = {o[2], o[3]}; break;
        case OpConstant:
            if (in.n == 4 && o[1] == t_float && t_float) float_constants[o[3]] = o[2];
            if (in.n == 4 && (o[1] == t_int || o[1] == t_uint) && o[1]) int_constants[((uint64_t)o[1] << 32) | o[3]] = o[2];
            break;
        case OpVariable:
            if (!first_function) var_type[o[2]] = o[1];
            break;
        case OpFunction:
            if (!first_function) first_function = in.at;
            break;
        default: break;
        }
    }
    if (has_multiview) return *why = "already multiview", false;
    if (entry_count != 1) return *why = "not one entry point", false;
    if (!first_function || !t_float) return *why = "no function or no float type", false;
    // the Position output: a vec4 Output variable (vertex, geometry), an array of them (mesh)
    uint32_t position = 0, position_vec4 = 0;
    bool position_array = false;
    for (uint32_t id : position_decorated) {
        auto v = var_type.find(id);
        if (v == var_type.end()) continue;
        auto p = pointer_types.find(v->second);
        if (p == pointer_types.end() || p->second.first != kStorageOutput) continue;
        uint32_t pointee = p->second.second;
        bool arr = false;
        if (array_elem.count(pointee)) pointee = array_elem[pointee], arr = true;
        if (!vec4_of_float.count(pointee)) continue;
        position = id, position_vec4 = pointee, position_array = arr;
    }
    if (!position) return *why = "no Position output", false;
    if ((stage == SpirvEyesStage::Mesh) != position_array) return *why = "Position not as the stage expects", false;

    // new ids
    std::vector<uint32_t> types;   // (declarations to add, in the type section)
    auto add = [&](std::vector<uint32_t>& v, uint32_t op, std::initializer_list<uint32_t> operands) {
        v.push_back(((uint32_t)(operands.size() + 1) << 16) | op);
        v.insert(v.end(), operands.begin(), operands.end());
    };
    uint32_t vi_type = t_int ? t_int : t_uint;
    if (!vi_type) vi_type = bound++, add(types, OpTypeInt, {vi_type, 32, 1});
    if (!t_bool) t_bool = bound++, add(types, OpTypeBool, {t_bool});
    uint32_t vi_ptr = 0;
    for (auto& [id, pt] : pointer_types)
        if (pt.first == kStorageInput && pt.second == vi_type) vi_ptr = id;
    if (!vi_ptr) vi_ptr = bound++, add(types, OpTypePointer, {vi_ptr, kStorageInput, vi_type});
    auto float_const = [&](float f) {
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        auto it = float_constants.find(bits);
        if (it != float_constants.end()) return it->second;
        const uint32_t id = bound++;
        add(types, OpConstant, {t_float, id, bits});
        float_constants[bits] = id;
        return id;
    };
    auto int_const = [&](uint32_t value) {
        auto it = int_constants.find(((uint64_t)vi_type << 32) | value);
        if (it != int_constants.end()) return it->second;
        const uint32_t id = bound++;
        add(types, OpConstant, {vi_type, id, value});
        int_constants[((uint64_t)vi_type << 32) | value] = id;
        return id;
    };
    const uint32_t k0 = float_const(kx[0]), k1 = float_const(kx[1]), one = int_const(1), zero_f = float_const(0.0f),
                   one_f = float_const(1.0f);
    const uint32_t view_index = bound++;
    add(types, OpVariable, {vi_ptr, view_index, kStorageInput});

    // the code: the shift of a value, made just before an instruction
    std::vector<uint32_t> body;   // (the new function section)
    auto shift_of = [&](std::vector<uint32_t>& v) {   // -> the float to subtract for this view
        const uint32_t idx = bound++, is1 = bound++, k = bound++;
        add(v, OpLoad, {vi_type, idx, view_index});
        add(v, OpIEqual, {t_bool, is1, idx, one});
        add(v, OpSelect, {t_float, k, is1, k1, k0});
        return k;
    };
    // functions: copy, with the edits
    std::unordered_map<uint32_t, std::pair<uint32_t, std::vector<uint32_t>>> chains;   // access chain id -> (base, indices)
    bool in_entry = false;
    int edits = 0;
    for (const Inst& in : ins) {
        if (in.at < first_function) continue;
        const uint32_t* o = w + in.at;
        if (in.op == OpFunction) in_entry = o[2] == entry_function;
        if ((in.op == OpAccessChain || in.op == OpInBoundsAccessChain) && in.n >= 4) {
            std::vector<uint32_t> idx(o + 4, o + in.n);
            chains[o[2]] = {o[3], idx};
        }
        const bool before_return = in.op == OpReturn && in_entry && stage == SpirvEyesStage::Vertex;
        const bool before_emit = (in.op == OpEmitVertex || in.op == OpEmitStreamVertex) && stage == SpirvEyesStage::Geometry;
        if (before_return || before_emit) {
            const uint32_t pos = bound++, x = bound++, x2 = bound++, pos2 = bound++, pw = bound++, flat = bound++, kk = bound++;
            add(body, OpLoad, {position_vec4, pos, position});
            add(body, OpCompositeExtract, {t_float, x, pos, 0});
            const uint32_t k = shift_of(body);
            // (w exactly 1: a screen draw, not moved: vk_vr_spirv_eyes.h)
            add(body, OpCompositeExtract, {t_float, pw, pos, 3});
            add(body, OpFOrdEqual, {t_bool, flat, pw, one_f});
            add(body, OpSelect, {t_float, kk, flat, zero_f, k});
            add(body, OpFSub, {t_float, x2, x, kk});
            add(body, OpCompositeInsert, {position_vec4, pos2, x2, pos, 0});
            add(body, OpStore, {position, pos2});
            edits++;
        }
        if (in.op == OpStore && stage == SpirvEyesStage::Mesh && in.n >= 3) {
            auto c = chains.find(o[1]);
            if (c != chains.end() && c->second.first == position) {
                const std::vector<uint32_t>& idx = c->second.second;
                if (idx.size() == 1) {   // (a whole vec4 of the array)
                    const uint32_t x = bound++, x2 = bound++, v2 = bound++;
                    add(body, OpCompositeExtract, {t_float, x, o[2], 0});
                    const uint32_t k = shift_of(body);
                    add(body, OpFSub, {t_float, x2, x, k});
                    add(body, OpCompositeInsert, {position_vec4, v2, x2, o[2], 0});
                    add(body, OpStore, {o[1], v2});
                    edits++;
                    continue;
                }
                if (idx.size() == 2) {   // (one component: x only, a constant 0 or a dynamic index that is 0)
                    uint32_t component = ~0u;
                    for (auto& [key, id] : int_constants)
                        if (id == idx[1]) component = (uint32_t)key;
                    if (component != ~0u && component != 0) goto copy;   // (y, z or w: untouched)
                    const uint32_t k = shift_of(body);
                    uint32_t kk = k;
                    if (component == ~0u) {   // (dynamic: the shift only where it is 0)
                        const uint32_t is0 = bound++, sel = bound++;
                        add(body, OpIEqual, {t_bool, is0, idx[1], int_const(0)});
                        add(body, OpSelect, {t_float, sel, is0, k, zero_f});
                        kk = sel;
                    }
                    const uint32_t v2 = bound++;
                    add(body, OpFSub, {t_float, v2, o[2], kk});
                    add(body, OpStore, {o[1], v2});
                    edits++;
                    continue;
                }
                return *why = "a Position store not understood", false;
            }
            if (o[1] == position) return *why = "a store to the whole Position array", false;
        }
    copy:
        body.insert(body.end(), w + in.at, w + in.at + in.n);
    }
    if (!edits) return *why = "nothing to shift", false;

    // the module: header, capabilities (+ MultiView), the extension, everything to the end of the decorations (+ ViewIndex's),
    // the type section (+ the new declarations), the functions as edited
    out.clear();
    out.insert(out.end(), w, w + 5);
    out[3] = bound;
    size_t at = 5;
    auto copy_to = [&](size_t end) {
        out.insert(out.end(), w + at, w + end);
        at = end;
    };
    copy_to(last_capability ? last_capability : 5);
    out.push_back((2u << 16) | OpCapability), out.push_back(kCapabilityMultiView);
    if (version < 0x00010300u) {
        static const char name[] = "SPV_KHR_multiview";
        const uint32_t nwords = (uint32_t)((sizeof name + 3) / 4);
        out.push_back(((1 + nwords) << 16) | OpExtension);
        uint32_t buf[8]{};
        std::memcpy(buf, name, sizeof name);
        out.insert(out.end(), buf, buf + nwords);
    }
    // (the entry point, with ViewIndex added to its interface)
    copy_to(entry_at);
    {
        const uint32_t n = w[entry_at] >> 16;
        out.push_back(((n + 1) << 16) | OpEntryPoint);
        out.insert(out.end(), w + entry_at + 1, w + entry_at + n);
        out.push_back(view_index);
        at = entry_at + n;
    }
    if (last_decoration < at) last_decoration = at;
    copy_to(last_decoration);
    out.push_back((4u << 16) | OpDecorate), out.push_back(view_index), out.push_back(kDecorationBuiltIn),
        out.push_back(kBuiltInViewIndex);
    copy_to(first_function);   // (the debug, annotation and type sections as they were; the new declarations last)
    out.insert(out.end(), types.begin(), types.end());
    out.insert(out.end(), body.begin(), body.end());
    return true;
}


bool spirv_hud_patch(const uint32_t* w, size_t count, float scale, std::vector<uint32_t>& out, const char** why) {
    const char* dummy;
    if (!why) why = &dummy;
    *why = "";
    if (!w || count < 5 || w[0] != 0x07230203u) return *why = "not SPIR-V", false;
    uint32_t bound = w[3];
    std::vector<Inst> ins;
    for (size_t i = 5; i < count;) {
        const uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (!n || i + n > count) return *why = "malformed", false;
        ins.push_back({i, op, n});
        i += n;
    }
    uint32_t t_float = 0, t_bool = 0, entry_function = 0;
    std::unordered_set<uint32_t> vec4_of_float, position_decorated;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> pointer_types;
    std::unordered_map<uint32_t, uint32_t> var_type;
    std::unordered_map<uint32_t, uint32_t> float_constants;
    size_t first_function = 0;
    int entry_count = 0;
    for (const Inst& in : ins) {
        const uint32_t* o = w + in.at;
        switch (in.op) {
        case OpEntryPoint:
            if (!entry_count++) entry_function = o[2];
            if (o[1] != 0) return *why = "not a vertex stage", false;
            break;
        case OpDecorate:
            if (in.n >= 4 && o[2] == kDecorationBuiltIn && o[3] == kBuiltInPosition) position_decorated.insert(o[1]);
            break;
        case OpTypeFloat:
            if (o[2] == 32 && in.n == 3) t_float = o[1];
            break;
        case OpTypeBool: t_bool = o[1]; break;
        case OpTypeVector:
            if (o[3] == 4 && o[2] && o[2] == t_float) vec4_of_float.insert(o[1]);
            break;
        case OpTypePointer: pointer_types[o[1]] = {o[2], o[3]}; break;
        case OpConstant:
            if (in.n == 4 && o[1] == t_float && t_float) float_constants[o[3]] = o[2];
            break;
        case OpVariable:
            if (!first_function) var_type[o[2]] = o[1];
            break;
        case OpFunction:
            if (!first_function) first_function = in.at;
            break;
        default: break;
        }
    }
    if (entry_count != 1 || !first_function || !t_float) return *why = "not one vertex entry point", false;
    uint32_t position = 0, position_vec4 = 0;
    for (uint32_t id : position_decorated) {
        auto v = var_type.find(id);
        if (v == var_type.end()) continue;
        auto pt = pointer_types.find(v->second);
        if (pt == pointer_types.end() || pt->second.first != kStorageOutput || !vec4_of_float.count(pt->second.second)) continue;
        position = id, position_vec4 = pt->second.second;
    }
    if (!position) return *why = "no Position output", false;
    std::vector<uint32_t> types, body;
    auto add = [&](std::vector<uint32_t>& v, uint32_t op, std::initializer_list<uint32_t> operands) {
        v.push_back(((uint32_t)(operands.size() + 1) << 16) | op);
        v.insert(v.end(), operands.begin(), operands.end());
    };
    if (!t_bool) t_bool = bound++, add(types, OpTypeBool, {t_bool});
    auto float_const = [&](float f) {
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        auto it = float_constants.find(bits);
        if (it != float_constants.end()) return it->second;
        const uint32_t id = bound++;
        add(types, OpConstant, {t_float, id, bits});
        float_constants[bits] = id;
        return id;
    };
    const uint32_t k_scale = float_const(scale), k_edge = float_const(0.996f);   // (0.998 squared: within 0.2% of the edge)
    bool in_entry = false;
    int edits = 0;
    for (const Inst& in : ins) {
        if (in.at < first_function) continue;
        const uint32_t* o = w + in.at;
        if (in.op == OpFunction) in_entry = o[2] == entry_function;
        if (in.op == OpReturn && in_entry) {
            // pos = Position; for x and y: on the edge (c*c >= 0.996 w*w) kept, else c * scale; Position = pos
            const uint32_t pos = bound++, wv = bound++, ww = bound++, lim = bound++;
            add(body, OpLoad, {position_vec4, pos, position});
            add(body, OpCompositeExtract, {t_float, wv, pos, 3});
            add(body, OpFMul, {t_float, ww, wv, wv});
            add(body, OpFMul, {t_float, lim, ww, k_edge});
            uint32_t cur = pos;
            for (uint32_t comp = 0; comp < 2; comp++) {
                const uint32_t c = bound++, cc = bound++, edge = bound++, scaled = bound++, pick = bound++, next = bound++;
                add(body, OpCompositeExtract, {t_float, c, pos, comp});
                add(body, OpFMul, {t_float, cc, c, c});
                add(body, OpFOrdGreaterThanEqual, {t_bool, edge, cc, lim});
                add(body, OpFMul, {t_float, scaled, c, k_scale});
                add(body, OpSelect, {t_float, pick, edge, c, scaled});
                add(body, OpCompositeInsert, {position_vec4, next, pick, cur, comp});
                cur = next;
            }
            add(body, OpStore, {position, cur});
            edits++;
        }
        body.insert(body.end(), w + in.at, w + in.at + in.n);
    }
    if (!edits) return *why = "no return in the entry point", false;
    out.assign(w, w + 5);
    out[3] = bound;
    out.insert(out.end(), w + 5, w + first_function);
    out.insert(out.end(), types.begin(), types.end());
    out.insert(out.end(), body.begin(), body.end());
    return true;
}

}  // namespace mkwvk::vr
