/* Hookshot VR: Owned gear near the eye (the sword in the VR hand, the Goron Bracelet on the VR wrist): the game's near-plane
   sphere test kept from rejecting it.

   Game-side code, included by host/hos_main.c (its VR HOOK block) in a fixed order: it uses that file's helpers and
   the files included before it. Not compiled on its own. */
/* Actual owned ModelNW culling path identified by hardware accesses, 14 Nov.
   Do NOT edit snapshot.near: depth/fog/other views can read that shared field.
   These hooks run AFTER ldr s3,[x9,#1c8] in f_8009bb80's two sphere branches.
   The native near-radius > -view_z test otherwise rejects the game-pose sword
   behind the headset, even though its replacement is in the VR hand. */
static volatile uint64_t oot_vr_gear_model[2];
static volatile int64_t oot_vr_gear_model_time;
static volatile uint64_t oot_vr_gear_hook_model;   /* (the held hookshot's model too, vr/items/hookshot/hookshot_host.c) */
static volatile int64_t oot_vr_gear_hook_time;
static uint64_t oot_vr_gear_load_ptr(Ctx *c, uint64_t p) {
    return p >= HOS_HEAP_BASE && !(p & 7) && hos_guest_range(p,8) ? recomp_load64(c,p) : 0;
}
static uint64_t oot_vr_gear_resolve(Ctx *c, uint64_t actor) {
    if (!actor) return 0;
    const uint64_t table=oot_vr_gear_load_ptr(c,actor+0x270);
    const uint64_t component=table ? oot_vr_gear_load_ptr(c,table+8) : 0;
    if (oot_vr_gear_load_ptr(c,component)!=0x84c81020ULL) return 0;
    const uint64_t resource=oot_vr_gear_load_ptr(c,component+0x18);
    if (oot_vr_gear_load_ptr(c,resource)!=0x84c288a0ULL) return 0;
    const uint64_t node=oot_vr_gear_load_ptr(c,resource+0x18);
    const uint64_t model=oot_vr_gear_load_ptr(c,node);
    return oot_vr_gear_load_ptr(c,model)==0x84c28cc8ULL ? model : 0;
}
/* The sword and shield are kept too while a VR hand holds the sword taken from the back (vr/input/controllers_host.c), drawn
   in the game or not: before the first swing the game has them on Link's back, behind the headset, where this test
   dropped them, so both VR hands showed nothing until a swing drew them (headset 18 Nov, child Link's Kokiri Sword and
   Deku Shield). */
int hos_vr_sword_holder(void);
/* Child Link's Goron Bracelet (worn, not held) is kept too: the renderer pins it to the VR arm
   (vr/items/gear/equipment_skin.inc, bracelet_skin), but the game still tests it where ITS arm is, at the hips in first
   person, so looking up put it behind the near plane and the bracelet on the VR wrist vanished. Its actor is found by
   vr/items/gear/equipment_host.c (0: none, or RX_VR_NO_BRACELET=1). */
static volatile uint64_t oot_vr_gear_worn_model;
static volatile int64_t oot_vr_gear_worn_time;
static void oot_vr_gear_worn_publish(Ctx *c, uint64_t actor) {
    const uint64_t model = actor ? oot_vr_gear_resolve(c, actor) : 0;
    static uint64_t last_actor, last_model;   /* (the log's: once per change) */
    static unsigned told;
    if ((actor != last_actor || model != last_model) && told < 16) {
        told++;
        if (actor) hos_log("vr: Goron Bracelet actor %#llx worn, its model %#llx%s", (unsigned long long)actor,
                           (unsigned long long)model, model ? " kept from the near test in first person" : " NOT resolved: the game's near test stays");
        else hos_log("vr: Goron Bracelet not worn (no Armor_Child_Gauntlet_Goron among Link's equipment)");
    }
    last_actor = actor, last_model = model;
    __atomic_store_n(&oot_vr_gear_worn_model, model, __ATOMIC_RELEASE);
    __atomic_store_n(&oot_vr_gear_worn_time, hos_now_ns(), __ATOMIC_RELEASE);
}
/* ---- Whatever a VR hand holds (VR tab Held items near the face, option 1002) -------------------------------------------
   The bottle and the slingshot vanished held close to the face or high, the ocarina at some angles at the mouth (headset 18
   Nov). The game tests each model where IT has it: an item the mod moves to the VR hand (bombs, a bottle, a Deku Stick, the
   Boomerang, the sword: every one goes through vr/items/bomb/bomb_host.c vr_bomb_place) is then a few cm from the eye,
   closer than the near plane; one the renderer draws in the VR hand from the game's pose (the bow and slingshot, the
   ocarina and its holder) is tested at the game's hands, at Link's face or behind it. The culling planes and the frustum's
   box already keep a 1.5 m ball around the eye (vr/camera/culling_host.c); the near test below did not, except for the
   sword, shield, hookshot and bracelet. So each update while a VR hand holds an item, its code calls hos_vr_held_keep with
   the item's actor, and the model found from it is kept from the near test for kVrHeldFreshNs: the same rescue, the same
   first-person and headset-view gates. Each item's model is logged once (vr: held item ... kept / NOT resolved). */
