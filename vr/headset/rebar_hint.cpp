// Hookshot VR: the Resizable BAR hint. Without Resizable BAR the CPU reaches only a small window of the graphics card's
// memory, and VR (two eyes, the headset's own images) needs more of it than a flat screen: the game runs out of graphics
// memory, the textures go white, then it crashes or the device is lost. Nothing warns at the start (decided 20 Nov); when one
// of those happens in VR on a discrete card whose policy facts say resizable BAR 0, this writes one line naming the cause and
// the fix. The crash window and its report name the same cause from that line (host/hos_crash_dialog.h).
//
// Called at Hookshot's own sites (VR HOOK): a lost device (vk_replay.cpp device_lost), an image put in system memory for want
// of VRAM (vk_vma.cpp), pipelines the driver refused for want of memory (vk_texture.cpp). Once a run.
#include "vk_vr.h"
#include "vk_internal.h"
#include "../../host/hos_report_prefixes.h"
#include <atomic>

namespace mkwvk {
namespace vr {

/// The hint is for this run: the headset runs, on a discrete card (an integrated one has no BAR to turn on) without
/// Resizable BAR (device_policy.h DeviceFacts::rebar: no DEVICE_LOCAL | HOST_VISIBLE type on its VRAM heap)
bool rebar_hint_wanted(bool headset, int device_type, bool rebar) {
    return headset && device_type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && !rebar;
}

void rebar_hint(const char* what) {
    if (!rebar_hint_wanted(active(), (int)dev.facts.device_type, dev.facts.rebar)) return;
    static std::atomic<bool> told{false};
    if (told.exchange(true)) return;
    logf_at(log_warn, HOS_REPORT_VR_REBAR_PREFIX " %s. VR needs the graphics card's whole memory reachable: white "
            "textures, then a crash, are the sign. Turn on Resizable BAR (on AMD: Smart Access Memory) and Above 4G Decoding "
            "in your PC's UEFI/BIOS settings, then start the game again", what);
}

}  // namespace vr
}  // namespace mkwvk
