"""Hookshot VR's game hooks: every place in the game's own code where Hookshot VR's functions are called.

What a game hook is: Hookshot translates the game's ARM code into C ahead of time (the "lift"). A hook is a line of C added
right after one instruction of the game, at its address in main (main + offset), so the game calls into Hookshot VR there.
Each entry is (offset in main, the instruction expected there, the C to add). The instruction is checked when the game is
translated: a game version whose code differs refuses the hook instead of patching the wrong place.

The functions these call are in vr/ (the *_host.c files): hos_vr_lod_snapshot and hos_vr_frustum_fovy in
vr/camera/detail_host.c, hos_vr_cull_snapshot in vr/camera/culling_host.c, hos_vr_gear_near in vr/items/gear/gear_near_host.c,
hos_vr_guard_direction in vr/items/shield/guard_host.c, hos_vr_equipment_visible in vr/items/gear/equipment_host.c,
hos_vr_bomb_ai in vr/items/bomb/bomb_host.c, hos_vr_hook_aim in vr/items/hookshot/hookshot_host.c,
hos_vr_boomerang_target in vr/items/boomerang/boomerang_host.c,
hos_vr_lod_depth and hos_vr_tan_reader in vr/camera/detail_host.c, hos_vr_bottle_keep_drawn in
vr/items/bottle/bottle_host.c, hos_vr_sensor_setup_seen, hos_vr_sensor_off_seen,
hos_vr_sensor_calc, hos_vr_action_request and hos_vr_sub_request in vr/items/sword/blade_hits.inc. The headset's field of view in the game's projection is hooked in nx2/oot_wide.py itself (its
_ratio, marked VR HOOK).

Changing anything here makes the next build translate the game again (~40 minutes): nx2/make_game.py counts this file in
the translation's key. Game version: 1.1.1.
"""

