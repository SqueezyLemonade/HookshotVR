// Hookshot VR: the Goron and Zora tunics: their own skinned models, given the VR body's pose.
//
// A piece of vr/body/vk_vr_body.cpp, included there in a fixed order: it uses the pieces before it. Not compiled on its own.
const rig::Tunic* tunic_model(uint64_t vb0, uint64_t bytes) {
    for (const rig::Tunic* T : {&rig::goron, &rig::zora})
        if (bytes == (uint64_t)T->slots * 48)
            for (int k = 0; k < T->shapes; k++)
                if (T->vb0[k] == vb0) return T;
    return nullptr;
}

// Goron and Zora tunics are their own skinned models (palettes of 120 and 117), animated with Link but outside his 281-matrix
// body palette: without this they kept the game's running/torso pose. Each tunic palette slot follows Player_Adult's bone of
// the same name (vk_vr_tunic_rig.h). Preferred: that body bone's change from the same frame's ORIGINAL body upload to the VR
// pose, applied to the tunic's own upload (keeps the game's runtime offsets and cloth motion, as the gauntlets do). Without
// a matching body upload: the VR body bone through the bind poses. Bones without a usable body slot (Goron's extra cloth and
// cap bones, or body bones the game collapsed to hide its default tunic) ride their nearest ancestor that has one, keeping
// their own runtime motion relative to it. Head/hood bones follow the body's folded head, so the hood stays out of view.
std::shared_ptr<const std::vector<float>> tunic_skin(const rig::Tunic& T, const std::vector<float>& tun, V3 o, uint64_t frame,
                                                     const LinkBody& body) {
    if (body.child) return nullptr;
    std::lock_guard lk(cache_mu);
    const Entry* vr = canonical_rig(frame, body);
    if (!vr || !vr->adult) return nullptr;
    const Entry* orig = nullptr;
    float best = .02f;
    for (const Entry& e : cache) {
        if (!e.out || !e.adult || e.frame > frame || frame - e.frame > 2 || e.raw.size() < (size_t)rig::adult.slots * 12) continue;
        float err = 0;
        for (int j = 0; j < T.joints; j++) {
            const rig::TunicJoint& J = T.joint[j];
            err = std::max(err, length(sub(add(e.origin, point(e.raw.data() + J.body * 12, v3(J.adult))),
                                           add(o, point(tun.data() + J.slot * 12, v3(J.tunic))))));
        }
        if (err < best) best = err, orig = &e;
    }
    const float* V = vr->out->data();
    const auto usable = [&](int b) { return rig::adult.region[b] == 2 || std::fabs(det(aff(V + b * 12).r)) > .05f; };
    auto out = std::make_shared<std::vector<float>>(tun.size());
    int bind_slots = 0;
    for (int i = 0; i < T.slots; i++) {
        int j = i;
        while (j >= 0 && (T.slot[j].body < 0 || !usable(T.slot[j].body))) j = T.slot[j].parent;
        const Aff Ti = shifted(aff(tun.data() + i * 12), o);
        Aff result = Ti, inv;
        if (j >= 0) {
            const int b = T.slot[j].body;
            const Aff Vw = shifted(aff(V + b * 12), vr->origin);
            if (orig && invert(shifted(aff(orig->raw.data() + b * 12), orig->origin), &inv)) result = mul(mul(Vw, inv), Ti);
            else if (invert(shifted(aff(tun.data() + j * 12), o), &inv))
                result = mul(mul(mul(Vw, aff(T.slot[j].bind)), inv), Ti), bind_slots++;
            else return nullptr;
        }
        store(out->data() + i * 12, shifted(result, mul(o, -1.0f)));
    }
    static std::atomic<int> told{0};
    if (told.fetch_add(1) < 4)
        logf("vr: %s tunic on the VR body (%s, %d bind-space slots, frame %llu)", T.name,
             orig ? "matched to the game's body upload" : "bind-space fallback", bind_slots, (unsigned long long)frame);
    for (float f : *out) if (!std::isfinite(f)) return nullptr;
    return out;
}
