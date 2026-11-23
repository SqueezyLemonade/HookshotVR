/* Hookshot VR: The game side's calls into the renderer's VR code (gpu side, vr/headset/vk_vr.cpp): the head pose, the
   hands, the bow, rumble, the drawn camera and the equipment state.

   A piece of host/rx_backend.c, included there at its VR HOOK (it uses that file's helpers). Not compiled on its own. */
/* (OoT VR) the back end's headset (vr/headset/vk_vr.cpp), when it has one: the head's pose, and which one a camera was built with */
typedef struct { uint32_t seq; int active, mode, move_head, level, height_cm, body_depth_cm; float q[4], p[3]; float lean, tan_half_x, tan_half_y; int detail, foliage; } HookshotVrHead;
int rx_vr_head(HookshotVrHead *out) {
    static int (*fn)(HookshotVrHead *);
    static int looked;
    if (!looked && vk_module) fn = (int (*)(HookshotVrHead *))(void *)GetProcAddress(vk_module, "hookshot_vr_head"), looked = 1;
    return fn ? fn(out) : 0;
}
/* (OoT VR) the headset shows the game: the player is in the headset, not at the desktop, so the controllers count even with
   the game's window behind other windows (host/nn_hid.c, gpu/vk/vk_menu.cpp). The keyboard and mouse stay with the window */
int rx_vr_presenting(void) {
    HookshotVrHead h;
    return rx_vr_head(&h) && h.active;
}
/* (OoT VR) the Quest/OpenXR controllers, in the headset's recentred frame */
typedef struct {
    int valid;
    float grip_q[4], grip_p[3], aim_q[4], aim_p[3], lin_vel[3], ang_vel[3];
    float trigger, squeeze, stick[2];
    uint32_t buttons;
} HookshotVrHand;
typedef struct { uint32_t seq; HookshotVrHand hand[2]; } HookshotVrHands;
int rx_vr_hands(HookshotVrHands *out) {
    static int (*fn)(HookshotVrHands *);
    static int looked;
    if (!looked && vk_module) fn = (int (*)(HookshotVrHands *))(void *)GetProcAddress(vk_module, "hookshot_vr_hands"), looked = 1;
    return fn ? fn(out) : 0;
}
/* (OoT VR) the VR menu (vr/menu/vr_menu.inc): toggle 1 opens or closes it, 0 only asks; open now, 1, or not, 0 */
int rx_vr_menu(int toggle) {
    static int (*fn)(int);
    static int looked;
    if (!looked && vk_module) fn = (int (*)(int))(void *)GetProcAddress(vk_module, "hookshot_vr_menu"), looked = 1;
    return fn ? fn(toggle) : 0;
}
/* (OoT VR) the game's full-screen menus, by their screens' updates (host/oot_wheel.c hos_wheel_menu_tick sees every menu
   screen's update, by its class): UICommonScreen, the menus' common screen (the title, the file select and the pause menu
   update it, play does not: headless 22 Nov), the pause menu's (UIPlusMenuScreen, UIMenuOverlayScreen over it) and the quick
   select's. Up
   while one of them updated within 150 ms. Each other class is logged once, with the second it was first seen, to tell the title's and the
   file select's apart (research, bounded) */
/* (and the item quick select's: its row, ITEM_CHOICE_VTABLE in host/oot_wheel.c, and its background, UIShortCutBGScreen
   (nx2/oot_wide.py KEEP_VTABLES 0). Opened from the pad it was a gameplay frame the game had stopped drawing a camera for,
   shown centred on the head, and its background, now in the band, short of the picture's bottom; headset 22 Nov) */
