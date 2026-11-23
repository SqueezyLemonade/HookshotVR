/* Hookshot VR: an item get, first person (VR tab: Item get). Game side: says when an item get may play, for the renderer.

   A piece of vr/camera/view_host.c (so of host/hos_main.c), included there before the view's re-align, which calls it
   every first-person update with the playing event's name. Not compiled on its own.

   An item get plays the game's Dm_SY_Get for its item (picked up: a heart piece, a token, a first Deku Nut; also
   Dm_SY_GetInEvent, Dm_SY_Get_WithoutPlayer, Dm_SY_ShopGet), in it the game binds the item's model to Link (its
   EventTriggerModelBindForItemGetDemo) above his head and turns him round to show it to its camera (its AS
   Rotate_GetDemo). In first person his head is the headset's, so the item was over the player's head (player, 18 Nov).
   Dm_SY_Get is mostly a sub-flow, and the name read is the top event's (vr/input/stick_host.c oot_vr_event_name): the
   chests with an event of their own play it (KokiriForest_TreasureBox_NaviChatOff in Mido's house,
   Get_StoneOfForest_GetSword, InsideTheDekuTree_GetSlingShot: 61 such chests in romfs Banc, 21 Nov), and an NPC's gift
   plays the NPC's talk. So the renderer is told an item get may play while any event plays but a story cinematic or the
   ocarina, and knows the item by where the game holds it (vr/items/item_get/item_get.inc: a small model over Link's
   head). It says back when it drew one in front of the headset: the view's heading then keeps out of Link's turn
   (vr/camera/view_host.c, VR tab Item get view). */
#define kVrItemGetTellNs 100000000LL   /* (the renderer told again this often: it forgets after 250 ms) */
void rx_vr_item_get(int playing);     /* (vr/items/item_get/item_get_bridges_host.c) */
int rx_vr_item_get_shown(void);

/* An item get may play in this event (its name; "an event" when unreadable): not a story cinematic (Dm_Kokiri_0030), whose
   props are not the player's to look at, nor the ocarina's Dm_SY_Ocarina (the ocarina in the VR hands) */
static int oot_vr_item_get_event(const char *event) {
    return event[0] && !oot_vr_cinematic_name(event) && strncmp(event, "Dm_SY_Ocarina", 13);
}
/* every first-person update: event, the playing event's name ("" none). 1: the renderer has the item in front of you */
static int oot_vr_item_get_frame(const char *event) {
    static int told = -1, shown;
    static int64_t told_at;
    const int64_t now = hos_now_ns();
    const int playing = oot_vr_item_get_event(event);
    /* (its item kept from the game's near test meanwhile: vr/items/gear/gear_near_host.c) */
    if (playing) __atomic_store_n(&oot_vr_item_get_time, now, __ATOMIC_RELEASE);
    if (playing != told || now - told_at >= kVrItemGetTellNs) told = playing, told_at = now, rx_vr_item_get(playing);
    const int now_shown = playing && rx_vr_item_get_shown();
    if (now_shown != shown) hos_log("vr: item get %s%s%s", now_shown ? "shown in front of you (" : "over", now_shown ? event : "",
                                    now_shown ? ")" : "");
    return shown = now_shown;
}
