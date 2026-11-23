// Hookshot VR: Link's first-person body, and the items in his hands.
//
// The game animates Link as usual. When the renderer replays a draw of his, his skin palette (one 3x4 matrix per skinned
// bone) is swapped for a VR pose, in a copy: the game's memory is never written. One palette upload gives one pose, shared by
// every pass that draws it (colour, depth, shadows), so they all agree. The pieces, in the order they build on each other:
//
//   vr/body/math.inc                 vectors, rotations, palette matrices
//   vr/body/arms_and_pose.inc        the pose: torso upright under the headset, legs from the game, head folded away,
//                                    arms reaching the controllers
//   vr/body/gait.inc                 (included by it) walking or running, legs that step the way Link travels
//   vr/body/rig_cache.inc            recognising Link's uploads; one pose per game frame
//   vr/body/skin.inc                 his draws get the pose; first-person head and hair left out
//   vr/items/bow/bow_pose.inc        the bow in the VR hand, its string
//   vr/items/bow/held_arrow.inc      the nocked arrow
//   vr/items/gear/held_gear.inc      swords, sheaths, shields: on the body or in a hand
//   vr/items/gear/tunics.inc         Goron and Zora tunics
//   vr/items/ocarina/held_ocarina.inc the ocarina and its holder on the VR belt
//   vr/items/gear/equipment_skin.inc which item an equipment draw is, and where it goes
//   vr/items/bow/bow_solution.inc    the bow solution for the game side
//
// The skeleton data (bone names, palette slots, bind poses) is generated from the player's own game files at build time
// by vr/tools/generate (vk_vr_rig_data.h, vk_vr_armor_bind.h, vk_vr_tunic_rig.h, vk_vr_sword_socket.h, vk_vr_bow_rest.h,
// vk_vr_worn_items.h, vk_vr_gait.h, vk_vr_arrows.h, vk_vr_ocarina.h).
#include "vk_vr.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#include "vk_internal.h"
#include "vk_menu.h"
#include "vk_vr_rig_data.h"
#include "vk_vr_armor_bind.h"
#include "vk_vr_tunic_rig.h"
#include "vk_vr_sword_socket.h"
#include "vk_vr_bow_rest.h"
#include "vk_vr_worn_items.h"
#include "vk_vr_gait.h"
#include "vk_vr_arrows.h"
#include "vk_vr_hookshot.h"
#include "vk_vr_slingshot.h"
#include "vk_vr_ocarina.h"
#include "vk_vr_boomerang.h"
#include "vk_vr_bottle.h"
#include "vk_vr_shield_grip.h"   // (the shields' grips, moved for the left hand: held_gear.inc, shield_grip.inc)
#include "vk_vr_bomb_flower.h"   // (a Bomb Flower's fruit in a VR hand: equipment_skin.inc, arms_and_pose.inc)
#include "../items/bow/arrow_effect_math.h"
#include "../items/bottle/bottle_pose.h"   // (where a VR hand holds a bottle: arms_and_pose.inc and held_bottle.inc)
#include "vk_texture.h"   // (texture_frame_no: the frame the arrow's effects are moved for, held_arrow.inc)