static const uint64_t vr_menu_screens[] = {0x84c04fb0ULL, 0x84c0baa8ULL, 0x84c08aa0ULL, 0x84c0d430ULL, 0x84c0d098ULL};
static volatile int64_t vr_menu_at;
void hos_vr_menu_screen_seen(uint64_t vt) {
    for (unsigned i = 0; i < sizeof vr_menu_screens / sizeof vr_menu_screens[0]; i++)
        if (vt == vr_menu_screens[i]) {
            __atomic_store_n(&vr_menu_at, hos_now_ns(), __ATOMIC_RELEASE);
            /* (the headset's FOV and shape off the menu's 3D cameras at once: the view hook, which also says so
               (vr/camera/view_host.c), may not run while the game is paused) */
            extern volatile float hos_vr_fov_y, hos_vr_aspect;
            hos_vr_fov_y = hos_vr_aspect = 0;
        }
    static uint64_t seen[48];
    static int64_t last[48];
    static volatile int n_seen, lock, told;
    if (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) return;
    static int64_t t0;
    const int64_t now = hos_now_ns();
    if (!t0) t0 = now;
    int k = -1;
    for (int i = 0; i < n_seen && k < 0; i++) if (seen[i] == vt) k = i;
    if (k < 0 && n_seen < 48) k = n_seen++, seen[k] = vt, last[k] = 0;
    if (k >= 0) {
        if ((!last[k] || now - last[k] > 1000000000LL) && told < 60)
            told++, hos_log("vr: menu screen %#llx updates %s at %.1f s", (unsigned long long)vt, last[k] ? "again" : "first",
                            (double)(now - t0) / 1e9);
        last[k] = now;
    }
    __atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
}
int hos_vr_menu_now(void) {
    const int64_t at = __atomic_load_n(&vr_menu_at, __ATOMIC_ACQUIRE);
    return at && hos_now_ns() - at < 150000000LL;
}
/* (OoT VR) each present: a full-screen menu is up (hos_vr_menu_now), its frame shown flat as the picture's middle 16:9
   (vr/headset/frame_capture.inc menu_tick) */
void rx_vr_menu_screen(int menu) {
    static void (*fn)(int);
    static int looked;
    if (!looked && vk_module) fn = (void (*)(int))(void *)GetProcAddress(vk_module, "hookshot_vr_menu_screen"), looked = 1;
    if (fn) fn(menu);
}
/* (OoT VR) VR was asked for at start (the headset's instance made): host_wide_menus has the game draw every menu in the
   picture's middle 16:9 */
int rx_vr_wanted(void) {
    static int (*fn)(void);
    static int looked;
    if (!looked && vk_module) fn = (int (*)(void))(void *)GetProcAddress(vk_module, "hookshot_vr_wanted"), looked = 1;
    return fn ? fn() : 0;
}
/* (OoT VR) Link's first-person root and age, associated with the camera pose above. */
void rx_vr_body(uint32_t seq, int active, int child, const float root[3], const float right[3], const float back[3]) {
    static void (*fn)(uint32_t, int, int, const float *, const float *, const float *);
    static int looked;
    if (!looked && vk_module) fn = (void (*)(uint32_t, int, int, const float *, const float *, const float *))(void *)GetProcAddress(vk_module, "hookshot_vr_body"), looked = 1;
    if (fn) fn(seq, active, child, root, right, back);
}
/* (OoT VR) a crawl, told just before the body of the same pose (vr/camera/crawl_host.c): how far into it, how far the
   view came down (m) */
void rx_vr_crawl(uint32_t seq, float amount, float lowered) {
    static void (*fn)(uint32_t, float, float);
    static int looked;
    if (!looked && vk_module) fn = (void (*)(uint32_t, float, float))(void *)GetProcAddress(vk_module, "hookshot_vr_crawl"), looked = 1;
    if (fn) fn(seq, amount, lowered);
}
/* (OoT VR) a buzz in one controller (0 left, 1 right): the recorders' start and end */
void rx_vr_rumble(int hand, float amplitude, float seconds) {
    static void (*fn)(int, float, float);
    static int looked;
    if (!looked && vk_module) fn = (void (*)(int, float, float))(void *)GetProcAddress(vk_module, "hookshot_vr_rumble"), looked = 1;
    if (fn) fn(hand, amplitude, seconds);
}
/* (OoT VR) the way Link's first-person torso faces (world yaw), while the renderer poses his tracked body */
int rx_vr_body_heading(float *yaw) {
    static int (*fn)(float *);
    static int looked;
    if (!looked && vk_module) fn = (int (*)(float *))(void *)GetProcAddress(vk_module, "hookshot_vr_body_heading"), looked = 1;
    return fn ? fn(yaw) : 0;
}
/* (OoT VR) both hands in the game world (per hand: valid, position, velocity, trigger, grip, the grip's axes, angular
   velocity), while the tracked body is posed */
