# Testing HookshotVR on Meta's PC runtime

Some players on **Meta's PC runtime** (Quest Link, Air Link, Rift S, Rift CV1) saw a doubled or wrong picture, flat and
far away, including on the title screen and the menus. HookshotVR v1.0.2 changes how the title, the menus, the movies and
the game are sent to the headset. **We could not test it on Meta's runtime, so we do not know yet if it helps.** If you
have a Quest on Link or Air Link, or a Rift, you can tell us. It takes about 20 minutes. Thank you.

You need HookshotVR Alpha v1.0.2 built and playing, the headset connected, and no secrets: the logs are safe to send
(see [How to send a log](vr-troubleshooting.md#how-to-send-a-log)).

## Before you start

1. **Close SteamVR.** Make **Meta** your OpenXR runtime: in the Meta Quest Link app, **Settings** › **General**.
2. **Render resolution 100%.** In the game's own menu (not the launcher): start the game, press **F1** (or **Insert**)
   with the game window active, and in **Graphics** set **Render resolution** to **100%** (not **Auto**). It may ask to
   restart: let it.
3. **Find the game's folder.** In the launcher, **Settings** › **Hookshot Folder** › **Show**, then open `main-game`.
   `hookshot_env.txt` goes there, next to `image.bin`. If it is not there, make a plain text file with that name (on
   Windows, check it is not really `hookshot_env.txt.txt`). One line each, nothing else.
4. **Put the headset on and keep it on** until the game is open. Press **Play** in the launcher (if it asks
   "Headset not found. Play anyway?", choose **Play**).

## What to do in each run

Each run is: edit `hookshot_env.txt`, start the game, do the steps, quit. You open and close the VR menu at each marker
**M**: hold the left Menu button (or **Y**) for half a second, again to close. This puts a timed line in the log.

At each **M**, close one eye, then the other, and write down:

- Is the picture **one** or **two** (doubled)? Left eye alone, right eye alone.
- Is the VR menu itself **one** or **two**?

## Run 1: no switches

`hookshot_env.txt` empty, or without any line starting with `RX_VR_`.

1. Title screen, head still, 10 seconds. **M1**.
2. Turn your head left and right slowly, then lean. **M2**.
3. Press and hold the Meta (Oculus) button to recenter. **M3**.
4. Open the file list, load a save, and stand still in play for 10 seconds. **M4**. (The VR menu's **3D** should say
   **Mono**.)
5. VR menu: **General** › **3D** › **Stereo**. Close the menu and look around for 15 seconds. **M5**.
6. VR menu: **3D** › **Mono**. 15 seconds. **M6**.
7. Open the game's pause menu (a flat screen). **M7**. Go back to play.
8. VR menu: **View** › **Third person**, then **First person**, 10 seconds each. **M8**.
9. Look at your wrist (the wrist HUD). **M9**. Quit the game.

## Run 2: a different way to send the flat screens

Put this one line in `hookshot_env.txt` (replace the old line):

```text
RX_VR_FLAT_LAYER=quad
```

Do steps 1, 2, 3 and 7 of run 1, with their markers. Quit.

## Run 3: the old way (it should look wrong, as before)

```text
RX_VR_FLAT_LAYER=projection
```

Do steps 1, 2 and 3. Quit.

## Run 4: the old gameplay pose

```text
RX_VR_MONO_POSE=head
```

Do steps 4 and 6. Quit.

## Run 5: only if the title or **Mono** was still doubled in run 1

```text
RX_VR_LAYER_FOV=symmetric
```

Do steps 1, 4 and 5. Quit.

## Send us the results

HookshotVR keeps six runs' logs, so five starts are all there is room for: do not start the game a sixth time before you
send them.

1. In the launcher, **Settings** › **Logs** › **Open**. (Windows: `%LOCALAPPDATA%\HookshotVR\logs`; Linux:
   `~/.local/share/HookshotVR/logs`.)
2. Send the **whole Logs folder** (zip it), or at least `last-run.log` and `last-run.1.log` to `last-run.5.log`.
3. In your message, say:
   - your headset, and how it connects (cable or Air Link), and the Meta PC app's version;
   - your graphics card;
   - for each marker M1 to M9: **single or doubled**, left eye alone, right eye alone, and the VR menu single or doubled;
   - which run was which (the newest log is `last-run.log`; the one before it is `.1`).

The Meta mirror window does not show doubling. A phone photo through each lens does help.

Send it as an issue on this project's GitHub page: the **Game problem** form asks for the rest.

## What we will look for

You do not need this part. A single VR menu with a doubled title means the flat picture's layer is at fault; both doubled
means it is not the layer type (the eye distance, the headset's own calibration, the eye buffers). "Run 1 single and run 3
doubled" confirms the flat-screen change, and "run 1 single and run 4 doubled" confirms Mono's.
