// Hookshot VR: Head rotations, recentering and matching a submitted camera to its tracked pose.
// Included by vr/headset/vk_vr.cpp in the order shown there; not a separate compilation unit.

// ---- the head ---------------------------------------------------------------------------------------------------------------

XrQuaternionf qmul(XrQuaternionf a, XrQuaternionf b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
void qrot(XrQuaternionf q, const float v[3], float out[3]) {
    // v' = v + 2w (q x v) + 2 q x (q x v)
    const float tx = 2 * (q.y * v[2] - q.z * v[1]), ty = 2 * (q.z * v[0] - q.x * v[2]), tz = 2 * (q.x * v[1] - q.y * v[0]);
    out[0] = v[0] + q.w * tx + (q.y * tz - q.z * ty);
    out[1] = v[1] + q.w * ty + (q.z * tx - q.x * tz);
    out[2] = v[2] + q.w * tz + (q.x * ty - q.y * tx);
}
XrQuaternionf qinv(XrQuaternionf q) { return {-q.x, -q.y, -q.z, q.w}; }
/// the yaw part of q (about +y), inverted (as the mod's yaw_inverse)
XrQuaternionf yaw_inverse(XrQuaternionf q) {
    const float fwd[3] = {0, 0, -1};
    float f[3];
    qrot(q, fwd, f);
    const float yaw = std::atan2(-f[0], -f[2]);
    return {0, std::sin(-yaw / 2), 0, std::cos(-yaw / 2)};
}

void publish_pose(const XrPosef& head) {
    std::lock_guard lk(s.pose_mu);
    const int recenter = opts.vr_recenter;
    if (!s.have_ref || recenter != s.recenter_seen) {
        s.ref = head, s.have_ref = true, s.recenter_seen = recenter;
        logf("vr: centred");
    }
    Pose& p = s.poses[++s.pose_seq % kPoses];
    p = Pose{};
    p.seq = s.pose_seq;
    p.pose = head;
}

/// The pose the game drew a camera with: the drawn view nearest to the camera's view (its rows from vp = proj * view); seq:
/// that pose's number (BODY-11: which game frame's body a replayed frame belongs to)
bool drawn_pose(const float vp[16], const float proj[16], XrPosef* out, float* view = nullptr, uint32_t* seq = nullptr) {
    if (proj[14] != -1.0f || !(proj[0] > 0) || !(proj[5] > 0)) return false;
    float v[12];
    for (int k = 0; k < 4; k++) {
        v[8 + k] = -vp[12 + k];                                 // (the w row of proj is 0 0 -1 0)
        v[0 + k] = (vp[0 + k] - proj[2] * v[8 + k]) / proj[0];  // (jitter in proj[2], proj[6])
        v[4 + k] = (vp[4 + k] - proj[6] * v[8 + k]) / proj[5];
    }
    std::lock_guard lk(s.pose_mu);
    float best = -1e9f;
    const Pose* found = nullptr;
    for (const Pose& p : s.poses) {
        if (!p.drawn) continue;
        float d = 0;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) d += p.view[r * 4 + c] * v[r * 4 + c];
        // (the translations too, a little: two poses with the same turn, the head moved between)
        float t = 0;
        for (int r = 0; r < 3; r++) t += std::fabs(p.view[r * 4 + 3] - v[r * 4 + 3]);
        d -= t * 0.1f;
        if (d > best) best = d, found = &p;
    }
    if (!found || best < 2.9f) return false;   // (3 for the same turn)
    *out = found->pose;
    if (view) std::memcpy(view, found->view, sizeof found->view);
    if (seq) *seq = found->seq;
    return true;
}