int rx_vr_hands_world(float out[42]) {
    static int (*fn)(float *);
    static int looked;
    if (!looked && vk_module) fn = (int (*)(float *))(void *)GetProcAddress(vk_module, "hookshot_vr_hands_world"), looked = 1;
    return fn ? fn(out) : 0;
}
/* (OoT VR) a bomb of this radius (m, as drawn) is in this VR hand (0 left, 1 right), or 0, and where the game has its
   centre (world, m; may be NULL): the hand's grip and the drawn bomb's lock (every game frame) */
void rx_vr_hand_holds(int hand, float radius, const float centre[3]) {
    static void (*fn)(int, float, const float *);
    static int looked;
    if (!looked && vk_module)
        fn = (void (*)(int, float, const float *))(void *)GetProcAddress(vk_module, "hookshot_vr_hand_holds"), looked = 1;
    if (fn) fn(hand, radius, centre);
}
/* (OoT VR) a hand's pointing line in the game world (origin xyz, unit direction xyz: the controller's aim pose), whatever it
   holds: the bombs' throw arc and gravity pull (vr/items/bomb/bomb_host.c) */
int rx_vr_hand_ray(int hand, float out[6]) {
    static int (*fn)(int, float *);
    static int looked;
    if (!looked && vk_module) fn = (int (*)(int, float *))(void *)GetProcAddress(vk_module, "hookshot_vr_hand_ray"), looked = 1;
    return fn ? fn(hand, out) : 0;
}
/* (OoT VR) the bombs' marks in the headset (vr/items/bomb/bomb_marks.inc), every game frame: n marks of 5 floats (kind, x, y,
   z, size; kind 1 the throw arc's dots, 2 where it lands: Arc to target, parked) */
void rx_vr_bomb_marks(const float *marks, int n) {
    static void (*fn)(const float *, int);
    static int looked;
    if (!looked && vk_module) fn = (void (*)(const float *, int))(void *)GetProcAddress(vk_module, "hookshot_vr_bomb_marks"), looked = 1;
    if (fn) fn(marks, n);
}
/* (OoT VR) the Comfort Aids' causes now (vr/comfort/comfort_host.c -> vr/comfort/vignette_layer.inc), every game frame in first
   person: moving, turning, in the air, the game moving Link, each 0 or 1 */
void rx_vr_comfort(const float cause[4]) {
    static void (*fn)(const float *);
    static int looked;
    if (!looked && vk_module) fn = (void (*)(const float *))(void *)GetProcAddress(vk_module, "hookshot_vr_comfort"), looked = 1;
    if (fn) fn(cause);
}
/* (OoT VR) the bombs' state for the renderer, every game frame (vr/items/bomb/bomb_host.c): per hand what it holds (0
   none, 1 a bomb, 2 a Bomb Flower), per hand whether Link's lift has it, the bomb the pull would take (its kind, 0 none,
   and its centre: world, m) */
void rx_vr_bomb_state(const float state[8]) {
    static void (*fn)(const float *);
    static int looked;
    if (!looked && vk_module) fn = (void (*)(const float *))(void *)GetProcAddress(vk_module, "hookshot_vr_bomb_state"), looked = 1;
    if (fn) fn(state);
}
/* (OoT VR) a Bomb Flower fruit's body, from your game's model (vr/tools/generate/bomb_flower.py): its centre above the
   actor's origin (x y z) and its radius, m: how a VR hand holds one (vr/items/bomb/bomb_host.c) */
int rx_vr_flower_shape(float out[4]) {
    static int (*fn)(float *);
    static int looked;
    if (!looked && vk_module) fn = (int (*)(float *))(void *)GetProcAddress(vk_module, "hookshot_vr_flower_shape"), looked = 1;
    return fn ? fn(out) : 0;
}
/* (OoT VR) the hand holding the sword taken from the back (0 left, 1 right, -1 none: vr/input/controllers_host.c), every
   input read */