_SITES = [
    # (OoT VR, view snapshot: host/hos_main.c hos_vr_lod_snapshot / hos_vr_frustum_fovy) the snapshot's detail and foliage
    # values after its frustum is made (str s2, [x19, #0x1e0]), and the FOV its frustum is built from (ldr s0, [x2, #0xa0])
    (0x33af7c, 0xbd01e262, "{ extern void hos_vr_lod_snapshot(Ctx *c, uint64_t snap); hos_vr_lod_snapshot(c, c->x[19]); }"),
    (0x33ab44, 0xbd40a040, "{ extern int hos_vr_frustum_fovy(Ctx *c, uint64_t proj, float in, float *out); float _a, _o; "
                           "memcpy(&_a, c->vreg[0], 4); if (hos_vr_frustum_fovy(c, c->x[2], _a, &_o)) { uint32_t _u; "
                           "memcpy(&_u, &_o, 4); a64_vset64(c->vreg[0], _u); } }"),
    # (OoT VR, host/hos_main.c hos_vr_cull_snapshot) after the snapshot's last world culling plane (str s0, [x19, #0x24c]):
    # in first person the headset camera's culling keeps a ball around the eye, so Link's own natively-posed equipment is drawn
    (0x33b674, 0xbd024e60, "{ extern void hos_vr_cull_snapshot(Ctx *c, uint64_t snap); hos_vr_cull_snapshot(c, c->x[19]); }"),
    # Actual owned ModelNW path: after the independent scalar near load, change only s3 for held FP gear.
    # The snapshot/projection stays unchanged. Both ordinary sphere branches use x19=model, x9=snapshot, x25=sphere.
    (0x7f47c, 0xbd41c923, "{ extern void hos_vr_gear_near(Ctx *c); hos_vr_gear_near(c); }"),
    (0x7df54, 0xbd41c923, "{ extern void hos_vr_gear_near(Ctx *c); hos_vr_gear_near(c); }"),
    # Untargeted guard: shared facing setter, AFTER reading z. Override x/y/z AND partial norm, not caller memory.
    (0x93533c, 0xbd400821, "{ extern void hos_vr_guard_direction(Ctx *c); hos_vr_guard_direction(c); }"),
    # (OoT VR, host rig/oot_vr_equipment_host.c hos_vr_equipment_visible) at the entry of f_82e1e260, the equipment
    # show/hide request (pacibsp): in first person a drawn sword/shield of Link's is kept visible
    (0x2dfe260, 0xd503237f, "{ extern void hos_vr_equipment_visible(Ctx *c); hos_vr_equipment_visible(c); }"),
    # (OoT VR, vr/items/bomb/bomb_host.c hos_vr_bomb_ai) the entry of the bomb AI node's update (ExecuteBarrelBombAI,
    # f_826584b0, pacibsp): the bombs held in the VR hands are found here
    (0x26384b0, 0xd503237f, "{ extern void hos_vr_bomb_ai(Ctx *c); hos_vr_bomb_ai(c); }"),
    # (OoT VR, vr/items/hookshot/hookshot_host.c) the hookshot's aim: its line check object (vtable 0x84b8b440) is found in
    # the aim builder f_80629fa0, which copies the result into Link's aim (there x19, at ldrh w8, [x19, #148]); its update
    # (f_8300c340, the object in x19) reads the aim camera and takes at - position (fsub s2, s2, s10): the VR hand's line
    # instead, while a hand holds the hookshot
    (0x60a074, 0x79412a68, "{ extern void hos_vr_hook_query(Ctx *c); hos_vr_hook_query(c); }"),
    (0x2fec420, 0x1e2a3842, "{ extern void hos_vr_hook_aim(Ctx *c); hos_vr_hook_aim(c); }"),
    # ... and right after it stored the line's start (the game's hand: stp s1, s2, [x19, #168]): the VR hookshot's tip
    (0x2fed8a4, 0x2d150a61, "{ extern void hos_vr_hook_start(Ctx *c); hos_vr_hook_start(c); }"),
    # (OoT VR, vr/items/boomerang/boomerang_host.c) the boomerang's aim: in QueryGetCameraForwardDir's calc (f_82a2b220, the
    # node's actor in x21), right after f_82bb4c50 left the point ahead of the camera at sp+0x14 (add x0, x19, #0x20): the
    # VR aim's point instead, for the boomerang's throw
    (0x2a0b2a8, 0x91008260, "{ extern void hos_vr_boomerang_target(Ctx *c); hos_vr_boomerang_target(c); }"),
    # (OoT VR, vr/items/bottle/bottle_host.c hos_vr_bottle_keep_drawn) the equipment stow f_80e54c70(user, slot), right after
    # it loads the handle's generation (ldr w8, [x25, #0x10]): while a VR hand holds a bottle (Link's handle 1), a stow of
    # that handle is turned away (w8 = -1, the stow's own early return); everything else as the game has it
    (0xe34cb0, 0xb9401328, "{ extern void hos_vr_bottle_keep_drawn(Ctx *c); hos_vr_bottle_keep_drawn(c); }"),
    # (OoT VR, vr/items/sword/blade_hits.inc) a SensorHit's attack set up, f_808f29c0(sensorHit, info), right after its first
    # load (ldr w8, [x1, #0x4]: the info's AttackType), and one of its At sensors switched off, f_808f56f0(sensorHit, kind),
    # at its entry (pacibsp): Link's own are logged (bounded), the game's slash's and the VR blade's
    (0x8d29c0, 0xb9400428, "{ extern void hos_vr_sensor_setup_seen(Ctx *c); hos_vr_sensor_setup_seen(c); }"),
    (0x8d56f0, 0xd503237f, "{ extern void hos_vr_sensor_off_seen(Ctx *c); hos_vr_sensor_off_seen(c); }"),
    # (OoT VR, vr/items/sword/blade_hits.inc hos_vr_sensor_calc) the entry of the SensorHit calc f_82ec3ef0 (pacibsp, x0 the
    # SensorHit): Link's attack of the VR blade set up here, before the calc retires what was not set up this frame
    (0x2ea3ef0, 0xd503237f, "{ extern void hos_vr_sensor_calc(Ctx *c); hos_vr_sensor_calc(c); }"),
    # (OoT VR, vr/items/sword/blade_hits.inc hos_vr_action_request) the entry of f_800e08f0(Link's components +0x188, request,
    # 1) (pacibsp), what the AI node ExecuteBSASetActionVerb sends: Link's requests logged, his draw's kept to replay
    (0xc08f0, 0xd503237f, "{ extern void hos_vr_action_request(Ctx *c); hos_vr_action_request(c); }"),
    # (OoT VR, vr/items/sword/blade_hits.inc hos_vr_sub_request) the entry of f_82b635b0 (pacibsp), the request handler of
    # Link's SelectorPlayerSubSchedulerMain (x0 the selector, x1 the request): the selector kept for its forced change
    (0x2b435b0, 0xd503237f, "{ extern void hos_vr_sub_request(Ctx *c); hos_vr_sub_request(c); }"),
]

