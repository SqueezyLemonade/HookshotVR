// Hookshot VR: Link's VR skin palette for the draw being recorded (vr_bones), bound in place of the game's.
//
// A piece of gpu/vk/vk_draw.cpp (the draw replay), included there at its VR HOOK: it uses that file's types and helpers
// (Targets, NvnState, Program, stream_copy, logf, ...). Not compiled on its own.
// (OoT VR, body v2) Link's VR skin palette for the draw being recorded: made by vk_vr_body.cpp once per palette upload.
struct VrBoneOverride { uint64_t addr = 0; std::shared_ptr<const std::vector<float>> palette; };
thread_local VrBoneOverride vr_bones;

static BufferRef vr_bone_buffer(uint64_t addr, uint64_t bytes) {
    if (!vr_bones.palette || vr_bones.addr != addr || bytes > vr_bones.palette->size() * sizeof(float)) return {};
    return stream_copy(vr_bones.palette->data(), bytes);
}

// (OoT VR) A held arrow's effect draw being recorded (vr_arrow_fx, set by skin_draw.inc vr_arrow_effect): its particle
// camera's buffer (vertex UBO 10, "C13": vr/items/bow/arrow_effect_math.h), what its check found, and the move from the
// game's arrow to the VR arrow. Bound, that camera's copy gets the move, so the whole draw, particles too, is drawn on the
// VR arrow; and its view depth stretched, so the flame's own near-camera fade lets it show half a metre from the eyes.
struct VrArrowFx { uint64_t camera = 0; float move[12], stretch = vr::arrow_fx::kNearFadeStretch; vr::arrow_fx::Camera info; };
thread_local VrArrowFx vr_arrow_fx;

static bool vr_arrow_fx_camera(float* c, uint64_t bytes) {
    if (!vr::arrow_fx::transform_camera(c, bytes, vr_arrow_fx.move, vr_arrow_fx.info)) return false;
    vr::arrow_fx::stretch_view_depth(c, bytes, vr_arrow_fx.stretch);   // (its opacity: Elemental arrow effects)
    return true;
}
