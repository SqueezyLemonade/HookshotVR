// Hookshot VR: the Fairy Slingshot in the VR hand (first person): which draws are it, and where the hand holds it.
//
// A piece of vr/body/vk_vr_body.cpp, included there in a fixed order (before the bow: bow_pose.inc uses it): it uses the
// pieces before it. Not compiled on its own.
// The game's slingshot is a bow to the game (its BowParam: mounted on Weapon_L, the shot chasing the pouch bone, fired
// through the bow's own shoot), and the VR slingshot is the VR bow in another shape: bow_pose takes the slingshot's fork
// for the bow's handle and its pouch for the string's centre. So the grab, the draw, the firing let-go, the headset's
// reticle and the shot's redirect (vr/items/bow/bow_host.c) are the bow's, and the shot flies from the pouch through the
// fork, as from a real slingshot: from the fork, along pouch -> fork. Data: vk_vr_slingshot.h (vr/tools/generate/slingshot.py).
// The draw (body.sling): the game side says the bow that is out is the slingshot (vr/camera/view_host.c, rx_vr_bow_active 2).
bool sling_model(uint64_t vb0, uint64_t bytes) {
    if (bytes < rig::sling_matrices * 48ull) return false;
    for (uint64_t s : rig::sling_shapes)
        if (s == vb0) return true;
    return false;
}
// The slingshot's place as the VR hand that holds it (body.bow_hand) holds it: the VR wrist x the game's own Weapon_L socket
// in the left, its mirror image in the right (as the hookshot, held_hookshot.inc). Takes cache_mu.
bool sling_place(uint64_t frame, const LinkBody& body, Aff* out) {
    const int side = body.bow_hand;
    if (side < 0 || side > 1 || !body.hand[side].valid) return false;
    std::lock_guard lk(cache_mu);
    const Entry* rig_now = canonical_rig(frame, body);
    if (!rig_now) return false;
    const rig::Model& M = rig_now->adult ? rig::adult : rig::child;
    const Aff socket = aff(side ? (rig_now->adult ? rig::sling_right_adult : rig::sling_right_child)
                                : (rig_now->adult ? rig::sling_left_adult : rig::sling_left_child));
    *out = mul(shifted(aff(rig_now->out->data() + M.arm[side].wrist * 12), rig_now->origin), socket);
    if (!std::isfinite(out->t.x + out->t.y + out->t.z)) return false;
    return true;
}
V3 sling_point(const Aff& a, const float (&p)[3]) { return add(apply(a.r, v3(p)), a.t); }
