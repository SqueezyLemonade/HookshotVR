/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
/* Renderer log protocol consumed by host crash/freeze records. Keep these in
   step with checkpoints_report (vk_draw.cpp) and replay_fatal (vk_replay.cpp).
   Only a prefix at the start of a line counts, never a quoted mention. */
#define HOS_REPORT_DEVICE_LOST_PREFIX "vk: device lost:"
#define HOS_REPORT_FATAL_PREFIX "vk: FATAL:"
/* VR HOOK: Hookshot VR's Resizable BAR hint (vr/headset/rebar_hint.cpp): a crash after it names Resizable BAR as the cause
   (host/hos_crash_dialog.h) */
#define HOS_REPORT_VR_REBAR_PREFIX "vk: VR: Resizable BAR is off:"
