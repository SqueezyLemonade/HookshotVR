// Hookshot VR: the third-person aim's reticle in the headset (vr/items/aim/third_person_aim_host.c says where, in the game
// world: where the game's own aim says the shot lands). The bow's artwork (marker 2), facing the head, the same size to the
// eye at any distance (the first-person reticle's at 10 m, times the Aim reticle size row). The Aim reticle row (VR tab, Bow;
// vr_bow_preview) turns it off, and the game's flat reticle is shown again then. Not in the Window view (the game through a
// frame in the room: a mark in the world would not line up with it).
//
// A piece of vr/headset/frame_loop.inc, included there after the Boomerang's reticle, inside the frame's layers: it uses that
// code's locals (n_layers, layers, head) and its file's helpers. Not compiled on its own.
{
    static XrCompositionLayerQuad aim_quad{XR_TYPE_COMPOSITION_LAYER_QUAD};   // (lives until the frame is ended)
    float at[3];
    // (at most 4 layers before it in third person: the eyes, two hands' markers, one other reticle)
    if (n_layers && n_layers <= 4 && opts.vr_mode == 0 && opts.vr_bow_preview && !window_view() && !s.menu_visible.load() && third_person_aim(at) && make_marker(2)) {
        Pose pose{};
        {   // (the newest pose the game drew a frame with: its view takes the game world to its camera)
            std::lock_guard lk(s.pose_mu);
            for (int back = 0; back < kPoses; back++)
                if (const Pose& p = s.poses[(s.pose_seq - (uint32_t)back) % kPoses]; p.drawn) {
                    pose = p;
                    break;
                }
        }
        if (pose.drawn) {
            float camera[3], local[3];
            for (int row = 0; row < 3; row++) {
                camera[row] = pose.view[row * 4 + 3];
                for (int j = 0; j < 3; j++) camera[row] += pose.view[row * 4 + j] * at[j];
            }
            qrot(pose.pose.orientation, camera, local);
            const XrVector3f where{pose.pose.position.x + local[0], pose.pose.position.y + local[1], pose.pose.position.z + local[2]};
            const float dx = where.x - head.position.x, dy = where.y - head.position.y, dz = where.z - head.position.z;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (std::isfinite(distance) && distance > .05f) {
                aim_quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                aim_quad.space = s.local;
                aim_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                aim_quad.subImage.swapchain = ctl.marker[2].chain;
                aim_quad.subImage.imageRect.extent = {64, 64};
                aim_quad.pose.orientation = head.orientation;
                aim_quad.pose.position = where;
                const float size = .012f * distance * std::clamp(opts.vr_bow_reticle_size, 50, 600) * .01f;
                aim_quad.size = {size, size};
                layers[n_layers++] = (const XrCompositionLayerBaseHeader*)&aim_quad;
            }
        }
    }
}
