/* Hookshot VR: a bottle's controls in the VR hands (first person, VR tab: Bottles in the hands, option 970).

   A piece of vr/input/controllers_host.c (so of host/nn_hid.c), included there after vr_stick: it uses that file's
   HookshotVrHand reads and the bridges. Not compiled on its own. The game side is vr/items/bottle/bottle_host.c (in
   host/nn_hid.c's neighbour host/hos_main.c): it draws the game's own bottle and places it on the hand that holds it.

   With a bottle selected (the VR radial menu, or the game's own):
   - Take: a grip, either hand (grip holds, the same for every item), not one behind the shoulder that is the sword's
     or one near a bomb. The game side then draws the selected bottle (the game's own actor for its content) into
     that hand. Letting go of the grip puts it away (the game side).
   - The game's item button (ZR, the right trigger) reaches the game from no other trigger while a bottle is selected in
     first person, and from none while no hand holds it.
   - The simple bottle (the default): the holding hand's trigger uses it (bottle_host.c vr_bottle_simple_update). Its
     press is decided once, as it goes down: a drink, a fairy or a Poe is asked of the game side (hos_vr_bottle_ask_use:
     the game's calls, no event) and the press reaches the game as nothing; anything else (the empty bottle's catch, a
     bug, a fish, Blue Fire, the letter, a Big Poe: hos_vr_bottle_to_game) is the game's own bottle use, so the press
     reaches the game as its item button, ZR (the player's own press, as the hookshot's trigger), whichever hand holds
     the bottle: first the game side hands the bottle back to the game (hos_vr_bottle_ask_game_use: the game ignores ZR
     while we hold it out), and once it has it (hos_vr_bottle_game_ready) ZR is pressed for kVrBottleZrNs, a tap: the
     game's bottle use is one press (headless 22 Nov: an 8-frame ZR swung and caught a fish; ZR held on put Link in a held
     stance, action 0x408 with the shield out, that the bow's controls then took for an aimed bow and held on).
   - The full bottle (RX_VR_BOTTLE_FULL=1, on hold): ZR never reaches the game while a bottle is selected (the VR hands'
     own actions replace the game's use). While a hand holds the bottle, the other hand's grip is its shoulder button
     again (lock-on, shield), except at the cork of a corked bottle (hos_vr_bottle_at_cork): there its grip and trigger
     pull the cork, and reach the game as nothing. The holding hand's trigger pulls the cork too (bottle_host.c), so it is
     never the game's ZL or ZR.
   The renderer is told the holding hand (vr::bottle_hand: 0 left, 1 right, -1 none, -2 let go and being put away)
   through rx_vr_bottle_hand (vr/headset/bridges_host.c), when it changes and every kVrBottleTellNs. */
#define kVrBottleTellNs 100000000LL     /* (told again this often: the renderer forgets it after 250 ms) */
#define kVrBottleZrNs 130000000LL       /* (the game's ZR tapped this long once the bottle is the game's again: 8 frames) */
#define kVrBottleHandNs 1500000000LL    /* (a press waits this long for the bottle to be handed back, then is dropped) */
void rx_vr_bottle_hand(int hand);      /* (vr/headset/bridges_host.c) */
int hos_vr_bottle_selected(void);       /* (vr/items/bottle/bottle_host.c: a bottle is the selected item) */
int hos_vr_bottle_away(void);           /* (the same: let go, the game still has it out: hidden meanwhile) */
int hos_vr_bottle_at_cork(void);        /* (the same: the other hand at the cork, or -1) */
int hos_vr_bottle_full(void);           /* (the same: RX_VR_BOTTLE_FULL, the full bottle; 0 the simple one) */
int hos_vr_bottle_to_game(void);        /* (the same, simple: the held bottle's content is the game's own use's) */
void hos_vr_bottle_ask_use(void);       /* (the same, simple: a use by the game's calls) */
void hos_vr_bottle_ask_game_use(void);  /* (the same, simple: hand the bottle back to the game for its own use) */
int hos_vr_bottle_game_ready(void);     /* (the same: the game has it back, its ZR may go) */
#ifdef HOOKSHOT_VR_RESEARCH
int hos_vr_bottle_probe_holder(void);   /* (the same: the research probe's stand-in hand, -1 none; its trigger below) */
int hos_vr_bottle_probe_trigger(void);
#else
#define hos_vr_bottle_probe_holder() (-1)   /* (the probe and its stand-in hand: research builds only) */
#define hos_vr_bottle_probe_trigger() 0
#endif
static volatile int vr_bottle_holder_now = -1;
int hos_vr_bottle_holder(void) { return __atomic_load_n(&vr_bottle_holder_now, __ATOMIC_RELAXED); }

