// Hookshot VR: the Boomerang's reticle in the headset while it aims (vr/items/boomerang/boomerang_host.c says where, in the
// game world: kVrBoomReach along the aiming line). The bow's reticle: its artwork (marker 2), its
// size row (Bow reticle size), facing the head.
//
// A piece of vr/headset/frame_loop.inc, included there after the bow's reticle, inside the frame's layers: it uses that
// code's locals (n_layers, layers, head) and its file's helpers. Not compiled on its own.
{
    static XrCompositionLayerQuad boomerang_quad{XR_TYPE_COMPOSITION_LAYER_QUAD};   // (lives until the frame is ended)
    float at[3];
    // (at most 4 layers before it: the eyes, two hands and the bow's; the radial and the menu come after, 7 in all)
    if (n_layers && n_layers <= 4 && opts.vr_mode == 1 && opts.vr_arms && !s.menu_visible.load() && boomerang_reticle(at) &&
        make_marker(2)) {
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
            boomerang_quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            boomerang_quad.space = s.local;
            boomerang_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            boomerang_quad.subImage.swapchain = ctl.marker[2].chain;
            boomerang_quad.subImage.imageRect.extent = {64, 64};
            boomerang_quad.pose.orientation = head.orientation;
            boomerang_quad.pose.position = {pose.pose.position.x + local[0], pose.pose.position.y + local[1],
                                            pose.pose.position.z + local[2]};
            const float size = .12f * std::clamp(opts.vr_bow_reticle_size, 50, 600) * .01f;
            boomerang_quad.size = {size, size};
            layers[n_layers++] = (const XrCompositionLayerBaseHeader*)&boomerang_quad;
        }
    }
}
