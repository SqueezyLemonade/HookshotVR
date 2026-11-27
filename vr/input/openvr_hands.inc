// Hookshot VR: OpenVR input fallback (27 Nov 2026)
// Included by vr/headset/vk_vr.cpp right after ../input/openxr_controllers.inc; not a separate compilation unit.
//
// Why: some OpenXR runtimes bind no controller profile at all. Seen with SteamVR 2.18.4 + PICO Connect 10.6.6 and a
// Pico 4: xrGetCurrentInteractionProfile answers (none) for both hands, "controllers: left/right hand uses (none)",
// and the game gets no hands though SteamVR's own OpenVR API lists and tracks both controllers ("Pico Phoenix
// Controller left/right", poses and buttons valid). This reads the controllers from OpenVR instead - only when
// OpenXR gave nothing for that hand - and hands them over in the same local space as the head.
//
// The hands are placed through the head: the head's pose this frame (s.local) and the HMD's pose in OpenVR stand
// for the same instant, so the controllers land where they are, whatever the two spaces' origins are.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define vr openvr_ns   // (the header's own namespace, out of this one's way: mkwvk::vr already exists)
#include "../third_party/openvr/openvr.h"
#undef vr

struct OpenVrIn {
    bool tried = false, ready = false;
    HMODULE dll = nullptr;
    openvr_ns::IVRSystem* sys = nullptr;
    void (*shut)() = nullptr;
    openvr_ns::TrackedDeviceIndex_t hmd = openvr_ns::k_unTrackedDeviceIndexInvalid;
    openvr_ns::TrackedDeviceIndex_t hand[2] = {openvr_ns::k_unTrackedDeviceIndexInvalid,
                                               openvr_ns::k_unTrackedDeviceIndexInvalid};
    int said = 0;
};
OpenVrIn ovr;
XrPosef ovr_head{};
bool ovr_head_ok = false;   // (frame_loop.inc: the head's pose this frame, in s.local)

/// the quaternion of an OpenVR tracking matrix's rotation (row-major 3x4)
XrQuaternionf ovr_quat(const openvr_ns::HmdMatrix34_t& m) {
    const float (*r)[4] = m.m;
    const float tr = r[0][0] + r[1][1] + r[2][2];
    XrQuaternionf q{};
    if (tr > 0) {
        const float s = std::sqrt(tr + 1.0f) * 2;
        q.w = 0.25f * s, q.x = (r[2][1] - r[1][2]) / s, q.y = (r[0][2] - r[2][0]) / s, q.z = (r[1][0] - r[0][1]) / s;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        const float s = std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2;
        q.w = (r[2][1] - r[1][2]) / s, q.x = 0.25f * s, q.y = (r[0][1] + r[1][0]) / s, q.z = (r[0][2] + r[2][0]) / s;
    } else if (r[1][1] > r[2][2]) {
        const float s = std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2;
        q.w = (r[0][2] - r[2][0]) / s, q.x = (r[0][1] + r[1][0]) / s, q.y = 0.25f * s, q.z = (r[1][2] + r[2][1]) / s;
    } else {
        const float s = std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2;
        q.w = (r[1][0] - r[0][1]) / s, q.x = (r[0][2] + r[2][0]) / s, q.y = (r[1][2] + r[2][1]) / s, q.z = 0.25f * s;
    }
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n > 0) q = {q.x / n, q.y / n, q.z / n, q.w / n};
    return q;
}
XrVector3f ovr_pos(const openvr_ns::HmdMatrix34_t& m) { return {m.m[0][3], m.m[1][3], m.m[2][3]}; }

bool ovr_start() {
    ovr.tried = true;
    ovr.dll = LoadLibraryA("openvr_api.dll");
    if (!ovr.dll) {
        logf("vr: openvr: no openvr_api.dll beside the game: the fallback stays off");
        return false;
    }
    auto init = (openvr_ns::EVRInitError(*)(openvr_ns::EVRInitError*, openvr_ns::EVRApplicationType, const char*))
        GetProcAddress(ovr.dll, "VR_InitInternal2");
    auto iface = (void* (*)(const char*, openvr_ns::EVRInitError*))GetProcAddress(ovr.dll, "VR_GetGenericInterface");
    ovr.shut = (void (*)())GetProcAddress(ovr.dll, "VR_ShutdownInternal");
    if (!init || !iface) {
        logf("vr: openvr: the C API is not exported: the fallback stays off");
        return false;
    }
    openvr_ns::EVRInitError e = openvr_ns::VRInitError_None;
    init(&e, openvr_ns::VRApplication_Background, nullptr);   // (background: it never takes the scene from the game)
    if (e != openvr_ns::VRInitError_None) {
        logf("vr: openvr: VR_Init %d: the fallback stays off", (int)e);
        return false;
    }
    openvr_ns::EVRInitError e2 = openvr_ns::VRInitError_None;
    ovr.sys = (openvr_ns::IVRSystem*)iface(openvr_ns::IVRSystem_Version, &e2);
    if (!ovr.sys) {
        logf("vr: openvr: IVRSystem %d: the fallback stays off", (int)e2);
        return false;
    }
    char hmd_model[128] = "";
    for (openvr_ns::TrackedDeviceIndex_t i = 0; i < openvr_ns::k_unMaxTrackedDeviceCount; i++) {
        const openvr_ns::ETrackedDeviceClass cls = ovr.sys->GetTrackedDeviceClass(i);
        if (cls == openvr_ns::TrackedDeviceClass_HMD) ovr.hmd = i;
        if (cls == openvr_ns::TrackedDeviceClass_Controller) {
            const openvr_ns::ETrackedControllerRole role = ovr.sys->GetControllerRoleForTrackedDeviceIndex(i);
            if (role == openvr_ns::TrackedControllerRole_LeftHand) ovr.hand[0] = i;
            if (role == openvr_ns::TrackedControllerRole_RightHand) ovr.hand[1] = i;
        }
    }
    openvr_ns::ETrackedPropertyError pe{};
    if (ovr.hmd != openvr_ns::k_unTrackedDeviceIndexInvalid)
        ovr.sys->GetStringTrackedDeviceProperty(ovr.hmd, openvr_ns::Prop_ModelNumber_String, hmd_model, sizeof hmd_model,
                                                &pe);
    ovr.ready = ovr.hmd != openvr_ns::k_unTrackedDeviceIndexInvalid &&
                (ovr.hand[0] != openvr_ns::k_unTrackedDeviceIndexInvalid ||
                 ovr.hand[1] != openvr_ns::k_unTrackedDeviceIndexInvalid);
    logf("vr: openvr: %s, hands %d/%d: the fallback is %s", hmd_model, (int)ovr.hand[0], (int)ovr.hand[1],
         ovr.ready ? "on" : "off (no HMD or no controller)");
    return ovr.ready;
}

