// Hookshot VR: recognising Link's palette uploads, and one VR pose per game frame shared by every pass.
//
// A piece of vr/body/vk_vr_body.cpp, included there in a fixed order: it uses the pieces before it. Not compiled on its own.
// ---- matching Link, and the per-upload cache --------------------------------------------------------------------------------
float neck_error(const float* neck, const rig::Model& M, V3 origin, V3 root) {
    const V3 animated = add(origin, point(neck, v3(M.neck_pos)));
    return length(sub(animated, add(root, v3(M.neck_pos))));
}

struct Entry {
    uint64_t addr = 0;
    std::vector<float> raw;
    std::shared_ptr<const std::vector<float>> out;
    uint64_t used = 0;
    V3 origin{};
    bool adult = false;
    uint64_t frame = 0;
    uint32_t pose_seq = 0;
};
std::mutex cache_mu;
Entry cache[32];
uint64_t cache_clock = 0;
V3 last_origin{};
bool have_origin = false;
// Real native uploads only. canonical_rig advances a VR entry's frame while
// retaining its source raw animation; that is NOT a current native hand sample.
struct NativeHands {
    uint64_t frame = ~0ull;
    bool adult = false;
    V3 root{}, wrist[2]{};
};
NativeHands native_hands[32];   // cache_mu held
unsigned native_hands_next = 0;
void remember_native_hands(const rig::Model& M, const std::vector<float>& raw, V3 origin, V3 root, uint64_t frame) {
    NativeHands sample;
    sample.frame = frame; sample.adult = &M == &rig::adult; sample.root = root;
    for (int k=0;k<2;k++) {
        if (raw.size() < (size_t)(M.arm[k].wrist+1)*12) return;
        sample.wrist[k] = add(origin,point(raw.data()+M.arm[k].wrist*12,v3(M.arm[k].wrist_pos)));
    }
    for (const NativeHands& h:native_hands)
        if (h.frame==frame && h.adult==sample.adult && length(sub(h.root,root))<1e-4f &&
            length(sub(h.wrist[0],sample.wrist[0]))<1e-4f && length(sub(h.wrist[1],sample.wrist[1]))<1e-4f) return;
    native_hands[native_hands_next++ % 32] = sample;
}
unsigned native_arrow_hand_match(V3 tail, V3 origin, uint64_t frame, const LinkBody& body) {
    unsigned match=0;
    for (const NativeHands& h:native_hands) {
        if (h.frame==~0ull || h.frame>frame || frame-h.frame>2 || h.adult==body.child ||
            !(length(sub(h.root,v3(body.root)))<1.0f)) continue;
        for (int k=0;k<2;k++) {
            if (length(sub(h.wrist[k],tail))<.30f) match|=1;
            if (length(sub(h.wrist[k],origin))<.30f) match|=2;
        }
    }
    return match;
}
std::shared_ptr<const std::vector<float>> recent;   // (the latest VR palette: for Link's draws whose own upload was not taken)
std::chrono::steady_clock::time_point recent_at;
std::mutex bow_solution_mu;
float latest_bow[12]{};
int64_t latest_bow_at = 0;
// One body per replayed frame, shared by every pass recorded for it (BODY-03): the controllers and settings frozen at its first
// draw of Link. The body is the frame's own game frame's (tracking.inc link_body_for_frame, BODY-11): when its first draws
// came before the frame's scene camera was seen (shadow passes), they took a guess; the first draw after the camera is seen
// replaces a guess with the frame's own body, once (how 0). Draws of a frame after that all agree.
struct FrameBody { uint64_t frame = ~0ull; LinkBody body; bool own = false; };
FrameBody frame_bodies[8];
std::atomic<int64_t> frame_body_reposed{0};   // (frames whose guessed body was replaced by their own: the 10 s report)
bool frame_body(uint64_t frame, LinkBody& out) {
    LinkBody live;
    int how = 2;
    if (!link_body_for_frame(frame, live, &how)) return false;
    std::lock_guard lk(cache_mu);
    FrameBody& f = frame_bodies[frame % 8];
    if (f.frame != frame) f.frame = frame, f.body = live, f.own = how == 0;
    else if (!f.own && how == 0) {   // (the frame's camera seen since its first draws of Link: its own body from now on)
        if (f.body.pose_seq != live.pose_seq) f.body = live, frame_body_reposed.fetch_add(1, std::memory_order_relaxed);
        f.own = true;
    }
    out = f.body;
    return true;
}

float neck_error(const float* neck, const rig::Model& M, V3 origin, V3 root);
// cache_mu is held. Equipment may precede the body draw: establish ONE current-frame rig before attaching either item.
Entry* canonical_rig(uint64_t frame, const LinkBody& body) {
    Entry* source=nullptr;
    for (Entry& e:cache)
        if (e.out && e.adult==!body.child && e.frame<=frame && frame-e.frame<=2 &&
            neck_error(e.raw.data()+(e.adult?rig::adult.neck:rig::child.neck)*12,
                       e.adult?rig::adult:rig::child,e.origin,v3(body.root))<1.25f &&
            (!source || e.frame>source->frame || (e.frame==source->frame && e.used<source->used))) source=&e;
    if (!source) return nullptr;
    if (source->frame==frame && source->pose_seq==body.pose_seq) return source;
    Entry fresh=*source;
    auto out=std::make_shared<std::vector<float>>(fresh.raw);
    if (!pose(out->data(),fresh.adult?rig::adult:rig::child,body,fresh.origin)) return nullptr;
    fresh.out=out;fresh.frame=frame;fresh.pose_seq=body.pose_seq;fresh.used=++cache_clock;
    Entry* slot=&cache[0];
    for (Entry& e:cache) if (e.used<slot->used) slot=&e;
    *slot=std::move(fresh);
    return slot;
}

bool head_shape(uint64_t vb0) {
    switch (vb0) {
    // adult: Face, Eye_Inner, Eye_Outer, EyeShadow, Eyebrow, Eyelashes, Lower/Upper_Eyelid, Teeth, Hairbase_Back
    case 229440: case 18228: case 25284: case 10836: case 110880: case 40032: case 12276: case 18324: case 32544: case 2912496:
    // child: Hair_FrontA, C_Eyelid_U (face and lid), EyeShadow, Eyelashes x3, Eye_Inner/Outer, Eyebrow, Teeth
    case 1577752: case 207360: case 38400: case 7904: case 7704: case 12456: case 12672: case 14924: case 14420: case 94824:
    case 30688:
        return true;
    }
    return false;
}
