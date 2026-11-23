/* Hookshot VR: the game side's item-get call into the renderer (vr/items/item_get/item_get_bridge.inc), first person.

   A piece of vr/headset/bridges_host.c (so of host/rx_backend.c), included at its end: it uses that file's vk_module and
   GetProcAddress. Not compiled on its own. */
/* (OoT VR) an item get plays (vr/items/item_get/item_get_host.c, when it changes and every 0.1 s) */
void rx_vr_item_get(int playing) {
    static void (*fn)(int);
    static int looked;
    if (!looked && vk_module) fn = (void (*)(int))(void *)GetProcAddress(vk_module, "hookshot_vr_item_get"), looked = 1;
    if (fn) fn(playing);
}
/* (OoT VR) the renderer drew an item get's item in front of the headset in the last quarter of a second: 1, or 0 */
int rx_vr_item_get_shown(void) {
    static int (*fn)(void);
    static int looked;
    if (!looked && vk_module) fn = (int (*)(void))(void *)GetProcAddress(vk_module, "hookshot_vr_item_get_shown"), looked = 1;
    return fn ? fn() : 0;
}
