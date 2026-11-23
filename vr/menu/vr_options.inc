// Hookshot VR: The VR settings' stored values (opts.vr_*) with their defaults: part of the menu's options struct. The rows
// that show and change them are in vr/menu/vr_tab_rows.inc.
//
// A piece of gpu/vk/vk_menu.h, included there at its VR HOOK (it uses that file's types and helpers). Not compiled on its own.
//
// Built in (19 Nov, the VR menu's spec): a test switch whose fix is final keeps that value for good and has no row; it is a
// constant (static constexpr), so code that still asks reads the constant and its other branch compiles away. A row can no
// longer point at one (&opts.x of a constant does not convert to int*), and an old options file's line for it is kept but not
// read. The constants: Tracked Link arms, Held gear grip, First-person horizon, Link's shadow and camera fade in first
// person, every item in the hands, the ocarina's song menu, Unlit bomb fire, Sword from the back, Swing detection, Held items
// near the face, Guard with the sword out, Put away, Crawl spaces, Grip holds the bow, Bow fires on string release, Stick
// click menus. Built in 22 Nov (the user's list after the release audit): Jump on right stick up (on), Legs (step with
// travel), Stowed gear (hidden), Sword hits (Blade), Hit test (Early), Draw test (Both), Sword attacks (Swing), Swing
// sensitivity (Medium), Unlit bomb frame (25), Body pose timing (its own frame), the four per-eye rows (all on), Window test
// (off); and for the release, the Debug tab's tests: World culling (Game), Headset frustum culling (Game), Bomb gravity (100%).
// The Debug tab itself (GPU profile, Show hands) shows only with HOOKSHOT_VR_DEBUG=1 (vr/menu/vr_menu.inc vrm_tab_count).
    // (OoT VR, vk_vr.cpp; the VR menu's)
    int vr = 0;             // the headset this run: set once at the start, as HOOKSHOT_VR (else vr= in the ini) asks (no row:
                            // vr/headset/runtime_session.inc instance_extensions)
    int vr_mode = 0;        // 0 third person (game orbit camera), 1 first person (Link's eyes)
    int vr_window = 0;      // View -> Window: third person (vr_mode 0) seen through a frame fixed in the room
                            // (vr/headset/window_view.inc)
    int vr_window_room = 0; // around the window: 0 dark, 1 the room (passthrough, where the runtime offers it)
    int vr_window_place = 0;   // changed: the window placed in front of you again
    static constexpr int vr_window_test = 0; // (Debug) the window shows its test scene instead of the game, for geometry checks
    int vr_window_width = 100;    // the window's width, cm
    int vr_window_169 = 1;        // its height follows the width at 16:9 (0: vr_window_height)
    int vr_window_height = 56;    // its height, cm, with Keep 16:9 off
    int vr_window_round = 0;      // its corners' roundness, %: 0 a rectangle, 100 an ellipse
    int vr_window_feather = 0;    // its edge's softness, mm
    int vr_window_tilt = 0;       // its tilt, degrees: 0 upright, more leans its top away
    int vr_window_distance = 90;  // how far ahead of you it is placed, cm
    int vr_window_move = 0;       // changed: Move mode (the window dragged with a controller's laser)
    int vr_window_eye_camera = 0; // (experimental) the game's camera turned from your head to the window's middle
    int vr_window_back = 0;       // the window seen from behind: 0 nothing (the room or dark), 1 a dark panel
    int vr_move_head = 0;   // first-person movement follows 0 camera, 1 headset heading
    int vr_stereo = 1;      // 3D: each eye from the picture and its depth
    int vr_depth_scale = 100;   // 3D strength in percent (the eye distance in the game, times yours)
    static constexpr int vr_level = 1;       // straight ahead is level (the game camera's tilt dropped)
    int vr_height = 0;      // first-person eye height above adult Link's root, cm (child Link's scaled from it, x125/160); 0: 160
    int vr_body_depth = 0;  // first-person Link body placement relative to the headset, cm (+ ahead, - behind)
    int vr_recenter = 0;    // changed: where you look now is straight ahead
    int vr_quality = 24;    // the 3D's depth search steps
    int vr_eye_height = 2160;   // each eye's image at most this tall (0: the picture's)
    int vr_detail = 1;      // model detail distance: 0 Low, 1 Switch 2 (the game's own), 2 High, 3 Ultra
    int vr_foliage = 1;     // foliage distance, the same
    int vr_gpu_profile = 0; // the next start times the GPU's passes (RX_VK_PASS_TIMING=1)
    int vr_show_hands = 0;  // (VR menu: Debug, HOOKSHOT_VR_DEBUG=1) a coloured square at each tracked controller, independent of
                            // Link's arms
    static constexpr int vr_arms = 1;        // experimental: Link's first-person head hides and his real arms follow the controllers
    int vr_wrist_calibrate = 0; // changed: capture both held controllers as the neutral Link wrist pose
    int vr_wrist_l_pitch = 0, vr_wrist_l_yaw = 0, vr_wrist_l_roll = 0;
    int vr_wrist_r_pitch = 0, vr_wrist_r_yaw = 0, vr_wrist_r_roll = 0;
    int vr_arm_length = 100;   // Link's arms this long, percent of his own (players' arms are usually longer)
    int vr_bow_scale = 130; // held bow size, percent
    int vr_bow_preview = 1; // fixed-distance hand-line preview; not a projectile impact indicator
    int vr_bow_string_trigger = 0; // string hand: 0 grip or trigger, 1 trigger, 2 grip (independent of gamepad remapping)
    static constexpr int vr_bow_string_fires = 1; // first person: the string's release fires (the game's ZR held for it); the real ZR does not
    int vr_bow_reticle_size = 250; // the headset's bow reticle, percent
    static constexpr int vr_bow_grip_hold = 1; // first person: a grip takes the bow out and holds it, letting go puts it away (B)
    static constexpr int vr_bomb_hands = 1; // first person: bombs are held in the VR hands (grip), lit with the trigger, thrown by hand
    int vr_bomb_throw = 120; // a released bomb's speed, percent of the hand's
    int vr_bomb_scale = 100; // bombs drawn at this size, percent (their collision keeps the game's)
    static constexpr int vr_bomb_gravity = 100; // a thrown bomb's gravity in flight, percent of real gravity (the game's is nearly 300%)
    static constexpr int vr_bomb_unlit_frame = 25; // the fuse animation's (Bomb_Blink) frame an unlit bomb is held at (25: the user's, 22
                                  // Nov; the animation starts the fuse's sound, flame and sparks at frame 20, kept asleep by vr_bomb_quiet)
    int vr_bomb_throw_mode = 0; // how a held bomb is thrown: 0 letting go of the grip, 1 squeezing the grip again
    static constexpr int vr_bomb_quiet = 1; // an unlit bomb in the VR hands: 1 no fuse effects or sound until lit (its XLink asleep), 0 the game's
    int vr_bomb_pull = 1; // first person: an open hand pointing at a bomb out of reach, then gripping, pulls it into the hand
    int vr_bomb_pull_reach = 40; // the pull's reach, decimetres
    int vr_bomb_pull_cone = 20; // the pull's aim: degrees either side of the line out of the palm
    static constexpr int vr_bomb_flowers = 1; // first person, the Goron's Bracelet owned: a VR grip picks Bomb Flowers, whatever is selected
    static constexpr int vr_show_stowed = 0; // first person: draw the sheath and a put-away sword/shield on the back (their placement can drift)
    static constexpr int vr_held_grip = 0; // the fingers on held gear (bow, drawn sword, drawn shield): 0 the game's own grip, 1 a plain fist
    static constexpr int vr_hook_hands = 1; // first person: the hookshot and longshot in a VR hand, aimed along the controller
    static constexpr int vr_boomerang_hands = 1; // first person: child Link's Boomerang in a VR hand: a grip takes it, thrown as Boomerang throw says
    int vr_boomerang_throw = 0; // how it is aimed: 0 the holding hand, 1 the empty hand, 2 the headset (3-5, saved when there
                                // were six: 0)
    static constexpr int vr_bottle_hands = 1; // first person: bottles in a VR hand: a grip takes the selected bottle, its trigger uses it (vr/items/bottle/bottle_host.c)
    static constexpr int vr_stick_hands = 1; // first person: child Link's Deku Stick in a VR hand: a grip takes it, a swing attacks, its head lights from fire
    static constexpr int vr_ocarina = 1; // first person: the ocarina on the front left of the VR belt (vr/items/ocarina; option 980)
    static constexpr int vr_ocarina_menu = 1; // first person, the ocarina at the mouth: 1 the other hand's grip opens its song menu, 0 off (981)
    int vr_song_effects = 1; // first person: a song's effects as the game draws them (1), or left out near you (0)
    int vr_item_get = 1;      // first person: an item get's item in front of you (1), or over Link's head as the game has it (0)
    int vr_item_get_view = 1; // first person: an item get keeps your view (1), or turns it with Link as any event (0; option 982)
    static constexpr int vr_stick_jump = 1; // first person: the right stick pushed up jumps (the left controller's upper button is then free)
    int vr_arrow_fx_opacity = 20; // first person: the elemental arrows' effects on the VR arrow, percent (0 off; 20 shows them
                                  // whole while the target stays visible through them) // first person: the held arrow's effects (fire, ice, light) drawn on the VR arrow, not the game's
    static constexpr int vr_gait = 1; // first person: 1 the legs step the way Link travels (the game's own side and back steps), 0 the game's legs
    static constexpr int vr_quick_select = 1; // first person: left stick click: 1 the VR radial item menu, 2 the game's quick select, 0 off
    int vr_radial_place = 0; // the VR radial menu opens 0 at the hand, 1 in front
    int vr_wrist_size = 2;   // the wrist check HUD's size (vr/menu/wrist_draw.inc kWristWidths): 0 small, 1 medium, 2 large
    int vr_wrist_hud = 1;    // the wrist check HUD (vr/menu/wrist_draw.inc): 0 off, 1 Simple, 2 Full (Simple until the minimap is built)
    int vr_hud_size = 60;    // the game's HUD, text boxes and menus scaled toward the middle of the view (percent; 100: as the game draws them)
    int vr_sword_angle = 35; // first person: the blade tilted this many degrees forward in the hand (a held controller leans back)
    static constexpr int vr_swing = 1;        // first person: the sword taken from behind the shoulder by either hand, swung to attack, let go to put away
    static constexpr int vr_sword_hits = 1;   // first person, the sword or a Deku Stick in a VR hand: 1 Blade, the VR blade's swing and contact set
                             // the game's own attack sensor up (no attack button, vr/items/sword/blade_hits.inc); 0 Button, a
                             // swing presses the attack button (option 1005)
    static constexpr int vr_hit_test = 0;     // test, Sword hits Blade: 0 Early (set up at the SensorHit calc, the weapon's own SensorHit suspended,
                             // the Weapon sensor alone), 1 Early all (the attack's own sensors), 2 No suspend, 3 Late (at the
                             // movement update: what never hit) (vr/items/sword/blade_hits.inc, option 1007)
    static constexpr int vr_draw_test = 0;    // test, Sword hits Blade: Link's drawn stance at a take: 0 Both (his AI's requests made drawn, and the
                             // forced change of his sub selector), 1 Rewrite, 2 Switch, 3 Off (vr/items/sword/blade_hits.inc,
                             // option 1008)
    int vr_swing_arc = 25;   // first person, Sword hits Blade: the cm the blade's tip must travel one way for a stroke (option 1009)
    int vr_swing_speed = 26; // ... and how fast, tenths of m/s at the tip (option 1013)
    int vr_hit_buzz = 1;     // first person, Sword hits Blade: a short buzz in the weapon's hand for a hit (option 1006)
    static constexpr int vr_sword_attack = 0; // first person, the sword in a VR hand: 0 a fast swing attacks, 1 the trigger held keeps attacking while
                             // the blade moves, 2 either
    int vr_shield_pose = 0;  // first person, the shield in a VR hand: 0 along the forearm as the game wears it (it follows the hand),
                             // 1 turned about the grip to face forward while guarding (the game's ZL), 2 always so
                             // (vr/items/gear/held_gear.inc, option 1004; 0 the default since 22 Nov, the player's choice)
    static constexpr int vr_swing_sensitivity = 1; // first person: how fast a swing must be to attack: 0 Low (faster), 1 Medium, 2 High
                             // (vr/input/controllers_host.c vr_swing_speed, option 1003)
    static constexpr int vr_swing_detect = 1; // first person, swinging the sword or a Deku Stick: 0 the hand's speed, one attack a swing; 1 the
                             // blade's too, the attack kept going through the swing (vr/input/controllers_host.c, option 1001)
    static constexpr int vr_held_near = 1;    // first person: what a VR hand holds (and the ocarina, the bow) kept from the game's near-plane
                             // test, so it does not vanish close to the face (vr/items/gear/gear_near_host.c, option 1002)
    static constexpr int vr_sword_guard = 1;  // first person, the sword in a VR hand: the other hand's grip or trigger raises the shield (the
                             // game's ZL; vr/input/controllers_host.c, option 1000)
    static constexpr int vr_put_away = 0;     // first person, letting go of a held weapon or item: 0 the game's own put-away call, 1 its B button
                             // (pressed only while Link stands clear) (vr/items/gear/put_away_host.c, option 945)
    int vr_events = 2;       // the game's events (Cutscene view, option 912): 0 every event's shots on the world screen, 1 talks and
                             // the rest in VR, story cinematics on the world screen, 2 those in 3D too (the default since 22 Nov,
                             // the user's: at the director's camera, vr_cinematic_view; vr/camera/events_host.c)
    int vr_cinematic_view = 0;   // a cinematic in 3D (Cutscene view In 3D, or a talk's far shot): 0 the director's camera, 1 a
                                 // theatre seat, 2 Link's eyes where it began, 3 following Link (option 1010)
    int vr_cinematic_light = 0;  // its lighting: 0 Auto (the game's per shot at the director's camera, else held), 1 every
                                 // shot's own, 2 the first shot's held, 3 the area's own (option 1011)
    int vr_event_detail = 0;     // test, a cinematic in 3D (in first person): 0 what the game decides from its camera follows your
                                 // view, 1 the game's camera (option 1012)
    int vr_cutscene_screen = 70; // the world screen's width, degrees where it opened (flat frames: cutscenes, the title, menus;
                                 // vr/headset/eye_rendering.inc)
    int vr_realign = 1;      // first person: an event that turns Link turns the view's heading by as much: 0 off, 1 a quick turn at its
                             // end, 2 a snap at its end, 3 following Link through it (option 927, vr/camera/view_host.c)
    static constexpr int vr_crawl = 1;        // first person: while Link crawls through a tunnel, the view comes down to the crawl and his body
                             // is the game's crawl (option 990, vr/camera/crawl_host.c); 0 the standing height, as before
    int vr_crawl_eye = 0;    // the eye height in a crawl, cm above Link's position (0 Auto: the game's own crawl view height; option 991)
    static constexpr int vr_world_cull = 0;   // test: 0 Game, 1 the game camera's float position/target/up are the headset's for the rest of each
                             // frame, 2 its double position/target too (vr/camera/culling_host.c, option 923)
    static constexpr int vr_frustum_cull = 0; // test, first person: 1 the headset view's culling planes cull nothing within 200 m of the eye,
                             // 2 they are 10 degrees wider each side (vr/camera/culling_host.c, option 931)
    static constexpr int vr_own_shadow = 0;   // first person: Link's own shadow (his body and the gear posed with it): 0 hidden, 1 shown
    static constexpr int vr_link_fade = 0;    // first person: the game's camera fade on Link himself (ActiveCameraAlpha): 0 kept off, 1 the game's
    static constexpr int vr_body_latest = 0;  // (VR menu: Debug) the first-person body for a frame: 0 its own game frame's (BODY-11), 1 the latest told
    static constexpr int vr_eyes_lights = 1;  // (true stereo) light volumes and decals moved for each eye (STE-27): on since 22 Nov, the
                             // user's (headless 20 Nov the forest path drew as flat polygons with it on: watch for it)
    static constexpr int vr_eyes_particles = 1;   // (true stereo) effects and particles moved for each eye (STE-26); 0 flat, as before
    static constexpr int vr_eyes_fog = 1;     // (true stereo) the volumetric fog grid made for each eye (STE-25; costs ~12% fps)
    static constexpr int vr_eyes_camera = 1;  // (true stereo) the lighting and shadow passes with each eye's camera (STE-28)
    int vr_menu_position = 0; // the VR menu (vr/menu/vr_menu_layer.inc): 0 fixed in the world where it opened, 1 follows your head
    int vr_turn_style = 0;   // first person, the right stick: 0 a smooth turn (as before), 1 snap turns (vr/camera/view_host.c, option 1030)
    int vr_snap_angle = 1;   // a snap turn: 0 30, 1 45, 2 60, 3 90 degrees (option 1031)
    int vr_turn_speed = 1;   // a smooth turn at a full stick: 0 Slow (half), 1 Medium (as before, about 90 degrees a second), 2 Fast
                             // (1.5 times), 3 Very fast (twice) (option 1032)
    // (first person: a dark ring round both eyes while ... : 0 Off, 1 Low, 2 Medium, 3 High; vr/comfort/vignette_layer.inc)
    int vr_vignette_move = 0;   // the left stick moves Link
    int vr_vignette_turn = 0;   // the view turns without the head (the right stick, lock-on, a re-align)
    int vr_vignette_air = 0;    // Link is in the air (a jump, a fall)
    int vr_vignette_carried = 0;   // the game moves Link (a hookshot pull, knockback, a current, Epona)
