// (OoT VR) The headset: OpenXR on the back end's own Vulkan device (vk_vr.cpp).
//
// With the VR row on (vr= in hookshot_options.ini, read at the start, or HOOKSHOT_VR=1), the OpenXR runtime (WiVRn) is asked
// before Vulkan starts which extensions it needs and which GPU it drives; the device is made with those and a second queue of
// the game's family for the headset. Each game frame, the scene's depth is copied once the world is in it (at the gameplay
// upscale's dispatch) and the finished picture at the present, into a ring of slots. A thread of its own runs the headset at its
// rate: it gives the host the head's pose (the host turns the game's camera with it: host/hos_main.c hos_oot_vr_view) and makes
// both eyes from the newest slot, each shifted by its own depth (shaders/vr_warp.comp), corrected for where the head is now.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#ifdef _WIN32   // (as gpu/vk/vk_loader.h: vulkan.h with the Win32 surface, whichever of the two a file includes first)
#define VK_USE_PLATFORM_WIN32_KHR
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef near   // (windows.h's empty 16-bit pointer macros: the VR code's near and far are distances)
#undef far
#endif
#include <vulkan/vulkan.h>

namespace mkwvk {
struct Texture;
namespace vr {

/// VR was asked for and the runtime answered (decided before the Vulkan instance: instance_extensions)
bool wanted();
/// a row's value in hookshot_options.ini as the game starts (the menu loads its file later), def without one
int option_at_start(const char* key, int def);
/// Before vkCreateInstance: the runtime's instance extensions added (false: no VR this run)
void instance_extensions(std::vector<const char*>& exts);
/// After the physical device is picked: whether the runtime drives this one (else no VR this run)
bool device_matches(VkPhysicalDevice physical);
/// The device's extensions: the runtime's added, when the GPU has them
void device_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& have);
/// How many queues of the game's family to make: 2 with VR (the headset's is index 1) when the family has them
uint32_t queue_count(uint32_t family_queues);
/// The device is made (with the queues queue_count asked for): the headset thread starts
void device_created();
/// The headset's queue (index 1 of the game's family): held by the headset thread while it and the OpenXR runtime submit to
/// it (a frame, from xrBeginFrame to xrEndFrame), and by the renderer's vkDeviceWaitIdle (vk_replay.cpp device_wait_idle_locked),
/// which needs every queue of the device to itself
std::mutex& queue_mutex();
/// The headset is running: the copies below are wanted
bool active();
/// The game quits (vk_main.cpp mkwvk_close_asked): the session ended and destroyed, then the instance, 2 s at most
void close();
/// (vr/headset/rebar_hint.cpp) VR ran out of graphics memory or lost the device (what: which, for the log): on a discrete card
/// without Resizable BAR, one log line naming it as the cause and the fix, once a run. Nothing otherwise
void rebar_hint(const char* what);
bool rebar_hint_wanted(bool headset, int device_type, bool rebar);

/// A private transparent target for one Hookshot menu frame. The caller clears and renders it in TRANSFER_DST layout.
VkImage menu_target(uint32_t width, uint32_t height, VkImageView* view);
/// The target returned above is complete in TRANSFER_DST layout; publish it to the headset's native OpenXR quad layer.
void menu_ready(VkCommandBuffer cmd);
/// The Hookshot menu closed; stop submitting its OpenXR quad.
void menu_hidden();

// ---- (OoT VR) the VR menu: the mod's own settings, a panel floating in the world (vr/menu/vr_menu.inc draws it on the present's
// thread, vr/menu/vr_menu_layer.inc shows it in the headset). Held left Menu button or Y: host_api.inc hookshot_vr_menu ----
/// Open: the game side gets the controllers' buttons, triggers and sticks let go meanwhile (host_api.inc)
bool panel_open();
void panel_set_open(bool open);
/// What opens and closes it on this controller, by the buttons' own names: "Hold left menu or Y"; value: as the Menu button
/// row's value, "Menu or Y" (vr/input/openxr_controllers.inc name_menu_buttons)
std::string menu_hint(bool value);
/// Opens or closes it; false when it cannot open now (Hookshot's own menu is open)
bool panel_toggle();
/// (the Window view, vr/headset/window_view.inc) whether its Move mode changed the window's options since last asked: the
/// VR menu saves them then
bool window_options_changed();
/// A private transparent target for one VR menu frame, as menu_target; then panel_ready once drawn, or panel_hidden
VkImage panel_target(uint32_t width, uint32_t height, VkImageView* view);
void panel_ready(VkCommandBuffer cmd);
void panel_hidden();
/// A controller at the panel: where its pointer meets it (u right, v down, 0..1 across the image), its trigger, buttons
/// (HOOKSHOT_VR_BTN_*) and stick
struct PanelHand {
    bool valid = false, pointing = false;
    float u = 0, v = 0, distance = 0;
    float trigger = 0;
    uint32_t buttons = 0;
    float stick[2]{};
};
/// Both controllers at the panel (false: no controllers)
bool panel_hands(PanelHand out[2]);
/// The VR menu's frame art, straight RGBA (false: not in this build: assets/vr/panel_frame.png was not there)
bool panel_frame_pixels(uint32_t* w, uint32_t* h, std::vector<unsigned char>& out);
/// A game icon's straight RGBA pixels, size x size (false: not in this build): the VR menu's item sections
bool icon_pixels(const char* name, int* size, std::vector<unsigned char>& out);
/// (vr/menu/vr_menu.inc, the present: gpu/vk/vk_replay.cpp) the VR menu drawn for the headset, when it is open and changed
void panel_record(VkCommandBuffer cmd);
/// (headless shots) the VR menu over a shot's image (TRANSFER_DST before and after), when it is open
void panel_record_image(VkCommandBuffer cmd, VkImage image, VkImageView view, uint32_t w, uint32_t h);

/// A draw into the scene's colour (vk_draw.cpp, beside the temporal upscalers' note): its camera and depth
void note_scene_draw(const Texture* color0, uint32_t width, uint32_t height, Texture* depth, uint64_t camera,
                     uint64_t camera_size);
/// The gameplay upscale's dispatch: the world is in the depth now. Its copy is recorded into cmd
void scene_done(VkCommandBuffer cmd);

// ---- (OoT VR) true stereo: both eyes in one multiview draw (vk_vr_eyes.cpp) ----
// With 3D "True stereo" chosen as the game starts (3d=4), or RX_VR_EYES_TEST=<folder> (a test without the headset: dumps of
// both eyes' scene there), the scene's large render targets are made with a second array layer, the scene's passes draw both
// layers at once (viewMask 3), and the draws with the scene's camera move each view's clip x by its eye
// (vk_vr_spirv_eyes.h). Layer 0 is the left eye: the game's own picture. Layer 1, the right eye, is copied out at the
// scene's end.
/// True stereo this run (decided once, before the first image)
bool eyes_wanted();
/// This frame's passes draw both eyes (wanted, and the eyes' shift known from the scene's projection)
bool eyes_frame();
/// A render target made as wanted for both eyes: its image has a second layer (vk_texture.cpp)
bool eyes_target(uint32_t type, uint32_t width, uint32_t height, uint32_t layers, uint32_t levels, uint32_t samples);
/// The two-layer attachment view of a target made for both eyes (VK_NULL_HANDLE: not possible)
VkImageView eyes_view(Texture* t, VkFormat format, uint32_t level);
/// One layer's attachment view of such a target (a split draw's layer 1: vk_draw.cpp EYES_SPLIT)
VkImageView eyes_layer_view(Texture* t, VkFormat format, uint32_t level, uint32_t layer);
/// Set while texture_view makes layer 1's view of a two-layer texture (vk_texture.cpp: its base layer moved to 1)
extern thread_local int eyes_layer_bias;
/// (true stereo) set while a view of a texture's right-eye twin is made (vk_texture.cpp texture_view, texture_vr_twin)
extern thread_local bool eyes_twin;
bool eyes_scene_sized(uint32_t w, uint32_t h);   // (the scene's size, or a half or quarter of it)
bool eyes_scene_chain(uint32_t w, uint32_t h);   // (the scene's size over 3, 6, 8, 12 or 16: a downsample chain's steps)
extern thread_local int eyes_storage_made;   // (vk_draw.cpp texture_view_of: the texture is made for a shader: 1 its storage image, 2 to sample)
/// RX_VR_EYES_DEBUG's bits: 1 no shared sampling, 2 no compute split, 4 mesh draws not moved, 8 mesh draws left out, 16 no eye routing
bool eyes_debug(int bit);
/// (research, STE-30) the program whose pipeline is being made on this thread (vk_draw.cpp build_graphics), for the log
extern thread_local uint64_t eyes_building_prog;
/// (vk_draw.cpp) a draw drawn one eye after the other, and why: counted for the log
void eyes_count_route(bool mesh, bool inputs, bool screen);
extern thread_local bool eyes_sample_array;
VkShaderModule eyes_sample_module(VkShaderModule module, uint64_t bindings, bool shifted);
/// Whether a draw's vertex uniform buffer 3 (NVN binding 0) is the scene's camera: its draws are moved for each eye
bool eyes_camera(uint64_t camera, uint64_t size, uint32_t width, uint32_t height);
/// The same, by the camera's own size field (a dispatch has no targets)
bool eyes_scene_camera(uint64_t camera, uint64_t size);
/// (research, STE-29) the scene camera as last seen, and a search for it in a dispatch's buffer (RX_VR_EYES_CAM_LOG)
void eyes_note_camera(const float* camera);
/// (STE-29) whether a program's binding may hold copies of the camera (unknown yet, or it does), and each eye's copy of it
/// with them all moved to that eye (false: none there)
bool eyes_camera_copies_maybe(uint64_t prog, int binding);
bool eyes_move_camera_copies(uint64_t prog, int binding, float* data, uint64_t bytes, int eye);
int eyes_camera_copies_learn(uint64_t prog, int binding, uint64_t addr, uint64_t size);   // (STE-31: 1 holds copies, 0 not, -1 not known yet)
/// (STE-31) a program's learned camera copies as a pipeline-key signature (0 none), and its fragment stage patched to correct
/// its reads of them per view (null: not patched)
uint64_t eyes_camera_signature(uint64_t prog);
/// (STE-33) the shadow-cascade check: 0 not decided yet, 1 the cascades are moved for each eye, -1 not (stereo_draw.inc asks a
/// program's camera signature again once when it changes)
int eyes_cascades_state();
/// (STE-34) one of the actors' shadow-box programs: drawn once for both eyes, its view-space matrices corrected per view
bool eyes_actor_shadow_box(uint64_t prog);
/// (research, STE-36: vr/stereo/fog_dump.inc) a program's learned camera copies as text, and the scene's P00
std::string eyes_copies_text(uint64_t prog);
float eyes_scene_p00();
/// (STE-33, research) the bytes about to be checked for the cascades: their binding's address, bound size, and whether the GPU
/// writes that range (memory_gpu_written); asked only while eyes_camera_dump_wanted
bool eyes_camera_dump_wanted();
void eyes_camera_source(uint64_t addr, uint64_t size, int gpu_written);
VkShaderModule eyes_camera_module(VkShaderModule module, uint64_t prog, uint64_t signature);
void eyes_find_camera(uint64_t prog, const char* kind, int index, uint64_t addr, uint64_t size);
/// (STE-26) whether a buffer is the particle camera the game's effects read (vertex UBO 10) with the scene's projection
bool eyes_particle_camera(uint64_t camera, uint64_t size);
/// (research, RX_VR_EYES_CAM_LOG=1) a program reading the scene's camera, logged once; kind 1 a draw, 2 a dispatch
void eyes_camera_research(uint64_t camera, uint64_t size, uint64_t prog, int kind, int stage, uint32_t width, uint32_t height,
                          int split);
/// The vertex, geometry or mesh stage made for both eyes (VK_NULL_HANDLE: as it is, the same in both)
VkShaderModule eyes_module(VkShaderModule module);
/// (OoT VR, HUD size) the vertex stage drawn smaller about the middle by scale, its edge-touching vertices kept (a full-screen
/// fade): spirv_hud_patch, made once per module and scale; VK_NULL_HANDLE when it cannot be
VkShaderModule hud_module(VkShaderModule module, float scale);
/// What the eyes' shift is now, for pipeline keys (pipelines made for an older shift are not used)
uint64_t eyes_key();
/// (research, RX_VR_PIPE_LOG) a hash of the SPIR-V a module was made from, 0 when its words are not kept
uint64_t module_words_hash(VkShaderModule module);
/// The eye offset along the camera's right the shift was made for (metres; the left eye at minus it)
float eyes_half();
/// Counted once a frame for the log: passes and draws drawn for both eyes
void eyes_count(int passes, int draws, int moved);
// (vk_vr.cpp's calls into vk_vr_eyes.cpp)
void eyes_note_projection(const float proj[16]);
void eyes_device_created();
void eyes_frame_end();
void eyes_scene_done(VkCommandBuffer cmd, const Texture* color, const Texture* depth);
/// (vk_draw.cpp, before a network's dispatch) the scene's end: both eyes' scene copied out, once a frame (outside a pass)
void eyes_scene_end(VkCommandBuffer cmd);
/// (vk_draw.cpp begin_pass) a pass drawing both eyes; (dispatch, outside a pass) the scene copied before compute work
void eyes_pass_drawn();
bool eyes_copy_due();
void eyes_before_dispatch(VkCommandBuffer cmd);
void eyes_frame_complete();   // (vk_vr_eyes.cpp: the frame's last copy is the scene's)
/// Whether an image was made with both eyes' layers (the present's source); and the test's dump of the final picture
bool eyes_two_layer(VkImage image);
void eyes_note_image(const Texture* t);   // (vk_texture.cpp: a texture made with both eyes' layers)
void eyes_present_test(VkCommandBuffer cmd, VkImage src, uint32_t w, uint32_t h);
/// (research) a draw in both eyes: how many two-layer textures it samples, whether it was moved, its vertex count
void eyes_note_sampling(uint64_t program, int sampled, bool moved, int count, bool mesh);
/// Whether eyes_note_sampling could still log this program (false past its cap or once noted): the draw skips its count then
bool eyes_note_sampling_open(uint64_t program);
/// The present of one of the game's frames (not a made one): the picture copied into the next slot, recorded into cmd.
/// head_locked keeps a flat overlay (the Hookshot menu) centred on the current headset view.
void present(VkCommandBuffer cmd, VkImage src, uint32_t sw, uint32_t sh, const int crop[4], bool head_locked = false);
/// (each present, from the game side's host_wide_menus) whether a full-screen 2D menu is up (the title, the file select, the
/// pause menu): its frame shown flat as the middle 16:9 of the picture (frame_capture.inc); menu_169: the answer, for this present
void menu_tick(bool menu);
bool menu_169();
/// The present's submit: a timeline signal to add (false: none this present)
bool present_signal(VkSemaphoreSubmitInfo& out);
/// (no window, vk_replay.cpp's offscreen copy) present's slot is ready once the replay's timeline reaches main_value: signalled
/// from the CPU by the headset thread, as no present submit takes present_signal
void present_done_at(uint64_t main_value);

/// A controller pose expressed in the game's world axes. The grip basis columns are its local +x, +y and +z axes.
struct LinkHand {
    bool valid = false;
    float position[3]{};
    float velocity[3]{};   // the controller's own velocity, game world axes (m/s)
    float angular_velocity[3]{};   // and its angular velocity (rad/s)
    float basis[9]{};
    float aim_at[3]{};     // the controller's aim pose (OpenXR: a laser pointer's line, its -z), in the game world: where,
    float aim[3]{};        // the way it points (unit),
    float aim_up[3]{};     // and its up (+y)
    float trigger = 0, squeeze = 0;
};
/// The first-person Link body associated with the camera pose the host built.
struct LinkBody {
    uint32_t pose_seq = 0;
    bool active = false, child = true;
    float root[3]{};
    float right[3]{1, 0, 0};
    float back[3]{0, 0, 1};
    float head_back[3]{};      // the headset's back axis in game-world coordinates (0: unknown)
    float eye[3]{};            // the headset's position in game-world coordinates
    float head_up[3]{};        // the headset's up axis, the same
    float arm_scale = 1;       // the VR tab's Arm length (Link's arms this many times their own)
    int bow_hand = 0;
    float bow_scale = 1.3f;
    float bow_grip[3]{};       // centimetre sliders converted to metres in the calibrated hand frame
    bool bow_equipped = false;
    bool sling = false;        // the bow that is out is the Fairy Slingshot (vr/items/slingshot): its model, fork and pouch
    int hook_hand = -1;        // the hand holding the hookshot or longshot (vr/items/hookshot), -1 none
    bool menu = false;         // the Hookshot menu is open (held interactions cancel)
    bool hook_out = false;     // the hookshot is out after a VR hand held it (and 0.6 s after it is put away): the shield stays away
    bool shield_away = false;  // hook_out, or a VR hand holds another item than the sword (and 0.6 s after): the shield stays away
    float eye_rest[3]{};       // where it would be with the head where it was at the last recentre (Link's root + height + depth)
    float crawl = 0;           // how far into a crawl Link is: 0 standing, 1 down on his hands and knees (vr/camera/crawl_host.c)
    float lowered = 0;         // how far the view came down for it (m): eye and eye_rest are this much lower
    float wrist_deg[2][3]{};   // per-hand local pitch/yaw/roll correction, captured by the VR menu
    uint32_t equipment_known = 0, equipment_drawn = 0; // native owned sword/shield state, bits 0/1
    uint64_t equipment_owner[2]{}; // reset attachment history when the owned actor changes
    LinkHand hand[2];
};
/// The replay-side body state. False means that ordinary game animation must be left untouched.
bool link_body(LinkBody& out);
/// (BODY-11) The body for one replayed frame (texture_frame_no): the one the game told with the camera that frame is drawn
/// with, not the newest (the game runs up to two frames ahead of the replay). how: 0 that frame's own (its scene camera
/// matched), 1 guessed from the last matched frame (Link's draws before the frame's scene draws), 2 the newest (no match yet,
/// or the Debug tab's Body pose timing set to Latest). vr/body/tracking.inc
bool link_body_for_frame(uint64_t frame, LinkBody& out, int* how);
/// Whether a replayed frame has a first-person body (its camera's update told one): not at the director's camera, a theatre
/// seat or Link's spot (vr/camera/events_host.c), where the game's Link is seen whole
bool link_body_shown(uint64_t frame);
/// Whether a draw's model stands where the frame's first-person body does (vr/items/gear/equipment_skin.inc)
bool link_draw_near_body(uint64_t object, uint64_t object_size, uint64_t frame);
/// (every 10 s, from the headset's status lines) the body timing counts (tracking.inc) and Link's draw counts (vk_vr_body.cpp)
void body_timing_report();
void body_draw_report();
/// (vr/headset/frame_capture.inc) whether a depth target is the scene's own depth, as its draws used it lately (a depth-only
/// pass into it is the scene's depth prepass, not a shadow map); and whether the scene's depth is known at all
bool scene_depth_is(const Texture* depth);
bool scene_depth_known();
/// (vr/headset/frame_capture.inc) whether a texture is this frame's scene colour (vk_draw.cpp: the gameplay upscale's input)
bool scene_color_is(const Texture* color);
/// (vk_draw.cpp via skin_draw.inc) one of Link's first-person draws, for the 10 s counts and the blink lines (vk_vr_body.cpp):
/// what became of it, whether its pass draws colour, and the frame
enum class LinkDraw { Posed, RecentPose, Equipment, Refused, NearNative, ShadowHidden, HeadHidden, Count };
void note_link_draw(LinkDraw what, bool colour, uint64_t frame);
/// Whether the last link_skin call on this thread refused a palette that is Link's (his joints hold together) but not the
/// player's where he stands, or could not read it (vr/body/skin.inc)
bool link_skin_refused();
/// Capture the present controller orientations as Link's neutral forward, palms-in pose. The resulting offsets are put in opts.
bool calibrate_wrists();
/// (vk_vr_body.cpp) Link's VR skin palette for one draw's palette upload (vertex uniform buffer 3), or null when the upload
/// is not first-person Link's. The result is made once per distinct upload and shared by every pass. hide: the draw is a
/// head-only shape (face, eyes, hair) to leave out.
std::shared_ptr<const std::vector<float>> link_skin(uint64_t bones, uint64_t bytes, uint64_t object, uint64_t object_size,
                                                    uint64_t vb0_size, bool* hide, uint64_t frame);
/// A draw of Link's hair shape, by its vertex buffer size or index count (vk_vr_body.cpp)
bool hair_draw(uint64_t vb0_size, uint32_t count);
/// The latest VR palette (at most 150 ms old), for one of Link's draws whose own palette upload was not taken; hide as link_skin
std::shared_ptr<const std::vector<float>> link_skin_recent(uint64_t vb0_size, bool* hide, uint64_t frame);
/// A head-only shape of Link's (face, eyes, brows, lashes, teeth, hair), by its vertex buffer size
bool head_draw(uint64_t vb0_size);
/// Retarget separate equipped models using the body's original-to-VR bone transforms.
std::shared_ptr<const std::vector<float>> link_equipment_skin(uint64_t bones, uint64_t bytes, uint64_t object,
                                                             uint64_t object_size, uint64_t vb0_size, uint64_t frame);
/// (OoT VR) a small model's draw while an item get plays (its palette in uniform buffer 4): the item moved in front of the
/// headset (vr/items/item_get/item_get.inc), or null, and the draw goes on as any other (a worn item stays hidden)
std::shared_ptr<const std::vector<float>> item_get_draw(uint64_t bones, uint64_t bytes, uint64_t object, uint64_t object_size,
                                                        uint64_t vb0_size, uint64_t frame);
/// The radius (m, as drawn) of the bomb a VR hand holds (0 left, 1 right), or 0: told by the game side every game frame
/// (hookshot_vr_hand_holds, from vr/items/bomb/bomb_host.c), and 0 once it stops telling (a quarter of a second).
float held_ball_radius(int hand);
/// (OoT VR) the VR hand holding the sword taken from the back (0 left, 1 right; hookshot_vr_sword_hand, from
/// vr/input/controllers_host.c), or -1: none, or not told for a quarter of a second; -2: let go, the game still putting it away
int sword_hand();
/// (OoT VR) whether the game guards: its ZL held (hookshot_vr_shield_guard, from vr/input/controllers_host.c every input
/// read); false when not told for a quarter of a second. The held shield faces forward then (vr/items/gear/held_gear.inc)
bool shield_guarding();
/// (OoT VR) The held shield's grip, moved across its back for the left VR hand (vr/items/gear/shield_grip.inc): whether
/// a vertex buffer (vb0 bytes, this stride) is a shield shape with a grip; that shape's game bytes copied into out with
/// the grip moved, false when they are not your game's model (null bytes: could not be read, not known meanwhile); and
/// whether a draw of it with this VR palette is the left hand's with the grip moved (its buffer then bound instead)
bool shield_grip_shape(uint64_t vb0, uint32_t stride);
bool shield_grip_make(uint64_t vb0, const uint8_t* game, uint64_t n, uint8_t* out);
bool shield_grip_drawn(uint64_t vb0, const void* palette);
/// (OoT VR) the VR hand holding the Deku Stick (0 left, 1 right; hookshot_vr_stick_hand, from vr/input/controllers_host.c
/// vr_stick), or -1: none, or not told for a quarter of a second; -2: let go, the game still putting it away (hidden meanwhile)
int stick_hand();
/// (OoT VR) the VR hand holding the ocarina (0 left, 1 right; hookshot_vr_ocarina_hand, from vr/items/ocarina/ocarina_host.c),
/// or -1: on the belt, or not told for a quarter of a second
int ocarina_hand();
/// (OoT VR) where the ocarina was just drawn on the VR belt (world, m), for the game side's grip (hookshot_vr_ocarina_belt)
void ocarina_belt_drawn(const float at[3]);
/// (OoT VR) a song the VR ocarina played is being played by the game now (hookshot_vr_ocarina_song): its effects
bool ocarina_song();
/// (OoT VR) an item get plays (hookshot_vr_item_get, from vr/items/item_get/item_get_host.c): its item in front of the headset
bool item_get_live();
/// (OoT VR) an item get's item was just drawn in front of the headset (item_get.inc), for the game side's
/// hookshot_vr_item_get_shown
void item_get_drawn();
/// (OoT VR) the item gets begun so far (item_get.inc: a model let go stays let go until the next)
unsigned item_get_number();
/// (OoT VR) the VR hand holding the Boomerang (0 left, 1 right; hookshot_vr_boomerang, from vr/items/boomerang/boomerang_host.c),
/// or -1: none, or not told for a quarter of a second; -2: let go, the game still putting it away (hidden meanwhile)
int boomerang_hand();
/// (OoT VR) where the Boomerang's reticle is (the game world) while it aims; false when it does not
bool boomerang_reticle(float at[3]);
/// (OoT VR) where a third-person shot will land (the game world) while an item aims in third person (hookshot_vr_aim_mark,
/// from vr/items/aim/third_person_aim_host.c): 1 shown there; 0 not aiming
int third_person_aim(float at[3]);
/// (OoT VR) the VR hand holding a bottle (0 left, 1 right; hookshot_vr_bottle_hand, from vr/items/bottle/bottle_host.c),
/// or -1: none, or not told for a quarter of a second; -2: let go, the game still putting it away (hidden meanwhile)
int bottle_hand();
/// (OoT VR) whether the held bottle is corked (hookshot_vr_bottle_cork, from vr/items/bottle/bottle_host.c): 1 corked, 0 open,
/// -1 not told for a quarter of a second
int bottle_cork();
/// (OoT VR, the VR tab's HUD size) the game's 2D layer (HUD, text boxes, menus) scaled toward the middle of the picture by
/// this much (1: as the game draws it; the picture is stretched to the headset's whole view, its edges out of sight), and
/// the images presented lately (hud_presented_recently), from which vk_draw.cpp learns which texture the game draws its 2D layer into
float hud_scale();
bool hud_presented_recently(VkImage image);   // (the presented images take turns: the last 4)
void hud_note_presented(VkImage image);
/// (vr/items/bomb/bomb_host.c, every game frame: hookshot_vr_bomb_state) what a hand holds (0 none, 1 a bomb, 2 a Bomb
/// Flower), whether Link's lift has it (draw it in the palm wherever the lift has it), and the bomb the pull would take
/// (its kind, 0 none; its centre, world m). All forgotten a quarter of a second after the game side last told.
int held_bomb_kind(int hand);
bool held_bomb_lifted(int hand);
int pull_target(float centre[3]);
/// (vr/items/gear/equipment_skin.inc) a Bomb Flower fruit's body: its centre above the actor's origin (x y z, m) and its radius
bool flower_shape(float out[4]);
/// Where the game has that held bomb's centre (world, m), as last told; false when the hand holds none.
bool held_ball_centre(int hand, float out[3]);
/// First person: whether a draw is something Link wears with no VR pose this frame, near Link (left out, not drawn in the
/// game's pose).
bool link_worn_hidden(uint64_t bones, uint64_t bytes, uint64_t object, uint64_t object_size, uint64_t vb0, uint64_t frame);
// The latest game frame's committed bow solution (vk_vr_body.cpp bow_pose, made once per game frame from the frozen body
// snapshot, whether or not the bow model itself was drawn): palm socket, aim direction, string hand, draw metres from the
// string's rest, string held (0/1), pose sequence. It never creates a projectile: the host consumes it only inside the
// verified player's launch call.
bool bow_solution(float out[12]);

/// Link's first-person torso heading (world yaw, atan2(x, z)) while the tracked body is posed (vr/body/vk_vr_body.cpp)
bool body_heading(float* yaw);
bool native_bow_reticle_hidden();
/// The held hookshot's barrel in the game world (vr/items/hookshot/held_hookshot.inc): its tip and the way it points, as last
/// drawn in a VR hand (fresh: the last 150 ms), for the game's aim and laser
bool hook_barrel(float out[6]);
/// The held hookshot's effects (its laser) moved onto the VR hookshot, as arrow_effect_move: live while it is held
bool hook_effects_live();
/// A draw of the fired hookshot chain (its colour pass binds the palette in uniform buffer 4: skin_draw.inc)
bool hook_chain_draw(uint64_t vb0);
/// A draw of the ocarina or its holder (its colour pass binds the palette in uniform buffer 4 too: skin_draw.inc)
bool ocarina_draw(uint64_t vb0);
/// (OoT VR) a draw of a bottle model (vr/items/bottle/held_bottle.inc): its palette is tried in vertex uniform buffer 4 first
bool bottle_draw(uint64_t vb0);
bool hook_effect_move(const float effect[16], const float camera[3], float move[12]);
/// The held Deku Stick's effects (its flame, emitted on the game's stick) moved onto the VR stick, as arrow_effect_move: live
/// while a VR hand holds it (vr/items/deku_stick/held_stick.inc)
bool stick_effects_live();
bool stick_effect_move(const float effect[16], const float camera[3], float move[12]);
bool sword_effects_live();   // (the held sword's effects, its charge glow: vr/items/gear/held_gear.inc)
bool sword_effect_move(const float effect[16], const float camera[3], float move[12]);
/// (vr/items/bow/held_arrow.inc) For a draw whose effect matrix (its vertex uniform buffer 11, "C14": x, y, z axes and
/// position, 16 floats, world) sits on the game's own held arrow, not at the camera (camera: its place): the move from the
/// game's arrow to the VR arrow (world, 3x4 rows), which the draw's particle camera is given so all of it (particles too) is
/// drawn on the VR arrow. False for anything else.
bool arrow_effect_move(const float effect[16], const float camera[3], float move[12]);
/// Whether an arrow is on the VR string this frame or the last two (cheap: the draw replay asks it before reading anything)
bool arrow_effects_live();

} // namespace vr
} // namespace mkwvk