# (OoT VR, host/hos_main.c hos_vr_lod_depth) after each place that works out an object's depth along the view (fadd of the view
# snapshot's third row), the true distance from the eye instead, for the detail it picks next
_VR_DEPTH = [
    (0x8413c, 0x1e232800, 21, 22, 0),
    (0x1214d78, 0x1e232800, 26, 19, 0),
    (0x3608e84, 0x1e232800, 28, 25, 0),
    (0x3609210, 0x1e232800, 28, 25, 0),
    (0x36095a8, 0x1e232800, 28, 25, 0),
    (0x3609944, 0x1e232800, 28, 25, 0),
    (0x3609cc4, 0x1e232800, 28, 25, 0),
    (0x360a050, 0x1e232800, 28, 25, 0),
    (0x360a3f8, 0x1e232800, 23, 19, 0),
    (0x360a7a4, 0x1e232800, 23, 19, 0),
    (0x360ab24, 0x1e232800, 28, 25, 0),
    (0x360aeb0, 0x1e232800, 28, 25, 0),
    (0x360b29c, 0x1e232800, 22, 21, 0),
    (0x360b688, 0x1e232800, 22, 21, 0),
    (0x360ba14, 0x1e232800, 28, 25, 0),
    (0x360bda0, 0x1e232800, 28, 25, 0),
    (0x360c148, 0x1e232800, 23, 19, 0),
    (0x360c4f4, 0x1e232800, 23, 19, 0),
    (0x360c874, 0x1e232800, 28, 25, 0),
    (0x360cc70, 0x1e232800, 28, 25, 0),
    (0x360d078, 0x1e232800, 28, 25, 0),
    (0x360d484, 0x1e232800, 28, 25, 0),
    (0x360d874, 0x1e232800, 28, 25, 0),
    (0x360dc70, 0x1e232800, 28, 25, 0),
    (0x360e088, 0x1e232800, 19, 23, 0),
    (0x360e4b4, 0x1e232800, 19, 23, 0),
    (0x360e8b4, 0x1e232800, 28, 25, 0),
    (0x360ecb0, 0x1e232800, 28, 25, 0),
    (0x360f110, 0x1e232800, 22, 21, 0),
    (0x360f57c, 0x1e232800, 22, 21, 0),
    (0x360f974, 0x1e232800, 28, 25, 0),
    (0x360fd70, 0x1e232800, 28, 25, 0),
    (0x3610188, 0x1e232800, 19, 23, 0),
    (0x36105b4, 0x1e232800, 19, 23, 0),
    (0x36109b4, 0x1e232800, 20, 28, 0),
    (0x3610d50, 0x1e232800, 20, 28, 0),
    (0x36110f8, 0x1e232800, 20, 28, 0),
    (0x36114a4, 0x1e232800, 20, 28, 0),
    (0x3611844, 0x1e232800, 20, 28, 0),
    (0x3611be0, 0x1e232800, 20, 28, 0),
    (0x3611fa0, 0x1e232800, 23, 21, 0),
    (0x361238c, 0x1e232800, 23, 21, 0),
    (0x3612744, 0x1e232800, 20, 28, 0),
    (0x3612ae0, 0x1e232800, 20, 28, 0),
    (0x3612edc, 0x1e232800, 21, 20, 0),
    (0x36132e8, 0x1e232800, 21, 20, 0),
    (0x3613684, 0x1e232800, 20, 28, 0),
    (0x3613a20, 0x1e232800, 20, 28, 0),
    (0x3613de0, 0x1e232800, 23, 21, 0),
    (0x36141cc, 0x1e232800, 23, 21, 0),
    (0x3614584, 0x1e232800, 20, 28, 0),
    (0x36149a0, 0x1e232800, 20, 28, 0),
    (0x3614dc8, 0x1e232800, 20, 28, 0),
    (0x36151f4, 0x1e232800, 20, 28, 0),
    (0x3615604, 0x1e232800, 20, 28, 0),
    (0x3615a20, 0x1e232800, 20, 28, 0),
    (0x3615e58, 0x1e232800, 19, 21, 0),
    (0x36162a4, 0x1e232800, 19, 21, 0),
    (0x36166c4, 0x1e232800, 20, 28, 0),
    (0x3616ae0, 0x1e232800, 20, 28, 0),
    (0x3616f64, 0x1e232800, 21, 20, 0),
    (0x36173f0, 0x1e232800, 21, 20, 0),
    (0x3617804, 0x1e232800, 20, 28, 0),
    (0x3617c20, 0x1e232800, 20, 28, 0),
    (0x3618058, 0x1e232800, 19, 21, 0),
    (0x36184a4, 0x1e232800, 19, 21, 0),
    (0x36188f8, 0x1e232800, 21, 26, 0),
    (0x3618e04, 0x1e232800, 21, 26, 0),
    (0x361931c, 0x1e232800, 21, 26, 0),
    (0x3619838, 0x1e232800, 21, 26, 0),
    (0x3619d48, 0x1e232800, 21, 26, 0),
    (0x361a254, 0x1e232800, 21, 26, 0),
    (0x361aca0, 0x1e232800, 11, 19, 0),
    (0x361b1b8, 0x1e232800, 21, 26, 0),
    (0x361b6c4, 0x1e232800, 21, 26, 0),
    (0x361bc10, 0x1e232800, 20, 22, 0),
    (0x361c168, 0x1e232800, 20, 22, 0),
    (0x361c678, 0x1e232800, 21, 26, 0),
    (0x361cb84, 0x1e232800, 21, 26, 0),
    (0x361d5ac, 0x1e232800, 21, 26, 0),
    (0x361db24, 0x1e232800, 21, 26, 0),
    (0x361e0c0, 0x1e232800, 21, 26, 0),
    (0x361e658, 0x1e232800, 21, 26, 0),
    (0x361ebdc, 0x1e232800, 21, 26, 0),
    (0x361f154, 0x1e232800, 21, 26, 0),
    (0x361fca8, 0x1e232800, 11, 23, 0),
    (0x362023c, 0x1e232800, 21, 26, 0),
    (0x36207b4, 0x1e232800, 21, 26, 0),
    (0x3620d80, 0x1e232800, 20, 22, 0),
    (0x3621358, 0x1e232800, 20, 22, 0),
    (0x36218ec, 0x1e232800, 21, 26, 0),
    (0x3621e64, 0x1e232800, 21, 26, 0),
    (0x36229b8, 0x1e232800, 11, 23, 0),
    (0x3622f44, 0x1e232800, 21, 20, 0),
    (0x3623460, 0x1e232800, 21, 20, 0),
    (0x3623988, 0x1e232800, 21, 20, 0),
    (0x3623eb4, 0x1e232800, 21, 20, 0),
    (0x36243d4, 0x1e232800, 21, 20, 0),
    (0x36248f0, 0x1e232800, 21, 20, 0),
    (0x3624e10, 0x1e232800, 19, 25, 0),
    (0x362535c, 0x1e232800, 19, 25, 0),
    (0x36258a4, 0x1e232800, 21, 20, 0),
    (0x3625dc0, 0x1e232800, 21, 20, 0),
    (0x3626320, 0x1e232800, 25, 21, 0),
    (0x3626888, 0x1e232800, 25, 21, 0),
    (0x3626da4, 0x1e232800, 21, 20, 0),
    (0x36272c0, 0x1e232800, 21, 20, 0),
    (0x36277e0, 0x1e232800, 19, 25, 0),
    (0x3628350, 0x1e232800, 10, 26, 0),
    (0x3629c54, 0x1e232800, 21, 20, 0),
    (0x362a200, 0x1e232800, 21, 20, 0),
    (0x362a798, 0x1e232800, 21, 20, 0),
    (0x362ad54, 0x1e232800, 21, 20, 0),
    (0x362b2e4, 0x1e232800, 21, 20, 0),
    (0x362b890, 0x1e232800, 21, 20, 0),
    (0x362be38, 0x1e232800, 21, 25, 0),
    (0x362c414, 0x1e232800, 21, 25, 0),
    (0x362c9d4, 0x1e232800, 21, 20, 0),
    (0x362cf80, 0x1e232800, 21, 20, 0),
    (0x362d560, 0x1e232800, 25, 21, 0),
    (0x362db48, 0x1e232800, 25, 21, 0),
    (0x362e0d4, 0x1e232800, 21, 20, 0),
    (0x362e680, 0x1e232800, 21, 20, 0),
    (0x362ec28, 0x1e232800, 21, 25, 0),
    (0x362f204, 0x1e232800, 21, 25, 0),
    (0x362f928, 0x1e232800, 20, 19, 0),
    (0x362ffd0, 0x1e232800, 10, 24, 0),
    (0x363054c, 0x1e232800, 10, 22, 0),
    (0x3630c20, 0x1e232800, 10, 24, 0),
    (0x3631170, 0x1e232800, 20, 19, 0),
    (0x3631824, 0x1e232800, 10, 24, 0),
    (0x3631dbc, 0x1e232800, 10, 23, 0),
    (0x36324a4, 0x1e232800, 10, 24, 0),
    (0x3632a68, 0x1e232801, 19, 22, 1),
    (0x363314c, 0x1e232801, 8, 24, 1),
    (0x3633708, 0x1e232801, 23, 22, 1),
    (0x3633e1c, 0x1e232801, 8, 24, 1),
    (0x36343d8, 0x1e232801, 19, 22, 1),
    (0x3634adc, 0x1e232801, 8, 24, 1),
    (0x36350b8, 0x1e232801, 23, 22, 1),
    (0x36357ec, 0x1e232801, 8, 24, 1),
    (0x3635f28, 0x1e232800, 19, 28, 0),
    (0x36367a8, 0x1e232800, 28, 22, 0),
    (0x3636f00, 0x1e232800, 19, 24, 0),
    (0x36377b8, 0x1e232800, 28, 22, 0),
    (0x3637f0c, 0x1e232800, 19, 28, 0),
    (0x3638818, 0x1e232800, 28, 22, 0),
    (0x3638f90, 0x1e232800, 19, 24, 0),
    (0x36398b8, 0x1e232800, 28, 22, 0),
    (0x363a074, 0x1e232800, 24, 28, 0),
    (0x363a980, 0x1e232800, 9, 23, 0),
    (0x363b150, 0x1e232800, 19, 24, 0),
    (0x363ba80, 0x1e232800, 9, 23, 0),
    (0x363c268, 0x1e232800, 9, 28, 0),
    (0x363cbdc, 0x1e232800, 8, 23, 0),
    (0x363d3d0, 0x1e232800, 19, 24, 0),
    (0x363dd5c, 0x1e232800, 8, 23, 0),
    (0x363e32c, 0x1e232800, 10, 19, 0),
    (0x363e9b8, 0x1e232800, 10, 24, 0),
    (0x363ef08, 0x1e232800, 19, 23, 0),
    (0x363f5b8, 0x1e232800, 10, 24, 0),
    (0x363fafc, 0x1e232800, 10, 19, 0),
    (0x36401a8, 0x1e232800, 10, 24, 0),
    (0x3640728, 0x1e232800, 19, 23, 0),
    (0x3640df8, 0x1e232800, 10, 24, 0),
    (0x364136c, 0x1e232800, 10, 19, 0),
    (0x36419f8, 0x1e232800, 10, 24, 0),
    (0x3641f48, 0x1e232800, 19, 23, 0),
    (0x36425f8, 0x1e232800, 10, 24, 0),
    (0x3642b3c, 0x1e232800, 10, 19, 0),
    (0x36431e8, 0x1e232800, 10, 24, 0),
    (0x3643768, 0x1e232800, 19, 23, 0),
    (0x3643e38, 0x1e232800, 10, 24, 0),
    (0x3644558, 0x1e232800, 19, 22, 0),
    (0x3644dd0, 0x1e232800, 28, 24, 0),
    (0x3645524, 0x1e232800, 19, 28, 0),
    (0x3645dd0, 0x1e232800, 28, 24, 0),
    (0x3646508, 0x1e232800, 19, 22, 0),
    (0x3646e00, 0x1e232800, 28, 24, 0),
    (0x3647574, 0x1e232800, 19, 28, 0),
    (0x3647ea0, 0x1e232800, 28, 24, 0),
    (0x36485f8, 0x1e232800, 19, 22, 0),
    (0x3648e70, 0x1e232800, 28, 24, 0),
    (0x36495c4, 0x1e232800, 19, 28, 0),
    (0x3649e70, 0x1e232800, 28, 24, 0),
    (0x364a5a8, 0x1e232800, 19, 22, 0),
    (0x364aea0, 0x1e232800, 28, 24, 0),
    (0x364b614, 0x1e232800, 19, 28, 0),
    (0x364bf40, 0x1e232800, 28, 24, 0),
]