/* the bottle's state for this read: which hand holds it (-1 none). on: first person, the VR tab's row on, a bottle
   selected; free: not in a menu. Keeps the game's ZR out of *raw while on. */
static int vr_bottle(const HookshotVrHand hands[2], int on, int free, uint64_t *raw, int64_t now) {
    int hos_vr_bomb_in_reach(int hand);   /* (vr/items/bomb/bomb_host.c: a grip near a bomb takes the bomb) */
    static int holder = -1, grip_was[2], told = -100, use_was, use_to_game;
    static int64_t told_at, pressed_at, zr_from;
    const int probe = hos_vr_bottle_probe_holder();   /* (the research probe's stand-in hand, headless; -1 none) */
    for (int k = 0; k < 2; k++) {
        const int grip = hands[k].valid && hands[k].squeeze > (grip_was[k] ? 0.35f : 0.55f);
        if (holder < 0 && on && free && grip && !grip_was[k] && hos_vr_sword_holder() < 0 && !hos_vr_sword_claims(k) &&
            !hos_vr_bomb_in_reach(k)) {
            holder = k;
            hos_log("vr: bottle: taken by the %s hand", k ? "right" : "left");
        }
        if (holder == k && (!grip || !on) && probe != k) {
            holder = -1;
            hos_log("vr: bottle: let go%s", on ? "" : " (not selected or not first person)");
        }
        grip_was[k] = grip;
    }
    {   /* (the research probe, headless: its stand-in hand holds the bottle while it says) */
        static int probe_was = -1;
        if (probe >= 0 && on) holder = probe;
        else if (probe_was >= 0 && holder == probe_was) holder = -1;
        probe_was = probe >= 0 && on ? probe : -1;
    }
    if (on) *raw &= ~(1ULL << HOS_PAD_RTRIGGER);   /* (no other trigger is the game's bottle use) */
    if (on && holder >= 0) {   /* (the holding hand's trigger, and the other hand's at the cork: the cork's) */
        const int cork = hos_vr_bottle_at_cork();
        *raw &= ~(1ULL << (holder ? HOS_PAD_RTRIGGER : HOS_PAD_LTRIGGER));
        if (cork == 0 || cork == 1) *raw &= ~(1ULL << (cork ? HOS_PAD_RTRIGGER : HOS_PAD_LTRIGGER));
    }
    if (on && holder >= 0 && !hos_vr_bottle_full()) {   /* (the simple bottle: the holding hand's trigger uses it) */
        const HookshotVrHand *h = &hands[holder];
        const int use = (h->valid && h->trigger > (use_was ? 0.35f : 0.55f)) || hos_vr_bottle_probe_trigger();
        if (use && !use_was && free && !use_to_game) {
            use_to_game = hos_vr_bottle_to_game();
            if (use_to_game) hos_vr_bottle_ask_game_use(), pressed_at = now, zr_from = 0;
            else hos_vr_bottle_ask_use();
            hos_log("vr: bottle: the %s hand's trigger: %s", holder ? "right" : "left",
                    use_to_game ? "the game's bottle use (its item button, once it has the bottle back)" : "used by the game's calls");
        }
        if (use_to_game) {   /* (the player's own press, as the game's item button, once the game has the bottle) */
            if (!zr_from && hos_vr_bottle_game_ready()) zr_from = now;
            if (zr_from && now - zr_from < kVrBottleZrNs) *raw |= 1ULL << HOS_PAD_RTRIGGER;
            else if ((zr_from && !use) || (!zr_from && !use && now - pressed_at > kVrBottleHandNs)) use_to_game = 0;
        }
        use_was = use;
    } else use_was = use_to_game = 0;
    __atomic_store_n(&vr_bottle_holder_now, holder, __ATOMIC_RELAXED);
    const int tell = holder >= 0 ? holder : hos_vr_bottle_away() ? -2 : -1;
    if (tell != told || now - told_at >= kVrBottleTellNs) told = tell, told_at = now, rx_vr_bottle_hand(tell);
    return holder;
}