void rx_vr_sword_hand(int hand) {
    static void (*fn)(int);
    static int looked;
    if (!looked && vk_module) fn = (void (*)(int))(void *)GetProcAddress(vk_module, "hookshot_vr_sword_hand"), looked = 1;
    if (fn) fn(hand);
}
/* (OoT VR) whether the game guards (its ZL held), every input read: vr/input/controllers_host.c; the held shield faces
   forward then (vr/items/gear/held_gear.inc) */
void rx_vr_shield_guard(int guarding) {
    static void (*fn)(int);
    static int looked;
    if (!looked && vk_module) fn = (void (*)(int))(void *)GetProcAddress(vk_module, "hookshot_vr_shield_guard"), looked = 1;
    if (fn) fn(guarding);
}
/* (OoT VR) the hand holding the Deku Stick (0 left, 1 right, -1 none, -2 let go and being put away:
   vr/input/controllers_host.c vr_stick), when it changes and every 0.1 s */
void rx_vr_stick_hand(int hand) {
    static void (*fn)(int);
    static int looked;
    if (!looked && vk_module) fn = (void (*)(int))(void *)GetProcAddress(vk_module, "hookshot_vr_stick_hand"), looked = 1;
    if (fn) fn(hand);
}
/* (OoT VR) the hand holding a bottle (0 left, 1 right, -1 none, -2 let go and being put away: vr/items/bottle/bottle_host.c),
   when it changes and every 0.1 s */
void rx_vr_bottle_hand(int hand) {
    static void (*fn)(int);
    static int looked;
    if (!looked && vk_module) fn = (void (*)(int))(void *)GetProcAddress(vk_module, "hookshot_vr_bottle_hand"), looked = 1;
    if (fn) fn(hand);
}
/* (OoT VR) the held bottle corked (1) or open (0): vr/items/bottle/bottle_host.c, every game frame while a hand holds it */
void rx_vr_bottle_cork(int corked) {
    static void (*fn)(int);
    static int looked;
    if (!looked && vk_module) fn = (void (*)(int))(void *)GetProcAddress(vk_module, "hookshot_vr_bottle_cork"), looked = 1;
    if (fn) fn(corked);
}
/* (OoT VR) the headset's position in the game world (vr/items/bottle/bottle_host.c: a bottle held to the mouth), 0 without */
int rx_vr_eye_world(float out[3]) {
    static int (*fn)(float *);
    static int looked;
    if (!looked && vk_module) fn = (int (*)(float *))(void *)GetProcAddress(vk_module, "hookshot_vr_eye_world"), looked = 1;
    return fn ? fn(out) : 0;
}
/* (OoT VR) coherent first-person bow solution, supplied by the replay backend. */
int rx_vr_bow(float out[12]) {
    static int (*fn)(float *);
    static int looked;
    if (!looked && vk_module) fn = (int (*)(float *))(void *)GetProcAddress(vk_module, "hookshot_vr_bow"), looked = 1;
    return fn ? fn(out) : 0;
}
void rx_vr_bow_active(int active) {
    static void (*fn)(int);
    static int looked;
    if (!looked && vk_module) fn = (void (*)(int))(void *)GetProcAddress(vk_module, "hookshot_vr_bow_active"), looked = 1;
    if (fn) fn(active);
}
/* (OoT VR) the hookshot: the controller's line in the game world (origin xyz, unit direction xyz), and whether a VR hand
   holds it (the game's flat reticle hidden): vr/items/hookshot/hookshot_host.c */
int rx_vr_hook_ray(int hand, float out[6]) {
    static int (*fn)(int, float *);
    static int looked;
    if (!looked && vk_module) fn = (int (*)(int, float *))(void *)GetProcAddress(vk_module, "hookshot_vr_hook_ray"), looked = 1;
    return fn ? fn(hand, out) : 0;
}
void rx_vr_hook_active(int active) {
    static void (*fn)(int);
    static int looked;
    if (!looked && vk_module) fn = (void (*)(int))(void *)GetProcAddress(vk_module, "hookshot_vr_hook_active"), looked = 1;
    if (fn) fn(active);
}
/* (OoT VR) the boomerang (vr/items/boomerang/boomerang_host.c): the hand that holds its model (0 left, 1 right, -1 none, -2
   hidden: being put away), and its reticle's place in the game world while it aims */
