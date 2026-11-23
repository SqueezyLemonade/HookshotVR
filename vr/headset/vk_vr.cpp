// Hookshot VR: headset entry point. Public API: vr/vk_vr.h. Folder guide: vr/README.md.
// The includes below are ordered sections of ONE compilation unit, not alternate headset implementations.
//
// OpenXR through XR_KHR_vulkan_enable on the back end's own device (no second device, nothing shared between processes). The
// loader is opened at run time (libopenxr_loader.so.1 / openxr_loader.dll), so a PC without it runs the game as before.
//
// The frames: the game thread builds its camera with the newest head pose (hookshot_vr_head) and says which one it used and the
// view it made (hookshot_vr_drawn). The replay notes the scene's camera (its view-projection and projection, the camera's uniform
// buffer as the temporal upscalers read it) and, once the world is drawn, copies the scene depth (shaders/vr_depth.comp: any depth
// format into R32F). The present copies the picture and that depth into a slot of a four-slot ring and signals the slot's value on
// a timeline semaphore. The headset thread takes the newest slot whose value the GPU reached, finds the head pose the game drew it
// with (the drawn view that matches the slot's camera), and makes each eye from it (shaders/vr_warp.comp): every pixel moved by its
// depth for that eye and for the head's movement since. The compositor turns it for the head's turn since.
#include "vk_vr.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "vk_internal.h"
#include "vk_menu.h"
#include "vk_texture.h"
#include "vk_paths.h"
// (rec::boundary only: this file's vkCmd* calls stay direct, not vk_work.h's. The headset thread records its own command buffers,
// which the recorder never touches; the replay-side calls below (frame_capture.inc) let the recorder catch up first)
#include "vk_recorder.h"
#include "zstd.h"           // (the VR menu's frame art: vr/menu/vr_menu_api.inc)
#include "vk_vr_bow_art.h"   // (the bow reticle masks, made from the player's game files by vr/tools/generate)
#include "vk_vr_icons.h"     // (the item icons, the same way: the radial menu, vr/menu/radial_draw.inc)
#include "vk_vr_wrist_maps.h"   // (the map menu's pictures, the same way: the wrist HUD's minimap, vr/menu/wrist_draw.inc)
#include "../menu/radial_items.h"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/imgui/imstb_truetype.h"   // (the wrist HUD's digits: vr/menu/wrist_draw.inc; Dear ImGui's own copy)
extern const unsigned char hookshot_font_heading[];    // (nx2/ui_fonts.py: Libertinus Serif Semibold, the VR menu's font)

#define XR_NO_PROTOTYPES
#define XR_USE_GRAPHICS_API_VULKAN
#include "openxr/openxr.h"
#include "openxr/openxr_platform.h"

#include "shaders/vr_warp_comp_spv.h"
#include "shaders/vr_depth_comp_spv.h"
#include "shaders/vr_window_comp_spv.h"
#include "shaders/vr_window_game_comp_spv.h"

#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace mkwvk {
extern int64_t presents_now;   // (vk_replay.cpp: the 2D menu's log, menu_tick)
namespace vr {
namespace {

#include "runtime_loader.inc"
#include "state_and_images.inc"
#include "poses.inc"
#include "eye_rendering.inc"
#include "../input/openxr_controllers.inc"
#include "markers.inc"
#include "../menu/radial_draw.inc"
#include "../menu/vr_menu_layer.inc"   // (the VR menu's panel in the world)
#include "../items/bomb/bomb_marks.inc"   // (the bombs' throw arc, target and pull ring)
#include "../comfort/vignette_layer.inc"   // VR HOOK (the Comfort Aids' vignette round both eyes)
#include "../menu/wrist_draw.inc"   // VR HOOK (the wrist check HUD on the left wrist)
#include "window_view.inc"   // (the Window view: the game through a frame fixed in the room)
#include "frame_loop.inc"
} // namespace (private headset helpers)

#include "runtime_session.inc"
#include "frame_capture.inc"
#include "../menu/vr_menu_api.inc"   // (the VR menu: its open state, its images, the controllers at it)
#include "../body/tracking.inc"
} // namespace vr
} // namespace mkwvk

#include "host_api.inc"