namespace mkwvk::vr {
namespace {
#include "math.inc"
#include "arms_and_pose.inc"
#include "rig_cache.inc"
} // namespace
#include "skin.inc"
namespace {
#include "../items/slingshot/sling_rig.inc"
#include "../items/bow/bow_pose.inc"
#include "../items/bow/held_arrow.inc"
#include "../items/bottle/held_bottle.inc"   // (a bottle in a VR hand: held_gear.inc hands it its draws)
#include "../items/gear/held_gear.inc"
#include "../items/hookshot/held_hookshot.inc"
#include "../items/boomerang/held_boomerang.inc"
#include "../items/slingshot/held_slingshot.inc"
#include "../items/item_get/item_get.inc"   // (an item get's item in front of the headset)
#include "../items/ocarina/held_ocarina.inc"
#include "../items/gear/tunics.inc"
} // namespace

std::shared_ptr<const std::vector<float>> link_skin(uint64_t bones, uint64_t bytes, uint64_t object, uint64_t object_size,
                                                    uint64_t vb0, bool* hide, uint64_t frame) {
    auto out = link_skin_body(bones, bytes, object, object_size, vb0, hide, frame);
    LinkBody body;
    if (out && frame_body(frame, body)) bow_pose(frame, body);   // (the frame's bow solution, whether or not the bow draws)
    return out;
}

// The held hookshot's barrel as last drawn, for the headset side's aim line (vr/items/hookshot/held_hookshot.inc)
bool hook_barrel(float out[6]) { return held_hook_barrel(out); }
bool hook_effects_live() { return held_hook_effects_live(); }
bool hook_chain_draw(uint64_t vb0) { return held_hookchain_draw(vb0); }
bool ocarina_draw(uint64_t vb0) { return held_ocarina_draw(vb0); }
bool bottle_draw(uint64_t vb0) { return held_bottle_draw(vb0); }
bool hook_effect_move(const float effect[16], const float camera[3], float move[12]) {
    return held_hook_effect_move(effect, camera, move);
}
// The held shield's grip, moved across its back for the left VR hand (vr/items/gear/shield_grip.inc)
bool shield_grip_shape(uint64_t vb0, uint32_t stride) {
    const rig::ShieldGrip* g = shield_grip_of(vb0);
    return g && g->stride == stride;
}
bool shield_grip_make(uint64_t vb0, const uint8_t* game, uint64_t n, uint8_t* out) {
    return shield_grip_edit(vb0, game, n, out);
}
bool shield_grip_drawn(uint64_t vb0, const void* palette) { return shield_grip_of(vb0) && grip_palette_listed(palette); }
// The held sword's effects (the spin's charge glow), drawn on the VR sword (vr/items/gear/held_gear.inc)
bool sword_effects_live() { return held_sword_effects_live(); }
bool sword_effect_move(const float effect[16], const float camera[3], float move[12]) {
    return held_sword_effect_move(effect, camera, move);
}
// The held Deku Stick's flame, drawn on the VR stick (vr/items/deku_stick/held_stick.inc)
bool stick_effects_live() { return held_stick_effects_live(); }
bool stick_effect_move(const float effect[16], const float camera[3], float move[12]) {
    return held_stick_effect_move(effect, camera, move);
}

// The way Link's first-person torso faces (a world yaw, as the game's facing takes it: atan2(x, z)), for the game side's
// untargeted shield (vr/items/shield/guard_host.c): it follows the head only past a dead zone, so looking aside does not turn
// the guard. Fresh for 200 ms after the last pose.
bool body_heading(float* yaw) {
    std::lock_guard lk(cache_mu);
    if (!motion.have || std::chrono::steady_clock::now() - motion.at > std::chrono::milliseconds(200) ||
        !(length(motion.forward) > .5f)) return false;
    *yaw = std::atan2(motion.forward.x, motion.forward.z);
    return true;
}

// ---- (BODY-12) what became of Link's first-person draws ------------------------------------------------------------------
// The player saw his first-person body blink while walking. Three causes leave three different traces, and these counts tell
// them apart in the next headset log: a frame where the game drew none of Link's colour but still his depth or shadow (the
// game left the colour out: a fade or a cull of the main view); a frame where his palette was refused (drawn in the game's
// pose); a frame with no draw of Link at all between frames with them. Each such frame after one with his colour gets a
// "Link blink" line (at most one a second, with how many were not said); every 10 s a summary (body_draw_report).
namespace {
struct DrawTally { uint64_t frame = 0; int counts[(int)LinkDraw::Count][2]{}; };   // ([what][colour pass])
std::mutex tally_mu;
DrawTally tally;                     // the frame being counted (tally_mu)
bool tally_prev_colour = false;      // the frame before it drew Link's posed colour
int64_t report_counts[(int)LinkDraw::Count][2]{};
int64_t report_frames = 0, report_missing = 0, report_runs = 0, report_longest = 0, report_run = 0, report_whole = 0;
int64_t blink_unsaid = 0;
std::chrono::steady_clock::time_point blink_said{};
constexpr uint64_t kShortGapFrames = 8;   // (frames with no draw of Link between two with them that count as a blink)

bool tally_colour(const DrawTally& t) {
    return t.counts[(int)LinkDraw::Posed][1] + t.counts[(int)LinkDraw::RecentPose][1] > 0;
}
bool tally_any(const DrawTally& t) {
    for (const auto& c : t.counts) if (c[0] + c[1]) return true;
    return false;
}
void blink_line(const char* what, const DrawTally& t, uint64_t gap) {   // (tally_mu held)
    constexpr auto kBlinkLineGap = std::chrono::seconds(1);
    const auto now = std::chrono::steady_clock::now();
    if (now - blink_said < kBlinkLineGap) { blink_unsaid++; return; }
    blink_said = now;
    const auto n = [&](LinkDraw w, int colour) { return t.counts[(int)w][colour]; };
    logf("vr: Link blink: frame %llu %s (posed depth-only draws %d, refused palettes %d+%d in colour, game-pose draws near "
         "him %d+%d in colour, shadow draws left out %d, head, hair and worn items left out %d; frames with no draw of him before it %llu; "
         "%lld such lines not said)",
         (unsigned long long)t.frame, what, n(LinkDraw::Posed, 0) + n(LinkDraw::RecentPose, 0), n(LinkDraw::Refused, 0),
         n(LinkDraw::Refused, 1), n(LinkDraw::NearNative, 0), n(LinkDraw::NearNative, 1), n(LinkDraw::ShadowHidden, 0),
         n(LinkDraw::HeadHidden, 0) + n(LinkDraw::HeadHidden, 1), (unsigned long long)gap, (long long)blink_unsaid);
    blink_unsaid = 0;
}
void finish_tally(uint64_t next) {   // (tally_mu held) the frame being counted is over: next is the next frame with a draw
    if (tally.frame && tally_any(tally)) {
        for (int w = 0; w < (int)LinkDraw::Count; w++)
            for (int c = 0; c < 2; c++) report_counts[w][c] += tally.counts[w][c];
        report_frames++;
        const bool colour = tally_colour(tally);
        if (!colour) {
            report_missing++;
            if (!report_run++) report_runs++;
            report_longest = std::max(report_longest, report_run);
            if (tally_prev_colour)
                blink_line(tally.counts[(int)LinkDraw::Refused][1] ? "drew his colour in the game's pose (his palette refused)" :
                           tally.counts[(int)LinkDraw::NearNative][1] ? "drew his colour in the game's pose (not recognised)" :
                           "drew none of his colour, only depth or shadow (the game left it out: a fade or a cull)", tally, 0);
        } else report_run = 0;
        tally_prev_colour = colour;
    }
    tally = DrawTally{};
    tally.frame = next;
}
} // namespace

void note_link_draw(LinkDraw what, bool colour, uint64_t frame) {
    if (what >= LinkDraw::Count) return;
    std::lock_guard lk(tally_mu);
    if (frame < tally.frame) return;   // (a late draw of a frame already counted)
    if (frame != tally.frame) {
        const uint64_t last = tally.frame;
        const bool had = tally_any(tally);
        finish_tally(frame);
        if (had && frame - last - 1 > 0 && frame - last - 1 <= kShortGapFrames) {   // (frames with no draw of Link at all)
            report_whole += (int64_t)(frame - last - 1);
            if (tally_prev_colour) {
                DrawTally empty;
                empty.frame = last + 1;
                blink_line("had no draw of Link at all", empty, frame - last - 1);
            }
        }
    }
    tally.counts[(int)what][colour ? 1 : 0]++;
}

void body_draw_report() {
    std::lock_guard lk(tally_mu);
    if (!report_frames) return;
    const auto n = [&](LinkDraw w, int c) { return (long long)report_counts[(int)w][c]; };
    logf("vr: Link's draws (10 s): %lld frames with his draws, his posed colour missing in %lld (%lld runs, the longest %lld "
         "frames), %lld frames with no draw of him between them; posed %lld colour + %lld depth-only, recent pose %lld + %lld, "
         "equipment %lld + %lld, refused palettes %lld + %lld, game-pose draws near him %lld + %lld, shadow draws left out "
         "%lld, head, hair and worn items left out %lld + %lld",
         (long long)report_frames, (long long)report_missing, (long long)report_runs, (long long)report_longest,
         (long long)report_whole, n(LinkDraw::Posed, 1), n(LinkDraw::Posed, 0), n(LinkDraw::RecentPose, 1),
         n(LinkDraw::RecentPose, 0), n(LinkDraw::Equipment, 1), n(LinkDraw::Equipment, 0), n(LinkDraw::Refused, 1),
         n(LinkDraw::Refused, 0), n(LinkDraw::NearNative, 1), n(LinkDraw::NearNative, 0), n(LinkDraw::ShadowHidden, 0),
         n(LinkDraw::HeadHidden, 1), n(LinkDraw::HeadHidden, 0));
    if (const int64_t r = frame_body_reposed.exchange(0)) logf("vr:   %lld frames re-posed with their own body once their camera was seen", (long long)r);
    std::memset(report_counts, 0, sizeof report_counts);
    report_frames = report_missing = report_runs = report_longest = report_whole = 0;
}

#include "../items/gear/equipment_skin.inc"
#include "../items/bow/bow_solution.inc"
} // namespace mkwvk::vr