_VR_DEPTH_TEXT = ("{ extern int hos_vr_lod_depth(Ctx *c, uint64_t snap, uint64_t obj, float z, float *out); float _z, _o; "
                  "memcpy(&_z, c->vreg[%d], 4); if (hos_vr_lod_depth(c, c->x[%d], c->x[%d], _z, &_o)) { uint32_t _u; "
                  "memcpy(&_u, &_o, 4); a64_vset64(c->vreg[%d], _u); } }")
_VR_DEPTH += [(0x361a768, 0x1e232800, 23, 19, 0), (0x361d098, 0x1e232800, 23, 19, 0), (0x361f6e8, 0x1e232800, 21, 23, 0),
               (0x36223f8, 0x1e232800, 21, 23, 0)]
_SITES += tuple((_off, _word, _VR_DEPTH_TEXT % (_z, _s, _o, _z)) for _off, _word, _s, _o, _z in _VR_DEPTH)
# (OoT VR, host/hos_main.c hos_vr_tan_reader) the two readers of the view snapshot's tan(FOV/2) that light and shadow the scene
# keep the headset's: ldr s<d>, [x<b>, #0x1dc]
_VR_TAN = [(0x335ad0, 0xbd41de80, 2, 20, 0), (0x6bc924, 0xbd41dd08, 6, 8, 8)]
_VR_TAN_TEXT = ("{ extern int hos_vr_tan_reader(Ctx *c, uint64_t snap, int reader, float in, float *out); float _t, _o; "
                "memcpy(&_t, c->vreg[%d], 4); if (hos_vr_tan_reader(c, c->x[%d], %d, _t, &_o)) { uint32_t _u; "
                "memcpy(&_u, &_o, 4); a64_vset64(c->vreg[%d], _u); } }")
_SITES += tuple((_off, _word, _VR_TAN_TEXT % (_d, _b, _r, _d)) for _off, _word, _r, _b, _d in _VR_TAN)


def sites():
    """Every Hookshot VR hook, as nx2/oot_wide.py's SITES entries"""
    return tuple(_SITES)
