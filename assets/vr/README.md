# Hookshot VR's own art

- `panel_frame.png` (1670 x 941): the leather and copper frame, made for Hookshot VR by its author. The VR menu draws it as
  a 9-slice (source edges 215 150 226 150, `vr/menu/vr_menu.inc`); the wrist HUD and the radial item wheel use its
  leather middle (`vr/menu/wrist_draw.inc`, `vr/menu/radial_draw.inc`). The build packs it into a header of its own
  (`vr/tools/generate/panel_frame.py`); without the file the menu draws a plain copper frame.

Nothing here is taken from the game: the item pictures the menus show come from the player's own game files at build time
(`vr/tools/generate/icons.py`).