void ovr_try_start() {
    if (!ovr.tried) ovr_start();
}

/// A hand from OpenVR, in s.local (through the head), when OpenXR gave that hand nothing
void ovr_fill(int h, HandState& hs) {
    if (!ovr.ready) return;
    const openvr_ns::TrackedDeviceIndex_t idx = ovr.hand[h];
    if (idx == openvr_ns::k_unTrackedDeviceIndexInvalid) return;
    openvr_ns::TrackedDevicePose_t poses[openvr_ns::k_unMaxTrackedDeviceCount]{};
    ovr.sys->GetDeviceToAbsoluteTrackingPose(openvr_ns::TrackingUniverseStanding, 0.0f, poses,
                                             openvr_ns::k_unMaxTrackedDeviceCount);
    const openvr_ns::TrackedDevicePose_t& hp = poses[idx];
    if (!hp.bDeviceIsConnected || !hp.bPoseIsValid) return;

    XrPosef out{};
    out.orientation = ovr_quat(hp.mDeviceToAbsoluteTracking);
    const XrVector3f p = ovr_pos(hp.mDeviceToAbsoluteTracking);
    out.position = p;
    if (ovr_head_ok && ovr.hmd != openvr_ns::k_unTrackedDeviceIndexInvalid) {
        const openvr_ns::TrackedDevicePose_t& dp = poses[ovr.hmd];
        if (dp.bPoseIsValid) {   // (this frame's head: OpenVR's space to s.local)
            const XrQuaternionf rel = qmul(qinv(ovr_quat(dp.mDeviceToAbsoluteTracking)), out.orientation);
            out.orientation = qmul(ovr_head.orientation, rel);
            const XrVector3f hd = ovr_pos(dp.mDeviceToAbsoluteTracking);
            const float d[3] = {p.x - hd.x, p.y - hd.y, p.z - hd.z};
            float r[3];
            qrot(ovr_head.orientation, d, r);
            out.position = {ovr_head.position.x + r[0], ovr_head.position.y + r[1], ovr_head.position.z + r[2]};
        }
    }

    openvr_ns::VRControllerState_t st{};
    if (!ovr.sys->GetControllerState(idx, &st, sizeof st)) return;
    const auto bit = [&](openvr_ns::EVRButtonId id) {
        return (st.ulButtonPressed & openvr_ns::ButtonMaskFromId(id)) != 0;
    };   // (the driver's own input paths: a/x, b/y, system, grip, trigger, joystick)
    hs.grip = out;
    hs.aim = out;
    hs.lin_vel[0] = hs.lin_vel[1] = hs.lin_vel[2] = 0;
    hs.ang_vel[0] = hs.ang_vel[1] = hs.ang_vel[2] = 0;
    hs.trigger = st.rAxis[1].x;
    if (bit(openvr_ns::k_EButton_SteamVR_Trigger)) hs.trigger = std::max(hs.trigger, 0.99f);
    hs.squeeze = bit(openvr_ns::k_EButton_Grip) ? 1.0f : 0.0f;
    hs.stick[0] = st.rAxis[0].x, hs.stick[1] = st.rAxis[0].y;
    uint32_t b = 0;
    if (bit(openvr_ns::k_EButton_A)) b |= HOOKSHOT_VR_BTN_LOWER;
    if (bit(openvr_ns::k_EButton_ApplicationMenu)) b |= HOOKSHOT_VR_BTN_UPPER;
    if (bit(openvr_ns::k_EButton_System)) b |= HOOKSHOT_VR_BTN_MENU;
    if (bit(openvr_ns::k_EButton_SteamVR_Touchpad)) b |= HOOKSHOT_VR_BTN_STICK;
    if (hs.trigger > 0.85f) b |= HOOKSHOT_VR_BTN_TRIGGER;
    if (hs.squeeze > 0.85f) b |= HOOKSHOT_VR_BTN_GRIP;
    hs.buttons = b;
    hs.valid = true;
    if (ovr.said < 6 && b) {   // (the first presses in the log, to map a controller's buttons from a run)
        ovr.said++;
        logf("vr: openvr: %s hand pressed %#x (mask %#llx, trigger %.2f, stick %.2f %.2f)", h ? "right" : "left", b,
             (unsigned long long)st.ulButtonPressed, hs.trigger, hs.stick[0], hs.stick[1]);
    }
}
#endif   // _WIN32
