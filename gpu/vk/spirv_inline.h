// (hookshot) Inlining of the functions a module marks Inline, at every call: the translator's global memory helpers
// (spirv_emit_context.cpp DefineGlobalMemoryFunctions), which search the program's storage buffers for an address and return
// at the first that holds it. NVIDIA's driver keeps them calls whatever their shape (measured: the Inline hint, and one return
// at the end, change nothing), and OoT's terrain mesh program 43ec86f3, 73 of them in its loops, drew 13% faster inlined
// (1.34 -> 1.17 ms at 2880x2304, its pipeline's cold compile no slower). Plain functions over the words, no Vulkan; ids are
// renamed through spirv_unroll's operand walk, so never a literal that happens to equal one.
//
// A call is inlined as a one-trip loop, as spirv-opt's merge-return and inline passes make it:
//   (the block up to the call) OpBranch %H
//   %H: OpLoopMerge %X %C ; OpBranch %entry'      (the callee's blocks, renamed, its returns now OpBranch %X)
//   %C: OpBranch %H                               (never reached)
//   %X: %result = OpPhi (each return's value and block)   (the rest of the block: its merge instruction and terminator)
// so a return from inside the callee's selections is a break out of that loop. Phis elsewhere that named the call's block as
// a predecessor name %X now. A function is taken only when it has no loop, no local variable and no call of its own, and
// every instruction in it is one the operand table describes; a call in a loop's header block is left as a call. A function
// no call is left to is removed, with its names and decorations.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "spirv_unroll.h"

