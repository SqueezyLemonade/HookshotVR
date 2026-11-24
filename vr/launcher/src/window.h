// The launcher's window: SDL2 loaded at run time (as the game does, host/hos_sdl.c: no SDL headers or import library needed
// to build), an OpenGL 3.3 context, and Dear ImGui's input and drawing on it (window.cpp).
#pragma once
#include <stdint.h>
#include "imgui.h"

namespace win {

// Opens the window. hidden: for --screenshot, nothing shows on the desktop. Width and height are in window units.
bool open(const char *title, int width, int height, bool hidden);
// Why the last open failed, in a player's words (SDL2 missing, no display, no OpenGL 3.3), with SDL's own reason
const char *open_error();
void close();

// Draw the next frames into an offscreen image of width x height pixels instead of the window (scale: pixels per unit).
void set_offscreen(int width, int height, float scale);
// The offscreen image's pixels after end_frame: RGBA, top row first, width * height * 4 bytes.
bool read_offscreen(uint8_t *rgba_out);

// One frame: waits up to wait_ms for an event, then takes them all. False when the window was closed.
bool begin_frame(int wait_ms);
// Draws ImGui's frame to the window (or the offscreen image) and shows it.
void end_frame();

// A texture of our own (art slots, the leather grain): RGBA, top row first. repeat: tiles past 0..1.
ImTextureID texture(const uint8_t *rgba, int width, int height, bool repeat);

void set_hand_cursor(bool hand);
void raise();
void hide();
void show();
bool focused();

}  // namespace win