/// What the host asks each time the game's camera is built (host/rx_backend.c rx_vr_head)
struct HookshotVrHead {
    uint32_t seq;      // this pose's number (rx_vr_drawn says which one a camera was built with)
    int active;        // the headset is running
    int mode;          // 0 third person (the game's orbit camera), 1 first person (Link's eye position)
    int move_head;     // in first person, rotate movement from camera-forward to headset-forward
    int level;         // straight ahead in the headset is level (the game camera's own tilt dropped)
    int height_cm;     // with level: the view this high above Link's feet (0: the game camera's height)
    int body_depth_cm; // first-person body placement relative to the headset: positive puts Link farther ahead
    float q[4];        // the head's turn since the last recentre, x y z w (OpenXR: x right, y up, z back)
    float p[3];        // its movement since then, metres, in the same frame
    float lean;        // game metres per metre of head movement
    float tan_half_x;  // symmetric source frustum that covers the runtime's two eye FOVs
    float tan_half_y;
    int detail;        // the VR tab's Detail distance: 0 Low, 1 Switch 2, 2 High, 3 Ultra (host/hos_main.c hos_vr_preset_fovy)
    int foliage;       // its Foliage distance, the same
};
/// (OoT VR, hands stage 1) each controller, in hookshot_vr_head's recentred frame (x right, y up, z back, metres)
enum {
    HOOKSHOT_VR_BTN_LOWER = 1,     // X (left) / A (right)
    HOOKSHOT_VR_BTN_UPPER = 2,     // Y / B
    HOOKSHOT_VR_BTN_STICK = 4,     // the thumbstick's click
    HOOKSHOT_VR_BTN_MENU = 8,      // the left menu button
    HOOKSHOT_VR_BTN_TRIGGER = 16,  // the trigger pressed past 85%
    HOOKSHOT_VR_BTN_GRIP = 32,     // the grip pressed past 85%
    HOOKSHOT_VR_BTN_MENU2 = 64,    // (the game side's) this hand's part of the VR menu's other button (its Menu button row: the
                                   // left upper button, or both stick clicks), also while the menu is open
};
struct HookshotVrHand {
    int valid;                     // tracked this frame
    float grip_q[4], grip_p[3];    // the grip pose: where the hand holds things (x y z w; metres)
    float aim_q[4], aim_p[3];      // the aim pose: a pointing ray along its -z
    float lin_vel[3], ang_vel[3];  // the grip's velocity (m/s) and spin (rad/s), for throws
    float trigger, squeeze;        // 0..1
    float stick[2];                // -1..1, x right, y forward
    uint32_t buttons;              // HOOKSHOT_VR_BTN_*
};
struct HookshotVrHands {
    uint32_t seq;                  // a new number each headset frame
    HookshotVrHand hand[2];        // 0 left, 1 right
};
// Every function the host finds by name (GetProcAddress(vk_module, "hookshot_vr_...")) is declared here with
// HOOKSHOT_VR_EXPORT: on Windows a DLL exports only what is marked dllexport, so a name missing here is NULL to the host
// and its bridge does nothing (tests/test_vr_exports.py)
#ifdef _WIN32
#define HOOKSHOT_VR_EXPORT __declspec(dllexport)
#else
#define HOOKSHOT_VR_EXPORT __attribute__((visibility("default")))
#endif
extern "C" {
HOOKSHOT_VR_EXPORT int hookshot_vr_bow(float out[12]);
HOOKSHOT_VR_EXPORT void hookshot_vr_bow_active(int active);
HOOKSHOT_VR_EXPORT int hookshot_vr_hook_ray(int hand, float out[6]);
HOOKSHOT_VR_EXPORT void hookshot_vr_hook_active(int active);
HOOKSHOT_VR_EXPORT void hookshot_vr_radial(const int32_t* data, int n);
HOOKSHOT_VR_EXPORT void hookshot_vr_wrist(const int32_t* data, int n);
HOOKSHOT_VR_EXPORT int hookshot_vr_wrist_flags(uint32_t* hashes, int max);
HOOKSHOT_VR_EXPORT void hookshot_vr_menu_screen(int menu);
HOOKSHOT_VR_EXPORT int hookshot_vr_wanted(void);
HOOKSHOT_VR_EXPORT void hookshot_vr_equipment(uint32_t seq, uint32_t known, uint32_t drawn, const uint64_t owners[2]);
HOOKSHOT_VR_EXPORT int hookshot_vr_head(HookshotVrHead* out);
HOOKSHOT_VR_EXPORT int hookshot_vr_hands(HookshotVrHands* out);
HOOKSHOT_VR_EXPORT int hookshot_vr_body_heading(float* yaw);
HOOKSHOT_VR_EXPORT int hookshot_vr_hands_world(float out[42]);
HOOKSHOT_VR_EXPORT void hookshot_vr_hand_holds(int hand, float radius, const float centre[3]);
HOOKSHOT_VR_EXPORT int hookshot_vr_hand_ray(int hand, float out[6]);
HOOKSHOT_VR_EXPORT void hookshot_vr_bomb_marks(const float* marks, int n);
HOOKSHOT_VR_EXPORT void hookshot_vr_comfort(const float cause[4]);
HOOKSHOT_VR_EXPORT void hookshot_vr_bomb_state(const float state[8]);
HOOKSHOT_VR_EXPORT int hookshot_vr_flower_shape(float out[4]);
HOOKSHOT_VR_EXPORT void hookshot_vr_sword_hand(int hand);
HOOKSHOT_VR_EXPORT void hookshot_vr_shield_guard(int guarding);
HOOKSHOT_VR_EXPORT void hookshot_vr_stick_hand(int hand);
HOOKSHOT_VR_EXPORT void hookshot_vr_ocarina_hand(int hand);
HOOKSHOT_VR_EXPORT int hookshot_vr_ocarina_belt(float out[3]);
HOOKSHOT_VR_EXPORT void hookshot_vr_ocarina_song(int playing);
HOOKSHOT_VR_EXPORT void hookshot_vr_item_get(int playing);
HOOKSHOT_VR_EXPORT int hookshot_vr_item_get_shown(void);
HOOKSHOT_VR_EXPORT void hookshot_vr_boomerang(int hand, int reticle, const float at[3]);
HOOKSHOT_VR_EXPORT void hookshot_vr_aim_mark(int kind, const float at[3]);
HOOKSHOT_VR_EXPORT void hookshot_vr_bottle_hand(int hand);
HOOKSHOT_VR_EXPORT void hookshot_vr_bottle_cork(int corked);
HOOKSHOT_VR_EXPORT int hookshot_vr_eye_world(float out[3]);
HOOKSHOT_VR_EXPORT void hookshot_vr_rumble(int hand, float amplitude, float seconds);
HOOKSHOT_VR_EXPORT void hookshot_vr_drawn(uint32_t seq, const float view[12]);
HOOKSHOT_VR_EXPORT void hookshot_vr_body(uint32_t seq, int active, int child, const float root[3],
                                        const float right[3], const float back[3]);
HOOKSHOT_VR_EXPORT void hookshot_vr_crawl(uint32_t seq, float amount, float lowered);
HOOKSHOT_VR_EXPORT int hookshot_vr_menu(int toggle);
}
