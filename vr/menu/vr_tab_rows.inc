// Hookshot VR: EVERY VR SETTING: its name, the value it changes (opts.vr_...), its range and step, how its value is shown, and
// the help text. Part of the menu's rows[] table. Hookshot's own (Insert) menu shows none of them: they are the VR menu's
// (vr/menu/vr_menu.inc, which lays them out under names of its own and shows this help), kept here as TAB_STORED rows so
// they are saved in hookshot_options.ini under their keys as before (ini_key: from these names, so a row keeps its name here
// when the VR menu calls it something else). Built-in switches have no row: vr/menu/vr_options.inc. VR itself has no row
// either: it is decided at the start (HOOKSHOT_VR, else vr= in the options file: vr/headset/runtime_loader.inc asked).
//
// A piece of gpu/vk/vk_menu.cpp, included there at its VR HOOK (it uses that file's types and helpers). Not compiled on its own.
    // (OoT VR) the VR rows: the VR menu's
    {TAB_STORED, "View", &opts.vr_mode, 0, 1, 1, tx_vr_mode,
     "Third person keeps the game's orbit camera. First person places the view at Link's eyes.", nullptr, false, true},
    {TAB_STORED, "Window view", &opts.vr_window, 0, 1, 1, tx_onoff,
     "View's third choice: third person seen through a frame fixed in your room.", nullptr, false, true},
    {TAB_STORED, "Window background", &opts.vr_window_room, 0, 1, 1, tx_vr_window_room,
     "Around the window: dark, or your room where the headset's runtime offers passthrough.", nullptr, false, true},
    {TAB_STORED, "Place window", &opts.vr_window_place, 0, 1, 1, tx_vr_recenter,
     "Places the window in front of you again.", nullptr, false, true},
    {TAB_STORED, "Window width", &opts.vr_window_width, 20, 300, 5, tx_vr_cm,
     "The window's width.", nullptr, false, true},
    {TAB_STORED, "Window 16 by 9", &opts.vr_window_169, 0, 1, 1, tx_onoff,
     "The window's height follows its width at 16:9. Off: set its height yourself.", nullptr, false, true},
    {TAB_STORED, "Window height", &opts.vr_window_height, 10, 200, 5, tx_vr_cm,
     "The window's height, with Keep 16:9 off.", nullptr, false, true},
    {TAB_STORED, "Window roundness", &opts.vr_window_round, 0, 100, 5, tx_percent,
     "Rounds the window's corners: none is a rectangle, all the way an oval (a circle when it is square).", nullptr, false, true},
    {TAB_STORED, "Window feather", &opts.vr_window_feather, 0, 100, 5, tx_vr_mm,
     "Softens the window's edge.", nullptr, false, true},
    {TAB_STORED, "Window tilt", &opts.vr_window_tilt, -45, 45, 5, tx_vr_degrees,
     "Tilts the window: more leans its top away from you.", nullptr, false, true},
    {TAB_STORED, "Window distance", &opts.vr_window_distance, 40, 300, 5, tx_vr_cm,
     "How far ahead of you the window is placed.", nullptr, false, true},
    {TAB_STORED, "Move window", &opts.vr_window_move, 0, 1, 1, tx_vr_recenter,
     "Point a controller's laser at the window, hold the trigger and drag it; the stick pushes it away or pulls it closer. "
     "Its corner dots resize it, the dots inside them round it. B or Y ends.", nullptr, false, true},
    {TAB_STORED, "Window eye camera", &opts.vr_window_eye_camera, 0, 1, 1, tx_onoff,
     "Experimental. The game's camera looks from where you are to the window's middle, so stepping far to a side still "
     "shows the world through it, from that side. Off: the camera keeps the window's own direction.", nullptr, false, true},
    {TAB_STORED, "Window back panel", &opts.vr_window_back, 0, 1, 1, tx_onoff,
     "Behind the window: a dark panel where it is. Off: nothing there, the room or dark.", nullptr, false, true},
    {TAB_STORED, "Movement direction", &opts.vr_move_head, 0, 1, 1, tx_vr_move,
     "First person only. Camera follows the right-stick heading; Headset moves Link where you are looking.",
     nullptr, false, true},
    {TAB_STORED, "First-person height", &opts.vr_height, 0, 220, 5, tx_vr_height,
     "Adult Link's eye height (Auto: 160 cm). Child Link's follows from it, scaled to his size, so switching ages needs no "
     "change here. Adjust this if the view is too high or low.", nullptr, false, true},
    {TAB_STORED, "Body alignment", &opts.vr_body_depth, -50, 50, 2, tx_vr_body_depth,
     "Moves Link's body ahead of or behind the headset in first person. Stand naturally, Recenter view, then adjust until his torso is under you.",
     nullptr, false, true},
    {TAB_STORED, "Recentre", &opts.vr_recenter, 0, 1, 1, tx_vr_recenter,
     "Makes the way you look now straight ahead.", nullptr, false, true},
    {TAB_STORED, "3D", &opts.vr_stereo, 0, 4, 1, tx_vr_stereo,   // (0 Mono, 4 Stereo, 1 From depth when offered: vr_options.inc)
     "Mono: both eyes see the same picture (no 3D, the lightest). Stereo: the game draws each eye in every frame (exact 3D, "
     "heavier).",
     nullptr, false, true},
    {TAB_STORED, "3D strength", &opts.vr_depth_scale, 0, 300, 10, tx_percent,
     "How far apart the eyes are in the game: 100% is your own. More makes the world look smaller.", nullptr, false, true},
    {TAB_STORED, "Near depth", &opts.vr_near_depth, 0, 2, 1, tx_vr_near_depth,
     "From depth: how far things near you stand out. Low keeps the gaps beside your hands and sword small; Full is all of it.",
     nullptr, false, true},
    {TAB_STORED, "3D quality", &opts.vr_quality, 8, 64, 8, tx_vr_steps,
     "How finely each eye searches the depth: more is cleaner at edges and costs more of the GPU.", nullptr, false, true},
    {TAB_STORED, "Eye resolution", &opts.vr_eye_height, 0, 4320, 360, tx_vr_eye,
     "Each eye's image at most this tall: smaller costs less of the GPU.", nullptr, false, true},
    {TAB_STORED, "Detail distance", &opts.vr_detail, 0, 4, 1, tx_vr_distance,
     "How far models keep their detail. Switch 2 matches the console at every distance; High and Ultra keep it about 1.5 and 2 "
     "times further, for more of the GPU.", nullptr, false, true},
    {TAB_STORED, "Foliage distance", &opts.vr_foliage, 0, 4, 1, tx_vr_distance,
     "How far grass, bushes and trees reach. Switch 2 matches the console; High and Ultra reach further, for more of the GPU.",
     nullptr, false, true},
    {TAB_STORED, "Show hands", &opts.vr_show_hands, 0, 1, 1, tx_onoff,
     "A small square in each hand where the headset tracks the controllers: blue left, red right. A test of the hands "
     "to check controller tracking independently of Link's arms.", nullptr, false, true},
    {TAB_STORED, "Arm length", &opts.vr_arm_length, 80, 160, 5, tx_percent,
     "Link's arms are shorter than most players'. Longer stretches his arms so his hands reach yours at full stretch.",
     nullptr, false, true},
    {TAB_STORED, "Calibrate wrists", &opts.vr_wrist_calibrate, 0, 1, 1, tx_vr_calibrate,
     "Hold both hands ahead, palms facing each other and thumbs up, then press A. The captured controller-to-wrist "
     "angles are saved and restored next time you play.", nullptr, false, true},
    {TAB_STORED, "Bow size", &opts.vr_bow_scale, 80, 200, 5, tx_percent,
     "Scale of the held bow. Grip stays at the tracked controller.", nullptr, false, true},
    {TAB_STORED, "Bow reticle", &opts.vr_bow_preview, 0, 1, 1, tx_onoff,
     "First person: the hand-direction reticle while you hold the bow and its string (the game's own reticle is always hidden then; not a ballistic impact predictor). Third person: while you aim the slingshot, bow, hookshot or Boomerang, a reticle where the game's aim says the shot lands, in place of the game's own.", nullptr, false, true},
    {TAB_STORED, "Bow string hand input", &opts.vr_bow_string_trigger, 0, 2, 1, tx_vr_string_input,
     "What grabs the string with the hand that is not holding the bow: its grip, its trigger, or either. Independent of remapped game controls.", nullptr, false, true},
    {TAB_STORED, "Elemental arrow effects", &opts.vr_arrow_fx_opacity, 0, 100, 10, tx_percent,
     "First person: how strongly a fire, ice or light arrow's flames, frost and glow show on the arrow on your VR string. "
     "100: as the game draws them. 20 (the default): whole, with the target still visible through them. 0: off.",
     nullptr, false, true},
    {TAB_STORED, "Song effects", &opts.vr_song_effects, 0, 1, 1, tx_vr_shown,
     "The game's effects when you play a song right (Saria's Song's leaves, ...), round you in first person. Hidden: those "
     "within 4 m of you are left out while the song plays (other effects that near too, for those seconds).",
     nullptr, false, true},
    {TAB_STORED, "Boomerang throw", &opts.vr_boomerang_throw, 0, 2, 1, tx_vr_boomerang_throw,
     "Boomerang hand: hold the trigger of the hand that holds it to aim (the reticle along the controller), let go "
     "to throw. Empty hand: point with the other hand; its trigger aims, letting go throws. Headset: the "
     "holding hand's trigger aims where you look, letting go throws.", nullptr, false, true},
    {TAB_STORED, "Bomb throw", &opts.vr_bomb_throw_mode, 0, 1, 1, tx_vr_bomb_throw,
     "Hold: a bomb is held while you squeeze, and thrown as you let go (best with wrist straps). Press to toggle: "
     "a squeeze takes the bomb and it stays in your hand; the next squeeze throws it with your arm's movement at that moment "
     "(squeeze with your hand still to drop it).", nullptr, false, true},
    {TAB_STORED, "Bomb size", &opts.vr_bomb_scale, 50, 150, 5, tx_percent,
     "How big bombs look. Your hand holds them by their drawn size; on the ground a resized bomb sits slightly into or "
     "above it, as its collision keeps the game's size.", nullptr, false, true},
    {TAB_STORED, "Bomb throw strength", &opts.vr_bomb_throw, 50, 400, 10, tx_percent,
     "How fast a bomb leaves your hand: 100% is your hand's own speed. More throws further. Takes effect at once.",
     nullptr, false, true},
    {TAB_STORED, "Bomb pull", &opts.vr_bomb_pull, 0, 1, 1, tx_onoff,
     "Hold an open hand out, its palm toward a bomb or a Bomb Flower out of reach: the one it faces pulses. Squeeze the "
     "grip, and it flies into that hand. Bomb Flowers need the Goron's Bracelet, and Link beside the plant.", nullptr, false, true},
    {TAB_STORED, "Bomb pull reach", &opts.vr_bomb_pull_reach, 10, 80, 5, tx_vr_decimetres,
     "How far away a bomb can be pulled from.", nullptr, false, true},
    {TAB_STORED, "Bomb pull aim", &opts.vr_bomb_pull_cone, 3, 45, 1, tx_vr_cone,
     "How closely your palm must face a bomb to pull it: the angle either side of straight out of the palm. Wider is easier "
     "to aim, narrower picks one bomb out of a group.", nullptr, false, true},
    {TAB_STORED, "Radial menu place", &opts.vr_radial_place, 0, 1, 1, tx_vr_radial_place,
     "Where the VR radial menu opens: At the hand, beside the controller you clicked; In front, in front of you at chest "
     "height. Either way it stays where it opened while you look around.", nullptr, false, true},
    {TAB_STORED, "Swing arc", &opts.vr_swing_arc, 10, 60, 5, tx_vr_cm,
     "First person, Sword hits on Blade: how far the tip of the blade must travel in one sweep for a swing to count. "
     "More, only long swings hit; less, short flicks too.", nullptr, false, true},
    {TAB_STORED, "Swing speed", &opts.vr_swing_speed, 10, 60, 2, tx_vr_tenths_ms,
     "First person, Sword hits on Blade: how fast the tip of the blade must move for a swing to count. Your own motion "
     "in the room counts, not Link's running.", nullptr, false, true},
    {TAB_STORED, "Hit buzz", &opts.vr_hit_buzz, 0, 1, 1, tx_onoff,
     "First person, Sword hits on Blade: a short buzz in the hand that holds the sword or the Deku Stick when its blade "
     "hits something.", nullptr, false, true},
    {TAB_STORED, "Shield when guarding", &opts.vr_shield_pose, 0, 2, 1, tx_vr_shield_pose,
     "First person, the shield in your hand: Arm, it stays along your forearm as Link wears it and follows your hand; "
     "Guard turn, while you guard it turns in your fist to face where the fist points; Fist, it faces where your fist "
     "points all the time.", nullptr, false, true},
    {TAB_STORED, "Wrist HUD", &opts.vr_wrist_hud, 0, 2, 1, tx_vr_wrist_hud,
     "Turn the back of either wrist toward your face and hold it there, as when checking a watch: a small panel shows your hearts, magic, "
     "rupees and, in a dungeon, its keys, map and compass. It hides when you lower your arm, and never shows in a fight. "
     "Simple shows the map and compass as icons; Full adds the map of where you are, with Link's arrow.", nullptr, false, true},
    {TAB_STORED, "Wrist size", &opts.vr_wrist_size, 0, 2, 1, tx_vr_wrist_size,
     "How big the wrist panel is.", nullptr, false, true},
    {TAB_STORED, "HUD size", &opts.vr_hud_size, 30, 100, 5, tx_percent,
     "The game's HUD, text boxes and menus, pulled toward the middle of your view: the picture fills the whole headset "
     "view, and at 100% (as the game draws them) its edges are out of sight.", nullptr, false, true},
    {TAB_STORED, "Sword angle", &opts.vr_sword_angle, -45, 90, 5, tx_vr_degrees,
     "First person: how far the Master Sword's blade leans forward from the game's own grip when held in a VR hand. More "
     "points it ahead of the controller as you hold it naturally; less leans it back toward your arm.", nullptr, false, true},
    {TAB_STORED, "Cutscene view", &opts.vr_events, 0, 2, 1, tx_vr_events,
     "Talks, signs, Navi, chests, doors and arrivals stay in VR. In 3D: story cutscenes play in the world around you, "
     "as 3D cutscenes says. Screen: on a screen in the room, where you faced when they began. Flat: every event on the "
     "screen.", nullptr, false, true},
    {TAB_STORED, "3D cutscenes", &opts.vr_cinematic_view, 0, 3, 1, tx_vr_cinematic_view,
     "Story cutscenes In 3D, and a talk's shot of something far away: Director puts you at the game's camera, with its "
     "cuts. Theatre seat keeps one place while the action stays near. Link's spot keeps you where Link stood when it "
     "began. Follow Link keeps you in Link's eyes.", nullptr, false, true},
    {TAB_STORED, "Cutscene light", &opts.vr_cinematic_light, 0, 3, 1, tx_vr_cinematic_light,
     "Story cutscenes in 3D: the game lights each shot its own way. Auto keeps that at the game's camera and holds the "
     "first shot's anywhere else; Every shot changes it at each cut; Held keeps the first; Area's uses the place's own.",
     nullptr, false, true},
    {TAB_STORED, "Screen size", &opts.vr_cutscene_screen, 50, 110, 10, tx_vr_width,
     "How wide the screen for cutscenes, the title and menus is, where it opened.", nullptr, false, true},
    {TAB_STORED, "Event culling", &opts.vr_event_detail, 0, 1, 1, tx_vr_event_detail,
     "Test, a cutscene in 3D: Your view makes what the game draws and lights follow where you are and look; Game "
     "camera leaves it to the game's camera.", nullptr, false, true},
    {TAB_STORED, "Re-align after events", &opts.vr_realign, 0, 3, 1, tx_vr_realign,
     "First person: when an event turns Link (talking, a door, a cutscene, arriving on a new map), your view turns by as "
     "much, so Link still faces where you face and the stick moves him straight ahead. Quick turn: a fast turn "
     "once the event is over; Snap at the end: at once; Follow Link: your view turns with him during the event. After a "
     "flat cutscene or a load, and at a warp, it is always at once. Off: your view keeps its heading (as before).",
     nullptr, false, true},
    {TAB_STORED, "Item get view", &opts.vr_item_get_view, 0, 1, 1, tx_vr_item_get_view,
     "First person: when you open a chest or pick up an item for the first time, the game turns Link round to show it to "
     "its camera. Keep your view: your view does not turn with him (no 180 degree snap). Turn with Link: as Re-align after "
     "events says, as before.", nullptr, false, true},
    {TAB_STORED, "Item get", &opts.vr_item_get, 0, 1, 1, tx_vr_item_get,
     "First person: the item you just got is shown in front of you, a little below your eyes, facing you. Over your head: "
     "where the game holds it, over Link's head (yours in first person).", nullptr, false, true},
    {TAB_STORED, "Crawl eye height", &opts.vr_crawl_eye, 0, 60, 5, tx_vr_height,
     "First person: how high your view is while Link crawls, above the tunnel's floor. Auto: the game's own (where its "
     "crawl camera sits, about 30 cm). Or a height: lower it if you see through the tunnel's roof, raise it if you would "
     "rather look over Link's hands.",
     nullptr, false, true},
    {TAB_STORED, "Bow reticle size", &opts.vr_bow_reticle_size, 50, 600, 25, tx_percent,
     "Size of the headset's aim reticles, first and third person (100% is the old size).", nullptr, false, true},
    {TAB_STORED, "GPU profile", &opts.vr_gpu_profile, 0, 1, 1, tx_onoff,
     "Diagnostics: from the next start, the log names what the GPU spends each frame on (every 10 s). Costs speed: it also "
     "makes the replay use one thread.", nullptr, true, true},
    {TAB_STORED, "VR menu button", &opts.vr_menu_button, 0, 1, 1, tx_vr_menu_button,
     "What opens and closes this menu, held half a second. Menu or Y: the left upper button, named Y or B. Both sticks: both "
     "stick (or trackpad) clicks together. The left Menu button does it either way, and F2 on the keyboard.", nullptr, false, true},
    {TAB_STORED, "VR menu position", &opts.vr_menu_position, 0, 1, 1, tx_vr_menu_position,
     "Where the VR menu floats. Fixed in world: where it opened, in front of you, and it stays there while you look around "
     "or walk. Follows your head: it comes back in front of you once you turn away from it.", nullptr, false, true},
    // (Comfort: labels and values only, no help line)
    {TAB_STORED, "Turn style", &opts.vr_turn_style, 0, 1, 1, tx_vr_turn_style, nullptr, nullptr, false, true},
    {TAB_STORED, "Snap angle", &opts.vr_snap_angle, 0, 3, 1, tx_vr_snap_angle, nullptr, nullptr, false, true},
    {TAB_STORED, "Turn speed", &opts.vr_turn_speed, 0, 3, 1, tx_vr_turn_speed, nullptr, nullptr, false, true},
    {TAB_STORED, "Move vignette", &opts.vr_vignette_move, 0, 3, 1, tx_vr_vignette, nullptr, nullptr, false, true},
    {TAB_STORED, "Turn vignette", &opts.vr_vignette_turn, 0, 3, 1, tx_vr_vignette, nullptr, nullptr, false, true},
    {TAB_STORED, "Air vignette", &opts.vr_vignette_air, 0, 3, 1, tx_vr_vignette, nullptr, nullptr, false, true},
    {TAB_STORED, "Carry vignette", &opts.vr_vignette_carried, 0, 3, 1, tx_vr_vignette, nullptr, nullptr, false,
     true},
    {TAB_STORED, "Left wrist pitch", &opts.vr_wrist_l_pitch, -180, 180, 1, tx_vr_degrees,
     "Fine adjustment after wrist calibration.", nullptr, false, true},
    {TAB_STORED, "Left wrist yaw", &opts.vr_wrist_l_yaw, -180, 180, 1, tx_vr_degrees,
     "Fine adjustment after wrist calibration.", nullptr, false, true},
    {TAB_STORED, "Left wrist roll", &opts.vr_wrist_l_roll, -180, 180, 1, tx_vr_degrees,
     "Fine adjustment after wrist calibration.", nullptr, false, true},
    {TAB_STORED, "Right wrist pitch", &opts.vr_wrist_r_pitch, -180, 180, 1, tx_vr_degrees,
     "Fine adjustment after wrist calibration.", nullptr, false, true},
    {TAB_STORED, "Right wrist yaw", &opts.vr_wrist_r_yaw, -180, 180, 1, tx_vr_degrees,
     "Fine adjustment after wrist calibration.", nullptr, false, true},
    {TAB_STORED, "Right wrist roll", &opts.vr_wrist_r_roll, -180, 180, 1, tx_vr_degrees,
     "Fine adjustment after wrist calibration.", nullptr, false, true},
