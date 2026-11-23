/* Hookshot VR: the game side's two ocarina calls into the renderer (vr/items/ocarina/ocarina_bridge.inc), first person.

   A piece of vr/headset/bridges_host.c (so of host/rx_backend.c), included at its end: it uses that file's vk_module and
   GetProcAddress. Not compiled on its own. */
/* (OoT VR) the VR hand holding the ocarina (0 left, 1 right, -1 on the belt): vr/items/ocarina/ocarina_host.c, when it
   changes and every 0.1 s */
void rx_vr_ocarina_hand(int hand) {
    static void (*fn)(int);
    static int looked;
    if (!looked && vk_module) fn = (void (*)(int))(void *)GetProcAddress(vk_module, "hookshot_vr_ocarina_hand"), looked = 1;
    if (fn) fn(hand);
}
/* (OoT VR) where the renderer drew the ocarina on the VR belt (world, m), drawn within the last quarter of a second: 1, else 0 */
int rx_vr_ocarina_belt(float out[3]) {
    static int (*fn)(float *);
    static int looked;
    if (!looked && vk_module) fn = (int (*)(float *))(void *)GetProcAddress(vk_module, "hookshot_vr_ocarina_belt"), looked = 1;
    return fn ? fn(out) : 0;
}
/* (OoT VR) a song the VR ocarina played is being played by the game now (its effect: the VR tab's Song effects,
   vr/items/ocarina/song_effects.inc): vr/items/ocarina/ocarina_host.c, when it changes and every 0.1 s */
void rx_vr_ocarina_song(int playing) {
    static void (*fn)(int);
    static int looked;
    if (!looked && vk_module) fn = (void (*)(int))(void *)GetProcAddress(vk_module, "hookshot_vr_ocarina_song"), looked = 1;
    if (fn) fn(playing);
}
