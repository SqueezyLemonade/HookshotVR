/* Hookshot VR: the dev map's marks (vr/devmap, docs/devmap.md): Menu + X on the left controller writes where Link stands
   and which way the view faces to <mods>/hookshot-vr-devmap/marks.jsonl, one line each, for the editor to place things
   at (its Marks). A short rumble says it was written.

   Game-side code, included by host/hos_main.c (its VR HOOK block) after vr/camera/view_host.c: it uses Link's place
   (oot_vr_player_xyz), the view's heading (oot_vr_move_yaw_bits) and the map (hos_oot_current_map). Not compiled on its own. */
int hos_mods_root(char *out, size_t cap);   /* (host/mods.c) */

void hos_vr_devmap_mark(void) {
    char root[512], dir[600], file[640], map[80] = "";
    if (!hos_mods_root(root, sizeof root)) return;
    hos_oot_current_map(map, sizeof map);
    snprintf(dir, sizeof dir, "%s/hookshot-vr-devmap", root);
#ifndef _WIN32
    for (char *p = dir; *p; p++) if (*p == '\\') *p = '/';
#endif
    CreateDirectoryA(root, NULL);
    CreateDirectoryA(dir, NULL);
    snprintf(file, sizeof file, "%s/marks.jsonl", dir);
    float xyz[3], yaw;
    for (int k = 0; k < 3; k++) { const uint32_t bits = __atomic_load_n(&oot_vr_player_xyz[k], __ATOMIC_RELAXED); memcpy(&xyz[k], &bits, 4); }
    const uint32_t yb = __atomic_load_n(&oot_vr_move_yaw_bits, __ATOMIC_RELAXED);
    memcpy(&yaw, &yb, 4);
    FILE *f = fopen(file, "a");
    if (!f) { hos_log("vr: dev map: mark not written (%s)", file); return; }
    fprintf(f, "{\"map\": \"%s\", \"position\": [%.3f, %.3f, %.3f], \"yaw\": %.1f}\n", map, xyz[0], xyz[1], xyz[2],
            isfinite(yaw) ? yaw * 57.29578f : 0.0f);
    fclose(f);
    rx_vr_rumble(0, 0.6f, 0.08f);
    hos_log("vr: dev map: marked %s %.2f %.2f %.2f", map, xyz[0], xyz[1], xyz[2]);
}