#define kVrHeldKept 8                   /* (models at once: two hands, a bow and its arrow, the ocarina and its holder) */
#define kVrHeldFreshNs 250000000LL
static volatile uint64_t oot_vr_held_model[kVrHeldKept];
static volatile int64_t oot_vr_held_time[kVrHeldKept];
static void oot_vr_equipment_name(Ctx *c, uint64_t actor, uint64_t offset, char name[64]);   /* (equipment_host.c) */
void hos_vr_held_keep(Ctx *c, uint64_t actor) {
    if (!actor || !rx_option(1002, 1)) return;
    const int64_t now = hos_now_ns();
    const uint64_t model = oot_vr_gear_resolve(c, actor);
    static uint64_t told[24];   /* (each actor's result once, bounded) */
    static unsigned n;
    int known = 0;
    for (unsigned i = 0; i < n && !known; i++) known = told[i] == actor;
    if (!known && n < 24) {
        char name[64];
        oot_vr_equipment_name(c, actor, 0x260, name);
        if (!name[0]) oot_vr_equipment_name(c, actor, 0x20, name);
        told[n++] = actor;
        hos_log("vr: held item %s %#llx: %s", name[0] ? name : "?", (unsigned long long)actor,
                model ? "its model kept from the near test in first person" : "its model NOT resolved: the game's near test stays");
    }
    if (!model) return;
    int slot = 0;   /* (its own slot, else the stalest) */
    for (int i = 0; i < kVrHeldKept; i++) {
        if (__atomic_load_n(&oot_vr_held_model[i], __ATOMIC_RELAXED) == model) { slot = i; break; }
        if (__atomic_load_n(&oot_vr_held_time[i], __ATOMIC_RELAXED) < __atomic_load_n(&oot_vr_held_time[slot], __ATOMIC_RELAXED)) slot = i;
    }
    __atomic_store_n(&oot_vr_held_model[slot], model, __ATOMIC_RELAXED);
    __atomic_store_n(&oot_vr_held_time[slot], now, __ATOMIC_RELEASE);
}
/* the model is one kept by hos_vr_held_keep lately */
static int oot_vr_held_kept(uint64_t model) {
    const int64_t now = hos_now_ns();
    for (int i = 0; i < kVrHeldKept; i++)
        if (__atomic_load_n(&oot_vr_held_model[i], __ATOMIC_ACQUIRE) == model) {
            const int64_t at = __atomic_load_n(&oot_vr_held_time[i], __ATOMIC_ACQUIRE);
            return now >= at && now - at < kVrHeldFreshNs;
        }
    return 0;
}

/* An item get's item (vr/items/item_get): the renderer draws it in front of the headset, but the game tests it where IT has
   it, over Link's head: above and behind the eye once the player looks down, so the near test dropped it and the item
   showed only when looking above eye level (headset 21 Nov, a rupee picked up). While an item get may play
   (vr/items/item_get/item_get_host.c sets oot_vr_item_get_time), a small model's sphere within kVrItemGetKeepReach of the
   eye and kVrItemGetKeepAbove over it is kept from the near test as the held gear is. */
