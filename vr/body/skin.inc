// Hookshot VR: Link's skin: the VR pose for his own draws, with first-person head and hair left out.
//
// A piece of vr/body/vk_vr_body.cpp, included there in a fixed order: it uses the pieces before it. Not compiled on its own.
bool head_draw(uint64_t vb0) { return head_shape(vb0); }

bool hair_draw(uint64_t vb0, uint32_t count) {
    // Hairbase_Back (adult) and Hair_FrontA (child): their vertex buffer, or a draw of one of their index counts (LOD 0-2)
    return vb0 == 2912496 || vb0 == 1577752 || count == 129477 || count == 47991 || count == 25434 || count == 57774 ||
           count == 31608 || count == 18507;
}

// (BODY-12) the last link_skin call on this thread refused a palette that is Link's (its joints hold together) but not the
// player's where he is, or could not read it: skin_draw.inc counts those draws apart from other actors' palettes
thread_local bool palette_refused = false;
bool link_skin_refused() { return palette_refused; }

static std::shared_ptr<const std::vector<float>> link_skin_body(uint64_t bones, uint64_t bytes, uint64_t object,
                                                                uint64_t object_size, uint64_t vb0, bool* hide, uint64_t frame) {
    *hide = false;
    palette_refused = false;
    if (!bones || bytes % 48 || bytes < 48 * (uint64_t)rig::child.slots || bytes > 48 * (uint64_t)rig::kSlots) return nullptr;
    LinkBody body;
    if (!frame_body(frame, body)) return nullptr;
    const uint32_t matrices = (uint32_t)(bytes / 48);
    const V3 root = v3(body.root);

    std::vector<float> raw(bytes / sizeof(float));
    if (!ub_read(bones, raw.data(), bytes)) {
        static std::atomic<int> told{0};
        if (told.fetch_add(1) < 8) logf("vr: Link-sized palette not taken (it could not be read: %#llx, %llu bytes)",
                                        (unsigned long long)bones, (unsigned long long)bytes);
        palette_refused = true;
        return nullptr;
    }

    // Which of Link's two models: the one whose joints hold together. Each pair is a joint seen from a bone and from its
    // parent; with the wrong model's table those land far apart. (Body v2's first builds compared the neck with the root:
    // running moved adult Link's neck enough that the child table won, which tore the mesh apart while moving.)
    const auto joint_error = [&](const rig::Model& M) {
        if (matrices < (uint32_t)M.slots) return 1e9f;
        float e = 0;
        for (const rig::Pair& q : M.pairs)
            e += length(sub(point(raw.data() + q.child * 12, v3(q.joint)), point(raw.data() + q.parent * 12, v3(q.joint))));
        return e;
    };
    const float ea = joint_error(rig::adult), ec = joint_error(rig::child);
    const rig::Model* model = ea <= ec ? &rig::adult : &rig::child;
    // (why a palette that looked like Link's was not taken: logged now and then, so a scene where his body keeps the game's
    // pose shows the cause. At most a line every 2 s, with how many were refused since the last line; the first builds
    // stopped after 40 lines, so a long run's end, where the blinking was, said nothing)
    const auto reject = [&](const char* why, float a, float b) {
        constexpr int64_t kRejectLineMs = 2000;
        static std::atomic<int64_t> last{0}, since{0};
        since.fetch_add(1, std::memory_order_relaxed);
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t was = last.load();
        if (now - was > kRejectLineMs && last.compare_exchange_strong(was, now)) {
            logf("vr: Link-sized palette not taken (%s %.3f %.3f): joint error adult %.3f child %.3f, root %.2f %.2f %.2f (%lld "
                 "refused since the last line)", why, a, b, ea, ec, root.x, root.y, root.z, (long long)since.exchange(0));
        }
    };
    if (std::min(ea, ec) > .08f) {   // (not a Link palette at all)
        reject("joints", ea, ec);
        return nullptr;
    }

    // And is it the player's Link (not a copy, a reflection or another actor): his neck near the reported position
    V3 origins[2];
    int n_origins = 0;
    float o[3];
    if (object && object_size >= 0x4c && ub_read(object + 0x40, o, sizeof o) && std::isfinite(o[0] + o[1] + o[2]))
        origins[n_origins++] = v3(o);
    std::unique_lock lk(cache_mu);
    if (have_origin) origins[n_origins++] = last_origin;
    V3 origin{};
    float best = 1.25f;
    bool found = false;
    for (int i = 0; i < n_origins; i++) {
        const float e = neck_error(raw.data() + model->neck * 12, *model, origins[i], root);
        if (e < best) best = e, origin = origins[i], found = true;
    }
    if (!found) {
        const float e0 = n_origins > 0 ? neck_error(raw.data() + model->neck * 12, *model, origins[0], root) : -1;
        const float e1 = n_origins > 1 ? neck_error(raw.data() + model->neck * 12, *model, origins[1], root) : -1;
        lk.unlock();
        reject("neck/root, per origin", e0, e1);
        palette_refused = true;   // (a palette of Link's, joints and all, not where the player's Link is: counted, BODY-12)
        return nullptr;
    }

    remember_native_hands(*model,raw,origin,root,frame); // BEFORE VR-cache reuse, never from a synthetic canonical entry
    std::shared_ptr<const std::vector<float>> canonical;
    for (Entry& e : cache)
        if (e.out && e.frame == frame && e.pose_seq == body.pose_seq && e.adult == (model == &rig::adult) &&
            length(sub(e.origin, origin)) < 1e-6f && e.out->size() == raw.size()) {
            // First matching player rig wins for the entire frame. Shadow/depth uploads may carry different animation
            // snapshots: re-posing each one changes the shoulder/IK clamp even though the controllers did not move.
            if (e.raw != raw) { canonical=e.out; continue; }
            e.used = ++cache_clock;
            *hide = head_shape(vb0);
            recent = e.out, recent_at = std::chrono::steady_clock::now();
            return e.out;
        }
    // Keep each original animation upload for attachment matching, but never solve a second VR pose for this frame.
    auto out = std::make_shared<std::vector<float>>(canonical ? *canonical : raw);
    if (!canonical && !pose(out->data(), *model, body, origin)) return nullptr;
    last_origin = origin, have_origin = true;
    Entry* slot = &cache[0];
    for (Entry& e : cache)
        if (e.used < slot->used) slot = &e;
    slot->addr = bones, slot->raw = std::move(raw), slot->out = out, slot->used = ++cache_clock;
    slot->origin = origin, slot->adult = model == &rig::adult;
    slot->frame = frame, slot->pose_seq = body.pose_seq;
    static std::atomic<bool> told{false};
    if (!told.exchange(true))
        logf("vr: tracked Link body (v2) matched: %s model, %u palette matrices, neck error %.3f m (joint error adult %.3f, child %.3f)",
             model->name, matrices, best, ea, ec);
    *hide = head_shape(vb0);
    recent = out, recent_at = std::chrono::steady_clock::now();
    return out;
}

std::shared_ptr<const std::vector<float>> link_skin_recent(uint64_t vb0, bool* hide, uint64_t frame) {
    std::lock_guard lk(cache_mu);
    *hide = head_shape(vb0);
    const Entry* best = nullptr;
    for (const Entry& e : cache) if (e.out && e.frame == frame && (!best || e.used > best->used)) best = &e;
    return best ? best->out : nullptr;
}