namespace spirv_inline {

using spirv_unroll::Ins;
using spirv_unroll::Module;
using spirv_unroll::for_each_id;

enum : uint32_t { kName = 5, kMemberName = 6, kDecorate = 71, kMemberDecorate = 72, kFunction = 54, kParam = 55, kFunctionEnd = 56,
                  kCall = 57, kVariable = 59, kLoopMerge = 246, kSelectionMerge = 247, kLabel = 248, kPhi = 245, kBranch = 249,
                  kReturn = 253, kReturnValue = 254 };
inline uint32_t op_of(const Ins& in) { return in[0] & 0xffff; }
inline Ins make(uint32_t op, std::initializer_list<uint32_t> w) {
    Ins in{((uint32_t)(w.size() + 1) << 16) | op};
    in.insert(in.end(), w.begin(), w.end());
    return in;
}

struct Callee {
    size_t begin = 0, end = 0;    // (OpFunction .. OpFunctionEnd, in m.ins)
    uint32_t type = 0;            // (its result type; void's id when it returns nothing)
    std::vector<uint32_t> params;
    std::vector<uint32_t> made;   // (every id its body makes)
};

/// The functions that may be inlined, by id
inline std::unordered_map<uint32_t, Callee> callees(Module& m) {
    std::unordered_map<uint32_t, Callee> out;
    auto& I = m.ins;
    for (size_t k = 0; k < I.size(); k++) {
        if (op_of(I[k]) != kFunction || I[k].size() < 5) continue;
        size_t e = k + 1;
        while (e < I.size() && op_of(I[e]) != kFunctionEnd) e++;
        if (e >= I.size()) break;
        const uint32_t control = I[k][3];
        if ((control & 0x1) && !(control & 0x2)) {   // (Inline, not DontInline)
            Callee c;
            c.begin = k, c.end = e, c.type = I[k][1];
            bool ok = true;
            for (size_t j = k + 1; j < e && ok; j++) {
                const uint32_t op = op_of(I[j]);
                if (op == kLoopMerge || op == kVariable || op == kCall) ok = false;
                if (op == kParam) c.params.push_back(I[j][2]);
                Ins tmp = I[j];
                uint32_t r = 0;
                if (!for_each_id(tmp, [&](uint32_t& id, bool res) { if (res) r = id; })) ok = false;
                if (r && op != kParam) c.made.push_back(r);
            }
            if (ok) out[I[k][2]] = std::move(c);
        }
        k = e;
    }
    return out;
}

/// One call inlined: true when the module changed
inline bool inline_one(Module& m, std::unordered_map<uint32_t, Callee>& fns, std::unordered_set<size_t>& refused) {
    auto& I = m.ins;
    // the callees' decorations, to be copied with each copy of their ids
    for (size_t k = 0; k < I.size(); k++) {
        if (op_of(I[k]) != kCall || I[k].size() < 4 || refused.count(k)) continue;
        const auto f = fns.find(I[k][3]);
        if (f == fns.end()) continue;
        const Callee& c = f->second;
        if (k >= c.begin && k <= c.end) continue;
        if (I[k].size() - 4 != c.params.size()) { refused.insert(k); continue; }
        // the call's block: its label before it, its end (the terminator) after it
        size_t b = k;
        while (b > 0 && op_of(I[b]) != kLabel) b--;
        size_t t = k + 1;
        while (t < I.size() && !spirv_unroll::is_terminator(op_of(I[t]))) t++;
        if (op_of(I[b]) != kLabel || t >= I.size()) { refused.insert(k); continue; }
        bool header = false;
        for (size_t j = k + 1; j < t; j++) header |= op_of(I[j]) == kLoopMerge;
        if (header) { refused.insert(k); continue; }   // (a call in a loop's header block stays a call)
        const uint32_t L = I[b][1], result_type = I[k][1], result = I[k][2];
        // the copy's ids
        std::unordered_map<uint32_t, uint32_t> ren;
        for (uint32_t id : c.made) ren[id] = m.header[3]++;
        for (size_t p = 0; p < c.params.size(); p++) ren[c.params[p]] = I[k][4 + p];
        const uint32_t H = m.header[3]++, C = m.header[3]++, X = m.header[3]++;
        std::vector<Ins> body;
        std::vector<uint32_t> phi;   // (value, block) pairs
        uint32_t entry = 0, current = 0;
        for (size_t j = c.begin + 1; j < c.end; j++) {
            const uint32_t op = op_of(I[j]);
            if (op == kParam) continue;
            Ins in = I[j];
            for_each_id(in, [&](uint32_t& id, bool) { const auto r = ren.find(id); if (r != ren.end()) id = r->second; });
            if (op == kLabel) {
                current = in[1];
                if (!entry) entry = current;
            }
            if (op == kReturnValue) {
                phi.push_back(in[1]), phi.push_back(current);
                body.push_back(make(kBranch, {X}));
                continue;
            }
            if (op == kReturn) {
                body.push_back(make(kBranch, {X}));
                continue;
            }
            body.push_back(std::move(in));
        }
        if (!entry) { refused.insert(k); continue; }
        // the decorations of the callee's ids, copied for the copy's
        std::vector<Ins> decorations;
        std::unordered_set<uint32_t> made(c.made.begin(), c.made.end());
        for (size_t j = 0; j < I.size() && op_of(I[j]) != kFunction; j++) {
            const uint32_t op = op_of(I[j]);
            if ((op == kDecorate || op == kMemberDecorate) && I[j].size() >= 3 && made.count(I[j][1])) {
                Ins d = I[j];
                d[1] = ren[d[1]];
                decorations.push_back(std::move(d));
            }
        }
        std::vector<Ins> out;
        out.reserve(I.size() + body.size() + decorations.size() + 8);
        size_t first_function = 0;
        while (first_function < I.size() && op_of(I[first_function]) != kFunction) first_function++;
        size_t last_decoration = 0;
        for (size_t j = 0; j < first_function; j++)
            if (op_of(I[j]) == kDecorate || op_of(I[j]) == kMemberDecorate) last_decoration = j;
        // the function holding the call: phis naming L as a predecessor name X now
        size_t fb = b, fe = t;
        while (fb > 0 && op_of(I[fb]) != kFunction) fb--;
        while (fe < I.size() && op_of(I[fe]) != kFunctionEnd) fe++;
        for (size_t j = 0; j < I.size(); j++) {
            if (j == k) {
                out.push_back(make(kBranch, {H}));
                out.push_back(make(kLabel, {H}));
                out.push_back(make(kLoopMerge, {X, C, 0}));
                out.push_back(make(kBranch, {entry}));
                for (Ins& in : body) out.push_back(std::move(in));
                out.push_back(make(kLabel, {C}));
                out.push_back(make(kBranch, {H}));
                out.push_back(make(kLabel, {X}));
                if (!phi.empty()) {
                    Ins p{((uint32_t)(phi.size() + 3) << 16) | kPhi, result_type, result};
                    p.insert(p.end(), phi.begin(), phi.end());
                    out.push_back(std::move(p));
                }
                continue;
            }
            Ins in = std::move(I[j]);
            if (op_of(in) == kPhi && j > fb && j < fe && j > t)
                for (size_t w = 4; w < in.size(); w += 2)
                    if (in[w] == L) in[w] = X;
            if (op_of(in) == kPhi && j > fb && j < fe && j < b)   // (a phi before the block, a loop header's, from a back edge)
                for (size_t w = 4; w < in.size(); w += 2)
                    if (in[w] == L) in[w] = X;
            out.push_back(std::move(in));
            if (j == last_decoration && last_decoration)
                for (Ins& d : decorations) out.push_back(std::move(d));
        }
        I.swap(out);
        refused.clear();   // (indices moved)
        return true;
    }
    return false;
}

/// The functions with no call left, removed with their names and decorations
inline void remove_unused(Module& m, const std::unordered_map<uint32_t, Callee>& fns) {
    auto& I = m.ins;
    std::unordered_set<uint32_t> called;
    for (const Ins& in : I)
        if (op_of(in) == kCall && in.size() >= 4) called.insert(in[3]);
    std::unordered_set<uint32_t> gone_fns, gone_ids;
    for (const auto& [id, c] : fns)
        if (!called.count(id)) gone_fns.insert(id);
    if (gone_fns.empty()) return;
    std::vector<Ins> out;
    out.reserve(I.size());
    for (size_t k = 0; k < I.size(); k++) {
        if (op_of(I[k]) == kFunction && I[k].size() >= 5 && gone_fns.count(I[k][2])) {
            size_t e = k;
            while (e < I.size() && op_of(I[e]) != kFunctionEnd) {
                Ins tmp = I[e];
                for_each_id(tmp, [&](uint32_t& id, bool res) { if (res) gone_ids.insert(id); });
                e++;
            }
            k = e;
            continue;
        }
        out.push_back(std::move(I[k]));
    }
    std::vector<Ins> kept;
    kept.reserve(out.size());
    for (Ins& in : out) {
        const uint32_t op = op_of(in);
        if ((op == kName || op == kMemberName || op == kDecorate || op == kMemberDecorate) && in.size() >= 2 && gone_ids.count(in[1]))
            continue;
        kept.push_back(std::move(in));
    }
    I.swap(kept);
}

/// Every call of a function marked Inline that can be, inlined: false when none was. calls_left: the ones left calls
inline bool inline_marked(const uint32_t* code, size_t words, std::vector<uint32_t>& out, int* inlined = nullptr,
                          int* calls_left = nullptr) {
    Module m;
    if (!spirv_unroll::parse(code, words, m)) return false;
    auto fns = callees(m);
    if (fns.empty()) return false;
    std::unordered_set<size_t> refused;
    int count = 0;
    while (inline_one(m, fns, refused)) {
        count++;
        fns = callees(m);   // (their places moved)
    }
    if (calls_left) {
        *calls_left = 0;
        for (const Ins& in : m.ins) *calls_left += op_of(in) == kCall && in.size() >= 4 && fns.count(in[3]);
    }
    if (inlined) *inlined = count;
    if (!count) return false;
    remove_unused(m, fns);
    out = spirv_unroll::emit(m);
    return true;
}

} // namespace spirv_inline