#define kVrItemGetKeepReach 1.5f    /* metres from the eye (the rescue's band) */
#define kVrItemGetKeepAbove 0.1f    /* metres over the eye: the item's spot is over Link's head */
#define kVrItemGetKeepRadius 0.6f   /* metres: a sphere this big or more is not an item */
static volatile int64_t oot_vr_item_get_time;
static int oot_vr_item_get_kept(Ctx *c, uint64_t sphere) {
    const int64_t at = __atomic_load_n(&oot_vr_item_get_time, __ATOMIC_ACQUIRE), now = hos_now_ns();
    if (!at || now < at || now - at > 250000000LL || !hos_guest_range(sphere, 16)) return 0;
    float p[3], d2 = 0;
    for (int k = 0; k < 3; k++) p[k] = vr_load(c, sphere + 4 * k), d2 += (p[k] - hos_vr_cull_eye[k]) * (p[k] - hos_vr_cull_eye[k]);
    const float r = vr_load(c, sphere + 12);
    return isfinite(d2) && isfinite(r) && r < kVrItemGetKeepRadius && p[1] > hos_vr_cull_eye[1] + kVrItemGetKeepAbove &&
           d2 < kVrItemGetKeepReach * kVrItemGetKeepReach;
}

static void oot_vr_gear_cull_publish(Ctx *c, uint32_t known, uint32_t drawn, const uint64_t owners[3]) {
    const uint32_t held = hos_vr_sword_holder() >= 0 ? 3u : 0u;
    for (int i=0;i<2;i++) {
        const uint64_t model=(known & (drawn | held) & (1u<<i)) ? oot_vr_gear_resolve(c,owners[i]) : 0;
        __atomic_store_n(&oot_vr_gear_model[i],model,__ATOMIC_RELEASE);
    }
    __atomic_store_n(&oot_vr_gear_model_time,hos_now_ns(),__ATOMIC_RELEASE);
}
void hos_vr_gear_near(Ctx *c) {
    const uint64_t model=c->x[19], snap=c->x[9], sphere=c->x[25];
    /* (every model's sphere passes these two hooks: the world-culling watch notes which reached the view test,
       vr/camera/culling_host.c; it only reads) */
    hos_vr_cull_model_seen(c,snap,sphere,model);
    const int hook=model && model==__atomic_load_n(&oot_vr_gear_hook_model,__ATOMIC_ACQUIRE);
    const int worn=model && !hook && model==__atomic_load_n(&oot_vr_gear_worn_model,__ATOMIC_ACQUIRE);
    const int held=model && !hook && !worn && oot_vr_held_kept(model);   /* (fresh already: hos_vr_held_keep) */
    const int item=model && !hook && !worn && !held && oot_vr_item_get_kept(c,sphere);   /* (fresh already) */
    if (!model || (!hook && !worn && !held && !item && model!=__atomic_load_n(&oot_vr_gear_model[0],__ATOMIC_ACQUIRE) &&
                   model!=__atomic_load_n(&oot_vr_gear_model[1],__ATOMIC_ACQUIRE))) return;
    const int64_t age=held || item ? 0 : hos_now_ns()-__atomic_load_n(hook ? &oot_vr_gear_hook_time : worn ? &oot_vr_gear_worn_time :
                                                              &oot_vr_gear_model_time,__ATOMIC_ACQUIRE);
    if (age<0 || age>250000000LL || !__atomic_load_n(&hos_vr_cull_first_person,__ATOMIC_ACQUIRE) ||
        !hos_vr_snapshot_ours(c,snap) || !hos_guest_range(sphere,16) ||
        oot_vr_gear_load_ptr(c,model)!=0x84c28cc8ULL) return;
    HookshotVrHead h;
    if (!rx_vr_head(&h) || !h.active || h.mode!=1) return;
    float ds=0;
    for (int k=0;k<3;k++) { const float d=vr_load(c,snap+0xe4+16*k)-hos_vr_cull_eye[k]; ds+=d*d; }
    if (!(ds<0.25f)) return;
    float near, z; memcpy(&near,c->vreg[3],4); memcpy(&z,c->vreg[1],4);
    const float radius=vr_load(c,sphere+12), relaxed=-1.5f;
    if (!(isfinite(near) && near>0 && isfinite(z) && isfinite(radius) && radius>=0)) return;
    /* Only rescue a native scalar-near rejection within the existing finite guard band. */
    if (!(near-radius > -z && relaxed-radius <= -z)) return;
    memcpy(c->vreg[3],&relaxed,4);
    static volatile unsigned reports;
    if (__atomic_fetch_add(&reports,1,__ATOMIC_RELAXED)<128)
        hos_log("vr: owned gear scalar-near rescue model %#llx near %.3f depth %.3f radius %.3f",
                (unsigned long long)model,near,-z,radius);
}