void rx_vr_boomerang(int hand, int reticle, const float at[3]) {
    static void (*fn)(int, int, const float *);
    static int looked;
    if (!looked && vk_module) fn = (void (*)(int, int, const float *))(void *)GetProcAddress(vk_module, "hookshot_vr_boomerang"), looked = 1;
    if (fn) fn(hand, reticle, at);
}
/* (OoT VR) the third-person aim (vr/items/aim/third_person_aim_host.c): where the shot will land in the game world, every game
   frame while an item aims in third person (kind 1 shown there, 0 not aiming) */
void rx_vr_aim_mark(int kind, const float at[3]) {
    static void (*fn)(int, const float *);
    static int looked;
    if (!looked && vk_module) fn = (void (*)(int, const float *))(void *)GetProcAddress(vk_module, "hookshot_vr_aim_mark"), looked = 1;
    if (fn) fn(kind, at);
}
/* (OoT VR) the radial menu's state (vr/menu/radial_host.c -> vr/menu/radial_draw.inc): open side (-1 closed), count, highlighted
   slot, current slot, then the shown items (vr/menu/radial_items.h indexes) */
int rx_vr_radial(const int32_t *data, int n) {
    static void (*fn)(const int32_t *, int);
    static int looked;
    if (!looked && vk_module) fn = (void (*)(const int32_t *, int))(void *)GetProcAddress(vk_module, "hookshot_vr_radial"), looked = 1;
    if (fn) fn(data, n);
    return fn != 0;
}
/* (OoT VR) the wrist check HUD's values and why it may not show (vr/menu/wrist_host.c -> vr/menu/wrist_draw.inc) */
int rx_vr_wrist(const int32_t *data, int n) {
    static void (*fn)(const int32_t *, int);
    static int looked;
    if (!looked && vk_module) fn = (void (*)(const int32_t *, int))(void *)GetProcAddress(vk_module, "hookshot_vr_wrist"), looked = 1;
    if (fn) fn(data, n);
    return fn != 0;
}
/* (OoT VR) the Bool flags (their MurmurHash3s) the wrist HUD's map pictures are shown by, at most max: how many */
int rx_vr_wrist_flags(uint32_t *hashes, int max) {
    static int (*fn)(uint32_t *, int);
    static int looked;
    if (!looked && vk_module) fn = (int (*)(uint32_t *, int))(void *)GetProcAddress(vk_module, "hookshot_vr_wrist_flags"), looked = 1;
    return fn ? fn(hashes, max) : 0;
}
/* (OoT VR) owned sword/shield state, captured on the game thread, not guessed from rendering. */
void rx_vr_equipment(uint32_t seq, uint32_t known, uint32_t drawn, const uint64_t owners[2]) {
    static void (*fn)(uint32_t, uint32_t, uint32_t, const uint64_t *);
    static int looked;
    if (!looked && vk_module) fn = (void (*)(uint32_t, uint32_t, uint32_t, const uint64_t *))(void *)GetProcAddress(vk_module, "hookshot_vr_equipment"), looked = 1;
    if (fn) fn(seq, known, drawn, owners);
}
/* (OoT VR) the view the game built a gameplay camera with, for the pose it was built from (seq) */
void rx_vr_drawn(uint32_t seq, const float view[12]) {
    static void (*fn)(uint32_t, const float *);
    static int looked;
    if (!looked && vk_module) fn = (void (*)(uint32_t, const float *))(void *)GetProcAddress(vk_module, "hookshot_vr_drawn"), looked = 1;
    if (fn) fn(seq, view);
}
#include "../items/ocarina/ocarina_bridges_host.c"   /* (the ocarina: its holding hand and its place on the VR belt) */
#include "../items/item_get/item_get_bridges_host.c"   /* (an item get plays: the item in front of the headset) */
