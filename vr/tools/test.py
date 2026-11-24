#!/usr/bin/env python3
"""Hookshot VR's master tester: every automatic check, in one file.

    python3 vr/tools/test.py                 run every check
    python3 vr/tools/test.py bow sword       only these systems (headset, camera, input, body, bow, sword, gear, bombs, shield, stereo, build)
    python3 vr/tools/test.py --list          list the checks and what each guards
    python3 vr/tools/test.py --work <folder> also check the translated game (the build's work folder) for the game hooks
    python3 vr/tools/test.py --romfs <folder> also run the checks that need your own game files (its romfs folder)

How a check works: it takes the REAL function from the source (found by a line of its text, through FILES below), compiles
it with a small fake of the game's memory or the renderer, and asserts what it must do. No game, headset or GPU needed.
A check that needs something you have to provide (a file from your own game, the build's output) says SKIP and why.

When code moves to another file, change its entry in FILES. Each check names the bug it guards.
"""
import argparse
import atexit
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import traceback
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# ---- where each piece of code lives (change here when code moves) ------------------------------------------------------
FILES = {
    'headset': 'vr/headset/vk_vr.cpp',
    'body': 'vr/body/vk_vr_body.cpp',             # Link's first-person body, held items' attachment
    'vr_h': 'vr/vk_vr.h',                         # the renderer side's VR API
    'stereo_eyes': 'vr/stereo/vk_vr_eyes.cpp',     # true stereo
    'spirv_eyes': 'vr/stereo/vk_vr_spirv_eyes.cpp',  # true stereo's shader patches
    'stereo_inputs': 'vr/stereo/inputs_draw.inc',  # true stereo's draw policy (which draws are moved)
    'reticle_draw': 'vr/items/bow/reticle_draw.inc',
    'stick_host': 'vr/input/stick_host.c',
    'controllers_host': 'vr/input/controllers_host.c',
    'quick_select_host': 'vr/input/quick_select_host.c',
    'radial_host': 'vr/menu/radial_host.c',
    'wrist_host': 'vr/menu/wrist_host.c',          # the wrist check HUD: the game's values, and why it may not show
    'wrist_draw': 'vr/menu/wrist_draw.inc',        # the wrist check HUD in the headset: its gesture and its panel
    'vr_menu': 'vr/menu/vr_menu.inc',              # the VR menu: its rows, controls and drawing (in vk_menu.cpp)
    'vr_menu_layer': 'vr/menu/vr_menu_layer.inc',  # the VR menu in the headset: its place, its pointers
    'vr_menu_api': 'vr/menu/vr_menu_api.inc',      # the VR menu's open state, images, and the controllers kept from the game
    'bow_host': 'vr/items/bow/bow_host.c',
    'bow_patch': 'vr/items/bow/vr_bow_patch.py',
    'culling_host': 'vr/camera/culling_host.c',
    'view_host': 'vr/camera/view_host.c',
    'crawl_host': 'vr/camera/crawl_host.c',          # crawl spaces in first person: the view comes down with Link
    'events_host': 'vr/camera/events_host.c',        # the game's events in VR: how each kind is shown (talks, cutscenes)
    'eye_rendering': 'vr/headset/eye_rendering.inc', # the eyes, and the world screen a flat frame is shown on
    'frame_loop': 'vr/headset/frame_loop.inc',
    'comfort_host': 'vr/comfort/comfort_host.c',      # the Comfort Aids: what moves the view, for the vignette
    'vignette': 'vr/comfort/vignette_layer.inc',      # the vignette round both eyes in the headset
    'head_camera_host': 'vr/camera/head_camera_host.c',   # the game's camera fade kept off Link in first person
    'skin_draw': 'vr/body/skin_draw.inc',          # Link's draws in the replay: his pose, head and shadow left out
    'gear_near_host': 'vr/items/gear/gear_near_host.c',
    'equipment_host': 'vr/items/gear/equipment_host.c',
    'put_away_host': 'vr/items/gear/put_away_host.c',   # putting away what Link holds with the game's own call
    'guard_host': 'vr/items/shield/guard_host.c',
    'sword_host': 'vr/items/sword/sword_host.c',   # the sword's real hits; what sits on Link (the bracelet)
    'bomb_host': 'vr/items/bomb/bomb_host.c',       # bombs in the VR hands (game side)
    'bomb_math': 'vr/items/bomb/bomb_math.h',       # their throw arc and pull arithmetic
    'bomb_marks': 'vr/items/bomb/bomb_marks.inc',   # the arc and its target in the headset (parked)
    'hookshot_host': 'vr/items/hookshot/hookshot_host.c',   # the hookshot and longshot in a VR hand
    'boomerang_host': 'vr/items/boomerang/boomerang_host.c',   # the Boomerang in the VR hands: controls and aim
    'tp_aim_host': 'vr/items/aim/third_person_aim_host.c',   # third person: where the aimed shot lands (its reticle)
    'game_hooks': 'vr/vr_game_hooks.py',
    'vk_replay': 'gpu/vk/vk_replay.cpp',
    'vk_draw': 'gpu/vk/vk_draw.cpp',
}
GENERATE = REPO / 'vr' / 'tools' / 'generate'   # makes data from the player's own game files
GENERATED = None   # where that data is for this run: the build's (<work>/vr_generated), or made from --romfs


def path(key):
    return REPO / FILES[key]


def expand(file):
    """A file's text with its #include "*.inc" pieces put in place, as the compiler sees them: a check finds its function
    wherever the piece holding it lives"""
    out = []
    for line in file.read_text(encoding='utf-8').split('\n'):
        s = line.strip()
        if s.startswith('#include "') and s.split('"')[1].endswith('.inc'):
            out.append(expand((file.parent / s.split('"')[1]).resolve()))
        else:
            out.append(line)
    return '\n'.join(out)


def read(key):
    return expand(path(key))


def game_tools():
    """vr/tools/generate's modules (they need the builder's Python: numpy and zstandard)"""
    if str(GENERATE) not in sys.path:
        sys.path.insert(0, str(GENERATE))
    try:
        import generate
        import game_files
        return generate, game_files
    except ImportError as e:
        raise Skip(f'reading your game files needs numpy and zstandard (the Hookshot builder\'s Python): {e}')


def generated(name):
    """A header made from your own game files: the build's, or made now (into a temporary folder) from --romfs"""
    global GENERATED
    if GENERATED is None:
        built = Path(ARGS.work) / 'vr_generated' if ARGS.work else None
        if built and (built / name).exists():
            GENERATED = built
        elif ARGS.romfs:
            generate, _ = game_tools()
            GENERATED = Path(tempfile.mkdtemp(prefix='hookshot-vr-generated-'))
            atexit.register(shutil.rmtree, GENERATED, True)
            generate.generate(ARGS.romfs, str(GENERATED))
        else:
            raise Skip(f'needs {name}, made from your game files: --work <a build\'s work folder> or --romfs <folder>')
    p = GENERATED / name
    if not p.exists():
        raise Skip(f'needs {name}, made from your game files (not in {GENERATED})')
    return p


def game_file(rel):
    """A file of your own game (--romfs), decompressed"""
    if not ARGS.romfs:
        raise Skip('needs your own game files: --romfs <folder>')
    _, game_files = game_tools()
    return game_files.GameFiles(ARGS.romfs).read(rel)


class Skip(Exception):
    pass


def skip(why):
    raise Skip(why)


def compile_run(source, cpp=False, flags=()):
    """Compiles a check's C or C++ and runs it: a failed assert stops it, and the check fails (flags: more for the compiler,
    as -pthread)"""
    with tempfile.TemporaryDirectory(prefix='hookshot-vr-test-') as directory:
        executable = str(Path(directory) / 'check')
        subprocess.run(['c++' if cpp else 'cc', '-x', 'c++' if cpp else 'c', '-std=c++17' if cpp else '-std=c11',
                        '-include', str(REPO / 'gpu/vk/compat/hookshot_env.h'), '-DHOOKSHOT_VR_RESEARCH', *flags,
                        '-', '-lm', '-o', executable], input=source, text=True, check=True, capture_output=True)
        subprocess.run([executable], check=True, capture_output=True)


# ======================================================================================================================
# The headset: source layout and tracked-pose mathematics
# ======================================================================================================================

def check_headset_layout():
    """the headset entry point expands its sections in order, keeping one API and no body recorder"""
    source = read('headset')
    anchors = ['bool asked()', 'bool load_loader()', 'struct State {', 'XrQuaternionf qmul(',
               'bool make_eyes(', 'struct HandState {', 'bool make_marker(', 'void headset_thread()',
               'bool wanted()', 'VkImage menu_target(', 'bool link_body(', 'extern "C" int hookshot_vr_head(']
    positions = [source.index(anchor) for anchor in anchors]
    assert positions == sorted(positions)
    for anchor in anchors:
        assert source.count(anchor) == 1, f'duplicate implementation: {anchor}'
    assert 'RX_VR_BODY_DUMP' not in read('body')
    assert 'vr-body-dump.bin' not in read('body')
    build = (REPO / 'nx2/build.py').read_text()
    assert '".inc"' in build, 'included sections must participate in build invalidation'


def check_headset_rotation():
    """head/controller rotations survive the file split: inverse round-trip and yaw-only recentering"""
    source = read('headset')
    functions = source[source.index('XrQuaternionf qmul('):source.index('void publish_pose(')]
    compile_run(r'''
    #include <cmath>
    #include <cassert>
    #include <initializer_list>
    struct XrQuaternionf { float x, y, z, w; };
    ''' + functions + r'''
    static void near(float a, float b) { assert(std::fabs(a-b)<1e-5f); }
    int main() {
        const float vector[3]={.3f,-.8f,1.2f};
        for (float yaw : {-2.f, -.7f, 0.f, .8f, 2.7f}) {
            XrQuaternionf q={0,std::sin(yaw/2),0,std::cos(yaw/2)};
            float rotated[3], restored[3];
            qrot(q,vector,rotated); qrot(qinv(q),rotated,restored);
            for (int k=0;k<3;k++) near(vector[k],restored[k]);
            XrQuaternionf identity=qmul(yaw_inverse(q),q);
            near(identity.x,0); near(identity.y,0); near(identity.z,0); near(identity.w,1);
            // Recentering removes yaw, not pitch: looking up must remain looking up.
            XrQuaternionf pitch={std::sin(.2f),0,0,std::cos(.2f)};
            XrQuaternionf relative=qmul(yaw_inverse(qmul(q,pitch)),qmul(q,pitch));
            near(relative.x,pitch.x); near(relative.y,0); near(relative.z,0); near(relative.w,pitch.w);
        }
    }
    ''', cpp=True)


B_DIRECT = '::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);'
# (vr/vk_vr.h functions the replay calls with one of its command buffers that record into it, or destroy what its commands use,
# themselves: each lets the recorder thread catch up first. FORWARD: ones that only pass the command buffer on to these)
REPLAY_DIRECT = {'headset': ('VkImage menu_target(', 'void menu_ready(', 'void scene_done(', 'void present(', 'VkImage panel_target(',
                             'void panel_ready('),
                 'stereo_eyes': ('void eyes_scene_done(', 'void eyes_present_test(')}
REPLAY_FORWARD = ('eyes_scene_end', 'eyes_before_dispatch')
# (ROUTED: the VR menu's drawing, vr/menu/vr_menu.inc, a piece of gpu/vk/vk_menu.cpp: its vkCmd* calls go through vk_work.h, the
# recorder's ring, and Dear ImGui's direct recording after a boundary, as the Insert menu's)
REPLAY_ROUTED = ('panel_record', 'panel_record_image')
RECORDS = re.compile(r'\b(vkCmd\w+|barrier|memory_barrier|make_image|vkDestroy\w+|vkFree\w+|depth_view1?)\s*\(')


def function_body(source, head):
    start = source.index(head)
    open_at = start
    if '(' in head:   # (past the parameter list: a default argument's {} is not the body)
        depth, i = 0, start + head.index('(')
        while True:
            depth += {'(': 1, ')': -1}.get(source[i], 0)
            if depth == 0:
                break
            i += 1
        open_at = i
    open_at = source.index('{', open_at)
    depth = 0
    for i in range(open_at, len(source)):
        depth += {'{': 1, '}': -1}.get(source[i], 0)
        if depth == 0:
            return source[open_at:i + 1]
    raise AssertionError(f'{head}: no end')


def replay_direct_problems(sources):
    """what is wrong with the replay-side functions' recorder boundaries in these sources ({key: text})"""
    problems = []
    for key, heads in REPLAY_DIRECT.items():
        for head in heads:
            body = re.sub(r'//[^\n]*', '', function_body(sources[key], head))
            first = RECORDS.search(body)
            if not first:
                problems.append(f'{head}: records nothing (move it to REPLAY_FORWARD)')
            elif body.find(B_DIRECT) < 0 or body.find(B_DIRECT) > first.start():
                problems.append(f'{head}: {first.group(1)} before the recorder boundary (B_DIRECT)')
    return problems


def check_replay_recorder():
    """Threaded rendering's recorder thread: every VR call that records into the replay's command buffers directly lets the
    recorder catch up first (without it two threads wrote one command buffer: Xid 13 / 31, device lost in Kakariko, 19 Nov)"""
    sources = {key: read(key) for key in REPLAY_DIRECT}
    assert '#include "vk_work.h"' not in sources['headset'] + sources['stereo_eyes'], \
        'vk_work.h would route the headset thread\'s own calls through the recorder too (and make it wait for it)'
    problems = replay_direct_problems(sources)
    assert not problems, problems
    # every function of the renderer-side API that takes a command buffer is one of the two kinds
    header = read('vr_h')
    taking = set(re.findall(r'^\w[\w\s\*]*?\b(\w+)\(VkCommandBuffer\b', header, re.M))
    known = {h.split()[-1].rstrip('(') for heads in REPLAY_DIRECT.values() for h in heads} | set(REPLAY_FORWARD) | set(REPLAY_ROUTED)
    assert taking <= known, f'new vr/vk_vr.h functions taking a command buffer, not checked here: {sorted(taking - known)}'
    menu = read('vr_menu')
    for name in REPLAY_ROUTED:   # (in a file that routes through vk_work.h; ImGui's recording only after a boundary)
        body = re.sub(r'//[^\n]*', '', function_body(menu, f'void vr::{name}('))
        assert body.count('ui::record_to(') == 1 and body.find(B_DIRECT) < body.find('ui::record_to('), name
    vk_menu = (REPO / 'gpu/vk/vk_menu.cpp').read_text(encoding='utf-8')
    assert vk_menu.index('#include "vk_work.h"') < vk_menu.index('#include "../../vr/menu/vr_menu.inc"')
    assert '#include' not in re.sub(r'//[^\n]*', '', menu), 'vr_menu.inc includes: vk_work.h must stay vk_menu.cpp\'s last'
    for name in REPLAY_FORWARD:   # (these only hand it on)
        text = sources['headset'] + sources['stereo_eyes']
        body = re.sub(r'//[^\n]*', '', function_body(text, f'void {name}('))
        assert not RECORDS.search(body.replace('eyes_scene_done(', '')), f'{name} records itself: make it a REPLAY_DIRECT one'
    # (controls: each boundary taken out, or moved after the first recording, is caught)
    for key, heads in REPLAY_DIRECT.items():
        for head in heads:
            body = function_body(sources[key], head)
            gone = dict(sources)
            gone[key] = sources[key].replace(body, body.replace(B_DIRECT, '', 1), 1)
            assert replay_direct_problems(gone), f'{head}: a missing boundary is not caught'


def check_headset_queue():
    """the headset's queue (index 1) is the headset thread's from xrBeginFrame to xrEndFrame, and the renderer's
    vkDeviceWaitIdle takes it too (it needs every queue; the suspect for a window resize that lost the GPU, 19 Nov, Xid 32)"""
    replay = path('vk_replay').read_text()
    wait = function_body(replay, 'void device_wait_idle_locked() {')
    assert wait.index('vr::queue_mutex()') < wait.index('vkDeviceWaitIdle('), 'the device-wide wait without the headset queue'
    for f in sorted((REPO / 'gpu/vk').glob('*.cpp')):   # (the renderer's other vkDeviceWaitIdle calls: the replay bench only)
        text = re.sub(r'//[^\n]*', '', f.read_text(encoding='utf-8', errors='replace'))
        calls = len(re.findall(r'\bvkDeviceWaitIdle\(', text))
        allowed = {'vk_replay.cpp': 3}.get(f.name, 0)
        assert calls <= allowed, f'{f.name}: {calls} vkDeviceWaitIdle calls: through device_wait_idle_locked (the headset queue)'
    source = read('headset')
    loop = function_body(source, 'void headset_thread() {')
    at = {k: loop.index(k) for k in ('std::unique_lock queue_lk(s.queue_mu);', 'xrBeginFrame(', 'make_eyes(', 'radial_layer(',
                                     'make_menu_quad(', 'xrEndFrame(', 'queue_lk.unlock();')}
    assert list(at.values()) == sorted(at.values()), f'the frame not inside the headset queue lock: {at}'
    assert loop.index('xrWaitFrame(') < at['std::unique_lock queue_lk(s.queue_mu);'], 'held through xrWaitFrame (a frame long)'
    poll = loop.index('poll_events();')
    assert loop.rfind('std::lock_guard queue_lk(s.queue_mu);', 0, poll) >= 0, 'the session\'s start and end without the lock'
    # every submit to the headset queue is in a function the locked frame calls, on the headset thread
    for fn in ('bool make_eyes(', 'bool make_menu_quad(', 'bool make_marker(', 'bool radial_layer(', 'int bomb_mark_layers('):
        assert fn in source, fn
    submits = [m.start() for m in re.finditer(r'vkQueueSubmit2\(s\.queue', source)]   # (all through headset_submit)
    assert len(submits) == 1 and 'vkQueueSubmit2(s.queue' in function_body(source, 'bool headset_submit('), submits
    assert 'std::mutex& queue_mutex() { return s.queue_mu; }' in source


def check_hud_decided_once():
    """HUD size: whether a draw is the 2D layer's is decided once, at the draw (Targets::hud); its key, its vertex stage
    (built later on a worker) and its scissor read that, never the 2D layer's image as learned at that later time"""
    draw = (REPO / 'gpu/vk/vk_draw.cpp').read_text()
    targets = function_body(draw, 'struct Targets {')
    assert 'float hud = 0;' in targets
    note = function_body(draw, 'inline void vr_hud_note(')
    assert 'vr_hud_target.load(' in note and 'vr::hud_scale()' in note
    assert re.search(r'inline bool vr_hud_draw\(const NvnState&, const Targets& at\) \{ return at\.hud > 0\.0f; \}', draw)
    assert 'const float hs = at.hud;' in function_body(draw, 'inline uint64_t vr_hud_salt(')
    build = function_body(draw, 'VkPipeline build_graphics(const Program& p, const NvnState& s, const Targets& at, VkPrimitiveTopology '
                                'topo, int part, bool* cold) {')
    assert 'vr::hud_module(si.module, at.hud)' in build and 'vr::hud_scale()' not in build
    uses = [m.start() for m in re.finditer(r'vr_hud_target\.load\(', draw)]
    assert len(uses) == 1, 'the 2D layer\'s image read outside vr_hud_note: %d reads' % len(uses)
    at = draw.index('Targets at = bare_targets(s, targets(s));')
    assert draw.index('vr_hud_note(s, at);', at) < draw.index('pipeline_key(', at), 'decided after the draw\'s key'


HEADSET_GPU_CALLS = re.compile(r'\b(xrAcquireSwapchainImage|xrWaitSwapchainImage|xrReleaseSwapchainImage|vkResetCommandPool|'
                               r'vkResetFences|vkWaitForFences|vkQueueSubmit2|vkBeginCommandBuffer)\s*\(')
HEADSET_HELPERS = ('void swap_give(', 'bool headset_gpu_free(', 'bool swap_take(', 'bool headset_record(', 'bool headset_submit(')
HEADSET_SITES = ('bool make_eyes(', 'bool make_menu_quad(', 'bool make_marker(', 'bool radial_upload(', 'bool bomb_mark_make(',
                 'bool panel_copy(', 'bool panel_make_ray(', 'bool panel_make_dot(', 'bool vignette_make(', 'bool blank_layer(')


def check_headset_clean_exit():
    """the game's quit ends the headset's session and destroys it and the OpenXR instance, waited for 2 s at most (a hung
    runtime cannot keep the game open); and no frame ends with zero layers (WiVRn went black and dropped input): a blank one"""
    loop = (REPO / FILES['frame_loop']).read_text(encoding='utf-8')
    session = (REPO / 'vr/headset/runtime_session.inc').read_text(encoding='utf-8')
    main = (REPO / 'gpu/vk/vk_main.cpp').read_text(encoding='utf-8')
    close_asked = function_body(main, 'void mkwvk_close_asked(void) {')
    assert 'vr::close();' in close_asked and close_asked.index('vr::close();') < close_asked.index('pipeline_cache_close();')
    close = function_body(session, 'void close() {')
    assert 'constexpr int kCloseWaitMs = 2000;' in close and 'waited() < kCloseWaitMs' in close, 'vr::close must not wait forever'
    assert 'compare_exchange_strong(idle, 1)' in close and 'std::thread(headset_destroy).detach();' in close
    destroy = function_body(loop, 'void headset_destroy() {')
    assert destroy.index('xrDestroySession(') < destroy.index('xrDestroyInstance(') and 'close_step.store(2' in destroy
    assert 's.running = false, s.session_running = false;' in destroy   # (active() false first: the game side lets go)
    thread = function_body(loop, 'void headset_thread() {')
    assert thread.rstrip().endswith('headset_destroy();   // (the game quits: the loop left above)\n}')
    assert 'xr_request_exit(s.session)' in thread and 'now_ns > close_until) break;' in thread
    # (zero layers never: the blank quad added just before the frame ends)
    end = thread.index('const XrResult ended = xrEndFrame(s.session, &ei);')
    blank = thread.index('if (!n_layers && blank_layer(blank_quad))')
    assert blank < end and 'ei.layerCount = n_layers;' in thread[blank:end]
    body = function_body(loop, 'bool blank_layer(XrCompositionLayerQuad& q) {')
    assert 'XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT' in body and 'VkClearColorValue clear{{0, 0, 0, 0}}' in body
    assert 'XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT' in body   # (transparent: nothing shows)
    loader = (REPO / 'vr/headset/runtime_loader.inc').read_text(encoding='utf-8')
    assert 'X(xrRequestExitSession)' not in loader, 'xrRequestExitSession is optional: a required one turns VR off on a runtime without it'


def check_headset_frame_ownership():
    """the headset thread's layers acquire, wait, submit and release through one helper: every result checked, nothing
    released and the shared pool and fence never reset while its submit is pending (the outside review's finding 1, 20 Nov)"""
    source = read('headset')
    code = re.sub(r'//[^\n]*', '', source)
    # (1) only the helpers make those calls
    helpers = ''.join(function_body(code, h) for h in HEADSET_HELPERS)
    rest = code
    for h in HEADSET_HELPERS:
        rest = rest.replace(function_body(code, h), '')
    loose = sorted({m.group(1) for m in HEADSET_GPU_CALLS.finditer(rest)})   # (the loader's X(...) list has no call in it)
    assert not loose, 'outside the helpers: %s' % loose
    assert HEADSET_GPU_CALLS.search(helpers)
    # (2) each site asks headset_gpu_free before it acquires
    for site in HEADSET_SITES:
        body = function_body(code, site)
        assert 'headset_gpu_free()' in body and 'swap_take(' in body, site
        assert body.index('headset_gpu_free()') < body.index('swap_take('), site + ': acquires before asking'
    assert function_body(code, 'bool make_eyes(').index('headset_gpu_free()') < \
        function_body(code, 'bool make_eyes(').index('vkUpdateDescriptorSets('), 'make_eyes updates its sets before asking'
    # (3) the helpers' behaviour, with stand-ins for OpenXR and Vulkan
    start = source.index('constexpr uint64_t kHeadsetWaitNs')
    block = source[start:source.index('bool make_eye(', start)]
    compile_run(r'''
    #include <cassert>
    #include <chrono>
    #include <cstdarg>
    #include <cstdint>
    #include <cstdio>
    #include <atomic>
    #include <functional>
    #include <initializer_list>
    #include <vector>
    #include <string>
    typedef int XrResult; typedef int VkResult; typedef uint64_t XrDuration;
    typedef struct XrSwapchain_* XrSwapchain; typedef void* VkSemaphore;
    enum { XR_SUCCESS = 0, XR_TIMEOUT_EXPIRED = 1, XR_ERROR_RUNTIME_FAILURE = -2, VK_SUCCESS = 0, VK_NOT_READY = 1, VK_TIMEOUT = 2,
           VK_ERROR_DEVICE_LOST = -4 };
    #define XR_FAILED(r) ((r) < 0)
    #define XR_INFINITE_DURATION 0x7fffffffffffffffLL
    enum { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO, XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO,
           VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
           VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT = 1,
           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT = 1, VK_TRUE = 1 };
    struct XrSwapchainImageAcquireInfo { int type; }; struct XrSwapchainImageWaitInfo { int type; long long timeout; };
    struct XrSwapchainImageReleaseInfo { int type; };
    struct VkSemaphoreSubmitInfo { int type; VkSemaphore semaphore; uint64_t value; int stageMask; };
    struct VkCommandBufferSubmitInfo { int type; void* commandBuffer; };
    struct VkSubmitInfo2 { int type; int waitSemaphoreInfoCount; const VkSemaphoreSubmitInfo* pWaitSemaphoreInfos;
                           int commandBufferInfoCount; const VkCommandBufferSubmitInfo* pCommandBufferInfos; };
    struct VkCommandBufferBeginInfo { int type; int flags; };
    struct { void* device; } dev; struct { void* queue; } s; void* cmd_pool; void* cmd; void* fence;
    std::vector<std::string> calls;   // (the order of what reached the stand-ins)
    int acquired = 0, released = 0, fail_acquire_at = -1, fail_wait_at = -1, acquires = 0;
    VkResult submit_result = VK_SUCCESS, wait_result = VK_SUCCESS, fence_status = VK_SUCCESS;
    void logf(const char* f, ...) { (void)f; calls.push_back("log"); }
    XrResult xrAcquireSwapchainImage(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t* i) {
        if (acquires++ == fail_acquire_at) return XR_ERROR_RUNTIME_FAILURE;
        acquired++, *i = 0; return XR_SUCCESS; }
    XrResult xrWaitSwapchainImage(XrSwapchain, const XrSwapchainImageWaitInfo*) {
        return acquires - 1 == fail_wait_at ? XR_ERROR_RUNTIME_FAILURE : XR_SUCCESS; }
    XrResult xrReleaseSwapchainImage(XrSwapchain, const XrSwapchainImageReleaseInfo*) { released++; calls.push_back("release"); return XR_SUCCESS; }
    VkResult vkResetCommandPool(void*, void*, int) { calls.push_back("reset pool"); return VK_SUCCESS; }
    VkResult vkBeginCommandBuffer(void*, const VkCommandBufferBeginInfo*) { return VK_SUCCESS; }
    VkResult vkEndCommandBuffer(void*) { return VK_SUCCESS; }
    VkResult vkResetFences(void*, int, void**) { calls.push_back("reset fence"); return VK_SUCCESS; }
    VkResult vkQueueSubmit2(void*, int, const VkSubmitInfo2*, void*) { calls.push_back("submit"); return submit_result; }
    VkResult vkWaitForFences(void*, int, void**, int, uint64_t ns) { return ns ? wait_result : fence_status; }
    ''' + block + r'''
    XrSwapchain A = (XrSwapchain)1, B = (XrSwapchain)2;
    void reset() { calls.clear(); acquired = released = acquires = 0; fail_acquire_at = fail_wait_at = -1;
                   submit_result = wait_result = fence_status = VK_SUCCESS; }
    int main() {
        uint32_t idx[2];
        SwapTake t;
        // the second eye's acquire fails: the first eye's image is released, nothing kept
        reset(); fail_acquire_at = 1;
        assert(!swap_take(t, {A, B}, idx, "eyes") && acquired == 1 && released == 1 && t.n == 0);
        // the second eye's wait fails: the first released; the second (acquired, not waited) cannot be
        reset(); fail_wait_at = 1;
        assert(!swap_take(t, {A, B}, idx, "eyes") && acquired == 2 && released == 1);
        // done: both released, the after-work run
        reset(); bool after = false;
        assert(swap_take(t, {A, B}, idx, "eyes") && headset_record());
        assert(headset_submit(t, nullptr, 0, "eyes", [&] { after = true; }) && released == 2 && after && headset_gpu_free());
        // not submitted: released, the after-work run, nothing pending
        reset(); after = false; submit_result = VK_ERROR_DEVICE_LOST;
        assert(swap_take(t, {A}, idx, "x") && headset_record() && !headset_submit(t, nullptr, 0, "x", [&] { after = true; }));
        assert(released == 1 && after && headset_gpu_free());
        // the wait gives up: nothing released, nothing run, every layer refused until the fence signals
        reset(); after = false; wait_result = VK_TIMEOUT;
        assert(swap_take(t, {A, B}, idx, "eyes") && headset_record());
        assert(!headset_submit(t, nullptr, 0, "eyes", [&] { after = true; }) && released == 0 && !after);
        fence_status = VK_TIMEOUT;
        const size_t before = calls.size();
        for (int i = 0; i < 3; i++) assert(!headset_gpu_free());
        for (size_t i = before; i < calls.size(); i++) assert(calls[i] != "reset pool" && calls[i] != "reset fence" && calls[i] != "release");
        fence_status = VK_SUCCESS;
        assert(headset_gpu_free() && released == 2 && after && headset_gpu_free());
        // the timeout said in the log
        bool said = false;
        for (auto& c : calls) said = said || c == "log";
        assert(said);
    }
    ''', cpp=True)


def check_menu_setting_keys():
    """renaming Bow reticle preserves bow_direction_preview in old and new options files"""
    source = (REPO / 'gpu/vk/vk_menu.cpp').read_text()
    fn = source[source.index('const char* ini_key('):source.index('// (hookshot) the controls, kept by the host')]
    compile_run(r'''
    #include <cctype>
    #include <cstddef>
    #include <cstring>
    #include <cassert>
    struct { int output_res, vsync, resolution, fps_cap, upscale, dlss, dlss_pre_bloom, handheld, ultrawide, audio_output, vibration, rumble, vr_bow_preview, other; } opts;   // (vibration, rumble: upstream v3's Vibration rows)
    int monitor_pick;   // (Hookshot 2.3's ini_key names it: the fullscreen monitor row)
    struct Row { const char* name; int* value; };
    ''' + fn + r'''
    int main() {
        assert(!std::strcmp(ini_key({"Bow reticle", &opts.vr_bow_preview}), "bow_direction_preview"));
        assert(!std::strcmp(ini_key({"Bow direction preview", &opts.vr_bow_preview}), "bow_direction_preview"));
        assert(!std::strcmp(ini_key({"Bow reticle size", &opts.other}), "bow_reticle_size"));
        assert(!std::strcmp(ini_key({"Output resolution", &opts.output_res}), "output_res"));
    }
    ''', cpp=True)


def check_controller_tables():
    """every controller's suggested bindings (vr/input/openxr_controllers.inc make_controls) bind the same actions on the same
    hands as Quest Touch's, the tested layout, but for the gaps listed here for a controller without those buttons: a
    controller left with no stick, no Menu or no grip on a hand would play wrong with nothing in the log saying why. Each path
    is on its action's hand and named once; a profile from an extension is suggested only with that extension, which the
    instance enables only where the runtime lists it (a missing one never fails the start), and dropped again if the runtime
    refuses its optional extensions; a change of controller is logged when the runtime says so"""
    source = read('headset')
    make = function_body(source, 'void make_controls() {')
    calls = list(re.finditer(r'suggest\("(/interaction_profiles/[a-z0-9_/]+)", (\w+), \{(.*?)\}\);', make, re.S))
    tables = {}
    for m in calls:
        profile, ext, body = m.group(1), m.group(2), m.group(3)
        assert profile not in tables, f'{profile}: suggested twice'
        pairs = re.findall(r'\{ctl\.(\w+), "(/user/hand/(left|right)/(?:input|output)/[a-z0-9_/]+)"\}', body)
        assert len(pairs) == body.count('{ctl.'), f'{profile}: a binding not of the form {{ctl.action, "/user/hand/..."}}'
        bound = set()
        for action, path, hand in pairs:
            key = (action, hand)
            assert key not in bound, f'{profile}: {action} bound twice on the {hand} hand'
            bound.add(key)
            want = {'grip': '/input/grip/pose', 'aim': '/input/aim/pose', 'haptic': '/output/haptic'}.get(action)
            assert not want or path.endswith(want), f'{profile}: {action} bound to {path}'
        tables[profile] = (ext, bound)
    touch = '/interaction_profiles/oculus/touch_controller'
    every = {'grip', 'aim', 'trigger', 'squeeze', 'stick', 'stick_click', 'lower', 'upper', 'menu', 'haptic'}
    assert {a for a, _ in tables[touch][1]} == every, 'Touch binds every action'
    assert ('menu', 'left') in tables[touch][1] and ('menu', 'right') not in tables[touch][1], 'Menu is the left hand\'s'
    # (the gaps: a controller without those buttons; anything else Touch binds, each table binds too)
    gaps = {
        '/interaction_profiles/htc/vive_controller': {('lower', 'left'), ('upper', 'left'), ('upper', 'right')},
        '/interaction_profiles/microsoft/motion_controller': {('upper', 'left')},
        '/interaction_profiles/khr/simple_controller': {('trigger', 'left'), ('trigger', 'right'), ('squeeze', 'left'),
            ('squeeze', 'right'), ('stick', 'left'), ('stick', 'right'), ('stick_click', 'left'), ('stick_click', 'right'),
            ('upper', 'left'), ('upper', 'right')},
    }
    extension = {
        '/interaction_profiles/oculus/touch_controller': 'nullptr',
        '/interaction_profiles/facebook/touch_controller_pro': 'XR_FB_TOUCH_CONTROLLER_PRO_EXTENSION_NAME',
        '/interaction_profiles/meta/touch_controller_plus': 'XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME',
        '/interaction_profiles/valve/index_controller': 'nullptr',
        '/interaction_profiles/htc/vive_controller': 'nullptr',
        '/interaction_profiles/htc/vive_cosmos_controller': 'XR_HTC_VIVE_COSMOS_CONTROLLER_INTERACTION_EXTENSION_NAME',
        '/interaction_profiles/htc/vive_focus3_controller': 'XR_HTC_VIVE_FOCUS3_CONTROLLER_INTERACTION_EXTENSION_NAME',
        '/interaction_profiles/microsoft/motion_controller': 'nullptr',
        '/interaction_profiles/hp/mixed_reality_controller': 'XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME',
        '/interaction_profiles/bytedance/pico4_controller': 'XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME',
        '/interaction_profiles/khr/simple_controller': 'nullptr',
    }
    assert set(tables) == set(extension), f'the profiles suggested: {sorted(set(tables) ^ set(extension))}'
    for profile, (ext, bound) in tables.items():
        assert ext == extension[profile], f'{profile}: suggested with {ext}, not {extension[profile]}'
        missing, extra = tables[touch][1] - bound, bound - tables[touch][1]
        assert not extra, f'{profile}: binds {sorted(extra)}, which Touch does not'
        assert missing == gaps.get(profile, set()), f'{profile}: leaves {sorted(missing)} unbound, not {sorted(gaps.get(profile, set()))}'
    # (the extensions: each listed, enabled only where the runtime lists it, the profile skipped without it)
    listed = function_body(source, 'void controller_instance_extensions(std::vector<const char*>& exts) {')
    table = source[source.index('ControllerExt ctl_ext[] = {'):]
    table = table[:table.index('};')]
    for ext in set(extension.values()) - {'nullptr'}:
        assert '{' + ext + ', false}' in table, f'{ext} is not in ctl_ext'
    assert 'e.on = e.on || !std::strcmp(p.extensionName, e.name);' in listed and 'if (e.on) exts.push_back(e.name)' in listed
    assert 'if (!on) {' in make and 'not offered' in make
    start = function_body(source, 'void instance_extensions(std::vector<const char*>& exts) {')
    assert start.index('controller_instance_extensions(xr_exts);') < start.index('xr_create_instance(&ci, &s.inst)')
    retry = start[start.index('refused its own optional extensions'):]
    assert retry.index('for (ControllerExt& e : ctl_ext) e.on = false;') < retry.index('xr_create_instance(&ci, &s.inst)')
    # (the profile the runtime picked: logged on its event, and checked now and then)
    events = function_body(source, 'void poll_events() {')
    changed = events[events.index('XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED'):]
    assert changed.index('note_profiles();') < changed.index('} else if')
    assert 'if (!ctl.attached) return;' in function_body(source, 'void note_profiles() {')


def check_menu_hold():
    """the left Menu button: a tap is the game's Plus (sent once let go), held 0.5 s it opens or closes the VR menu and sends no
    Plus, a chord (Menu+LS, Menu+RS, Menu+X) is never either; while the VR menu is open a tap sends no Plus, and the game side
    gets the controllers let go but Menu, the grips held as they were"""
    host = read('controllers_host')
    start = host.index('#define kVrMenuHoldNs')
    end = host.index('\n}\n', host.index('static int vr_menu_hold_step(')) + 3
    compile_run(r"""
    #include <stdint.h>
    #include <assert.h>
    """ + host[start:end] + r"""
    static VrMenuHold m;
    static int64_t t;
    /* (90 readings a second) the button down or up for ms milliseconds, a chord pressed from chord_at ms on (-1 none):
       how many hold toggles (1) and taps (2) that made */
    static void run(int down, int ms, int chord_at, int *toggles, int *taps) {
        for (int at = 0; at < ms; at += 11) {
            const int r = vr_menu_hold_step(&m, down, chord_at >= 0 && at >= chord_at, t);
            t += 11000000LL;
            *toggles += r == 1, *taps += r == 2;
        }
    }
    int main(void) {
        int toggles = 0, taps = 0;
        run(0, 200, -1, &toggles, &taps);
        run(1, 150, -1, &toggles, &taps);   /* a quick tap: one Plus, once let go */
        assert(toggles == 0 && taps == 0);
        run(0, 100, -1, &toggles, &taps);
        assert(toggles == 0 && taps == 1);
        taps = 0;
        run(1, 480, -1, &toggles, &taps);   /* held just under half a second, then let go: still a tap */
        run(0, 100, -1, &toggles, &taps);
        assert(toggles == 0 && taps == 1);
        taps = 0;
        run(1, 520, -1, &toggles, &taps);   /* held half a second: the menu toggles once, while still held */
        assert(toggles == 1 && taps == 0);
        run(1, 3000, -1, &toggles, &taps);  /* held on: nothing more */
        run(0, 100, -1, &toggles, &taps);   /* let go: no Plus leaks */
        assert(toggles == 1 && taps == 0);
        run(1, 600, -1, &toggles, &taps);   /* held again: it toggles again (closes) */
        run(0, 100, -1, &toggles, &taps);
        assert(toggles == 2 && taps == 0);
        toggles = 0;
        run(1, 1500, 100, &toggles, &taps); /* Menu+RS (or LS, X) at 0.1 s, held long: neither a toggle nor a Plus */
        run(0, 100, -1, &toggles, &taps);
        assert(toggles == 0 && taps == 0);
        run(1, 150, 50, &toggles, &taps);   /* a quick chord: no Plus either */
        run(0, 100, -1, &toggles, &taps);
        assert(toggles == 0 && taps == 0);
        return 0;
    }
    """)
    # (the wiring: a toggle asks the renderer, a tap is the game's Plus only while the VR menu is closed)
    pad = host[host.index('static void vr_pad_read('):]
    assert 'const int hold = vr_menu_hold_step(&menu_hold, menu, chord, now);' in pad
    assert 'if (hold == 1) {' in pad and 'const int open = rx_vr_menu(1);' in pad
    assert '} else if (hold == 2 && !rx_vr_menu(0)) {' in pad and 'if (!menu && now < plus_until) *raw |= 1ULL << HOS_PAD_START;' in pad
    for chord in ('if (ls) *raw |= 1ULL << HOS_PAD_BACK, chord = 1;', 'if (rs) *raw |= 1ULL << HOS_PAD_GUIDE, chord = 1;',
                  'if (x && !x_was) hos_vr_devmap_mark(), chord = 1;'):
        assert chord in pad, chord
    # (the controllers while it is open: buttons but Menu, trigger and stick let go, the grip no more than when it opened; and
    # after it closes until all is let go)
    layer = read('vr_menu_api')
    mask = layer[layer.index('void panel_mask('):layer.index('\n}\n', layer.index('void panel_mask(')) + 3]
    compile_run(r"""
    #include <stdint.h>
    #include <assert.h>
    #include <algorithm>
    #include <atomic>
    enum { HOOKSHOT_VR_BTN_LOWER = 1, HOOKSHOT_VR_BTN_UPPER = 2, HOOKSHOT_VR_BTN_STICK = 4, HOOKSHOT_VR_BTN_MENU = 8,
           HOOKSHOT_VR_BTN_MENU2 = 64 };
    struct { int vr_menu_button = 0; } opts;
    struct HandState { float trigger = 0, squeeze = 0; uint32_t buttons = 0; };
    struct { HandState shared[2]; } ctl;
    struct { bool holding = false; float grip_held[2]{}; std::atomic<bool> open{false}; } panel;
    bool panel_open() { return panel.open.load(); }
    """ + mask + r"""
    int main() {
        float trigger = .9f, squeeze = .8f, stick[2] = {.5f, .5f};
        uint32_t buttons = HOOKSHOT_VR_BTN_LOWER | HOOKSHOT_VR_BTN_MENU;
        panel_mask(1, trigger, squeeze, stick, &buttons);   /* (closed, never opened: untouched) */
        assert(trigger == .9f && squeeze == .8f && stick[0] == .5f && buttons == (HOOKSHOT_VR_BTN_LOWER | HOOKSHOT_VR_BTN_MENU));
        panel.open = true, panel.holding = true, panel.grip_held[1] = .8f;   /* (opened with the right grip squeezed) */
        panel_mask(1, trigger, squeeze, stick, &buttons);
        assert(trigger == 0 && squeeze == .8f && stick[0] == 0 && stick[1] == 0 && buttons == HOOKSHOT_VR_BTN_MENU);
        float more = 1;   /* (squeezed harder: no more than when it opened; let go: let go, and not squeezed again) */
        trigger = .9f, panel_mask(1, trigger, more, stick, &buttons);
        assert(more == .8f);
        float off = 0, again = 1;
        panel_mask(1, trigger, off, stick, &buttons), panel_mask(1, trigger, again, stick, &buttons);
        assert(off == 0 && again == 0);
        panel.open = false;   /* (closed with B still down: still held) */
        ctl.shared[1].buttons = HOOKSHOT_VR_BTN_UPPER;
        buttons = HOOKSHOT_VR_BTN_UPPER, panel_mask(1, trigger, squeeze, stick, &buttons);
        assert(buttons == 0 && panel.holding);
        ctl.shared[1].buttons = 0;   /* (all let go: the game has them again) */
        buttons = HOOKSHOT_VR_BTN_LOWER, trigger = .2f, panel_mask(1, trigger, squeeze, stick, &buttons);
        assert(!panel.holding && buttons == HOOKSHOT_VR_BTN_LOWER && trigger == .2f);
        return 0;
    }
    """, cpp=True)
    api = (REPO / 'vr/headset/host_api.inc').read_text()
    assert 'vr::panel_mask(h, o.trigger, o.squeeze, o.stick, &o.buttons);' in api   # (hookshot_vr_hands)
    assert 'vr::panel_mask(h, o[7], o[8], nullptr, nullptr);' in api                # (hookshot_vr_hands_world)


def check_menu_y_hold():
    """the VR menu's second way: the left upper button (Y; Index: B) held as long as Menu (one constant) opens the VR menu and
    closes it from any of its levels (SteamVR keeps Touch's left Menu button for its dashboard). A shorter press is Back in the
    menu, as it is let go, and nothing else elsewhere; the press that opened it is no Back and does not close it, the one that
    closed it opens nothing; a hold that was something else's meanwhile (Menu, Hookshot's menu, an ocarina's notes) does nothing
    until let go. With Menu button at Both sticks, both stick clicks held do all that instead of Y (one alone is nothing), and the
    game never gets them. The game side's reading (vr_menu_y_step, panel_mask) and the menu's (vrm_y_tap) run together here"""
    host, layer, menu = read('controllers_host'), read('vr_menu_api'), read('vr_menu')
    start = host.index('#define kVrMenuHoldNs')
    end = host.index('\n}\n', host.index('static int vr_menu_y_step(')) + 3
    mask = layer[layer.index('void panel_mask('):layer.index('\n}\n', layer.index('void panel_mask(')) + 3]
    tap = menu[menu.index('bool vrm_y_tap(bool y) {'):menu.index('\n}\n', menu.index('bool vrm_y_tap(bool y) {')) + 3]
    compile_run(r"""
    #include <stdint.h>
    #include <assert.h>
    #include <algorithm>
    #include <atomic>
    enum { HOOKSHOT_VR_BTN_UPPER = 2, HOOKSHOT_VR_BTN_STICK = 4, HOOKSHOT_VR_BTN_MENU = 8, HOOKSHOT_VR_BTN_GRIP = 32, HOOKSHOT_VR_BTN_MENU2 = 64 };
    struct { int vr_menu_button = 0; } opts;
    struct HandState { float trigger = 0, squeeze = 0; uint32_t buttons = 0; };
    struct { HandState shared[2]; } ctl;
    struct { bool holding = false; float grip_held[2]{}; std::atomic<bool> open{false}; } panel;
    bool panel_open() { return panel.open.load(); }
    void panel_set_open(bool open) { panel.open = open; if (open) panel.holding = true; }
    struct { bool was_open = false, y_was = false, y_pressed = false; } vrm;
    """ + host[start:end] + mask + tap + r"""
    int64_t t;
    int focus, backs, toggles;   /* (the menu's level, as vrm_back walks it: 2 the rows, 1 the sections, 0 the tabs) */
    bool game_saw_y, game_saw_clicks, busy;
    /* 11 ms: the game side's reading, then the menu's (offset: the other way round, which comes first is anybody's) */
    void game_side() {
        uint32_t b = ctl.shared[0].buttons;
        float trigger = 0, squeeze = ctl.shared[0].squeeze, stick[2] = {};
        panel_mask(0, trigger, squeeze, stick, &b);
        game_saw_y = game_saw_y || (b & HOOKSHOT_VR_BTN_UPPER);
        uint32_t r = ctl.shared[1].buttons;
        panel_mask(1, trigger, squeeze, stick, &r);
        const int y = (b & HOOKSHOT_VR_BTN_MENU2) && (opts.vr_menu_button != 1 || (r & HOOKSHOT_VR_BTN_MENU2));   /* (vr_pad_read's) */
        game_saw_clicks = game_saw_clicks || ((b | r) & HOOKSHOT_VR_BTN_STICK);
        if (vr_menu_y_step(y, busy, panel_open(), t)) panel_set_open(!panel_open()), toggles++;
    }
    void menu_side() {   /* (vrm_tick and vrm_input, the Y in them) */
        if (panel_open() != vrm.was_open) vrm.was_open = panel_open(), vrm.y_was = true, vrm.y_pressed = false;
        if (!panel_open()) return;
        if (vrm_y_tap((ctl.shared[0].buttons & HOOKSHOT_VR_BTN_UPPER) != 0)) {
            backs++;
            if (focus > 0) focus--; else panel_set_open(false);
        }
    }
    bool menu_first;
    void hold(bool y, int ms) {
        ctl.shared[0].buttons = (ctl.shared[0].buttons & ~(uint32_t)HOOKSHOT_VR_BTN_UPPER) | (y ? HOOKSHOT_VR_BTN_UPPER : 0);
        for (int at = 0; at < ms; at += 11) {
            if (menu_first) menu_side(), game_side(); else game_side(), menu_side();
            t += 11000000LL;
        }
    }
    void clear() { backs = toggles = 0; game_saw_y = game_saw_clicks = false; }
    void sticks(bool l, bool r, int ms) {   /* (Menu button: Both sticks) */
        ctl.shared[0].buttons = l ? HOOKSHOT_VR_BTN_STICK : 0, ctl.shared[1].buttons = r ? HOOKSHOT_VR_BTN_STICK : 0;
        hold(false, ms);
    }
    int main() {
        assert(kVrMenuHoldNs == 500000000LL);
        for (int order = 0; order < 2; order++) {
            menu_first = order;
            focus = 2;
            hold(false, 300), clear();
            hold(true, 150), hold(false, 100);               /* a quick press: no menu, and the game has its Y */
            assert(!panel_open() && !toggles && game_saw_y);
            hold(true, 480), hold(false, 100);               /* just under the Menu button's time: nothing */
            assert(!panel_open() && !toggles);
            hold(true, 530);                                 /* held that long: it opens, while still held */
            assert(panel_open() && toggles == 1);
            hold(true, 5000), hold(false, 200);              /* held on and let go: the press is neither Back nor a close */
            assert(panel_open() && toggles == 1 && backs == 0 && focus == 2);
            clear();
            hold(true, 100), hold(false, 100);               /* a tap: Back a level, never the game's */
            assert(panel_open() && backs == 1 && focus == 1 && !game_saw_y);
            for (int level = 2; level >= 0; level--) {       /* held in it: it closes from any level, with no Back on the way */
                focus = level, clear();
                hold(true, 530);
                assert(!panel_open() && toggles == 1 && backs == 0 && focus == level);
                hold(true, 5000), hold(false, 200);          /* held on, let go: not opened again, and the game sees no Y */
                assert(!panel_open() && toggles == 1 && backs == 0 && !game_saw_y);
                hold(true, 530), hold(false, 100);           /* the next hold opens it */
                assert(panel_open() && toggles == 2 && backs == 0 && focus == level);
            }
            focus = 0, clear();
            hold(true, 100), hold(false, 600);               /* a tap on the tabs: Back closes it, and it stays closed */
            assert(!panel_open() && backs == 1 && !toggles && !game_saw_y);
            ctl.shared[0].squeeze = 1, ctl.shared[0].buttons = HOOKSHOT_VR_BTN_GRIP;
            hold(false, 100), clear();
            hold(true, 530), hold(false, 100);               /* with a sword in the hand (its grip held): opens, */
            assert(panel_open());
            hold(true, 530), hold(false, 100);               /* closes, */
            assert(!panel_open());
            hold(true, 530), hold(false, 100);               /* and opens again, the grip never let go */
            assert(panel_open() && toggles == 3);
            hold(true, 530), ctl.shared[0] = HandState{}, hold(false, 100);
            assert(!panel_open());
            clear();
            hold(true, 200), busy = true, hold(true, 100), busy = false, hold(true, 1000), hold(false, 100);   /* busy meanwhile */
            assert(!panel_open() && !toggles);
            busy = true, hold(true, 22), busy = false, hold(true, 1000), hold(false, 100);                     /* as it was pressed */
            assert(!panel_open() && !toggles);
            hold(true, 200), panel_set_open(true), hold(true, 1000);   /* (opened by something else during the hold: not closed) */
            assert(panel_open() && !toggles);
            hold(false, 100), hold(true, 200), panel_set_open(false), hold(true, 1000);   /* (closed so: not opened again) */
            assert(!panel_open() && !toggles);
            hold(false, 100);
            opts.vr_menu_button = 1, focus = 2, clear();     /* Both sticks: */
            hold(true, 2000), hold(false, 100);              /* Y's hold is nothing, and a tap still Back in the menu */
            sticks(true, false, 2000), sticks(false, true, 2000), sticks(false, false, 100);   /* one stick alone: nothing */
            assert(!panel_open() && !toggles);
            sticks(true, false, 300), sticks(true, true, 480), sticks(false, false, 100);      /* half a second of both */
            assert(!panel_open() && !toggles);
            sticks(true, false, 300), sticks(true, true, 530);
            assert(panel_open() && toggles == 1);
            sticks(true, true, 3000), sticks(true, false, 300), sticks(false, false, 100);
            assert(panel_open() && toggles == 1 && backs == 0);
            hold(true, 100), hold(false, 100);
            assert(backs == 1 && focus == 1);
            clear(), sticks(true, true, 530);
            assert(!panel_open() && toggles == 1 && backs == 0);
            sticks(true, true, 3000), sticks(false, true, 300), sticks(false, false, 100);     /* (the game never sees those clicks) */
            assert(!panel_open() && toggles == 1 && !game_saw_clicks);
            opts.vr_menu_button = 0;
            hold(false, 100);
        }
        return 0;
    }
    """, cpp=True)
    # (Both sticks, where the clicks are the game's: one alone reaches it, late or as it is let go; both never)
    start = host.index('#define kVrBothClicksNs')
    compile_run(r"""
    #include <stdint.h>
    #include <assert.h>
    """ + host[start:host.index('\n}\n', host.index('static int vr_both_clicks(')) + 3] + r"""
    static VrClick c[2];
    static int64_t t = 1000000000LL;
    static int run(int ls, int rs, int ms) {   /* what the game saw of them */
        int seen = 0;
        for (int at = 0; at < ms; at += 11) seen |= vr_both_clicks(c, ls, rs, t), t += 11000000LL;
        return seen;
    }
    int main(void) {
        assert(run(0, 0, 100) == 0);
        assert(run(1, 0, 60) == 0 && run(0, 0, 60) == 1 && run(0, 0, 200) == 1 && run(0, 0, 200) == 0);   /* a tap: as it is let go */
        assert(run(0, 1, 100) == 0 && run(0, 1, 50) == 2 && run(0, 1, 2000) == 2);                       /* held: late, as long */
        assert(run(0, 0, 11) == 0);
        assert(run(1, 0, 90) == 0 && run(1, 1, 2000) == 0 && run(0, 1, 2000) == 0 && run(0, 0, 500) == 0);   /* both: never */
        assert(run(0, 1, 50) == 0 && run(1, 1, 50) == 0 && run(0, 0, 500) == 0);                         /* a quick both: never */
        assert(run(1, 0, 500) == 1 && run(1, 1, 500) == 0 && run(1, 0, 500) == 0 && run(0, 0, 100) == 0);   /* joined late */
        assert(run(1, 0, 50) == 0 && run(0, 0, 30) == 1);
        return 0;
    }
    """)
    # (the wiring: one threshold for Menu and Y; MENU2 is the game side's own bit, past the menu's mask but not Window's Move
    # mode's; busy while Menu is down with it, Hookshot's menu is up, or either ocarina's notes are played with the menu closed;
    # hands lost end the hold; the game's Y, in third person, left as it was)
    pad = host[host.index('static void vr_pad_read('):]
    assert host.count('#define kVrMenuHoldNs') == 1 and 'kVrMenuYHoldNs' not in host
    busy = ('const int y_busy = menu || hookshot_menu || (!vr_open && (ocarina_playing || hos_oc_wait == 1 || (sticks && '
            'hos_vr_radial_open())));')
    call = 'if (vr_menu_y_step(y, y_busy, vr_open, now)) {'
    assert 'const int y = (lb & VR_BTN_MENU2) && (!sticks || (rb & VR_BTN_MENU2));' in pad
    assert 'const int sticks = rx_option(1034, 0) == 1, both = sticks && ls && rs;' in pad
    assert 'if (id == 1034) return opts.vr_menu_button;' in (REPO / 'gpu/vk/vk_replay.cpp').read_text(encoding='utf-8')
    assert 'const int click = sticks ? vr_both_clicks(clicks, ls, rs, now) : ls | rs << 1;' in pad
    assert '!both && ls && (!sticks || now - ls_at >= kVrBothClicksNs) &&' in pad and '!menu && !both && !ocarina_playing' in pad
    assert '{TAB_STORED, "VR menu button", &opts.vr_menu_button, 0, 1, 1, tx_vr_menu_button,' in (REPO / 'vr/menu/vr_tab_rows.inc').read_text()
    assert 'int vr_menu_button = 0;' in (REPO / 'vr/menu/vr_options.inc').read_text(encoding='utf-8')
    for before in ('const int ocarina_hand = vr_ocarina(', 'const int hookshot_menu = rx_option(1, 0) != 0;',
                   'const int hold = vr_menu_hold_step(&menu_hold, menu, chord, now);', 'const int vr_open = rx_vr_menu(0);', busy):
        assert pad.index(before) < pad.index(call), before
    assert 'vr_y_hold.was = 0;' in pad[:pad.index('const HookshotVrHand *l')]
    assert 'if ((lb & VR_BTN_UPPER) && !stick_jump) *raw |= 1ULL << HOS_PAD_NORTH;   /* Y */' in pad
    assert 'VR_BTN_MENU2 = 64' in host and 'HOOKSHOT_VR_BTN_MENU2 = 64,' in (REPO / 'vr/vk_vr.h').read_text()
    api = (REPO / 'vr/headset/host_api.inc').read_text()
    assert api.index('vr::panel_mask(h, o.trigger,') < api.index('vr::window_mask(h, o.trigger,')
    window = (REPO / 'vr/headset/window_view.inc').read_text(encoding='utf-8')
    assert 'if (buttons) *buttons &= HOOKSHOT_VR_BTN_MENU;' in window[window.index('void window_mask('):]
    # (the menu's: B, the right hand's, is Back on its press; Y as it is let go; opened or closed, a Y still held is no press)
    assert 'const bool back = (hands[1].buttons & HOOKSHOT_VR_BTN_UPPER) != 0;' in menu
    assert 'const bool y_tap = vrm_y_tap((hands[0].buttons & HOOKSHOT_VR_BTN_UPPER) != 0);' in menu
    assert 'if ((back && !vrm.back_was) || y_tap) vrm_back();' in menu
    assert 'vrm.select_was = vrm.back_was = vrm.y_was = true, vrm.y_pressed = false;' in menu
    # (every controller with a left upper button binds it: Touch's y, Index's b; the ones without (Vive wand, WMR, simple)
    # keep the Menu hold alone; Y is never bound to Menu itself, whose tap is the game's Plus)
    make = function_body(read('headset'), 'void make_controls() {')
    tables = dict(re.findall(r'suggest\("(/interaction_profiles/[a-z0-9_/]+)", \w+, \{(.*?)\}\);', make, re.S))
    left_upper = {p: re.findall(r'\{ctl\.upper, "/user/hand/left/input/([a-z0-9_/]+)"\}', b) for p, b in tables.items()}
    assert left_upper['/interaction_profiles/valve/index_controller'] == ['b/click'], left_upper
    assert {p for p, v in left_upper.items() if not v} == {'/interaction_profiles/htc/vive_controller',
        '/interaction_profiles/microsoft/motion_controller', '/interaction_profiles/khr/simple_controller'}, left_upper
    assert all(v in ([], ['y/click'], ['b/click']) for v in left_upper.values()), left_upper
    for p, b in tables.items():
        assert not re.search(r'\{ctl\.menu, "/user/hand/left/input/[yb]/click"\}', b), f'{p}: Y bound to Menu (a quick Y would be Plus)'


def check_menu_f2():
    """F2 on the keyboard opens and closes the VR menu: one press once, only with the window in front, VR running, no control
    waiting for its key, no text field with the keyboard and no game control bound to F2; a press refused is not kept for
    later; no controller is asked for; the window's keys reach it on both systems (the queue F12 is taken from)"""
    menu = read('vr_menu')
    start = menu.index('void vrm_key(bool in_front) {')
    compile_run(r"""
    #include <assert.h>
    #include <stddef.h>
    enum { VK_F2 = 0x71, HOS_KEY_N = 40, HOS_BIND_KEY = 1 };
    struct Host { int (*take_key_press)(int); int (*binding)(int, int, int); };
    static int queued, bound = -1, bound_slot, presses;
    static int take(int key) { assert(key == VK_F2); const int q = queued; queued = 0; return q; }
    static int binding(int what, int b, int slot) { assert(what == HOS_BIND_KEY); return b == bound && slot == bound_slot ? VK_F2 : 0x41; }
    static Host the_host = {take, binding}, *host = &the_host;
    static int listening;
    static int field, *editing;
    static void logf(const char*, ...) {}
    namespace vr {
    static bool on = true, open, hookshot_menu;
    static bool active() { return on; }
    static bool panel_open() { return open; }
    static bool panel_toggle() { if (!open && hookshot_menu) return false; open = !open, presses++; return true; }
    }
    """ + menu[start:menu.index('\n}\n', start) + 3] + r"""
    static void press(bool front = true) { queued = 1, vrm_key(front); }
    int main() {
        vrm_key(true); assert(!vr::open);
        press(); assert(vr::open && presses == 1);
        vrm_key(true), vrm_key(true); assert(vr::open && presses == 1);    /* held: the window queues it once */
        press(); assert(!vr::open && presses == 2);
        press(false); assert(presses == 2); vrm_key(true); assert(presses == 2);   /* another window in front: nothing, nor later */
        listening = 2, press(), listening = 1, press(), listening = 0; assert(presses == 2);
        editing = &field, press(), editing = nullptr; assert(presses == 2);
        vr::on = false, press(), vr::on = true; assert(presses == 2);
        for (bound_slot = 0; bound_slot < 2; bound_slot++)
            for (bound = 0; bound < HOS_KEY_N; bound++) { press(); assert(presses == 2); }
        bound = -1;
        vr::hookshot_menu = true, press(), vr::hookshot_menu = false; assert(presses == 2 && !vr::open);
        press(); assert(vr::open && presses == 3);
        return 0;
    }
    """, cpp=True)
    body = menu[start:menu.index('\n}\n', start)]
    assert 'hands' not in body and 'ctl' not in body
    tick = (REPO / 'gpu/vk/vk_menu.cpp').read_text(encoding='utf-8')
    tick = tick[tick.index('void menu_tick() {'):]
    assert tick.index('const bool mine = pid == GetCurrentProcessId();') < tick.index('    vrm_key(mine);') < tick.index('auto pad = [&]')
    keys = (REPO / 'host/posix/display_sdl.c').read_text(encoding='utf-8')
    assert 'if (v[i] && !was[i]) hos_vk_note_key_press((unsigned)i);' in keys
    assert 'if (!(lp & (1 << 30))) hos_vk_note_key_press((unsigned)wp);' in (REPO / 'host/display.c').read_text(encoding='utf-8')


def check_menu_hint():
    """the VR menu's hint names the buttons as the runtime does for what is bound (its localized names, shortened; a name the
    menu cannot show, ours), else ours for the profile: no Y where the controller has none, B on an Index; with no controller
    the last names stay; asked again whenever the profile is (note_profiles). A runtime without the calls still starts"""
    source, layer, menu = read('headset'), read('vr_menu_api'), read('vr_menu')
    def function(text, head):
        at = text.index(head)
        return text[at:text.index('\n}\n', at) + 3]
    code = ''.join(function(source, h) for h in ('std::string short_button_name(std::string n) {', 'void own_button_names(',
                                                 'int bound_button(XrAction a, std::string& name) {', 'void name_menu_buttons() {'))
    compile_run(r"""
    #include <assert.h>
    #include <algorithm>
    #include <cctype>
    #include <cstdint>
    #include <cstring>
    #include <map>
    #include <mutex>
    #include <string>
    #include <vector>
    typedef uint64_t XrPath, XrAction, XrSession, XrInstance; typedef int XrResult;
    #define XR_SUCCEEDED(r) ((r) >= 0)
    #define XR_FAILED(r) ((r) < 0)
    enum { XR_MAX_PATH_LENGTH = 256, XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO, XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO,
           XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT = 4 };
    struct XrBoundSourcesForActionEnumerateInfo { int type; const void* next; XrAction action; };
    struct XrInputSourceLocalizedNameGetInfo { int type; const void* next; XrPath sourcePath; uint64_t whichComponents; };
    static struct { XrSession session = 1; XrInstance inst = 1; std::mutex pose_mu; } s;
    static struct { std::string profile[2]; std::string menu_name = "menu", upper_name = "Y"; bool pads = false; XrAction menu = 8, upper = 2; } ctl;
    static void logf(const char*, ...) {}
    struct Source { std::string path, name; };
    static std::map<XrAction, std::vector<Source>> sources;   // (path ids: action * 10 + index)
    static bool works = true;
    static XrResult bound(XrSession, const XrBoundSourcesForActionEnumerateInfo* i, uint32_t cap, uint32_t* n, XrPath* out) {
        if (!works) return -1;
        auto& v = sources[i->action];
        *n = (uint32_t)v.size();
        if (cap && cap < v.size()) return -1;
        for (uint32_t k = 0; k < cap; k++) out[k] = i->action * 10 + k;
        return 0;
    }
    static XrResult copy(const std::string& t, uint32_t cap, uint32_t* n, char* out) {
        *n = (uint32_t)t.size() + 1;
        if (!cap) return 0;
        if (cap < *n) return -1;
        memcpy(out, t.c_str(), *n);
        return 0;
    }
    static XrResult source_name(XrSession, const XrInputSourceLocalizedNameGetInfo* i, uint32_t cap, uint32_t* n, char* out) {
        assert(i->whichComponents == XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT);
        return copy(sources[i->sourcePath / 10][i->sourcePath % 10].name, cap, n, out);
    }
    static XrResult xrPathToString(XrInstance, XrPath p, uint32_t cap, uint32_t* n, char* out) { return copy(sources[p / 10][p % 10].path, cap, n, out); }
    static XrResult (*xr_bound_sources)(XrSession, const XrBoundSourcesForActionEnumerateInfo*, uint32_t, uint32_t*, XrPath*) = bound;
    static XrResult (*xr_source_name)(XrSession, const XrInputSourceLocalizedNameGetInfo*, uint32_t, uint32_t*, char*) = source_name;
    """ + code + function(layer, 'std::string menu_hint_words(') + r"""
    static std::string hint(int sticks = 0) { return menu_hint_words(ctl.menu_name, ctl.upper_name, sticks, ctl.pads, false); }
    static std::string value(int sticks = 0) { return menu_hint_words(ctl.menu_name, ctl.upper_name, sticks, ctl.pads, true); }
    static void use(const char* profile) { ctl.profile[0] = profile, name_menu_buttons(); }
    #define P "/interaction_profiles/"
    #define L "/user/hand/left/input/"
    #define R "/user/hand/right/input/"
    int main() {
        assert(short_button_name("Y Button") == "Y" && short_button_name("Menu Button") == "menu" && short_button_name("Left Menu") == "menu");
        assert(short_button_name("B") == "B" && short_button_name("Trackpad") == "trackpad" && short_button_name("") == "");
        assert(short_button_name("Bouton Y\xC3\xA9") == "" && short_button_name("A very long button name") == "");
        assert(short_button_name("Button") == "button" && short_button_name("left") == "left");
        assert(hint() == "Hold left menu or Y" && value() == "Menu or Y");                 /* no controller seen yet */
        sources[8] = {{L "menu/click", "Menu Button"}}, sources[2] = {{R "b/click", "B Button"}, {L "y/click", "Y Button"}};
        use(P "oculus/touch_controller"); assert(hint() == "Hold left menu or Y");         /* (the left one of the two) */
        assert(hint(1) == "Hold left menu or both sticks" && value(1) == "Both sticks");
        sources[8] = {{L "trackpad/force", "Trackpad"}}, sources[2] = {{L "b/click", "B"}, {R "b/click", "B"}};
        use(P "valve/index_controller"); assert(hint() == "Hold left trackpad or left B" && value() == "Menu or B");
        use("(none)"); assert(hint() == "Hold left trackpad or left B");                   /* asleep: the last ones stay */
        sources[8] = {{L "menu/click", "Menu"}}, sources[2] = {};
        use(P "htc/vive_controller"); assert(hint() == "Hold left menu" && value() == "Menu" && hint(1) == "Hold left menu or both pads");
        sources[2] = {{R "menu/click", "Menu"}};                                           /* (WMR's right Menu is B) */
        use(P "microsoft/motion_controller"); assert(hint() == "Hold left menu" && hint(1) == "Hold left menu or both sticks");
        sources[2] = {{L "x/click", "X Button"}};                                          /* rebound by the player */
        use(P "oculus/touch_controller"); assert(hint() == "Hold left menu or left X");
        sources[2] = {{L "y/click", "\xE3\x83\xA1"}};                                      /* a name the menu's font lacks: ours */
        use(P "oculus/touch_controller"); assert(hint() == "Hold left menu or Y");
        sources[8] = {}, sources[2] = {{L "y/click", "Y"}};                                /* Menu unbound (SteamVR's) */
        use(P "oculus/touch_controller"); assert(hint() == "Hold Y");
        for (int way = 0; way < 3; way++) {                                                /* ours: lists nothing, fails, has no calls */
            sources.clear(), works = way != 1;
            if (way == 2) xr_bound_sources = nullptr, xr_source_name = nullptr;
            use(P "valve/index_controller"); assert(hint() == "Hold left trackpad or left B");
            use(P "htc/vive_controller"); assert(hint() == "Hold left menu");
            use(P "khr/simple_controller"); assert(hint() == "Hold left menu" && !ctl.pads);
            use(P "microsoft/motion_controller"); assert(hint() == "Hold left menu");
            use(P "hp/mixed_reality_controller"); assert(hint() == "Hold left menu or Y");
        }
        assert(menu_hint_words("", "", false, false, false) == "F2");
        return 0;
    }
    """, cpp=True)
    assert function(source, 'void note_profiles() {').rstrip().endswith('name_menu_buttons();\n}')
    loader = (REPO / 'vr/headset/runtime_loader.inc').read_text(encoding='utf-8')
    wanted = loader[loader.index('#define XRX(X)'):loader.index('#define XRX_PTR')]
    assert 'xrEnumerateBoundSourcesForAction' not in wanted and 'xrGetInputSourceLocalizedName' not in wanted, 'optional calls'
    assert 'xr_bound_sources = nullptr, xr_source_name = nullptr;' in loader
    # (our own names, and the tables they are of: a profile has a left upper name only when its table binds one)
    make = function_body(source, 'void make_controls() {')
    none = {p for p, b in re.findall(r'suggest\("(/interaction_profiles/[a-z0-9_/]+)", \w+, \{(.*?)\}\);', make, re.S)
            if '{ctl.upper, "/user/hand/left/' not in b}
    own = function(source, 'void own_button_names(')
    assert none == set(re.findall(r'"(/interaction_profiles/[a-z0-9_/]+)"', own)) - {'/interaction_profiles/valve/index_controller'}, none
    # (shown: the Menu row's words and the Menu button row's value, drawn again when they change)
    assert 'const std::string words = vrm_info_words(r);' in menu and 'for (const char c : vr::menu_hint(false)) mix(c);' in menu
    assert 'if (r.value == &opts.vr_menu_button) return vr::menu_hint(true);' in menu


def check_menu_rows():
    """the VR menu's rows: every VR setting is in exactly one place, the VR menu (Hookshot's own menu has no VR tab and none of
    the mod's rows; VR on or off is decided at the start, Replay threads is upstream's switch again), saved under its old key; nothing Planned; the built-in switches are
    constants with no row; a sub-row is hidden by its switch in the same section, a first-person row greyed in third person"""
    menu = read('vr_menu')
    tab_rows = (REPO / 'vr/menu/vr_tab_rows.inc').read_text(encoding='utf-8')
    defaults = (REPO / 'vr/menu/vr_options.inc').read_text(encoding='utf-8')
    rows = re.findall(r'\{(TAB_\w+), "([^"]+)", (?:&opts\.(\w+)|nullptr)', tab_rows)
    by_tab = {}
    for tab, name, field in rows:
        by_tab.setdefault(tab, []).append(name)
    assert set(by_tab) == {'TAB_STORED'}, f'a VR row on a tab of Hookshot\'s own menu: {set(by_tab) - {"TAB_STORED"}}'
    assert not (REPO / 'vr/menu/vr_debug_rows.inc').exists()
    vk_menu_text = (REPO / 'gpu/vk/vk_menu.cpp').read_text(encoding='utf-8')
    assert 'TAB_VR' not in vk_menu_text and '"VR"}' not in vk_menu_text and '"debug", "vr"' not in vk_menu_text, 'a VR tab'
    # (VR on or off: no row; decided at the start, HOOKSHOT_VR or vr= in the ini, and opts.vr follows it)
    session = (REPO / 'vr/headset/runtime_session.inc').read_text(encoding='utf-8')
    assert 'opts.vr = asked() ? 1 : 0;' in session and '&opts.vr,' not in tab_rows
    assert 'if (const char* e = getenv("HOOKSHOT_VR")) return atoi(e) != 0;' in (REPO / 'vr/headset/runtime_loader.inc').read_text()
    # (Replay threads: upstream's own again, RX_VK_PARALLEL alone; no ini row that turns the late pages' fix off)
    assert 'replay_threads' not in (REPO / 'gpu/vk/vk_replay.cpp').read_text() + tab_rows
    layout = menu[menu.index('const VrmRow vrm_play[] = {'):menu.index('const VrmSection vrm_general[]')]
    entries = re.findall(r'VRM_(?!PAGE)\w+\("([^"]+)", (\w+),|\{"([^"]+)", &opts\.(\w+), VrmKind::cards|VRM_PAGE\("([^"]+)"', layout)
    pages = [e[4] for e in entries if e[4]]
    assert pages == ['Controller mapping'], pages
    # (Body > Controls: Hookshot VR's own controls, a label and its words each, the user's list of 23 Nov; then Controller mapping)
    controls = menu[menu.index('const VrmRow vrm_controls[] = {'):]
    controls = controls[:controls.index('};')]
    info = re.findall(r'VRM_INFO\("([^"]+)", "([^"]+)"\)', controls)
    assert info == [('Menu', 'Hold left menu or Y'), ('Items wheel', 'Left stick click'), ('Gear wheel', 'Right stick click'),
                    ('Walk', 'Left stick'), ('Turn', 'Right stick'), ('Jump', 'Right stick up'), ('Wrist HUD', 'Wrist up'),
                    ('Draw sword', 'Grip over shoulder'), ('Slash', 'Swing'), ('Spin', 'Trigger'), ('Lock on', 'Shield trigger'),
                    ('Hold item', 'Grip'), ('Use item', 'Trigger'), ('Throw', 'Release while moving'), ('Bow', 'Pull string')], info
    assert controls.index('VRM_INFO("Bow"') < controls.index('VRM_PAGE("Controller mapping"'), 'Controller mapping after the VR controls'
    assert '#define VRM_INFO(name, words) {name, nullptr, VrmKind::info, 0, nullptr, words, {}}' in menu
    assert 'selected->kind == VrmKind::info ? kVrmVrControlsHelp' in menu and 'if (r.kind == VrmKind::info) {' in menu
    assert 'if (strcmp(r.name, "Controller mapping") || !can_bind()) return;' in menu, 'an info row opens nothing'
    assert 'if (!r.value) return vrm_greyed(r) ? void() : vrm_open(r);' in menu, 'a greyed page row opens nothing'
    entries = [e[:4] for e in entries if not e[4]]
    entries = [(a or c, b or d) for a, b, c, d in entries]
    fields = [f for _, f in entries]
    assert len(fields) == len(set(fields)), 'a value in two places of the VR menu'
    stored = {f for tab, _, f in rows if tab == 'TAB_STORED'}
    wrists = {'vr_wrist_l_pitch', 'vr_wrist_l_yaw', 'vr_wrist_l_roll', 'vr_wrist_r_pitch', 'vr_wrist_r_yaw', 'vr_wrist_r_roll'}
    window = {'vr_window'}   # (set by View's third card, Window: vrm_set)
    assert set(fields) == stored - wrists - window, f'only in the menu: {set(fields) - stored}; rows not in it: {stored - wrists - window - set(fields)}'
    # (nothing Planned on the canvas, 19 Nov, is in it until it is built)
    planned = ['Play position', 'HUD distance', 'Crawl spaces',
               'Dominant hand', 'Haptics', 'Draw sword', 'Sword hand', 'Take shield', 'Guard with', 'Mirror Shield aim', 'Draw with',
               'Take a bomb', 'Bomb bag at', 'Fire on', 'Catch with', 'Worn at', 'Dialogue view', 'hitbox', 'Bomb Flower size']
    names = [n for n, _ in entries]
    for p in planned:
        assert not any(p.lower() in n.lower() for n in names), f'planned: {p}'
    want = ['Recenter view', 'View', '3D', 'Background', 'Move', 'Place window', 'Size', 'Keep 16:9', 'Height', 'Roundness',
            'Feather', 'Tilt', 'Distance', 'Eye camera', 'Back panel', 'Eye height', 'HUD size', 'Wrist HUD', 'Wrist size',
            'Menu position', 'Turn style', 'Snap angle',
            'Turn speed', 'Move direction', 'Move vignette',
            'Turn vignette', 'Air vignette', 'Carry vignette', 'Arm length', 'Body alignment', 'Crawl eye height', 'Calibrate wrists', 'Item wheel opens',
            'Menu button', 'Sword angle', 'Swing arc', 'Swing speed', 'Hit buzz', 'Shield when guarding', 'Bow size', 'String grab with',
            'Aim reticle', 'Aim reticle size', 'Elemental arrow effects', 'Grip', 'Throw strength', 'Bomb size', 'Distance grab',
            'Distance grab reach', 'Distance grab aim', 'Aim with', 'Song effects', 'Cutscene view', '3D cutscenes',
            'Cutscene light', 'Screen size', 'Re-align after events', 'Event culling',
            'Item Get view', 'Item shown', 'Eye resolution', 'Depth quality', '3D strength', 'Near depth', 'Detail distance',
            'Foliage distance', 'GPU profile', 'Show hands']
    assert names == want, f'the rows differ from the spec: {names}'
    assert 'VRM_SECTION("Body & Hands", vrm_body, "@hand"), VRM_SECTION("Controls", vrm_controls, "@controller")' in menu   # (Controller mapping's place)
    # (no empty squares: every section has an icon, a drawn one vrm_icon_shapes draws or a game icon the build makes; the 3D cards too)
    icons_py = (REPO / 'vr/tools/generate/icons.py').read_text(encoding='utf-8')
    section_icons = re.findall(r'VRM_SECTION\("[^"]+", \w+, "([^"]+)"\)', menu)
    assert len(section_icons) == len(re.findall(r'VRM_SECTION\(', menu)) - 1, 'a section without an icon'
    drawn = set(re.findall(r'k == "(\w+)"', menu[menu.index('bool vrm_icon_shapes('):menu.index('bool vrm_has_icon(')]))
    for icon in section_icons + re.findall(r'\{"[^"]+", "[^"]+", \w+, "(@\w+)"\}', menu):
        assert (icon[1:] in drawn) if icon.startswith('@') else f'"{icon}"' in icons_py, f'icon {icon} is neither drawn nor generated'
    assert 'int vr_menu_position = 0;' in defaults and '"VR menu position", &opts.vr_menu_position, 0, 1, 1,' in tab_rows
    # (View: first person by default since v1.0.2; still saved as view= (the row's name, no key of its own in ini_key), 0 third
    # person and 1 first, so a saved View is kept)
    assert 'int vr_mode = 1;' in defaults and '{TAB_STORED, "View", &opts.vr_mode, 0, 1, 1, tx_vr_mode,' in tab_rows
    assert '&opts.vr_mode' not in vk_menu_text[vk_menu_text.index('const char* ini_key('):vk_menu_text.index('static char k[48];')]
    assert 'const char* tx_vr_mode(int v) { return v ? "First person" : "Third person"; }' in (REPO / 'vr/menu/vr_values_menu.inc').read_text(encoding='utf-8')
    # (the release: Show hands off, and the Debug tab, the last, shown only with HOOKSHOT_VR_DEBUG=1)
    assert 'int vr_show_hands = 0;' in defaults, 'Show hands draws squares at the hands for every player: off by default'
    assert '{"Debug", vrm_debug, VRM_N(vrm_debug)}};' in menu and 'env_on("HOOKSHOT_VR_DEBUG") ? kVrmTabs : kVrmTabs - 1' in menu
    assert menu.count('kVrmTabs') == 4, 'the tabs are walked or drawn by kVrmTabs, not vrm_tab_count(): the Debug tab shows'
    # (built in: a constant, no row)
    built_in = {'vr_arms': 1, 'vr_held_grip': 0, 'vr_level': 1, 'vr_own_shadow': 0, 'vr_link_fade': 0, 'vr_hook_hands': 1,
                'vr_stick_hands': 1, 'vr_ocarina': 1, 'vr_boomerang_hands': 1, 'vr_bottle_hands': 1, 'vr_bomb_hands': 1,
                'vr_bomb_flowers': 1, 'vr_ocarina_menu': 1, 'vr_bomb_quiet': 1, 'vr_swing': 1, 'vr_swing_detect': 1,
                'vr_held_near': 1, 'vr_sword_guard': 1, 'vr_put_away': 0, 'vr_crawl': 1, 'vr_bow_grip_hold': 1,
                'vr_bow_string_fires': 1, 'vr_quick_select': 1,
                # (22 Nov, the user's list after the release audit)
                'vr_stick_jump': 1, 'vr_gait': 1, 'vr_show_stowed': 0, 'vr_sword_hits': 1, 'vr_hit_test': 0, 'vr_draw_test': 0,
                'vr_sword_attack': 0, 'vr_swing_sensitivity': 1, 'vr_bomb_unlit_frame': 25, 'vr_body_latest': 0,
                'vr_eyes_lights': 1, 'vr_eyes_particles': 1, 'vr_eyes_camera': 1, 'vr_eyes_fog': 1, 'vr_window_test': 0,
                # (the release: the Debug tab's tests)
                'vr_world_cull': 0, 'vr_frustum_cull': 0, 'vr_bomb_gravity': 100}
    for f, v in built_in.items():
        assert f'static constexpr int {f} = {v};' in defaults, f'{f} is not built in as {v}'
        assert f'&opts.{f},' not in tab_rows, f'{f} still has a row'
    # (the row table compiled with stand-ins: hidden and greyed rows, this menu's own words for values)
    start, end = menu.index('enum class VrmKind'), menu.index('constexpr const char* kVrmControlsHelp')
    used = sorted(set(re.findall(r'opts\.(\w+)', menu[start:end])) | set(fields))
    compile_run(r"""
    #include <algorithm>
    #include <cassert>
    #include <cstring>
    #include <string>
    struct Options { int """ + ', '.join(f + ' = 0' for f in used) + r"""; } opts;
    const char* tx(int) { return "row text"; }
    constexpr int kVr3dMono = 0, kVr3dFromDepth = 1, kVr3dStereo = 4;   // (vr_values_menu.inc's)
    struct Row { int* value; int lo, hi, step; const char* (*text)(int); const char* help; };
    namespace vr { static std::string menu_hint(bool value) { return value ? "Menu or Y" : "Hold left menu or Y"; } }
    Row rows[] = {""" + ', '.join('{&opts.%s, 0, 3, 1, tx, ""}' % f for f in used) + r"""};
    """ + menu[start:end] + menu[menu.index('constexpr float kVrmW'):menu.index('enum VrmHitKind')] + r"""
    const VrmRow& find(const char* name) {
        for (const VrmTab& t : vrm_tabs)
            for (int s = 0; s < t.n; s++)
                for (int r = 0; r < t.sections[s].n; r++)
                    if (!strcmp(t.sections[s].rows[r].name, name)) return t.sections[s].rows[r];
        assert(!"no such row");
        return vrm_tabs[0].sections[0].rows[0];
    }
    int main() {
        opts.vr_mode = 1, opts.vr_stereo = 1, opts.vr_bow_preview = 1, opts.vr_bomb_pull = 1;
        int shown[16];
        assert(vrm_shown(vrm_graphics[0], shown) == 4);           // (From depth, kept for later: Depth quality and Near depth too)
        opts.vr_stereo = 0;
        assert(vrm_shown(vrm_graphics[0], shown) == 1 && vrm_hidden(find("3D strength")));   // (Mono: Eye resolution only)
        opts.vr_stereo = 4;
        assert(!vrm_hidden(find("3D strength")));
        assert(vrm_shown(vrm_graphics[0], shown) == 2 && vrm_hidden(find("Depth quality")) && vrm_hidden(find("Near depth")));   // (Stereo: hidden)
        assert(vrm_shown(vrm_items[2], shown) == 5);              // (the Bow, its reticle on: all five)
        opts.vr_bow_preview = 0;
        assert(vrm_hidden(find("Aim reticle size")) && !vrm_hidden(find("Aim reticle")));
        opts.vr_bomb_pull = 0;
        // (Comfort -> Turning: Snap angle only with Snap, Smooth turn speed only with Smooth, the default)
        assert(opts.vr_turn_style == 0 && vrm_hidden(find("Snap angle")) && !vrm_hidden(find("Turn speed")));
        opts.vr_turn_style = 1;
        assert(!vrm_hidden(find("Snap angle")) && vrm_hidden(find("Turn speed")) && vrm_shown(vrm_comfort[0], shown) == 2);
        opts.vr_turn_style = 0;
        assert(vrm_shown(vrm_items[3], shown) == 4 && vrm_hidden(find("Distance grab reach")) && vrm_hidden(find("Distance grab aim")));
        assert(!vrm_greyed(find("Sword angle")) && !vrm_greyed(find("HUD size")));
        opts.vr_mode = 0;   // (third person: first-person rows greyed, with the reason; the rest not)
        assert(!strcmp(vrm_greyed(find("Sword angle")), "First person only") && vrm_greyed(find("Move direction")));
        assert(!vrm_greyed(find("HUD size")) && !vrm_greyed(find("Eye resolution")) && !vrm_greyed(find("View")));
        opts.vr_bomb_throw_mode = 1, opts.vr_boomerang_throw = 2, opts.vr_realign = 1;
        assert(vrm_value_text(find("Menu button")) == "Menu or Y" && vrm_info_words(find("Menu")) == "Hold left menu or Y" &&
               vrm_info_words(find("Jump")) == "Right stick up");   // (the first by the controller's own names)
        assert(vrm_value_text(find("Grip")) == "Press to toggle" && vrm_value_text(find("Aim with")) == "Headset");
        assert(vrm_value_text(find("Re-align after events")) == "Quick turn" && vrm_value_text(find("Snap angle")) == "row text");
        assert(find("View").cards[0].value == 1 && find("View").cards[1].value == 0);              // (First person, Third person)
        assert(vrm_cards(find("View")) == 3 && find("View").cards[2].value == 2 && vrm_cards(find("3D")) == 2);   // (and Window)
        opts.vr_mode = 0, opts.vr_window = 1;   // (Window: third person with the window on; its rows shown)
        assert(vrm_card_now(find("View")) == 2 && vrm_card_of(find("View")) == 2 && !vrm_greyed(find("Background")));
        assert(vrm_shown(vrm_general[0], shown) == 3 && vrm_value_text(find("Background")) == "Dark");
        assert(vrm_shown(vrm_general[1], shown) == 12 && !vrm_hidden(find("Height")));   // (the Window section, Keep 16:9 off)
        opts.vr_window_169 = 1;
        assert(vrm_shown(vrm_general[1], shown) == 11 && vrm_hidden(find("Height")) && find("Size").kind == VrmKind::slider);
        opts.vr_mode = 1;   // (first person: not Window, whatever the flag says; its rows greyed)
        assert(vrm_card_now(find("View")) == 1 && vrm_card_of(find("View")) == 0);
        assert(!strcmp(vrm_greyed(find("Place window")), "Window view only") && vrm_greyed(find("Size")));
        opts.vr_mode = 0, opts.vr_window = 0;
        assert(vrm_card_of(find("View")) == 1 && vrm_greyed(find("Move")));
        assert(find("3D").cards[0].value == 0 && find("3D").cards[1].value == 4);   // (Mono, Stereo)
        assert(!strcmp(find("3D").cards[0].label, "Mono") && !strcmp(find("3D").cards[1].label, "Stereo"));
        // (a long section scrolls, headset 20 Nov: the Debug tab's eleven rows ran off the panel)
        const float room = kVrmRowsBottom - kVrmRowsY;
        float rows11[11];
        for (float& h : rows11) h = kVrmRowH + 1;
        int first = 0, visible = 0;
        vrm_scroll(rows11, 11, room, 0, true, &first, &visible);
        assert(first == 0 && visible == 7);                        // (seven rows fit)
        vrm_scroll(rows11, 11, room, 8, true, &first, &visible);
        assert(first == 2 && visible == 7);                        // (down to the ninth: just enough scrolled to show it)
        vrm_scroll(rows11, 11, room, 10, true, &first, &visible);
        assert(first == 4 && visible == 7);                        // (the last: the last seven)
        vrm_scroll(rows11, 11, room, 3, true, &first, &visible);
        assert(first == 3);                                        // (back up: the selection at the top)
        first = 9, vrm_scroll(rows11, 11, room, 0, false, &first, &visible);
        assert(first == 4 && visible == 7);                        // (never past where the last rows fill the room)
        const float play[3] = {kVrmRowH + 1, kVrmCardsH + 1, kVrmCardsH + 1};   // (Play Mode: Recenter and the two cards rows)
        first = 0, vrm_scroll(play, 3, room, 2, true, &first, &visible);
        assert(first == 0 && visible == 3);                        // (fits whole: no scroll)
        float seven[7];
        for (float& h : seven) h = kVrmRowH + 1;
        vrm_scroll(seven, 7, room, 6, true, &first, &visible);
        assert(first == 0 && visible == 7);
        // (Controller mapping: third person and Window only; greyed in first person, and its page not opened then)
        opts.vr_mode = 1;
        assert(!strcmp(vrm_greyed(find("Controller mapping")), "Third person only"));
        opts.vr_mode = 0;
        assert(!vrm_greyed(find("Controller mapping")));
        for (const char* onoff : {"Hit buzz", "Aim reticle", "Distance grab", "GPU profile", "Show hands"})
            assert(find(onoff).kind == VrmKind::step);   // (On/Off rows: arrows either side, as the other rows: headset 19 Nov)
        return 0;
    }
    """, cpp=True)
    # (the pointers' dots are layers of their own: a pointer's move redraws nothing, headset 20 Nov: parts of the panel flashed
    # while it was drawn and copied every frame)
    signature = function_body(menu, 'uint64_t vrm_signature() {')
    assert 'p.u' not in signature and 'p.v' not in signature and 'AddCircleFilled(at' not in menu
    layer = read('vr_menu_layer')
    assert 'if (panel_dot(hands[h], rays[*n_rays])) ++*n_rays' in layer and 'panel_rays[4]' in read('headset')
    assert 'kBombMarkLayersMax - 9 - (int)n_layers' in read('headset')   # (panel, two rays, two dots, the radial, the menu, the comfort vignette and the wrist HUD)
    # (a cards row: only the focused card is highlighted, not the whole row: headset 19 Nov)
    assert 'if (sel && r.kind != VrmKind::cards)' in menu and 'VrmKind::toggle' not in menu
    # (the hook points stay small: one include in vk_menu.cpp, one call in the present and one in the headless shots)
    vk_menu = (REPO / 'gpu/vk/vk_menu.cpp').read_text()
    assert vk_menu.rstrip().endswith('#include "../../vr/menu/vr_menu.inc"   // VR HOOK: the VR menu, the mod\'s own settings in the headset (it uses the rows above)')
    replay = read('vk_replay')
    assert replay.count('vr::panel_record(f.cmd);') == 1 and replay.count('vr::panel_record_image(r.cmd, shot_image, shot_view, kShotW, kShotH);') == 1


# ======================================================================================================================
# The camera: culling around the eye
# ======================================================================================================================

def check_window_frustum():
    """the Window view: each eye's off-axis view goes through the frame's four corners from where the eye is, in the frame's
    own axes (a head roll changes nothing), and an eye at or behind the frame's plane sees no window"""
    source = read('headset')
    window = (REPO / 'vr/headset/window_view.inc').read_text(encoding='utf-8')
    poses = source[source.index('XrQuaternionf qmul('):source.index('void publish_pose(')]
    start = window.index('constexpr float kWindowWidth')
    math = window[start:window.index('// ---- the runtime\'s offer')]
    compile_run(r'''
    #include <algorithm>
    #include <chrono>
    #include <cassert>
    #include <cmath>
    #include <cstdint>
    struct XrQuaternionf { float x, y, z, w; };
    struct XrVector3f { float x, y, z; };
    struct XrPosef { XrQuaternionf orientation; XrVector3f position; };
    ''' + poses + math + r'''
    static void near(float a, float b) { assert(std::fabs(a - b) < 1e-4f); }
    int main() {
        for (float yaw : {0.f, .9f, -2.4f}) {   // (placed level ahead of the head, whatever its heading and tilt)
            const float pitch = .3f, roll = -.2f;
            XrQuaternionf q = qmul(XrQuaternionf{0, std::sin(yaw / 2), 0, std::cos(yaw / 2)},
                                   qmul(XrQuaternionf{std::sin(pitch / 2), 0, 0, std::cos(pitch / 2)},
                                        XrQuaternionf{0, 0, std::sin(roll / 2), std::cos(roll / 2)}));
            const XrPosef head{q, {.3f, 1.6f, -.2f}};
            const WindowFrame f = window_place(head);
            near(f.pose.orientation.x, 0), near(f.pose.orientation.z, 0);
            near(f.pose.position.y, 1.6f - kWindowDrop);
            near(std::hypot(f.pose.position.x - .3f, f.pose.position.z + .2f), kWindowAhead);
            WindowFrustum fr;
            assert(window_frustum(f, head.position, &fr));   // (the head is straight in front of the middle, kWindowAhead off)
            near(fr.e[0], 0), near(fr.e[1], kWindowDrop), near(fr.e[2], kWindowAhead);
            // the edges' tangents reach the corners from the eye: e + tangent * distance is each edge
            near(fr.e[0] + fr.l * fr.e[2], -.5f * f.w), near(fr.e[0] + fr.r * fr.e[2], .5f * f.w);
            near(fr.e[1] + fr.b * fr.e[2], -.5f * f.h), near(fr.e[1] + fr.t * fr.e[2], .5f * f.h);
            assert(-fr.b > fr.t && fr.t > 0 && std::fabs(fr.l + fr.r) < 1e-4f);   // (its middle under the eye: off-axis)
        }
        WindowFrame f;   // (at the room's origin, facing +z)
        WindowFrustum a, b;
        window_frustum(f, XrVector3f{0, 0, .5f}, &a);
        window_frustum(f, XrVector3f{.2f, 0, .5f}, &b);   // (a lean right: the frame's right edge closer in angle)
        assert(b.r < a.r && b.l < a.l && b.e[0] > a.e[0]);
        window_frustum(f, XrVector3f{0, 0, .25f}, &b);    // (closer: the frame wider in the view)
        assert(b.r - b.l > a.r - a.l);
        near((b.r - b.l) * .25f, f.w);
        assert(!window_frustum(f, XrVector3f{0, 0, .01f}, &b) && !b.front);   // (at the plane)
        assert(!window_frustum(f, XrVector3f{0, 0, -.3f}, &b) && std::isfinite(b.l));   // (behind it: not shown, still finite)
        // the game picture's crop: the frame's corners in a picture drawn from e with the half-FOV tangents tx, ty
        const float hw = .5f * f.w, hh = .5f * f.h;
        float e[3] = {0, 0, .9f};
        WindowCrop c = window_crop(f, e, hw / .9f, hh / .9f);   // (a picture exactly the frame: the whole of it)
        near(c.u0, 0), near(c.u1, 1), near(c.v0, 0), near(c.v1, 1);
        assert(!c.miss);
        c = window_crop(f, e, 2 * hw / .9f, 2 * hh / .9f);      // (twice as wide: its middle half)
        near(c.u0, .25f), near(c.u1, .75f), near(c.v0, .25f), near(c.v1, .75f);
        e[0] = .1f;                                             // (drawn from further right: the frame further left in it)
        WindowCrop r = window_crop(f, e, 2 * hw / .9f, 2 * hh / .9f);
        assert(r.u0 < c.u0 && r.u1 < c.u1 && !r.miss);
        near(r.u1 - r.u0, c.u1 - c.u0);
        e[0] = 0, e[1] = kWindowDrop;                           // (drawn from above the frame's middle: the frame lower in it)
        r = window_crop(f, e, 2 * hw / .9f, 2 * hh / .9f);
        assert(r.v0 > c.v0 && r.v1 > c.v1);
        e[1] = 0, e[2] = .2f;                                   // (very near the frame: its corners past the picture's edge)
        assert(window_crop(f, e, 1.4f, 1.4f).miss);
        // a flat frame as large as fits: a 16:9 menu fills the 16:9 frame, a 5:4 title has bars at the sides
        c = window_fit(f, 1920, 1080);
        near(c.u0, 0), near(c.u1, 1), near(c.v0, 0), near(c.v1, 1);
        c = window_fit(f, 2880, 2304);
        near(c.v0, 0), near(c.v1, 1);
        assert(c.u0 < 0 && c.u1 > 1);
        near((c.u1 - c.u0) * 1.25f, f.w / f.h);   // (the frame's width in the picture's widths: its shape kept)
        // the shape: a rectangle, rounder corners, an ellipse; its handles; the roundness a rounding dot's place gives
        WindowFrame g;
        assert(window_inside(g, 0, 0) > 0 && window_inside(g, .49f * g.w, .49f * g.h) > 0 && window_inside(g, .51f * g.w, 0) < 0);
        g.round = 1;   // (an ellipse: the corner is outside, the middle of each edge on it)
        assert(window_inside(g, .45f * g.w, .45f * g.h) < 0 && std::fabs(window_inside(g, .5f * g.w, 0)) < 1e-4f);
        float hd[2];
        window_handle(g, 1, hd);
        near(hd[0], .5f * g.w), near(hd[1], .5f * g.h);   // (a corner dot: the corner, whatever the roundness)
        float last = 2;
        for (float r : {0.f, .3f, .6f, 1.f}) {
            g.round = r;
            window_handle(g, 5, hd);   // (its rounding dot: inside the corner, further in as it rounds; its place gives r back)
            assert(hd[0] > 0 && hd[1] > 0 && hd[0] / (.5f * g.w) < last && hd[0] / (.5f * g.w) <= .99f);
            last = hd[0] / (.5f * g.w);
            assert(std::fabs(window_round_at(g, hd[0] / (.5f * g.w)) - r) < 1e-3f);
        }
        g.w = g.h = 2;   // (a large frame's ellipse: its dot on the ellipse)
        window_handle(g, 5, hd);
        assert(std::fabs(window_inside(g, hd[0], hd[1])) < 2e-3f);
        assert(window_round_at(g, 1) == 0);
        const WindowFrame tilted = window_frame_at(XrVector3f{0, 1.6f, 0}, 0, .9f, kWindowDrop, .3f, 1, .5f);
        float top[3];
        const float up[3] = {0, 1, 0};
        qrot(tilted.pose.orientation, up, top);
        assert(top[2] < 0 && top[1] > .9f);   // (tilted: its top leans away from the viewpoint, which is at +z)
        near(tilted.pose.position.z, -.9f);
        // the frame-to-picture map: without a turn, the same as the crop; with Eye camera's turn, the frame's middle in the
        // picture's middle from anywhere, and its corners inside a picture that did not reach them straight on
        WindowFrame h1;
        const float ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0}, ez[3] = {0, 0, 1};
        float cam[3] = {.15f, .05f, .9f}, uv[2];
        WindowMap m;
        assert(window_map(h1, cam, ex, ey, ez, 1.4f, 1.2f, &m));
        WindowCrop cr = window_crop(h1, cam, 1.4f, 1.2f);
        assert(window_map_at(m, 0, 0, uv)); near(uv[0], cr.u0), near(uv[1], cr.v0);
        assert(window_map_at(m, 1, 1, uv)); near(uv[0], cr.u1), near(uv[1], cr.v1);
        cam[0] = 1.6f;   // (far to the right: straight on, the frame is past the picture's edge)
        assert(window_crop(h1, cam, 1.4f, 1.2f).miss);
        float lq[4];
        window_look(cam, lq);
        const XrQuaternionf turn{lq[0], lq[1], lq[2], lq[3]};
        float tr[3], tu[3], tb[3], fwd[3];
        qrot(turn, ex, tr), qrot(turn, ey, tu), qrot(turn, ez, tb);
        const float back[3] = {0, 0, 1};
        qrot(turn, back, fwd);
        near(std::fabs(tr[1]), 0);   // (level)
        const float n = std::sqrt(cam[0] * cam[0] + cam[1] * cam[1] + cam[2] * cam[2]);
        near(fwd[0], cam[0] / n), near(fwd[2], cam[2] / n);   // (its back points from the middle to the eye)
        assert(window_map(h1, cam, tr, tu, tb, 1.4f, 1.2f, &m));
        assert(window_map_at(m, .5f, .5f, uv)); near(uv[0], .5f), near(uv[1], .5f);
        for (int c = 0; c < 4; c++) {
            assert(window_map_at(m, (float)(c & 1), (float)(c >> 1), uv));
            assert(uv[0] > 0 && uv[0] < 1 && uv[1] > 0 && uv[1] < 1);
        }
        return 0;
    }
    ''', cpp=True)
    # (the room around it: found per runtime, never by name; the frame end's result can give a way up; the underlay first)
    loop = function_body(source, 'void headset_thread() {')
    assert 'ei.environmentBlendMode = window_blend_mode();' in loop and 'window_frame_ended(ended);' in loop
    assert loop.index('layers[0] = (const XrCompositionLayerBaseHeader*)&room_layer') > loop.index('make_menu_quad(')
    for name in ('Quest', 'WiVRn', 'SteamVR', 'Monado'):
        assert name not in function_body(window, 'int window_room_want() {'), name
    assert 'exts.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME)' in window and 'window_instance_extensions(xr_exts);' in source
    # (the game through the frame: the camera given no turn and the head's place from the frame's viewpoint; each eye cropped
    # from where its picture was drawn (the slot's drawn pose and eye shift), never from the head now)
    assert 'vr::window_head(p.pose, out->q, out->p);' in source
    game = function_body(window, 'bool window_game_draw(int k, const WindowEyeView ev[2]) {')
    assert 'window_local(window.frame, sl.drawn.position, mid);' in game and 'sl.eye_shift[' in game and 'window.eye' not in game
    # (a flat frame in Window view: drawn into the window's own quads, never the world screen's quad, whose edge moved)
    assert 'if (sl.head_locked) return window_flat_draw(k, ev);' in game and 'window_flat_screen' not in source
    # (one projection layer by default, at each eye's own pose and FOV; the alpha flag only where something shows through)
    layers = function_body(window, 'int window_layers(')
    assert 'v.pose = {window.eye_q[e], window.eye[e]};' in layers and 'v.fov = window.fov[e];' in layers
    assert '(as_quads ? test : (room || test)) ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0' in layers
    # (an eye at or behind the frame's plane: the window fades out, but a layer is still sent every frame; a headset frame
    # with none made WiVRn's frame loop spin and the headset go black, 22 Nov; a frame not drawn sends the last one again)
    assert 'if (!front) return 0;' not in layers and layers.count('return window_last_layers(out);') >= 3
    assert 'window.fade = std::clamp((nearest - kWindowNear) / kWindowFadeNear, 0.0f, 1.0f);' in layers
    # (the last layers sent again re-pointed at their own copy by their count: a stale pointer sent a layer of type 0)
    last = function_body(window, 'int window_last_layers(WindowLayers& out) {')
    assert 'if (last.n == 1) {' in last and 'out.proj.views = out.views;' in last and '&last.proj' not in last
    # (a refused frame end rests the room 2 s and tries again; given up only after kWindowRefusals)
    assert 'if (++n >= kWindowRefusals) {' in function_body(window, 'void window_frame_ended(XrResult r) {')


def check_cull_margin():
    """first-person culling keeps a ball around the eye, so Link's own gear isn't culled"""
    patcher = read('culling_host')
    start = patcher.index('static volatile int hos_vr_cull_first_person;')
    end = patcher.index('\n}\n', patcher.index('void hos_vr_cull_snapshot(Ctx *c, uint64_t snap) {')) + 3
    # Exercise the culling margin only; the optional visibility recorder between
    # its globals and function has its own complete-memory/context harness.
    snapshot_start = patcher.index('void hos_vr_cull_snapshot(Ctx *c, uint64_t snap) {')
    function = patcher[start:snapshot_start] + patcher[snapshot_start:end]

    compile_run(r'''
    #include <stdint.h>
    #include <string.h>
    #include <math.h>
    #include <assert.h>
    #include <stdio.h>
    typedef struct { int unused; } Ctx;
    static unsigned char memory[0x400];
    static float vr_load(Ctx *c, uint64_t a) { float f; (void)c; memcpy(&f, memory + a, 4); return f; }
    static void vr_store(Ctx *c, uint64_t a, float f) { (void)c; memcpy(memory + a, &f, 4); }
    static int ours = 1;
    static int hos_vr_snapshot_ours(Ctx *c, uint64_t s) { (void)c; (void)s; return ours; }
    static void hos_log(const char *f, ...) { (void)f; }
    #define HOS_HEAP_BASE 0x1000000000ULL
    static uint64_t recomp_load64(Ctx *c, uint64_t a) { (void)c; (void)a; return 0; }
    static void hos_vr_cull_note_view(Ctx *c, uint64_t snap) { (void)c; (void)snap; }   /* (the world-culling watch: check_world_cull) */
    static int frustum_option = 0;   /* (the VR tab's Headset frustum culling, option 931: 1 Off) */
    int rx_option(int id, int otherwise) { return id == 931 ? frustum_option : otherwise; }
    ''' + function + r'''
    static void vr_cull_lag_note(Ctx *c, uint64_t snap) { (void)c; (void)snap; }   /* (the lag log: check_world_cull) */
    static float dot3(const float *a, const float *b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
    static void put(uint64_t a, float f) { vr_store(0, a, f); }
    static float get(uint64_t a) { return vr_load(0, a); }
    /* Exact independent sphere/near rejection in the owned ModelNW path,
       guest 8009f478..8009f48c: if (near - radius > -view_z) reject. */
    static int scalar_near_visible(float view_z, float radius) {
        return !(get(0x1c8) - radius > -view_z);
    }
    /* A frustum as the game stores one: camera looking along fwd from eye, half-angles ty/tx in tan, near/far; sign picks
       inward (+1) or outward (-1) normals for both the view-space and world planes. */
    static float R[3][3];   /* camera-to-world rotation: columns right, up, back */
    static float eye[3];
    static void build(int sign, float yaw) {
        memset(memory, 0, sizeof memory);
        const float c = cosf(yaw), s = sinf(yaw);
        const float right[3] = {c, 0, -s}, up[3] = {0, 1, 0}, back[3] = {s, 0, c};
        for (int k = 0; k < 3; k++) R[k][0] = right[k], R[k][1] = up[k], R[k][2] = back[k];
        eye[0] = 3, eye[1] = 1.6f, eye[2] = -2;
        for (int k = 0; k < 3; k++) {
            for (int j = 0; j < 3; j++) put(0xd8 + 16 * k + 4 * j, R[k][j]);
            put(0xe4 + 16 * k, eye[k]);
        }
        const float zn = .1f, zf = 1000, tx = 1.4f, ty = 1.2f;
        put(0x1c8, zn), put(0x1cc, zf);
        /* view-space normals of the four side planes, through the eye (the camera looks down -z) */
        float vn[4][3] = {{-1, 0, -tx}, {1, 0, -tx}, {0, -1, -ty}, {0, 1, -ty}};   /* (inward: -x - tx z >= 0 etc.) */
        for (int k = 0; k < 4; k++) {
            float l = sqrtf(dot3(vn[k], vn[k]));
            for (int j = 0; j < 3; j++) vn[k][j] = vn[k][j] / l * sign;
            for (int j = 0; j < 3; j++) put(0x08 + 16 * k + 4 * j, vn[k][j]);
            put(0x08 + 16 * k + 12, 0.0f);   /* n.corner, the corner on the plane through the eye */
        }
        float wn[6][3], p[6][3];
        for (int k = 0; k < 4; k++) {
            for (int j = 0; j < 3; j++) wn[k][j] = R[j][0] * vn[k][0] + R[j][1] * vn[k][1] + R[j][2] * vn[k][2];
            memcpy(p[k], eye, sizeof eye);
        }
        for (int j = 0; j < 3; j++) {
            wn[4][j] = -back[j] * sign, p[4][j] = eye[j] - back[j] * zn;   /* near: inside is ahead */
            wn[5][j] = back[j] * sign, p[5][j] = eye[j] - back[j] * zf;
        }
        for (int k = 0; k < 6; k++) {
            for (int j = 0; j < 3; j++) put(0x1f0 + 16 * k + 4 * j, wn[k][j]);
            put(0x1f0 + 16 * k + 12, -dot3(wn[k], p[k]));
        }
        for (int k = 0; k < 3; k++) put(0x250 + 4 * k, eye[k] - 1000), put(0x25c + 4 * k, eye[k] + 1000);
        put(0x250 + 4, eye[1] - 0.05f);   /* (a box whose floor is just under the eye, to see it grow) */
    }
    static int inside(const float *q, int sign) {
        for (int k = 0; k < 6; k++) {
            float n[3], d = get(0x1f0 + 16 * k + 12);
            for (int j = 0; j < 3; j++) n[j] = get(0x1f0 + 16 * k + 4 * j);
            if (sign * (dot3(n, q) + d) < -1e-4f) return 0;
        }
        for (int k = 0; k < 3; k++) if (q[k] < get(0x250 + 4 * k) - 1e-4f || q[k] > get(0x25c + 4 * k) + 1e-4f) return 0;
        return 1;
    }
    static int view_inside(const float *v, int sign) {   /* a view-space point against the four view planes n.p = w */
        for (int k = 0; k < 4; k++) {
            float n[3];
            for (int j = 0; j < 3; j++) n[j] = get(0x08 + 16 * k + 4 * j);
            if (sign * (dot3(n, v) - get(0x08 + 16 * k + 12)) < -1e-4f) return 0;
        }
        return 1;
    }
    static void at(const float *local, float *world) {   /* camera-local (right, up, back) to world */
        for (int k = 0; k < 3; k++) world[k] = eye[k] + R[k][0] * local[0] + R[k][1] * local[1] + R[k][2] * local[2];
    }
    int main(void) {
        Ctx c;
        for (int sign = -1; sign <= 1; sign += 2)
            for (int turn = 0; turn < 4; turn++) {
                build(sign, turn * 1.3f);
                const float hips[3] = {.15f, -.95f, -.2f}, behind[3] = {0, -.3f, .6f}, far_side[3] = {150, 0, -100};
                const float far_in[3] = {120, 0, -100};
                float w[3];
                at(hips, w); assert(!inside(w, sign));   /* the game's own hands at the hips: culled as the game builds it */
                assert(!scalar_near_visible(0.6f,0.2f)); /* native weapon behind the eye: scalar near rejects it */
                at(far_in, w); assert(inside(w, sign));
                for (int k = 0; k < 3; k++) hos_vr_cull_eye[k] = eye[k];
                hos_vr_cull_first_person = 0;
                hos_vr_cull_snapshot(&c, 0);
                at(hips, w); assert(!inside(w, sign));   /* third person: untouched */
                assert(get(0x1c8)==0.1f && !scalar_near_visible(0.6f,0.2f));
                hos_vr_cull_first_person = 1;
                ours = 0; hos_vr_cull_snapshot(&c, 0); ours = 1;
                at(hips, w); assert(!inside(w, sign));   /* not the headset camera's snapshot */
                assert(get(0x1c8)==0.1f);
                hos_vr_cull_eye[0] += 2; hos_vr_cull_snapshot(&c, 0); hos_vr_cull_eye[0] -= 2;
                at(hips, w); assert(!inside(w, sign));   /* another camera at the same FOV elsewhere */
                hos_vr_cull_snapshot(&c, 0);
                at(hips, w); assert(inside(w, sign));    /* first person, headset camera: Link's own equipment is kept */
                at(behind, w); assert(inside(w, sign));
                assert(view_inside(hips, sign) && view_inside(behind, sign));
                assert(get(0x1c8)==0.1f && get(0x1cc)==1000.0f);
                assert(!scalar_near_visible(0.6f,0.2f)); /* independent scalar test needs the narrowly scoped gear hook */
                assert(!scalar_near_visible(2.0f,0.2f)); /* still finite: farther behind is not globally uncullable */
                assert(get(0x1d0)==0.0f); /* no projection/FOV field was changed */
                at(far_in, w); assert(inside(w, sign));
                at(far_side, w); assert(!inside(w, sign));   /* 50 m outside at 100 m: still culled */
                const float edge[3] = {140 + 2.4f, 0, -100}, out[3] = {140 + 2.8f, 0, -100};   /* (1.5 m across the plane: 2.58 m sideways at this depth) */
                at(edge, w); assert(inside(w, sign));    /* (the margin: 1.5 m across each plane, anywhere) */
                at(out, w); assert(!inside(w, sign));
                float snapshot[0x100];
                memcpy(snapshot, memory, sizeof snapshot);
                hos_vr_cull_snapshot(&c, 0);                 /* idempotent: a second call changes nothing */
                assert(!memcmp(snapshot, memory, sizeof snapshot));
            }
        /* Headset frustum culling Off (the test): nothing within 200 m is outside, wherever it is; farther still is */
        build(1, 0.7f);
        for (int k = 0; k < 3; k++) hos_vr_cull_eye[k] = eye[k];
        hos_vr_cull_first_person = 1, frustum_option = 1;
        hos_vr_cull_snapshot(&c, 0);
        {
            const float side[3] = {150, 0, -10}, behind[3] = {0, 20, 150}, up_close[3] = {.3f, 2, .5f}, beyond[3] = {0, 0, 205};
            float w[3];
            at(side, w); assert(inside(w, 1));
            at(behind, w); assert(inside(w, 1));
            at(up_close, w); assert(inside(w, 1));
            at(beyond, w); assert(!inside(w, 1));   /* (201 m behind the near plane's new place: still culled) */
            assert(view_inside(side, 1) && view_inside(behind, 1));
            assert(get(0x1c8) == 0.1f && get(0x1cc) == 1000.0f);   /* (near and far untouched) */
        }
        frustum_option = 0;
        build(1, 0.7f);
        hos_vr_cull_snapshot(&c, 0);
        { const float side[3] = {150, 0, -10}; float w[3]; at(side, w); assert(!inside(w, 1)); }   /* (Game: as before) */
        /* Wider: the side planes 10 degrees out each way (the half-angle across is atan(1.4), up atan(1.2)), in world and view
           space alike; near and far stay; and the ball is still there */
        for (int sign = -1; sign <= 1; sign += 2) {
            build(sign, 0.7f);
            for (int k = 0; k < 3; k++) hos_vr_cull_eye[k] = eye[k];
            frustum_option = 2;
            hos_vr_cull_snapshot(&c, 0);
            const float across = atanf(1.4f), up = atanf(1.2f), d = 100;
            const float in5[3] = {d * tanf(across + 5 / 57.2958f), 0, -d}, out15[3] = {d * tanf(across + 15 / 57.2958f), 0, -d};
            const float up5[3] = {0, d * tanf(up + 5 / 57.2958f), -d}, up15[3] = {0, d * tanf(up + 15 / 57.2958f), -d};
            const float hips[3] = {.15f, -.95f, -.2f};
            float w[3];
            at(in5, w); assert(inside(w, sign)); at(out15, w); assert(!inside(w, sign));
            at(up5, w); assert(inside(w, sign)); at(up15, w); assert(!inside(w, sign));
            at(hips, w); assert(inside(w, sign));
            assert(view_inside(in5, sign) && !view_inside(out15, sign) && view_inside(up5, sign) && !view_inside(up15, sign));
            assert(get(0x1c8) == 0.1f && get(0x1cc) == 1000.0f);
            frustum_option = 0;
        }
        build(1, 0);   /* inconsistent planes (one flipped): left as the game made them */
        for (int j = 0; j < 4; j++) put(0x1f0 + 16 * 2 + 4 * j, -get(0x1f0 + 16 * 2 + 4 * j));
        float before[0x100];
        memcpy(before, memory, sizeof before);
        hos_vr_cull_snapshot(&c, 0);
        assert(!memcmp(before, memory, sizeof before));
        return 0;
    }
    ''')

def check_window_test_verdict():
    """the windowed headset test (vr/tools/fake_headset/window_test.py) passes only with the evidence for what it was asked: the
    title's menu and pictures and headset frames; a whole run also every wmctrl call, a new swapchain after each resize and
    headset frames after the last; never with a lost GPU. bench.py's restore_ini puts the game folder's options back as they
    were (the file, or no file)"""
    sys.path[:0] = [str(REPO / 'scripts'), str(REPO / 'vr/tools'), str(REPO / 'vr/tools/fake_headset')]
    import bench
    import window_test
    frames = b'vr: 640 headset frames shown in the last 10 s (eye distance 64.0 mm)\n'
    chain = b'vk: swapchain 1280x720, 3 images, FIFO (V-Sync on)\n'
    title = frames + b'title\n'
    def ev(**kw):
        e = dict(alive=True, menu=True, title_shots=2, in_play=True, area='kakariko', wmctrl_failed=[], marks=[], resizes=2)
        e.update(kw)
        return e
    assert window_test.verdict(ev(), title, True) == []
    assert window_test.verdict(ev(menu=False), title, True), 'the title wait ran out'
    assert window_test.verdict(ev(title_shots=1), title, True)
    assert window_test.verdict(ev(alive=False), title, True)
    assert window_test.verdict(ev(), b'title\n', True), 'no headset frames'
    assert window_test.verdict(ev(), title + b'[error] vk: device lost\n', True)
    # (a whole run: two resizes, marked where run.log had got to)
    one, two = len(title), len(title) + len(chain)
    run = title + chain + chain + frames
    assert window_test.verdict(ev(marks=[one, two]), run, False) == []
    assert window_test.verdict(ev(marks=[one, two], wmctrl_failed=['resize 1 -e: exit 1']), run, False)
    assert window_test.verdict(ev(marks=[one]), title + chain + frames, False), 'a resize not made'
    assert window_test.verdict(ev(marks=[one, two]), title + chain + frames, False), 'no swapchain after resize 2'
    assert window_test.verdict(ev(marks=[one, two]), title + chain + chain, False), 'no frames after the last resize'
    assert window_test.verdict(ev(marks=[one, two]), title + chain + chain + frames.replace(b'640', b'0'), False)
    assert window_test.verdict(ev(marks=[one, two]),
                               title + chain + b'vk: swapchain creation failed for drawable 1x1\n' + frames, False)
    assert window_test.verdict(ev(in_play=False), title, False)
    with tempfile.TemporaryDirectory() as tmp:
        game, out = Path(tmp) / 'game', Path(tmp) / 'out'
        game.mkdir(), out.mkdir()
        ini = game / 'hookshot_options.ini'
        ini.write_text('3d=1\nvolume=7\n')
        bench.write_ini(game, None, {'3d': '4', 'fov': '2'}, out)
        assert ini.read_text() == '3d=4\nvolume=7\nfov=2\n' == (out / 'options.run.ini').read_text()
        bench.restore_ini(game, out)
        assert ini.read_text() == '3d=1\nvolume=7\n'
        # (no options file before: the run's is removed again; nothing written: nothing touched)
        ini.unlink()
        source, out2, out3 = Path(tmp) / 'source.ini', Path(tmp) / 'out2', Path(tmp) / 'out3'
        out2.mkdir(), out3.mkdir()
        source.write_text('3d=1\n')
        bench.write_ini(game, source, {'3d': '4'}, out2)
        assert ini.read_text() == '3d=4\n'
        bench.restore_ini(game, out2)
        assert not ini.exists()
        try:
            bench.write_ini(game, None, {'3d': '4'}, out3)
            raise AssertionError('write_ini with no options file and no source')
        except FileNotFoundError:
            pass
        bench.restore_ini(game, out3)
        assert not ini.exists()


def check_world_cull():
    """the world-culling watch names each actor (its +0x260 name) and tells whether ITS OWN model reached the sphere test
    and what it did (another model nearby is not its own), and whether the
    game camera's own fields saw it; the World culling switch gives those fields the headset's view for one frame and puts
    the game's back (vr/camera/culling_host.c)"""
    source = read('culling_host')
    compile_run(r'''
    #include <stdint.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
    #include <stdarg.h>
    #include <math.h>
    #include <assert.h>
    #define HOS_HEAP_BASE 0x1000000000ULL
    #define B HOS_HEAP_BASE
    typedef struct { int unused; } Ctx;
    static unsigned char memory[0x8000];
    static void *hos_guest_range(uint64_t a, uint64_t n) { return a >= B && a - B <= sizeof memory && n <= sizeof memory - (a - B) ? memory + (a - B) : 0; }
    static uint32_t recomp_load32(Ctx *c, uint64_t a) { (void)c; uint32_t v; assert(hos_guest_range(a, 4)); memcpy(&v, hos_guest_range(a, 4), 4); return v; }
    static uint64_t recomp_load64(Ctx *c, uint64_t a) { (void)c; uint64_t v; assert(hos_guest_range(a, 8)); memcpy(&v, hos_guest_range(a, 8), 8); return v; }
    static uint8_t recomp_load8(Ctx *c, uint64_t a) { (void)c; assert(hos_guest_range(a, 1)); return *(uint8_t *)hos_guest_range(a, 1); }
    static void recomp_store32(Ctx *c, uint64_t a, uint32_t v) { (void)c; assert(hos_guest_range(a, 4)); memcpy(hos_guest_range(a, 4), &v, 4); }
    static void recomp_store64(Ctx *c, uint64_t a, uint64_t v) { (void)c; assert(hos_guest_range(a, 8)); memcpy(hos_guest_range(a, 8), &v, 8); }
    static float vr_load(Ctx *c, uint64_t a) { uint32_t b = recomp_load32(c, a); float f; memcpy(&f, &b, 4); return f; }
    static void vr_store(Ctx *c, uint64_t a, float f) { uint32_t b; memcpy(&b, &f, 4); recomp_store32(c, a, b); }
    static void putf(uint64_t a, float f) { vr_store(0, a, f); }
    static float getf(uint64_t a) { return vr_load(0, a); }
    static void putd(uint64_t a, double d) { uint64_t b; memcpy(&b, &d, 8); recomp_store64(0, a, b); }
    static double getd(uint64_t a) { uint64_t b = recomp_load64(0, a); double d; memcpy(&d, &b, 8); return d; }
    static int64_t now = 5000000000LL;
    static int64_t hos_now_ns(void) { return now; }
    static char logged[64][400]; static int lines;
    static void hos_log(const char *f, ...) { va_list a; va_start(a, f); vsnprintf(logged[lines % 64], 400, f, a); va_end(a); lines++; }
    static const char *last(void) { return lines ? logged[(lines - 1) % 64] : ""; }
    static int option = 0;
    int rx_option(int id, int otherwise) { return id == 923 ? option : otherwise; }
    static volatile float hos_vr_fov_y = 1.9f, hos_vr_aspect = 1.0f, hos_vr_game_fovy = 0.87f;
    volatile float hos_wide_ratio = 1.25f;   /* (the headset's 5:4 picture: a full-screen menu's 3D, hos_vr_menu_projection) */
    static int menu_up;
    int hos_vr_menu_now(void) { return menu_up; }
    int setenv(const char *name, const char *value, int overwrite);   /* (POSIX) */
    #define SNAP (B + 0x1000)
    static int hos_vr_snapshot_ours(Ctx *c, uint64_t s) { (void)c; return s == SNAP; }
    ''' + source + r'''
    /* a snapshot as the game keeps one: the eye at E looking down -z (no turn), half-angle tangent 1 each way */
    static const float E[3] = {10, 1.5f, 20};
    static void snapshot(void) {
        const float t = 1, l = sqrtf(2);
        const float out[4][3] = {{-1, 0, t}, {1, 0, t}, {0, -1, t}, {0, 1, t}};   /* view space, outward: n.v - w > r is out */
        for (int k = 0; k < 4; k++) {
            for (int j = 0; j < 3; j++) putf(SNAP + 0x08 + 16 * k + 4 * j, out[k][j] / l);
            putf(SNAP + 0x08 + 16 * k + 12, 0);
        }
        for (int k = 0; k < 3; k++) {   /* view rows (right, up, back) and camera-to-world rows (+0xd8, eye in +0xe4) */
            for (int j = 0; j < 3; j++) putf(SNAP + 0xa8 + 16 * k + 4 * j, k == j), putf(SNAP + 0xd8 + 16 * k + 4 * j, k == j);
            putf(SNAP + 0xa8 + 16 * k + 12, -E[k]), putf(SNAP + 0xe4 + 16 * k, E[k]);
        }
        putf(SNAP + 0x1c8, 0.1f), putf(SNAP + 0x1cc, 1000), putf(SNAP + 0x1d0, hos_vr_fov_y);
        float wn[6][3], wp[6][3];
        for (int k = 0; k < 4; k++) for (int j = 0; j < 3; j++) wn[k][j] = out[k][j] / l, wp[k][j] = E[j];   /* (outward: any sign will do) */
        for (int j = 0; j < 3; j++) wn[4][j] = j == 2 ? 1 : 0, wp[4][j] = E[j] - (j == 2) * 0.1f, wn[5][j] = j == 2 ? -1 : 0, wp[5][j] = E[j] - (j == 2) * 1000;
        for (int k = 0; k < 6; k++) {
            float d = 0;
            for (int j = 0; j < 3; j++) putf(SNAP + 0x1f0 + 16 * k + 4 * j, wn[k][j]), d -= wn[k][j] * wp[k][j];
            putf(SNAP + 0x1f0 + 16 * k + 12, d);
        }
        for (int k = 0; k < 3; k++) hos_vr_cull_eye[k] = E[k];
        hos_vr_cull_note_view(0, SNAP);
    }
    #define SPHERE (B + 0x2000)
    static void model(float x, float y, float z, float r, uint64_t m) {   /* (m: the ModelNW tested, 0 another's) */
        putf(SPHERE, x), putf(SPHERE + 4, y), putf(SPHERE + 8, z), putf(SPHERE + 12, r);
        hos_vr_cull_model_seen(0, SNAP, SPHERE, m);
    }
    #define ACTOR (B + 0x3000)
    #define NAMES (B + 0x3800)
    static void actor_at(uint64_t a, float x, float y, float z, const char *name) {   /* (its name's pointer at +0x260) */
        putf(a + 796, x), putf(a + 800, y), putf(a + 804, z);
        const uint64_t text = NAMES + (a - ACTOR);
        recomp_store64(0, a + 0x260, text);
        memcpy(memory + (text - B), name, strlen(name) + 1);
    }
    /* an actor's model as the game links it: +0x270 component table, +8 ModelComponent, +0x18 resource, +0x18 node, ModelNW */
    #define MODELS (B + 0x6000)
    static uint64_t give_model(uint64_t a, int i) {
        const uint64_t t = MODELS + 0x80 * i;
        recomp_store64(0, a + 0x270, t);
        recomp_store64(0, t + 8, t + 0x10), recomp_store64(0, t + 0x10, 0x84c81020ULL);
        recomp_store64(0, t + 0x28, t + 0x30), recomp_store64(0, t + 0x30, 0x84c288a0ULL);
        recomp_store64(0, t + 0x48, t + 0x50), recomp_store64(0, t + 0x50, t + 0x60), recomp_store64(0, t + 0x60, 0x84c28cc8ULL);
        return t + 0x60;
    }
    #define VIEW (B + 0x5000)
    static void game_camera(float px, float pz) {   /* the game's own: at (px, 1.5, pz) looking at the eye's spot, doubles too */
        const float f[9] = {px, 1.5f, pz, E[0], 1.5f, E[2], 0, 1, 0};
        for (int k = 0; k < 9; k++) putf(VIEW + 0x38 + 4 * k, f[k]);
        for (int k = 0; k < 6; k++) putd(VIEW + 0x60 + 8 * k, f[k]);
        recomp_store64(0, VIEW, 0x84c18a88ULL);
    }
    static void headset_matrix(float yaw) {   /* the mod's view: rows right, up, back with -axis.eye, the eye at E turned by yaw */
        const float c = cosf(yaw), s = sinf(yaw), ax[3][3] = {{c, 0, -s}, {0, 1, 0}, {s, 0, c}};
        for (int r = 0; r < 3; r++) {
            for (int k = 0; k < 3; k++) putf(VIEW + 8 + 16 * r + 4 * k, ax[r][k]);
            putf(VIEW + 8 + 16 * r + 12, -(ax[r][0] * E[0] + ax[r][1] * E[1] + ax[r][2] * E[2]));
        }
    }
    int main(void) {
        setenv("RX_VR_CULL_WATCH", "1", 1);   /* (off by default: this check is the watch's) */
        Ctx *c = 0;
        snapshot();
        /* the census: a model ahead is kept, one behind the eye culled, an empty place untested; it forgets in time */
        model(10, 1.5f, 15, 0.5f, MODELS + 0x1000);
        model(10, 1.5f, 26, 0.5f, MODELS + 0x1008);
        const float ahead[3] = {10, 1.5f, 15}, behind[3] = {10, 1.5f, 26}, empty[3] = {10, 1.5f, 5};
        assert(vr_seen_near(ahead, now) == VR_SEEN_KEPT && vr_seen_near(behind, now) == VR_SEEN_CULLED && vr_seen_near(empty, now) == -1);
        assert(vr_seen_near(ahead, now + 400000000LL) == -1);
        { const float side[3] = {10 + 1.5f, 1.5f, 15}; assert(vr_seen_near(side, now) & VR_SEEN_KEPT); }   /* (within a cell of it) */
        assert(vr_seen_get(MODELS + 0x1000, now) == VR_SEEN_KEPT && vr_seen_get(MODELS + 0x1008, now) == VR_SEEN_CULLED);   /* (and each model itself) */
        assert(vr_seen_get(MODELS + 0x1010, now) == -1 && vr_seen_get(MODELS + 0x1000, now + 400000000LL) == -1);
        /* names: the ActorAccessor's pointer at +0x260, else +0x20 (not the Player object's +16 -> +88 record) */
        {
            char name[48];
            actor_at(ACTOR + 0x200, 0, 0, 0, "Obj_Name_A_01");
            assert(vr_actor_name(c, ACTOR + 0x200, name) && !strcmp(name, "Obj_Name_A_01"));
            recomp_store64(0, ACTOR + 0x200 + 0x20, recomp_load64(0, ACTOR + 0x200 + 0x260)), recomp_store64(0, ACTOR + 0x200 + 0x260, 0);
            assert(vr_actor_name(c, ACTOR + 0x200, name) && !strcmp(name, "Obj_Name_A_01"));
            recomp_store64(0, ACTOR + 0x200 + 0x20, 0);
            assert(!vr_actor_name(c, ACTOR + 0x200, name) && !strcmp(name, "?"));
        }
        /* the watch: an actor ahead (in the headset's view) whose model never reached the test is told by name */
        game_camera(E[0], E[2] + 4);   /* (the game camera 4 m behind the eye, looking the same way) */
        now += 300000000LL;
        snapshot();
        hos_vr_cull_camera(c, VIEW, 0);
        actor_at(ACTOR, 10, 1.5f, 8, "Obj_CandleStick_A_01");
        const uint64_t candle = give_model(ACTOR, 0);
        int before = lines;
        hos_vr_cull_actor(c, ACTOR);
        assert(lines == before + 1 && strstr(last(), "Obj_CandleStick_A_01") && strstr(last(), "headset view 1") &&
               strstr(last(), "did NOT reach the sphere test") && strstr(last(), "game camera view 1"));
        /* another model kept at the same place is not its own: still not reached, no new line (the first watch pooled
           every model within 6 m, so a wall's or a floor's made a torch "kept") */
        now += 1100000000LL;
        snapshot(); hos_vr_cull_camera(c, VIEW, 0);
        model(10, 1.5f, 8, 0.6f, 0), model(10.5f, 1.5f, 8.5f, 0.6f, MODELS + 0x1000);
        before = lines;
        hos_vr_cull_actor(c, ACTOR);
        assert(lines == before);
        /* the same a moment later, its own model now tested and kept: one more line saying so */
        now += 1100000000LL;
        snapshot(); hos_vr_cull_camera(c, VIEW, 0);
        model(10, 1.5f, 8, 0.6f, candle);
        before = lines;
        hos_vr_cull_actor(c, ACTOR);
        assert(lines == before + 1 && strstr(last(), "kept by the sphere test"));
        /* unchanged: no line */
        now += 1100000000LL;
        snapshot(); hos_vr_cull_camera(c, VIEW, 0); model(10, 1.5f, 8, 0.6f, candle);
        before = lines; hos_vr_cull_actor(c, ACTOR); assert(lines == before || strstr(last(), "cull watch:"));
        /* its model kept by one view and culled by another (a test per view a frame): told so */
        now += 1100000000LL;
        snapshot(); hos_vr_cull_camera(c, VIEW, 0);
        model(10, 1.5f, 26, 0.5f, candle), model(10, 1.5f, 8, 0.6f, candle);
        before = lines; hos_vr_cull_actor(c, ACTOR);
        { int found = 0; for (int i = before; i < lines; i++) found |= strstr(logged[i % 64], "kept by one view's sphere test, culled by another's") != 0; assert(found); }
        /* the watch's lock held by another worker: the update is dropped and counted, no spin and no line; then looked at again */
        {
            const unsigned dropped = vr_watch_dropped;
            now += 1100000000LL;
            snapshot(); hos_vr_cull_camera(c, VIEW, 0); model(10, 1.5f, 8, 0.6f, candle);
            vr_watch_lock = 1;
            before = lines; hos_vr_cull_actor(c, ACTOR);
            assert(lines == before && vr_watch_dropped == dropped + 1 && vr_watch_lock == 1);
            vr_watch_lock = 0;
            hos_vr_cull_actor(c, ACTOR);
            assert(vr_watch_dropped == dropped + 1);
            { int found = 0; for (int i = before; i < lines; i++) found |= strstr(logged[i % 64], "Obj_CandleStick_A_01") &&
                                                                       strstr(logged[i % 64], "kept by the sphere test") != 0; assert(found); }
        }
        /* an actor beside the headset's view, never in it: never told; one far away is not watched */
        actor_at(ACTOR + 0x100, 40, 1.5f, 20, "Obj_Far_A_01");
        actor_at(ACTOR + 0x200, 10, 1.5f, 30, "Obj_Behind_A_01");
        before = lines;
        hos_vr_cull_actor(c, ACTOR + 0x100); hos_vr_cull_actor(c, ACTOR + 0x200);
        for (int i = before; i < lines; i++) assert(!strstr(logged[i % 64], "Obj_Far") && !strstr(logged[i % 64], "Obj_Behind"));
        /* ahead of the headset but behind a game camera that looks the other way: "game camera view 0" */
        game_camera(E[0], E[2] - 4);   /* (4 m ahead of the eye, looking back at it) */
        hos_vr_cull_camera(c, VIEW, 0);
        actor_at(ACTOR + 0x300, 10, 1.5f, 2, "Obj_InsideTheDekuTree_ThornStick_A_01");   /* (and no model: told so) */
        now += 300000000LL; snapshot(); hos_vr_cull_camera(c, VIEW, 0);
        before = lines;
        hos_vr_cull_actor(c, ACTOR + 0x300);
        assert(lines >= before + 1);
        { int found = 0; for (int i = before; i < lines; i++) found |= strstr(logged[i % 64], "ThornStick") && strstr(logged[i % 64], "game camera view 0") &&
                                                                   strstr(logged[i % 64], "no model found: no model component") != 0; assert(found); }
        /* places to watch (RX_VR_CULL_AT): a place ahead is told, untested and then kept; one behind the eye never */
        setenv("RX_VR_CULL_AT", "10,1.5,12; 10,1.5,30", 1);
        now += 300000000LL; snapshot(); option = 0;
        before = lines;
        hos_vr_cull_camera(c, VIEW, 0); hos_vr_cull_camera(c, VIEW, 1);
        { int found = 0; for (int i = before; i < lines; i++) found += strstr(logged[i % 64], "place 0") && strstr(logged[i % 64], "never reached the sphere test");
          for (int i = before; i < lines; i++) assert(!strstr(logged[i % 64], "place 1")); assert(found == 1); }
        now += 300000000LL; snapshot();
        model(10, 1.5f, 12, 0.5f, 0);
        before = lines;
        hos_vr_cull_camera(c, VIEW, 0); hos_vr_cull_camera(c, VIEW, 1);
        assert(lines == before + 1 && strstr(last(), "place 0") && strstr(last(), "kept"));
        /* the switch, Game: nothing of the camera is written */
        game_camera(E[0], E[2] + 4);
        unsigned char was[0x100];
        memcpy(was, memory + (VIEW - B), sizeof was);
        option = 0;
        hos_vr_cull_camera(c, VIEW, 0); headset_matrix(0.5f); memcpy(was, memory + (VIEW - B), sizeof was);
        hos_vr_cull_camera(c, VIEW, 1);
        assert(!memcmp(was, memory + (VIEW - B), sizeof was));
        /* Headset: the floats are the headset's eye and look (the target as far ahead as the game's), the doubles kept... */
        option = 1;
        hos_vr_cull_camera(c, VIEW, 1);
        const float yaw = 0.5f;
        assert(fabsf(getf(VIEW + 0x38) - E[0]) < 1e-4f && fabsf(getf(VIEW + 0x40) - E[2]) < 1e-4f);
        assert(fabsf(getf(VIEW + 0x44) - (E[0] - sinf(yaw) * 4)) < 1e-3f && fabsf(getf(VIEW + 0x4c) - (E[2] - cosf(yaw) * 4)) < 1e-3f);
        assert(getd(VIEW + 0x60) == E[0] && getd(VIEW + 0x70) == E[2] + 4);
        /* ... and the game's own back before the mod's view reads them next frame */
        hos_vr_cull_camera(c, VIEW, 0);
        assert(getf(VIEW + 0x38) == E[0] && getf(VIEW + 0x40) == E[2] + 4 && getf(VIEW + 0x44) == E[0] && getf(VIEW + 0x4c) == E[2]);
        assert(vr_game_cam[2] == E[2] + 4);   /* (the watch sees the game's own camera) */
        /* a frame where the game wrote its camera anew: its new values stay */
        hos_vr_cull_camera(c, VIEW, 1);
        putf(VIEW + 0x38, 77);
        hos_vr_cull_camera(c, VIEW, 0);
        assert(getf(VIEW + 0x38) == 77 && vr_game_cam[0] == 77);
        /* Headset+: the doubles too, put back the same way */
        game_camera(E[0], E[2] + 4);
        option = 2;
        hos_vr_cull_camera(c, VIEW, 0);
        hos_vr_cull_camera(c, VIEW, 1);
        assert(fabs(getd(VIEW + 0x60) - E[0]) < 1e-4 && fabs(getd(VIEW + 0x70) - E[2]) < 1e-4);
        hos_vr_cull_camera(c, VIEW, 0);
        assert(getd(VIEW + 0x60) == E[0] && getd(VIEW + 0x70) == E[2] + 4 && getf(VIEW + 0x40) == E[2] + 4);
        /* doubles that are not the floats' (not this camera's layout): left alone */
        putd(VIEW + 0x60, 9999);
        hos_vr_cull_camera(c, VIEW, 0); hos_vr_cull_camera(c, VIEW, 1);
        assert(getd(VIEW + 0x60) == 9999);
        hos_vr_cull_camera(c, VIEW, 0);
        assert(getf(VIEW + 0x38) == E[0]);
        /* the culling's lag: a headset culling view made from the view before the newest is told, every 10 s */
        option = 0;
        headset_matrix(0); hos_vr_cull_camera(c, VIEW, 1);
        headset_matrix(0.3f); hos_vr_cull_camera(c, VIEW, 1);   /* (the newest: turned 17 degrees) */
        hos_vr_cull_first_person = 1;
        snapshot(); hos_vr_cull_snapshot(c, SNAP);   /* (looking where the view before it looked) */
        now += 10000000000LL;
        snapshot(); before = lines; hos_vr_cull_snapshot(c, SNAP);
        { int found = 0; for (int i = before; i < lines; i++) found |= strstr(logged[i % 64], "culling vs headset (10 s): 2 headset culling views, 17.19") &&
                                                                   strstr(logged[i % 64], "2 of them nearer the view before it") != 0;
          if (!found) for (int i = before; i < lines; i++) fprintf(stderr, "%s\n", logged[i % 64]); assert(found); }
        headset_matrix(0.3f); hos_vr_cull_camera(c, VIEW, 1);   /* (no turn since: made from the newest, nothing older) */
        snapshot(); hos_vr_cull_snapshot(c, SNAP);
        headset_matrix(0); hos_vr_cull_camera(c, VIEW, 1);
        now += 10000000000LL;
        snapshot(); before = lines; hos_vr_cull_snapshot(c, SNAP);
        { int found = 0; for (int i = before; i < lines; i++) found |= strstr(logged[i % 64], "0 of them nearer the view before it") != 0; assert(found); }
        {   /* (a full-screen menu: its 3D keeps the game's horizontal view, so the headset's middle 16:9 crop of the 5:4 picture
               is the game's own 16:9 frame; the game's angle kept, never compounded, and put back once the menu is gone) */
            const uint64_t P = B + 0x6000;
            const float game = 0.5f, want = 2 * atanf(tanf(0.25f) * (16.0f / 9.0f) / 1.25f);
            putf(P + 0xa0, game), putf(P + 0xa4, sinf(0.25f)), putf(P + 0xa8, cosf(0.25f)), putf(P + 0xac, tanf(0.25f));
            float tn = 0;
            hos_vr_fov_y = 0, hos_vr_aspect = 0, menu_up = 1;
            for (int k = 0; k < 3; k++) {
                assert(hos_vr_projection_locked(c, P, 16.0f / 9.0f, &tn) == 1);
                assert(fabsf(getf(P + 0xa0) - want) < 1e-5f && fabsf(tn - tanf(0.25f) * (16.0f / 9.0f) / 1.25f) < 1e-5f);
                assert(fabsf(getf(P + 0xac) - tn) < 1e-5f);
            }
            assert(hos_vr_projection_locked(c, P, 1.5f, &tn) == 0 && fabsf(getf(P + 0xa0) - want) < 1e-5f);   /* (not a 16:9 camera: left as it is) */
            assert(hos_vr_projection_locked(c, P, 1.25f, &tn) == 1 && fabsf(getf(P + 0xa0) - want) < 1e-5f);   /* (its ratio already the picture's) */
            menu_up = 0;
            assert(hos_vr_projection_locked(c, P, 16.0f / 9.0f, &tn) == 1 && fabsf(getf(P + 0xa0) - game) < 1e-6f);
            assert(fabsf(tn - tanf(0.25f)) < 1e-5f);
            assert(hos_vr_projection_locked(c, P, 16.0f / 9.0f, &tn) == 0 && fabsf(getf(P + 0xa0) - game) < 1e-6f);
        }
        return 0;
    }
    ''')


# ======================================================================================================================
# Controls: quick select, the selected item
# ======================================================================================================================

def check_pad_combo_binding():
    """a controller binding's buttons, decoded once for everyone (vk_api.h hos_pad_binding_mask): an empty slot (-1) is no
    button, not a chord of every one (review 20 Nov: an empty ocarina-note alternate swallowed all pad input, and the
    Controls page showed an empty slot as every button); a malformed tagged code is none either"""
    api = (REPO / 'gpu/vk/vk_api.h').read_text(encoding='utf-8')
    hid = (REPO / 'host/nn_hid.c').read_text(encoding='utf-8')
    menu = (REPO / 'gpu/vk/vk_menu.cpp').read_text(encoding='utf-8')
    pads = api[api.index('enum { HOS_PAD_SOUTH'):]
    pads = pads[:pads.index('};') + 2]
    defines = '\n'.join(l for l in api.splitlines() if l.startswith('#define HOS_PAD_COMBO'))
    decoder = function_body(api, 'static inline uint32_t hos_pad_binding_mask(')
    fixture = '#include <stdint.h>\n' + pads + '\n' + defines + '\nstatic inline uint32_t hos_pad_binding_mask(int code) ' + decoder
    host_fns = hid[hid.index('static uint32_t pad_binding_mask('):hid.index('static int is_note_row(')]
    text_fn = menu[menu.index('std::string pad_binding_text(int code) {'):menu.index('const char* key_name(int vk) {')]
    compile_run(r"""
    #include <assert.h>
    #include <string>
    """ + fixture + "\n" + host_fns + r"""
    namespace menu {   // (the Controls page's: vk_menu.cpp, a pad's name by its place)
    const char* pad_name(int p) { static const char* n[] = {"A", "B", "X", "Y"}; return p < 4 ? n[p] : "other"; }
    uint32_t pad_binding_mask(int code) { return hos_pad_binding_mask(code); }
    """ + text_fn + r"""
    }
    int main() {
        const int chord = HOS_PAD_COMBO | (1 << HOS_PAD_SOUTH) | (1 << HOS_PAD_EAST);   /* (a real two-button combination) */
        const uint32_t both = (1u << HOS_PAD_SOUTH) | (1u << HOS_PAD_EAST);
        assert(pad_binding_mask(-1) == 0 && pad_binding_valid(-1));                     /* (empty: no button, and allowed) */
        assert(pad_binding_mask(HOS_PAD_NORTH) == 1u << HOS_PAD_NORTH && pad_binding_valid(HOS_PAD_NORTH));
        assert(pad_binding_mask(chord) == both && pad_binding_valid(chord));
        /* (malformed: the flag with one button or none, bits past the pads' mask, the sign bit, an index past the pads) */
        const int bad[] = {HOS_PAD_COMBO | (1 << HOS_PAD_WEST), HOS_PAD_COMBO, chord | (int)(1u << (HOS_PAD_N + 1)),
                           (int)((unsigned)chord | 0x80000000u), HOS_PAD_N, -2, INT32_MIN};
        for (int code : bad) assert(pad_binding_mask(code) == 0 && !pad_binding_valid(code));
        /* (an ocarina note bound to the chord with its alternate left empty: what oc_notes takes from the pads is the chord's
           two buttons, not every button) */
        const int note[2] = {chord, -1};
        uint64_t consumed = 0;
        for (int k = 0; k < 2; k++) consumed |= pad_binding_mask(note[k]);
        assert(consumed == both);
        /* (and is held only with both of its buttons; the empty alternate is never held, whatever is pressed) */
        const uint32_t all = HOS_PAD_COMBO_MASK;
        int held = 0;
        for (int k = 0; k < 2; k++) {
            const uint32_t mask = pad_binding_mask(note[k]);
            if (mask && ((1u << HOS_PAD_NORTH) & mask) == mask) held = 1;
        }
        assert(!held);
        held = 0;
        for (int k = 0; k < 2; k++) {
            const uint32_t mask = pad_binding_mask(note[k]);
            if (mask && (all & mask) == mask) held |= 1 << k;
        }
        assert(held == 1);   /* (all buttons down: the chord, slot 0, holds; the empty slot 1 does not) */
        /* (the Controls page's names: an empty slot is None, the chord its two buttons) */
        assert(menu::pad_binding_text(-1) == "None" && menu::pad_binding_text(chord) == "A + B");
        assert(menu::pad_binding_text(HOS_PAD_COMBO | (1 << HOS_PAD_WEST)) == "None");
        return 0;
    }
    """, cpp=True)
    # (every reader goes through the one decoder; no other place decodes the flag itself)
    assert 'static uint32_t pad_binding_mask(int code) { return hos_pad_binding_mask(code); }' in hid
    assert 'uint32_t pad_binding_mask(int code) { return hos_pad_binding_mask(code); }' in menu
    for name, text in (('host/nn_hid.c', hid), ('gpu/vk/vk_menu.cpp', menu)):
        assert '(code & HOS_PAD_COMBO) == HOS_PAD_COMBO' not in text, f'{name} decodes HOS_PAD_COMBO itself'
    oc = function_body(hid, 'static void oc_notes(')
    assert oc.count('pad_binding_mask(pad_bind[row][k])') == 2   # (the note's held test and what it takes from the pads)


def check_stick_turn():
    """first person: the left stick is turned to the headset's heading; third person keeps the player's stick"""
    host = read('stick_host')
    start = host.index('#define kVrLeftStickMoving')   # (with the left stick's "moving Link" timestamp it sets)
    end = host.index('\n}\n', host.index('void hos_oot_vr_input(int32_t stick[4]) {')) + 3
    function = host[start:end]
    compile_run(r'''
    #include <stdint.h>
    #include <math.h>
    #include <string.h>
    #include <assert.h>
    typedef struct { int active,mode; } HookshotVrHead;
    static HookshotVrHead simulated={1,1};
    static int64_t clock_ns=1000000000;
    static int64_t hos_now_ns(void) { return clock_ns; }
    static int rx_vr_head(HookshotVrHead* h) { *h=simulated; return 1; }
    static volatile int64_t oot_vr_turn_bow_seen,oot_vr_turn_input_seen,oot_vr_move_seen;
    static volatile int32_t oot_vr_turn_stick;
    static volatile uint32_t oot_vr_move_yaw_bits;
    static int radial_open;
    static int hos_vr_radial_open(void) { return radial_open; }   // (vr/menu/radial_host.c: the item menu open mutes the left stick)
    ''' + function + r'''
    int main(void) {
        int32_t s[4]={100,200,30000,1000};
        oot_vr_turn_bow_seen=clock_ns;
        simulated.mode=0; hos_oot_vr_input(s);
        assert(s[2]==30000 && s[3]==1000 && !oot_vr_turn_input_seen);
        simulated.mode=1; hos_oot_vr_input(s);
        assert(s[2]==0 && s[3]==0 && oot_vr_turn_stick==30000);   // (first person: the game never gets the right stick's pitch)
        s[3]=1000; radial_open=2; hos_oot_vr_input(s); assert(s[2]==0 && s[3]==0 && oot_vr_turn_stick==0); radial_open=0;
        s[2]=23000; clock_ns+=200000000; hos_oot_vr_input(s); assert(s[2]==23000);
        oot_vr_turn_bow_seen=clock_ns; simulated.active=0; hos_oot_vr_input(s); assert(s[2]==23000);
        simulated.active=1; oot_vr_turn_bow_seen=0; hos_oot_vr_input(s); assert(s[2]==23000);
        // (the left stick moving Link is noted for the heading's re-align after events; a small push or the item menu is not)
        oot_vr_left_stick_seen=0; s[0]=3000; s[1]=0; hos_oot_vr_input(s); assert(!oot_vr_left_stick_seen);
        s[0]=0; s[1]=20000; radial_open=1; hos_oot_vr_input(s); assert(!oot_vr_left_stick_seen); radial_open=0;
        s[0]=0; s[1]=20000; hos_oot_vr_input(s); assert(oot_vr_left_stick_seen==clock_ns);
        return 0;
    }
    ''')

def check_stick_jump():
    """first person: a clear push up on the right stick is one jump; turning, diagonals and a held stick never jump again"""
    host = read('controllers_host')
    start = host.index('#define kVrJumpPush')
    end = host.index('\n}\n', host.index('static int vr_stick_jump(')) + 3
    compile_run(r'''
    #include <stdint.h>
    #include <math.h>
    #include <assert.h>
    ''' + host[start:end] + r'''
    static int64_t t;
    static int at(float x, float y) { t += 11000000LL; return vr_stick_jump(x, y, t); }   /* (90 readings a second) */
    static int presses(const float (*path)[2], int n) {   /* how many separate presses a stick path makes */
        int was = 0, count = 0;
        for (int i = 0; i < n; i++) { const int now = at(path[i][0], path[i][1]); count += now && !was; was = now; }
        for (int i = 0; i < 30; i++) { const int now = at(0, 0); count += now && !was; was = now; }   /* (let go) */
        return count;
    }
    int main(void) {
        /* straight up and held there: one jump, the button held while the stick stays up */
        float up[60][2];
        for (int i = 0; i < 60; i++) up[i][0] = 0, up[i][1] = i < 5 ? i * .2f : 1;
        assert(presses(up, 60) == 1);
        /* up, back to the middle, up again: two jumps */
        float twice[40][2];
        for (int i = 0; i < 40; i++) twice[i][0] = 0, twice[i][1] = (i / 10) % 2 ? 0 : 1;
        assert(presses(twice, 40) == 2);
        /* up, eased to half way and back up without reaching the middle: still one jump */
        float wobble[40][2];
        for (int i = 0; i < 40; i++) wobble[i][0] = 0, wobble[i][1] = (i / 10) % 2 ? .5f : 1;
        assert(presses(wobble, 40) == 1);
        /* a turn: full right, then drifting up to the corner and to straight up without passing the middle: no jump */
        float turn[60][2];
        for (int i = 0; i < 60; i++) {
            const float a = i < 20 ? 0 : (i - 20) / 40.0f * 1.5707963f;   /* (right, swinging up to straight up) */
            turn[i][0] = cosf(a), turn[i][1] = sinf(a);
        }
        assert(presses(turn, 60) == 0);
        /* 45 degrees up and right (a sloppy turn): no jump; 20 degrees off straight up: a jump */
        float corner[20][2], near[20][2];
        for (int i = 0; i < 20; i++) corner[i][0] = corner[i][1] = .75f, near[i][0] = .34f, near[i][1] = .94f;
        assert(presses(corner, 20) == 0);
        assert(presses(near, 20) == 1);
        /* a quick flick up and straight back: still pressed long enough for the game to see (kVrJumpHoldNs) */
        int held = 0;
        at(0, 0);
        held += at(0, 1);
        for (int i = 0; i < 30; i++) held += at(0, 0);
        assert(held * 11000000LL >= kVrJumpHoldNs);
        /* pulled down: nothing */
        float down[20][2];
        for (int i = 0; i < 20; i++) down[i][0] = 0, down[i][1] = -1;
        assert(presses(down, 20) == 0);
        return 0;
    }
    ''')


def check_radial():
    """the VR radial menus open on a stick click (left: items, right: equipment), pick by that stick's direction, select on its
    release, close on a click of the same stick and switch on the other; an open menu keeps the list it opened on, whatever the
    game lists meanwhile (review 20 Nov: a list rewritten as the menu opened)"""
    host = read('radial_host')
    start = host.index('static volatile int oot_vr_radial_side = -1;')
    end = host.index('/* the gear now worn')
    compile_run(r'''
    #include <stdint.h>
    #include <string.h>
    #include <math.h>
    #include <assert.h>
    #define VR_RADIAL_SLOTS 16
    static int64_t clock_ns = 5000000000LL;
    static int64_t hos_now_ns(void) { return clock_ns; }
    static void hos_log(const char *f, ...) { (void)f; }
    static float sx, sy, rsx, rsy;   // (the controllers' sticks, and their clicks: button 4)
    static int both_sticks;          // (Menu button: Both sticks)
    static int rx_option(int id, int otherwise) { return id == 1034 ? both_sticks : otherwise; }
    static uint32_t click, rclick;
    static int rx_vr_hands(void *out) {
        uint32_t *w = (uint32_t *)out;
        memcpy(&w[1 + 23], &sx, 4); memcpy(&w[1 + 24], &sy, 4); w[1 + 25] = click;
        memcpy(&w[1 + 26 + 23], &rsx, 4); memcpy(&w[1 + 26 + 24], &rsy, 4); w[1 + 26 + 25] = rclick;
        return 1;
    }
    ''' + host[start:end] + r'''
    static const uint64_t LS = 1ULL << 4, RS = 1ULL << 5;
    static uint64_t frame(uint64_t b, float x, float y) {
        clock_ns += 16000000; sx = x, sy = y; rsx = rsy = 0; click = (b & LS) ? 4 : 0; rclick = (b & RS) ? 4 : 0;
        return oot_vr_radial_controls(b);
    }
    static uint64_t right(uint64_t b, float x, float y) {
        clock_ns += 16000000; sx = sy = 0; rsx = x, rsy = y; click = (b & LS) ? 4 : 0; rclick = (b & RS) ? 4 : 0;
        return oot_vr_radial_controls(b);
    }
    int main(void) {
        const int items[4] = {10, 11, 12, 13}, gear[3] = {5, 6, 7};
        oot_vr_radial_publish(items, 4, gear, 3);
        assert(frame(LS, 0, 0) == 0 && !hos_vr_radial_open());      // (the click never reaches the game)
        frame(0, 0, 0); assert(hos_vr_radial_open());                // let go quickly: open
        frame(0, .9f, .1f); assert(oot_vr_radial_highlight == 1);    // right: slot 1 of 4
        frame(0, -.1f, -.9f); assert(oot_vr_radial_highlight == 2);  // down: slot 2
        frame(0, 0, 0); assert(!hos_vr_radial_open() && oot_vr_radial_pending == 12);   // back to the middle: selected
        oot_vr_radial_pending = -1;
        frame(LS, 0, 0); frame(0, 0, 0); assert(hos_vr_radial_open());
        frame(LS, 0, 0); frame(0, 0, 0); assert(!hos_vr_radial_open() && oot_vr_radial_pending == -1);   // clicked again: closed
        // the right click: the equipment menu, picked with the right stick (3 slots: right-down is slot 1)
        assert(right(RS, 0, 0) == 0);                                     // (the click never reaches the game)
        right(0, 0, 0); assert(hos_vr_radial_open() == 2);
        right(0, 0, .9f); right(0, .8f, -.5f); assert(oot_vr_radial_highlight == 1);
        frame(0, .9f, 0); assert(oot_vr_radial_highlight == 1);           // (the left stick does not pick in it)
        right(0, 0, 0); assert(!hos_vr_radial_open() && oot_vr_radial_pending == 256 + 6);   // selected: equipment 6
        oot_vr_radial_pending = -1;
        frame(LS, 0, 0); frame(0, 0, 0); assert(hos_vr_radial_open() == 1);
        right(RS, 0, 0); right(0, 0, 0); assert(hos_vr_radial_open() == 2);   // the other click: switched
        right(RS, 0, 0); right(0, 0, 0); assert(!hos_vr_radial_open() && oot_vr_radial_pending == -1);
        // the game lists again while the item menu is open: the menu keeps the list it opened on, its slots and its count
        frame(LS, 0, 0); frame(0, 0, 0); assert(hos_vr_radial_open() == 1);
        frame(0, .9f, .1f); assert(oot_vr_radial_highlight == 1);        // (slot 1 of 4: item 11)
        const int other[2] = {30, 31};
        oot_vr_radial_publish(other, 2, gear, 3);                        // (two items now, another order)
        VrRadialList seen;
        assert(oot_vr_radial_now(&seen, 0) == 0 && seen.count == 4 && seen.index[1] == 11);   // (what is drawn: the open one)
        frame(0, -.1f, -.9f); assert(oot_vr_radial_highlight == 2);      // (still four slots to pick from)
        frame(0, 0, 0); assert(!hos_vr_radial_open() && oot_vr_radial_pending == 12);   // (the slot shown, item 12, selected)
        oot_vr_radial_pending = -1;
        assert(oot_vr_radial_now(&seen, 0) == -1 && seen.count == 2 && seen.index[0] == 30);   // (closed: the new list)
        frame(LS, 0, 0); frame(0, 0, 0); assert(hos_vr_radial_open() == 1);   // (the next opening takes the new list)
        frame(0, .9f, 0); frame(0, 0, 0); assert(oot_vr_radial_pending == 31);   // (right, of two: slot 1, item 31)
        oot_vr_radial_pending = -1;
        // Menu button at Both sticks: both clicked, however briefly, opens no wheel (by default each is its own click)
        oot_vr_radial_publish(items, 4, gear, 3);
        both_sticks = 1;
        for (int last = 0; last < 2; last++) {
            frame(LS, 0, 0); frame(LS | RS, 0, 0); frame(LS | RS, 0, 0); frame(last ? LS : RS, 0, 0); frame(0, 0, 0); frame(0, 0, 0);
            assert(!hos_vr_radial_open());
        }
        frame(LS, 0, 0); frame(0, 0, 0); assert(hos_vr_radial_open() == 1);   // (one alone still does)
        frame(LS, 0, 0); frame(0, 0, 0); assert(!hos_vr_radial_open());
        both_sticks = 0;
        oot_vr_radial_publish(items, 0, gear, 3);
        frame(LS, 0, 0); frame(0, 0, 0); assert(!hos_vr_radial_open());   // nothing to show: not opened
        assert(!oot_vr_radial_lock_word);                                // (and the lock left free)
        return 0;
    }
    ''')


def check_quick_select():
    """the stick-click quick select opens, picks and closes like the game's own"""
    host = read('quick_select_host')
    start = host.index('static volatile int oot_vr_quick_open;')
    end = host.index('\n}\n', start) + 3
    compile_run(r'''
    #include <stdint.h>
    #include <string.h>
    #include <math.h>
    #include <assert.h>
    static int64_t clock_ns = 5000000000LL;
    static int64_t hos_now_ns(void) { return clock_ns; }
    static void hos_log(const char *f, ...) { (void)f; }
    typedef struct { unsigned seq; int active, mode; } HookshotVrHead;
    static int first_person = 1, option = 2;   // (Stick click menus: 2 the game's quick select; 1, the VR radial, has its own check)
    static int rx_vr_head(HookshotVrHead *h) { h->active = 1, h->mode = first_person; return 1; }
    static int rx_option(int id, int otherwise) { return id == 901 ? option : otherwise; }
    ''' + host[start:end] + r'''
    static uint64_t oot_vr_radial_controls(uint64_t b) { return b; }
    static const uint64_t LS = 1ULL << 4, L = 1ULL << 6, A = 1, UP = 1ULL << 13;
    static uint64_t frame(uint64_t b, float x, float y) {
        clock_ns += 16000000;
        const int32_t s[4] = {(int32_t)(x * 32767), (int32_t)(y * 32767), 0, 0};
        return hos_oot_vr_menu_controls(b, s);
    }
    int main(void) {
        assert(frame(LS, 0, 0) == 0 && frame(LS, 0, 0) == 0);          // the click never reaches the game
        assert(frame(0, 0, 0) == L && oot_vr_quick_open);               // let go quickly: open, the game's L held
        for (int i = 0; i < 20; i++) assert(frame(0, .1f, 0) == L);     // (small drift: still open)
        for (int i = 0; i < 20; i++) assert(frame(A, .9f, 0) == (L | A));   // a held direction moves through it
        assert(frame(0, .1f, 0) == 0 && !oot_vr_quick_open);            // let go: L let go, the game selects
        for (int i = 0; i < 40; i++) assert(frame(LS, 0, 0) == 0);      // held long (the D-pad chord's hold)
        assert(frame(0, 0, 0) == 0 && !oot_vr_quick_open);
        frame(LS, 0, 0); assert(frame(LS | UP, 0, 0) == UP); assert(frame(0, 0, 0) == 0 && !oot_vr_quick_open);   // D-pad used
        frame(LS, 0, 0); frame(0, 0, 0); assert(oot_vr_quick_open);     // open ...
        frame(LS, 0, 0); assert(frame(0, 0, 0) == 0 && !oot_vr_quick_open);   // ... a second click closes it
        frame(LS, 0, 0); frame(0, 0, 0);
        for (int i = 0; i < 1000; i++) frame(0, 0, 0);                  // 15 s: closed
        assert(!oot_vr_quick_open && frame(0, 0, 0) == 0);
        option = 0; assert(frame(LS, 0, 0) == LS && frame(0, 0, 0) == 0);    // option off: the game's stick click
        option = 1; first_person = 0; assert(frame(LS, 0, 0) == LS);          // third person: the game's
        return 0;
    }
    ''')

def check_selected_item():
    """the selected item is re-read until it resolves, and a same-slot change is noticed; the Deku Stick is told apart"""
    s=read('quick_select_host')
    a=s.index('static volatile int oot_vr_selected_bow = -1, oot_vr_selection;')
    b=s.index('\n}\n',a)+3
    compile_run(r'''
    #include <stdint.h>
    #include <string.h>
    #include <assert.h>
    #define HOS_HEAP_BASE 0x1000000000ULL
    typedef struct {uint64_t x[32];} Ctx;
    static int64_t now=1000000000;
    static int idx=-1,sel=0,calls,oot_vr_quick_open;
    static const char *name="Arrow";
    static int64_t hos_now_ns(void){return now;}
    static void hos_log(const char*f,...){(void)f;}
    static void *hos_guest_range(uint64_t a,uint64_t n){return a==0x84e98e28ULL && n==8 ? (void*)1 : 0;}
    static uint64_t recomp_load64(Ctx*c,uint64_t a){(void)c;
     if(a==0x84e9cfa8)return HOS_HEAP_BASE+0x1000;
     if(a==0x84c9e0d0)return 0x84e98e28ULL; // observed static singleton slot, below heap base
     if(a==0x84e98e28ULL)return HOS_HEAP_BASE+0x3000;
     if(a==0x84ca1140)return 0x90000000;
     assert(0);return 0;}
    static uint8_t recomp_load8(Ctx*c,uint64_t a){(void)c;
     if(a==HOS_HEAP_BASE+0x15c0)return sel;
     assert(a>=0x90001000 && a<0x90001040);return name[a-0x90001000];}
    static uint32_t recomp_load32(Ctx*c,uint64_t a){(void)c;assert(a>=0x90000250 && a<0x90000350);return 0x1000;}
    static void lookup(Ctx*c){calls++;c->x[0]=(uint64_t)(int64_t)idx;}
    typedef void (*A64Fn)(Ctx*);
    static A64Fn a64_lookup(uint64_t a){assert(a==0x8019b6e0);return lookup;}
    ''' + s[a:b] + r'''
    int main(){Ctx c={{0}};c.x[31]=0x123400;Ctx before=c;
     oot_vr_note_selected_item(&c);assert(oot_vr_selected_bow==-1 && calls==1);
     idx=5;now+=110000000;oot_vr_note_selected_item(&c);assert(oot_vr_selected_bow==1 && calls==2);
     assert(!memcmp(&before,&c,sizeof c));
     int seq=oot_vr_selection;now+=110000000;oot_vr_note_selected_item(&c);assert(oot_vr_selection==seq);
     idx=-1;now+=110000000;oot_vr_note_selected_item(&c);assert(oot_vr_selected_bow==1 && oot_vr_selection==seq);
     idx=6;name="Bomb";now+=110000000;oot_vr_note_selected_item(&c);assert(!oot_vr_selected_bow && oot_vr_selection==seq+1);
     assert(hos_vr_selected_is_bomb && !hos_vr_stick_selected());
     idx=0;name="DekuStick";now+=110000000;oot_vr_note_selected_item(&c);assert(hos_vr_stick_selected() && !hos_vr_selected_is_bomb);
     idx=6;name="Bomb";now+=110000000;oot_vr_note_selected_item(&c);assert(!hos_vr_stick_selected());
     sel=1;idx=-1;now+=110000000;oot_vr_note_selected_item(&c);assert(oot_vr_selected_bow==-1);
     return 0;}
    ''')

# ======================================================================================================================
# Link's first-person body
# ======================================================================================================================

def check_frame_body():
    """all passes of a frame share one body pose; the next frame updates it"""
    body = read('body')
    start = body.index('struct FrameBody {')
    end = body.index('\n}\n', start) + 3
    snapshot = body[start:end]
    compile_run(r'''
    #include <atomic>
    #include <cstdint>
    #include <mutex>
    #include <cassert>
    struct LinkBody { unsigned pose_seq=0; float marker=0; };
    static std::mutex cache_mu;
    static LinkBody live;
    static bool valid=true;
    static int how_now=2;   // (link_body_for_frame's how: 0 the frame's own body, 1 guessed, 2 the latest)
    static bool link_body_for_frame(uint64_t, LinkBody& out, int* how) { out=live; *how=how_now; return valid; }
    ''' + snapshot + r'''
    int main() {
        LinkBody out; live.pose_seq=10;live.marker=1;
        assert(frame_body(100,out) && out.pose_seq==10 && out.marker==1);
        live.pose_seq=11;live.marker=2;
        assert(frame_body(100,out) && out.pose_seq==10 && out.marker==1);
        assert(frame_body(101,out) && out.pose_seq==11 && out.marker==2);
        valid=false; assert(!frame_body(101,out));
        valid=true;live.pose_seq=20; assert(frame_body(108,out) && out.pose_seq==20);
        // BODY-11: a frame first posed from a guess (its shadow passes) takes its own body once its camera is seen, once
        how_now=1; live.pose_seq=30; assert(frame_body(110,out) && out.pose_seq==30);
        how_now=0; live.pose_seq=29; live.marker=9; assert(frame_body(110,out) && out.pose_seq==29 && out.marker==9);
        assert(frame_body_reposed.load()==1);
        live.pose_seq=31; assert(frame_body(110,out) && out.pose_seq==29);   // (then frozen: every later pass agrees)
        how_now=1; assert(frame_body(110,out) && out.pose_seq==29);
        // a frame whose first draw already knew its own body is never re-posed
        how_now=0; live.pose_seq=40; assert(frame_body(111,out) && out.pose_seq==40);
        live.pose_seq=41; assert(frame_body(111,out) && out.pose_seq==40 && frame_body_reposed.load()==1);
        return 0;
    }
    ''', cpp=True)

def check_body_timing():
    """each replayed frame is posed with the body the game told with that frame's camera, not a newer one (BODY-11)"""
    source = read('headset')
    start = source.index('// ---- (BODY-11) which game frame')
    end = source.index('bool link_body(LinkBody& out) {', start)
    compile_run(r'''
    #include <algorithm>
    #include <atomic>
    #include <cassert>
    #include <cstdint>
    #include <cstring>
    #include <mutex>
    struct XrPosef { float x; };
    struct LinkBody { unsigned pose_seq = 0; float root[3]{}; };
    struct BodySample { uint32_t seq = 0; uint64_t gen = 0; bool active = false, child = true; float root[3]{}, right[3]{}, back[3]{}; };
    constexpr int kBodies = 16;
    struct { std::mutex pose_mu; BodySample bodies[kBodies]; uint64_t body_gen = 0; uint64_t matched_frame = ~0ull, matched_gen = 0;
             uint32_t matched_seq = 0; std::atomic<bool> camera_owned{true}; } s;
    struct { int vr_body_latest = 0, vr_arms = 1, vr_mode = 1; } opts;
    struct { bool attached = true; } ctl;
    static bool active() { return true; }
    static void logf(const char*, ...) {}
    static uint32_t camera_seq = 0;   // (the pose a frame's camera matches: drawn_pose's answer)
    static bool drawn_pose(const float*, const float*, XrPosef*, float*, uint32_t* seq) { *seq = camera_seq; return camera_seq != 0; }
    ''' + source[start:end] + r'''
    static bool link_body_locked(const BodySample* b, LinkBody& out) {
        if (!b || !b->active || !b->seq) return false;
        out.pose_seq = b->seq, out.root[0] = b->root[0];
        return true;
    }
    static void game_frame(uint32_t seq, float root, bool active = true) {   // (as hookshot_vr_body: one per camera update)
        const uint64_t gen = ++s.body_gen;
        BodySample& b = s.bodies[gen % kBodies];
        b = BodySample{}; b.seq = seq, b.gen = gen, b.active = active, b.root[0] = root;
    }
    static float body_of(uint64_t frame, int want_how) {
        LinkBody out; int how = -1;
        assert(link_body_for_frame(frame, out, &how));
        assert(how == want_how);
        return out.root[0];
    }
    int main() {
        const float vp[16]{}, proj[16]{};
        for (int k = 1; k <= 5; k++) game_frame(10 + 3 * k, (float)k);   // (bodies 1..5, poses 13..25)
        assert(body_of(100, 2) == 5);                 // no frame matched yet: the newest, as before
        camera_seq = 19; note_frame_camera(100, vp, proj);   // frame 100 was drawn with pose 19: body 3, two behind the newest
        assert(body_of(100, 0) == 3);
        assert(timing_lag[2].load() == 1);
        assert(body_of(101, 1) == 4);                 // frame 101's first draws (its shadows): the next body, a guess
        camera_seq = 22; note_frame_camera(101, vp, proj);
        assert(body_of(101, 0) == 4);
        assert(body_of(103, 1) == 5);                 // a guess never goes past the newest body
        assert(body_of(101 + 9, 2) == 5);             // long after the last match (a load): the newest
        camera_seq = 99; note_frame_camera(102, vp, proj);   // a camera matching no body told: nothing learned
        assert(s.matched_frame == 101);
        opts.vr_body_latest = 1; assert(body_of(101, 2) == 5); opts.vr_body_latest = 0;   // Debug tab: Latest
        game_frame(0, 0, false);                      // a frame with no first-person body (a cutscene)
        camera_seq = 25; note_frame_camera(104, vp, proj);
        assert(body_of(104, 0) == 5);                 // its own body, though the newest has none
        LinkBody out; int how;
        assert(!link_body_for_frame(105, out, &how)); // the guess is the body-less frame: no pose
        opts.vr_mode = 0; assert(!link_body_for_frame(104, out, &how)); opts.vr_mode = 1;   // third person: never
        return 0;
    }
    ''', cpp=True)


def check_link_shadow():
    """first person leaves Link's own posed draws out of shadow passes only: never the scene's depth or colour (Link's shadow)"""
    source = read('skin_draw')
    start = source.index("// (OoT VR) Link's own shadow in first person")
    compile_run(r'''
    #include <cassert>
    #include <cstdint>
    #include <memory>
    #include <mutex>
    #include <unordered_set>
    #include <vector>
    typedef int VkFormat;
    struct Texture { uint32_t width, height; };
    struct Targets { int n = 0; Texture* color[8]{}; Texture* depth = nullptr; VkFormat depth_format = 126; uint32_t width = 0, height = 0; };
    struct NvnState {}; struct Program {}; struct DrawArgs {};
    struct { int vr_own_shadow = 0; } opts;
    struct { uint64_t addr = 0; std::shared_ptr<const std::vector<float>> palette; } vr_bones;
    static void logf(const char*, ...) {}
    static uint64_t texture_frame_no() { return 7; }
    namespace vr {
    enum class LinkDraw { Posed, RecentPose, Equipment, Refused, NearNative, ShadowHidden, HeadHidden, Count };
    static Texture* scene = nullptr;
    static bool scene_depth_known() { return scene != nullptr; }
    static bool scene_depth_is(const Texture* d) { return d == scene; }
    static LinkDraw noted = LinkDraw::Count;
    static void note_link_draw(LinkDraw w, bool, uint64_t) { noted = w; }
    }
    static vr::LinkDraw pick_what = vr::LinkDraw::Posed;   // (what vr_link_skin_pick made of the draw)
    static bool pick_skip = false;
    static bool vr_link_skin_pick(NvnState&, const Program&, const DrawArgs&, const Targets&, vr::LinkDraw* what) {
        *what = pick_what;
        if (!pick_skip && (pick_what == vr::LinkDraw::Posed || pick_what == vr::LinkDraw::Equipment || pick_what == vr::LinkDraw::RecentPose))
            vr_bones.palette = std::make_shared<std::vector<float>>(12, 1.f), vr_bones.addr = 1;
        return pick_skip;
    }
    ''' + source[start:] + r'''
    static bool left_out(Targets at) {
        NvnState s; Program p; DrawArgs d;
        vr_bones = {};
        return vr_link_skin(s, p, d, at);
    }
    int main() {
        Texture scene_depth{2880, 2304}, shadow_map{1600, 1600}, square_scene{2048, 2048}, colour{2880, 2304};
        vr::scene = &scene_depth;
        Targets shadow; shadow.depth = &shadow_map, shadow.width = shadow.height = 1600;
        Targets prepass; prepass.depth = &scene_depth, prepass.width = 2880, prepass.height = 2304;
        Targets scene_pass = prepass; scene_pass.n = 1, scene_pass.color[0] = &colour;
        assert(left_out(shadow) && vr::noted == vr::LinkDraw::ShadowHidden && !vr_bones.palette);   // his shadow: left out
        assert(!left_out(prepass) && vr_bones.palette);           // the scene's depth prepass keeps him (and his VR pose)
        assert(!left_out(scene_pass) && vr::noted == vr::LinkDraw::Posed);
        Targets level = shadow; level.width = level.height = 115;   // (a level of a shadow map, as the headset log had)
        assert(left_out(level));
        vr::scene = &square_scene;                                // a square scene: its own depth is still never a shadow
        Targets square_prepass; square_prepass.depth = &square_scene, square_prepass.width = square_prepass.height = 2048;
        assert(!left_out(square_prepass) && left_out(shadow));
        vr::scene = nullptr; assert(!left_out(shadow));           // the scene's depth unknown: shown (the safe side)
        vr::scene = &scene_depth;
        pick_what = vr::LinkDraw::Equipment; assert(left_out(shadow));   // the gear posed with him: left out too
        pick_what = vr::LinkDraw::Refused; assert(!left_out(shadow));    // not posed (the game's pose): untouched
        pick_what = vr::LinkDraw::Count; assert(!left_out(shadow));      // not Link's (or third person): untouched
        pick_what = vr::LinkDraw::HeadHidden; pick_skip = true; assert(left_out(scene_pass));   // already left out everywhere
        pick_skip = false; pick_what = vr::LinkDraw::Posed;
        opts.vr_own_shadow = 1; assert(!left_out(shadow) && vr_bones.palette);   // Link's shadow (first person): Shown
        return 0;
    }
    ''', cpp=True)


def check_camera_fade():
    """first person keeps the game's camera fade off Link himself, and gives it back after (BODY-13)"""
    source = read('head_camera_host')
    start = source.index('/* (OoT VR, BODY-13)')
    compile_run(r'''
    #include <assert.h>
    #include <math.h>
    #include <stdint.h>
    #include <stdlib.h>
    #include <string.h>
    #define HOS_HEAP_BASE 0x1000000000ULL
    typedef struct { uint64_t x[32]; } Ctx;
    static unsigned char memory[0x10000];
    static void *hos_guest_range(uint64_t a, uint64_t n) {
        return a >= HOS_HEAP_BASE && a - HOS_HEAP_BASE <= sizeof memory && n <= sizeof memory - (a - HOS_HEAP_BASE) ? memory + (a - HOS_HEAP_BASE) : 0;
    }
    static uint64_t rd(uint64_t a, int n) { uint64_t v = 0; void *p = hos_guest_range(a, n); assert(p); memcpy(&v, p, n); return v; }
    static void put(uint64_t a, uint64_t v, int n) { void *p = hos_guest_range(a, n); assert(p); memcpy(p, &v, n); }
    static uint64_t recomp_load64(Ctx *c, uint64_t a) { (void)c; return rd(a, 8); }
    static uint32_t recomp_load32(Ctx *c, uint64_t a) { (void)c; return (uint32_t)rd(a, 4); }
    static uint8_t recomp_load8(Ctx *c, uint64_t a) { (void)c; return (uint8_t)rd(a, 1); }
    static void recomp_store8(Ctx *c, uint64_t a, uint64_t v) { (void)c; put(a, v, 1); }
    static void recomp_store32(Ctx *c, uint64_t a, uint64_t v) { (void)c; put(a, v, 4); }
    static int lines;
    static void hos_log(const char *f, ...) { (void)f; lines++; }
    static int64_t clock_ns;
    static int64_t hos_now_ns(void) { return clock_ns += 3000000000LL; }
    static int keep_off = 1, driving = 1;
    int rx_option(int id, int d) { return id == 920 ? keep_off : d; }
    int hos_oot_vr_first_person(void) { return driving; }
    #define B HOS_HEAP_BASE
    static uint64_t oot_vr_player_actor = B + 0x1000;
    ''' + source[start:] + r'''
    static const uint64_t table = B + 0x2000, component = B + 0x3000;
    static float alpha(void) { uint32_t b = (uint32_t)rd(component + 0x44, 4); float f; memcpy(&f, &b, 4); return f; }
    static void set_alpha(float f) { uint32_t b; memcpy(&b, &f, 4); put(component + 0x44, b, 4); }
    int main(void) {
        Ctx c = {{0}};
        put(oot_vr_player_actor + 0x270, table, 8), put(table + 0x108, component, 8);
        put(component + 0x48, 0x81, 1), set_alpha(0.25f);              // the game: switch on, Link faded to a quarter
        oot_vr_camera_fade(&c, 0);                                     // third person: untouched
        assert(rd(component + 0x48, 1) == 0x81 && alpha() == 0.25f);
        oot_vr_camera_fade(&c, 1);                                     // first person: switch off, Link opaque
        assert(rd(component + 0x48, 1) == 0x80 && alpha() == 1.0f);
        set_alpha(0.5f), put(component + 0x48, 0x81, 1);               // the game fades him again (an event's one-shot)
        oot_vr_camera_fade(&c, 1);
        assert(rd(component + 0x48, 1) == 0x80 && alpha() == 1.0f);
        oot_vr_camera_fade(&c, 0);                                     // back to third person: the game's switch put back
        assert(rd(component + 0x48, 1) == 0x81);
        set_alpha(0.3f); oot_vr_camera_fade(&c, 0); assert(alpha() == 0.3f);   // and never written there
        keep_off = 0; oot_vr_camera_fade(&c, 1);                       // the row at Game: the game's fade, untouched
        assert(rd(component + 0x48, 1) == 0x81 && alpha() == 0.3f);
        keep_off = 1; driving = 0; oot_vr_camera_fade(&c, 1);       // first person, an event as a flat screen: the game's
        assert(rd(component + 0x48, 1) == 0x81 && alpha() == 0.3f); driving = 1;
        put(component + 0x44, 0x7fc00000, 4);           // not an alpha (NaN): an unknown layout, left alone
        oot_vr_camera_fade(&c, 1); assert(rd(component + 0x48, 1) == 0x81 && rd(component + 0x44, 4) == 0x7fc00000);
        set_alpha(0.6f); oot_vr_camera_fade(&c, 1); assert(rd(component + 0x48, 1) == 0x80);
        put(table + 0x108, 0, 8); oot_vr_camera_fade(&c, 1);           // Link without the component (another actor): nothing
        put(table + 0x108, component, 8);
        assert(rd(component + 0x48, 1) == 0x80);                       // (the old one is not written once it is gone)
        oot_vr_camera_fade(&c, 1); oot_vr_camera_fade(&c, 0); assert(rd(component + 0x48, 1) == 0x80 || rd(component + 0x48, 1) == 0x81);
        assert(lines > 0);
        return 0;
    }
    ''')


def check_link_blink():
    """a frame without Link's posed colour after one with it gets a 'Link blink' line naming why; the 10 s counts add up (BODY-12)"""
    source = read('body')
    start = source.index('// ---- (BODY-12) what became of')
    end = source.index('\n}\n', source.index('void body_draw_report() {', start)) + 3
    compile_run(r'''
    #include <algorithm>
    #include <atomic>
    #include <cassert>
    #include <chrono>
    #include <cstdarg>
    #include <cstdint>
    #include <cstdio>
    #include <cstring>
    #include <mutex>
    #include <string>
    enum class LinkDraw { Posed, RecentPose, Equipment, Refused, NearNative, ShadowHidden, HeadHidden, Count };
    static std::atomic<int64_t> frame_body_reposed{0};
    static std::string said;
    static int lines;
    static void logf(const char* f, ...) { char b[2048]; va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a); said = b; lines++; }
    ''' + source[start:end] + r'''
    static void frame(uint64_t n, LinkDraw w, bool colour) { note_link_draw(w, colour, n); }
    int main() {
        for (uint64_t f = 1; f <= 3; f++) frame(f, LinkDraw::Posed, true), frame(f, LinkDraw::Posed, false);
        frame(4, LinkDraw::Posed, false);             // frame 4: his depth/shadow, none of his colour
        frame(5, LinkDraw::Posed, true);              // (frame 4 is counted when frame 5 begins)
        assert(lines == 1 && said.find("Link blink: frame 4 drew none of his colour") != std::string::npos);
        frame(6, LinkDraw::Refused, true);            // frame 6: his palette refused (drawn in the game's pose)
        frame(7, LinkDraw::Posed, true);
        assert(lines == 1);                           // (at most a line a second: counted, said with the next)
        frame(10, LinkDraw::Posed, true);             // frames 8 and 9: no draw of him at all
        frame(11, LinkDraw::Posed, true);
        body_draw_report();
        assert(said.find("Link's draws (10 s): 8 frames") != std::string::npos);
        assert(said.find("missing in 2 (2 runs, the longest 1 frames), 2 frames with no draw of him") != std::string::npos);
        assert(said.find("refused palettes 1 + 0") != std::string::npos);
        const int before = lines; body_draw_report(); assert(lines == before);   // nothing since: no line
        return 0;
    }
    ''', cpp=True)


def check_canonical_rig():
    """one body pose per pass: an equipment draw first must not make a second pose"""
    source=read('body')
    start=source.index('Entry* canonical_rig(uint64_t frame, const LinkBody& body) {')
    end=source.index('\n}\n',start)+3
    fixture=r'''
    #include <cstdint>
    #include <memory>
    #include <vector>
    #include <cassert>
    struct V3 {float x,y,z;};
    static V3 v3(const float*p){return {p[0],p[1],p[2]};}
    struct LinkBody {bool child=false;unsigned pose_seq=2;float root[3]{};};
    namespace rig {struct Model{int neck=0;};static Model adult,child;}
    struct Entry {uint64_t addr=0;std::vector<float>raw;std::shared_ptr<const std::vector<float>>out;
    uint64_t used=0;V3 origin{};bool adult=false;uint64_t frame=0;uint32_t pose_seq=0;};
    static Entry cache[32];static uint64_t cache_clock=1;static int poses=0;
    static float neck_error(const float*,const rig::Model&,V3,V3){return 0;}
    static bool pose(float*out,const rig::Model&,const LinkBody&body,V3){out[0]=body.pose_seq;poses++;return true;}
    '''
    checks=r'''
    int main(){
        cache[0].adult=true;cache[0].frame=10;cache[0].pose_seq=1;cache[0].used=1;
        cache[0].raw.resize(12);cache[0].out=std::make_shared<std::vector<float>>(12,1);
        LinkBody body;
        Entry* first=canonical_rig(11,body);assert(first && first->frame==11 && first->out->at(0)==2 && poses==1);
        auto palette=first->out;
        for(int i=0;i<20;i++) assert(canonical_rig(11,body)->out==palette && poses==1);
        body.pose_seq=3;assert(canonical_rig(12,body)->out->at(0)==3 && poses==2);
        assert(!canonical_rig(15,body));
        body.child=true;assert(!canonical_rig(12,body));
        // Regression: a canonical frame is NOT proof of fresh native raw animation.
        // An earlier synthetic VR entry may win over a later actual native upload.
        for(auto& e:cache)e=Entry{};
        body.child=false;body.pose_seq=4;
        cache[0].adult=true;cache[0].frame=20;cache[0].used=1;
        cache[0].raw.resize(12,1);cache[0].out=std::make_shared<std::vector<float>>(12,1);
        Entry* synthetic=canonical_rig(21,body);assert(synthetic && synthetic->raw[0]==1);
        cache[2].adult=true;cache[2].frame=21;cache[2].used=100;
        cache[2].raw.resize(12,2);cache[2].out=synthetic->out;cache[2].pose_seq=4;
        body.pose_seq=5;
        Entry* next=canonical_rig(22,body);assert(next && next->frame==22 && next->raw[0]==1);
        // Arrow ownership therefore uses the separately aged actual-upload hand history.
    }
    '''
    compile_run(fixture+source[start:end]+checks,cpp=True)

def check_arm_reach():
    """a tracked hand past Link's reach (his shoulders dropped in a roll or on a slope) is still reached: the arm stretches;
    a held bomb's fingers rest on it, never through it, and ignore the buttons"""
    src = read('body')
    math = src[src.index('constexpr float kPi'):src.index('// ---- body state kept between frames')]
    arm = src[src.index('enum class Grip {'):src.index('bool pose(float* p, const rig::Model& M')]
    compile_run(r'''
    #include <algorithm>
    #include <cassert>
    #include <cmath>
    #include <cstdint>
    #include <cstring>
    #include <vector>
    #include "''' + str(generated('vk_vr_rig_data.h')) + r'''"
    namespace rig = mkwvk::vr::rig;
    ''' + math + arm + r'''
    int main() {
        const rig::Model& M = rig::adult;
        const rig::Arm& A = M.arm[1];
        const V3 R{1, 0, 0}, F{0, 0, 1};
        const auto wrist_after = [&](V3 target, float k) {
            std::vector<float> p(M.slots * 12, 0.0f);
            for (int i = 0; i < M.slots; i++) p[i * 12] = p[i * 12 + 5] = p[i * 12 + 10] = 1;   // (the bind pose)
            Hand h{};
            h.tracked = true, h.wrist = target, h.f = {0, 0, 1}, h.a = {0, 1, 0}, h.c = cross(h.f, h.a);
            solve_arm(p.data(), A, identity(), {0, 0, 0}, 1, h, R, F, k);
            return point(p.data() + A.wrist * 12, v3(A.wrist_pos));
        };
        const V3 sh = v3(A.shoulder);
        const float reach = length(sub(v3(A.elbow_pos), sh)) + length(sub(v3(A.wrist_pos), v3(A.elbow_pos)));
        for (float k : {1.0f, 1.3f}) {
            // within reach, just past it, and (dropped shoulders) twice the arm away: the hand is where the controller is
            for (float far : {.7f, 1.05f, 2.0f}) {
                const V3 target = add(sh, mul(unit({.3f, -.2f, 1}, F), reach * far));
                const float miss = length(sub(wrist_after(target, k), target));
                assert(miss < .01f);
            }
        }
        // past the most stretch (a tracking error): the hand stops on the line to the controller, not further
        const V3 dir = unit({.3f, -.2f, 1}, F);
        const V3 got = wrist_after(add(sh, mul(dir, reach * 4)), 1);
        assert(std::fabs(length(sub(got, sh)) - reach * kMaxArmStretch) < .02f);
        // a held bomb (Grip::ball): whatever the buttons, every finger joint ends outside the ball (never through it), and
        // the same ball gives the same pose
        for (float radius : {.11f, .22f, .33f})
            for (int side = 0; side < 2; side++) {
                const rig::Arm& B = M.arm[side];
                std::vector<float> pose[2];
                for (int run = 0; run < 2; run++) {
                    std::vector<float>& p = pose[run];
                    p.assign(M.slots * 12, 0.0f);
                    for (int i = 0; i < M.slots; i++) p[i * 12] = p[i * 12 + 5] = p[i * 12 + 10] = 1;
                    Hand h{};
                    h.tracked = true, h.f = {0, 0, 1}, h.a = {0, 1, 0}, h.c = cross(h.f, h.a);
                    h.wrist = add(v3(B.shoulder), {side ? .1f : -.1f, -.3f, .35f});
                    h.trigger = run ? 1.0f : 0.0f, h.squeeze = 1;
                    h.grip = Grip::ball, h.ball_radius = radius;
                    const V3 palm = side == 0 ? h.c : mul(h.c, -1.0f);   // (as solve_arm's curl: the palm's way)
                    h.ball = add(add(h.wrist, mul(h.f, .075f)), mul(palm, .03f + radius));
                    solve_arm(p.data(), B, identity(), {0, 0, 0}, side, h, R, F, 1);
                    for (int f = 0; f < 5; f++)
                        for (int k = 0; k < 3; k++) {
                            const rig::Segment& g = B.finger[f][k];
                            if (g.slot < 0) continue;
                            assert(length(sub(point(p.data() + g.slot * 12, v3(g.joint)), h.ball)) > radius);
                        }
                }
                for (size_t i = 0; i < pose[0].size(); i++) assert(std::fabs(pose[0][i] - pose[1][i]) < 1e-5f);
            }
    }
    ''', cpp=True)


def check_gait():
    """first-person legs step the way Link travels: in each of the eight directions (and walking) at the clip's own pace,
    the planted foot stays where it was put on the ground while the body moves on (a reversed direction would slide it at
    twice the speed); the legs keep their length through every blend; and turning the travel direction degree by degree
    never makes the feet jump"""
    src = read('body')
    math = src[src.index('constexpr float kPi'):src.index('// ---- body state kept between frames')]
    gait = (REPO / 'vr/body/gait.inc').read_text(encoding='utf-8')
    compile_run(r'''
    #include <algorithm>
    #include <cassert>
    #include <cmath>
    #include <cstdint>
    #include <cstdio>
    #include <cstring>
    #include "''' + str(generated('vk_vr_gait.h')) + r'''"
    namespace rig = mkwvk::vr::rig;
    ''' + math + gait + r'''
    static float leg(const GaitPose& p, int side) {   // hip (waist) to ankle, through the knee's bone chain
        return length(sub(p.ankle[side], p.waist));
    }
    int main() {
        GaitPose p;
        const float rad[8] = {0, 45, 90, 135, 180, -135, -90, -45};
        for (int walking = 0; walking < 2; walking++)
            for (float deg : rad) {
                if (walking && std::fabs(std::fmod(deg, 90.0f)) > 1) continue;   // (walking has the four main directions)
                const float a = deg * kDeg;
                GaitState g;
                gait_weights(g, std::sin(a), std::cos(a));
                // the blend's own speed: its stride times its pace, so the phase is not held back by kGaitMaxPace
                const float speed = walking ? 1.0f : 3.4f;
                const float left = std::sin(a) * speed, ahead = std::cos(a) * speed;
                const float dt = 1 / 90.0f;
                V3 body{0, 0, 0};
                float worst = 0, slid = 0, planted_time = 0, low = 1e9f;
                for (int pass = 0; pass < 2; pass++)
                    for (int i = 0; i < 400; i++) {
                        gait_step(g, left, ahead, dt);
                        gait_pose(g, p);
                        body = add(body, {left * dt, 0, ahead * dt});
                        static V3 was[2];
                        for (int side = 0; side < 2; side++) {
                            const V3 at = add(body, p.ankle[side]);
                            if (pass == 0) low = std::min(low, p.ankle[side].y);
                            // planted: the ankle at its lowest (within 1 cm)
                            else if (i > 0 && p.ankle[side].y < low + .01f) {
                                slid += length(flat(sub(at, was[side])));
                                planted_time += dt;
                            }
                            was[side] = at;
                        }
                    }
                // how fast a planted foot moves over the ground, against the body's speed: a foot stepping the right way
                // stays nearly put; the wrong way it would move at about twice the speed
                worst = planted_time > 0 ? slid / planted_time / speed : 1;
                std::printf("gait %s %+5.0f deg: planted foot slides at %.0f%% of the speed\n", walking ? "walk" : "run ",
                            deg, worst * 100);
                assert(planted_time > .2f);
                assert(worst < .35f);
            }
        // no blend stretches a leg: hip to ankle never longer (by 2 %) than the game's own clips ever hold it
        float straight[2] = {0, 0};
        for (int c = 0; c < rig::kGaitClips; c++)
            for (int k = 0; k < 40; k++) {
                GaitState g;
                g.uses = 1, g.use[0] = {c, 1}, g.phase = k / 40.0f;
                gait_pose(g, p);
                for (int side = 0; side < 2; side++) straight[side] = std::max(straight[side], leg(p, side));
            }
        float longest = 0;
        for (float speed : {.8f, 2.0f, 3.5f, 5.3f})
            for (int deg = -180; deg < 180; deg += 15)
                for (int k = 0; k < 20; k++) {
                    GaitState g;
                    gait_weights(g, std::sin(deg * kDeg) * speed, std::cos(deg * kDeg) * speed);
                    g.phase = k / 20.0f;
                    gait_pose(g, p);
                    for (int side = 0; side < 2; side++) longest = std::max(longest, leg(p, side) / straight[side]);
                }
        std::printf("gait: longest blended leg %.1f%% of the clips' longest\n", longest * 100);
        assert(longest < 1.02f);
        // turning the travel direction a degree at a time at a run moves no foot more than 3 cm
        float jump = 0;
        V3 before[2]{};
        for (int deg = -180; deg <= 180; deg++) {
            GaitState g;
            g.phase = .3f;
            gait_weights(g, std::sin(deg * kDeg) * 4, std::cos(deg * kDeg) * 4);
            gait_pose(g, p);
            if (deg > -180)
                for (int side = 0; side < 2; side++) jump = std::max(jump, length(sub(p.ankle[side], before[side])));
            before[0] = p.ankle[0], before[1] = p.ankle[1];
        }
        std::printf("gait: largest step between directions 1 degree apart %.1f cm\n", jump * 100);
        assert(jump < .03f);
        // on the ground: every cycle played at its own pace keeps a foot down often enough (its flight between strides is
        // not a jump), while a jump stops the stepping legs within kGaitAirborne and keeps them stopped until kGaitLanded
        // after the landing (the first build's 0.3 s grace stepped through half a jump: a leg stuck up in the air)
        const char* why = "";
        for (int c = 0; c < rig::kGaitClips; c++) {
            if (c == rig::gait_tuck) continue;   // (the held jump frame is in the air by design)
            GaitState g;
            g.uses = 1, g.use[0] = {c, 1};
            GaitGround ground;
            const float dt = 1 / 90.0f, cycle = rig::gait_clips[c].frames / 60.0f;
            for (int i = 0; i < 540; i++) {
                g.phase = std::fmod(i * dt / cycle, 1.0f);
                gait_pose(g, p);
                const bool down = gait_on_ground(ground, p.waist.y - std::min(p.ankle[0].y, p.ankle[1].y), 0, 0, dt, &why);
                if (i * dt > kGaitLanded + dt) assert(down);
            }
        }
        {
            GaitGround ground;
            const float dt = 1 / 90.0f;
            for (int i = 0; i < 90; i++) {   // (landed counts this frame too; the frame on the boundary is left to rounding)
                const bool down = gait_on_ground(ground, .75f, 0, 0, dt, &why);
                const float t = (i + 1) * dt;
                if (std::fabs(t - kGaitLanded) > dt / 2) assert(down == (t > kGaitLanded));
            }
            int off_after = -1, on_after = -1;
            for (int i = 0; i < 54; i++)   // (0.6 s in the air, feet tucked)
                if (!gait_on_ground(ground, .4f, 0, 0, dt, &why) && off_after < 0) off_after = i;
            for (int i = 0; i < 90; i++)
                if (gait_on_ground(ground, .75f, 0, 0, dt, &why) && on_after < 0) on_after = i;
            assert(off_after >= 0 && off_after * dt <= kGaitAirborne + dt);
            assert(on_after >= 0 && on_after * dt >= kGaitLanded - dt);
            // a roll (the pelvis turned over) or a fast rise is never stepping, feet down or not
            assert(!gait_on_ground(ground, .75f, 0, 60 * kDeg, dt, &why) && !gait_on_ground(ground, .75f, 4, 0, dt, &why));
            // mid-flip, one frame that looks upright does not let the legs step again: kGaitLanded of strides first
            for (int i = 0; i < 40; i++) gait_on_ground(ground, .75f, 0, 0, dt, &why);
            assert(gait_on_ground(ground, .75f, 0, 0, dt, &why));
            assert(!gait_on_ground(ground, .75f, 0, 170 * kDeg, dt, &why));
            assert(!gait_on_ground(ground, .75f, 0, 4 * kDeg, dt, &why));
            assert(!gait_on_ground(ground, .75f, 0, 173 * kDeg, dt, &why));
            // running down a slope at the game's full run (5.3 m/s, 45 degrees: 3.7 m/s down) keeps stepping; a fall does not
            // (after the jump just above: back on the ground first)
            for (int i = 0; i < 40; i++) gait_on_ground(ground, .75f, -3.7f, 0, dt, &why);
            assert(gait_on_ground(ground, .75f, -3.7f, 0, dt, &why) && !gait_on_ground(ground, .75f, -6, 0, dt, &why));
        }
        // every slot's palette matrix is finite
        for (int i = 0; i < rig::kGaitSlots; i++)
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++) assert(std::isfinite(p.slot[i].r.v[r][c]));
    }
    ''', cpp=True)


def check_arrow_effect_camera():
    """a held arrow's effect draw is moved through its particle camera (arrow_effect_math.h): any point seen through the moved
    camera lands where the original sees the moved point, with a scaled bow too; the eye moves by the inverse; only a
    perspective camera in that layout is taken; and an effect matrix counts on the arrow, never at the camera"""
    compile_run(r'''
    #include <cassert>
    #include <cmath>
    #include <initializer_list>
    #include "''' + str(REPO / 'vr/items/bow/arrow_effect_math.h') + r'''"
    using namespace mkwvk::vr::arrow_fx;
    static void apply(const float* m, const float p[3], float out[4]) {   // (column-major 4x4 times (p, 1))
        for (int r = 0; r < 4; r++) out[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r];
    }
    int main() {
        float cam[0x200 / 4] = {};
        // a view: turned 30 degrees about y, the eye at (2, 1.5, -3): view = R^T (p - eye)
        const float c = std::cos(.5236f), s = std::sin(.5236f), eye[3] = {2, 1.5f, -3};
        float* v = cam + kView;
        const float R[9] = {c, 0, -s, 0, 1, 0, s, 0, c};   // (rows: the camera's axes in world)
        for (int r = 0; r < 3; r++) {
            for (int k = 0; k < 3; k++) v[k * 4 + r] = R[r * 3 + k];
            v[12 + r] = -(R[r * 3] * eye[0] + R[r * 3 + 1] * eye[1] + R[r * 3 + 2] * eye[2]);
        }
        v[15] = 1;
        float* pr = cam + kProjection;   // (a perspective projection, column-major: last row 0 0 -1 0)
        pr[0] = 1.2f, pr[5] = 1.6f, pr[10] = -1.0f, pr[11] = -1, pr[14] = -.2f;
        for (int k = 0; k < 3; k++) cam[k] = eye[k];   // (the eye at 0)
        cam[4] = R[6] * -1, cam[5] = R[7] * -1, cam[6] = R[8] * -1;   // (its forward at 0x10: minus the camera's z axis)
        Camera info;
        assert(inspect_camera(cam, sizeof cam, info) && info.position_at_zero);
        for (int k = 0; k < 3; k++) assert(std::fabs(info.eye[k] - eye[k]) < 1e-4f);
        // the move: a scaled (1.3) turn about z and a shift (rows, 3x4)
        const float sc = 1.3f, cz = std::cos(.4f), sz = std::sin(.4f);
        const float move[12] = {sc * cz, -sc * sz, 0, .7f, sc * sz, sc * cz, 0, -.2f, 0, 0, sc, .45f};
        float before[16];
        for (int i = 0; i < 16; i++) before[i] = v[i];
        assert(transform_camera(cam, sizeof cam, move, info));
        const float p[3] = {.3f, -.4f, 1.1f};
        float moved_p[3];
        for (int r = 0; r < 3; r++) moved_p[r] = move[r * 4] * p[0] + move[r * 4 + 1] * p[1] + move[r * 4 + 2] * p[2] + move[r * 4 + 3];
        float a[4], b[4];
        apply(v, p, a);
        apply(before, moved_p, b);
        for (int k = 0; k < 4; k++) assert(std::fabs(a[k] - b[k]) < 1e-4f);
        // the eye: D^-1 eye, so the moved camera sees the moved world from where it stood
        float e2[3];
        for (int r = 0; r < 3; r++) e2[r] = move[r * 4] * cam[0] + move[r * 4 + 1] * cam[1] + move[r * 4 + 2] * cam[2] + move[r * 4 + 3];
        for (int k = 0; k < 3; k++) assert(std::fabs(e2[k] - eye[k]) < 1e-4f);
        // the forward at 0x10: still minus the moved view's z axis (its third row), so "in front" means the same to both
        {
            const float z[3] = {v[2], v[6], v[10]};
            const float zn = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
            for (int k = 0; k < 3; k++) assert(std::fabs(cam[4 + k] + z[k] / zn) < 1e-4f);
        }
        // the near-fade stretch: the view depth is 10 times larger, the clip position exactly the same
        {
            float stretched[0x200 / 4];
            for (int i = 0; i < 0x200 / 4; i++) stretched[i] = cam[i];
            stretch_view_depth(stretched, sizeof stretched, kNearFadeStretch);
            float vz[4], vz2[4], clip[4], clip2[4], vv[4], vv2[4];
            apply(cam + kView, p, vv);
            apply(stretched + kView, p, vv2);
            assert(std::fabs(vv2[2] - vv[2] * kNearFadeStretch) < 1e-3f && std::fabs(vv2[0] - vv[0]) < 1e-5f);
            const float v3[3] = {vv[0], vv[1], vv[2]}, w3[3] = {vv2[0], vv2[1], vv2[2]};
            apply(cam + kProjection, v3, clip);
            apply(stretched + kProjection, w3, clip2);
            for (int k = 0; k < 4; k++) assert(std::fabs(clip[k] - clip2[k]) < 1e-4f);
            (void)vz, (void)vz2;
        }
        // Elemental arrow effects: the stretch puts the fade (clamp(0.4 * depth - 1, 0, 1)) at the opacity asked for
        for (float depth : {.4f, .6f, .9f})
            for (float o : {.1f, .5f, .9f}) {
                const float k = fade_stretch(depth, o);
                assert(std::fabs(std::fmin(std::fmax(.4f * k * depth - 1, 0.0f), 1.0f) - o) < 1e-4f);
            }
        assert(fade_stretch(.6f, 1) == kNearFadeStretch && .4f * kNearFadeStretch * .3f - 1 >= 0.2f);
        // not a perspective camera: refused
        float flat[0x200 / 4] = {};
        for (int i = 0; i < 16; i++) flat[kView + i] = before[i];
        flat[kProjection] = flat[kProjection + 5] = flat[kProjection + 10] = flat[kProjection + 15] = 1;
        assert(!inspect_camera(flat, sizeof flat, info));
        // an effect matrix 0.4 m from the game's arrow counts, scale 1 included; at the camera, or 2 m off, it does not
        const float arrow[3] = {25.3f, 104.3f, -22.9f}, head[3] = {25.6f, 104.8f, -22.7f};
        float fx[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 25.5f, 104.6f, -22.8f, 1};
        assert(source_matches(fx, arrow, head));
        fx[12] = head[0], fx[13] = head[1], fx[14] = head[2];
        assert(!source_matches(fx, arrow, head));
        fx[12] = 27.3f, fx[13] = 104.3f, fx[14] = -22.9f;
        assert(!source_matches(fx, arrow, head));
        return 0;
    }
    ''', cpp=True)


def check_armor_attachment():
    """Goron/Zora armour follows the body wherever the origin floats"""
    s=read('body')
    math=s[s.index('constexpr float kPi'):s.index('M3 axis_angle')]
    start=s.index('            for(unsigned i=0;i<bytes/48;i++) {',s.index('if (original_body)'))
    end=s.index('            return out;',start)
    loop=s[start:end]
    shared=s[s.index('        static const int shared_fallback[48]'):s.index('        const Entry* original_body')]
    fixture=r'''
    #include <algorithm>
    #include <cmath>
    #include <cstdint>
    #include <vector>
    #include <memory>
    #include <cassert>
    '''+math+r'''
    struct Entry{std::vector<float>raw;V3 origin;};
    static std::shared_ptr<std::vector<float>> check(bool gold) {
        Entry old,now;old.raw.resize(288*12);now.origin={100,200,300};old.origin={0,0,0};
        std::vector<float> target(288*12);
        for(int s=0;s<288;s++) {
            float* a=old.raw.data()+s*12;float* b=target.data()+s*12;
            a[0]=a[5]=a[10]=b[0]=b[5]=b[10]=1;
            a[3]=5;a[7]=6;a[11]=7;b[3]=-95;b[7]=-194;b[11]=-293;
        }
        const Entry* original_body=&old;const Entry* candidate=&now;
        const uint64_t bytes=(gold?52:50)*48;
        auto out=std::make_shared<std::vector<float>>(bytes/4);
        for(unsigned i=0;i<bytes/48;i++) {
            float*m=out->data()+i*12;m[0]=m[5]=m[10]=1;m[3]=.03f*i;m[7]=.17f;m[11]=-.09f;
        }
        const auto original=*out;
        float o[3]={10,20,30};const V3 origin_delta=sub(v3(o),old.origin);
    '''+shared+r'''
        const auto body_slot = [&](unsigned i) {
            const int rigid_gold[4]={7,9,48,50};
            return i<48?shared_fallback[i]:gold?rigid_gold[i-48]:i==48?9:50;
        };
    '''+loop+r'''
        for(unsigned i=0;i<original.size();i++) assert(std::fabs(original[i]-(*out)[i])<.0001f);
        return out;
    }
    int main(){assert(check(true));assert(check(false));}
    '''
    compile_run(fixture,cpp=True)

def check_tunic_attachment():
    """tunic uploads consistent with the game's body get the VR body's pose"""
    src = read('body')
    math = src[src.index('constexpr float kPi'):src.index('// ---- body state kept between frames')]
    tunic = src[src.index('const rig::Tunic* tunic_model('):src.index('} // namespace\n\nstd::shared_ptr<const std::vector<float>> link_skin(')]

    compile_run(r'''
    #include <algorithm>
    #include <atomic>
    #include <cassert>
    #include <cmath>
    #include <cstdint>
    #include <cstdio>
    #include <cstring>
    #include <memory>
    #include <mutex>
    #include <random>
    #include <vector>
    #include "''' + str(generated('vk_vr_rig_data.h')) + r'''"
    #include "''' + str(generated('vk_vr_tunic_rig.h')) + r'''"
    namespace rig = mkwvk::vr::rig;
    ''' + math + r'''
    static void logf(const char*, ...) {}
    struct LinkBody { bool child = false; };
    struct Entry { std::vector<float> raw; std::shared_ptr<const std::vector<float>> out; V3 origin{}; bool adult = true; uint64_t frame = 0; };
    static Entry cache[4];
    static std::mutex cache_mu;
    static Entry* vr_entry = nullptr;
    static Entry* canonical_rig(uint64_t, const LinkBody&) { return vr_entry; }
    ''' + tunic + r'''
    static std::mt19937 rng(7);
    static float u(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }
    static Aff rigid(float spread) {
        const M3 r = axis_angle({u(-1, 1), u(-1, 1), u(-1, 1)}, u(-spread, spread));
        return {r, {u(-.5f, .5f), u(-.5f, .5f), u(-.5f, .5f)}};
    }
    static M3 scaled(M3 r, float k) { for (auto& row : r.v) for (float& x : row) x *= k; return r; }
    static float diff(const Aff& a, const Aff& b) {
        float e = length(sub(a.t, b.t));
        for (int i = 0; i < 3; i++) e = std::max(e, length(sub(column(a.r, i), column(b.r, i))));
        return e;
    }
    static void run(const rig::Tunic& T, bool matched) {
        const int n = rig::adult.slots;
        const V3 body_origin{100, 20, -30}, vr_origin{100.5f, 20, -29}, o{99, 21, -31};
        // the game's body upload: a world transform per bone (palette = world * inverse bind; any world works here)
        std::vector<Aff> W(n), D(n);
        for (int b = 0; b < n; b++) W[b] = rigid(3), D[b] = rigid(1.2f);
        std::vector<float> raw(n * 12), vr(n * 12);
        for (int b = 0; b < n; b++) {
            store(raw.data() + b * 12, shifted(W[b], mul(body_origin, -1)));
            Aff v = mul(D[b], W[b]);
            if (b == 85 || rig::adult.region[b] == 2) v.r = scaled(v.r, .001f);   // (the head folded away, as the VR pose hides it)
            store(vr.data() + b * 12, shifted(v, mul(vr_origin, -1)));
        }
        // a body bone the game collapsed (hiding its own default tunic part): never an anchor
        int collapsed = -1;
        for (int i = 0; i < T.slots && collapsed < 0; i++)
            if (T.slot[i].body >= 0 && T.slot[i].parent >= 0 && T.slot[T.slot[i].parent].body >= 0 && rig::adult.region[T.slot[i].body] != 2)
                collapsed = i;
        float* cv = vr.data() + T.slot[collapsed].body * 12;
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) cv[r * 4 + c] *= 1e-4f;
        // the tunic upload as the game makes it: mapped bones = the body bone in the tunic's bind; cloth = parent * own motion
        std::vector<Aff> Tw(T.slots);
        for (int i = 0; i < T.slots; i++) {
            const rig::TunicSlot& s = T.slot[i];
            if (s.body >= 0) Tw[i] = mul(W[s.body], aff(s.bind));
            else Tw[i] = mul(Tw[s.parent], rigid(.6f));
        }
        std::vector<float> tun(T.slots * 12);
        for (int i = 0; i < T.slots; i++) store(tun.data() + i * 12, shifted(Tw[i], mul(o, -1)));
        for (Entry& e : cache) e = {};
        cache[0].raw = matched ? raw : std::vector<float>(n * 12, 0.0f);   // (unmatched: an upload of another pose)
        if (!matched) for (int b = 0; b < n; b++) store(cache[0].raw.data() + b * 12, shifted(rigid(3), mul(body_origin, -1)));
        cache[0].out = std::make_shared<std::vector<float>>(vr), cache[0].origin = body_origin;
        cache[1].raw = raw, cache[1].out = std::make_shared<std::vector<float>>(vr), cache[1].origin = vr_origin;
        vr_entry = &cache[1];
        if (!matched) cache[1].raw = cache[0].raw;   // (the canonical entry's own original is another pose too)
        LinkBody body;
        auto out = tunic_skin(T, tun, o, 5, body);
        assert(out && out->size() == tun.size());
        int checked = 0, anchored = 0;
        for (int i = 0; i < T.slots; i++) {
            int j = i;
            while (j >= 0 && (T.slot[j].body < 0 || j == collapsed)) j = T.slot[j].parent;
            assert(j >= 0);
            const int b = T.slot[j].body;
            if (b == 85 || rig::adult.region[b] == 2) continue;   // (folded head bones: collapsed, checked below)
            const Aff want = mul(D[b], Tw[i]);
            const Aff got = shifted(aff(out->data() + i * 12), o);
            const float e = diff(got, want);
            if (!(e < 2e-3f)) std::fprintf(stderr, "%s slot %d (anchor %d body %d) error %g matched %d\n", T.name, i, j, b, e, matched);
            assert(e < 2e-3f);
            checked++, anchored += j != i;
        }
        for (int i = 0; i < T.slots; i++)
            if (T.slot[i].body == 85) assert(std::fabs(det(aff(out->data() + i * 12).r)) < 1e-6f);   // (the hood's neck: folded)
        std::fprintf(stderr, "%s %s: %d slots checked, %d anchored to an ancestor\n", T.name, matched ? "matched" : "fallback", checked, anchored);
        assert(checked > 80 && anchored >= 1);
        // no VR pose this frame, or child Link: the game's tunic
        vr_entry = nullptr;
        assert(!tunic_skin(T, tun, o, 5, body));
        vr_entry = &cache[1];
        body.child = true;
        assert(!tunic_skin(T, tun, o, 5, body));
    }
    int main() {
        for (const rig::Tunic* T : {&rig::goron, &rig::zora}) run(*T, true), run(*T, false);
        assert(tunic_model(rig::goron.vb0[0], 120 * 48) == &rig::goron && tunic_model(rig::zora.vb0[3], 117 * 48) == &rig::zora);
        assert(!tunic_model(rig::goron.vb0[0], 117 * 48) && !tunic_model(81384, 120 * 48));
    }
    ''', cpp=True)

def check_bracelet_attachment():
    """child Link's Goron Bracelet rides the VR forearm/wrist it is worn on, kept there through the game's arm motion (a
    swing's fast arm too)"""
    src = read('body')
    math = src[src.index('constexpr float kPi'):src.index('// ---- body state kept between frames')]
    change = src[src.index('float aff_change('):src.index('std::shared_ptr<const std::vector<float>> anchored_skin(')]
    bracelet = src[src.index("// ---- child Link's Goron Bracelet"):src.index('static std::shared_ptr<const std::vector<float>> equipment_skin_uncached(')]
    compile_run(r'''
    #include <algorithm>
    #include <atomic>
    #include <cassert>
    #include <chrono>
    #include <cmath>
    #include <cstdint>
    #include <cstdio>
    #include <cstring>
    #include <memory>
    #include <mutex>
    #include <random>
    #include <vector>
    #include "''' + str(generated('vk_vr_rig_data.h')) + r'''"
    #include "''' + str(generated('vk_vr_worn_items.h')) + r'''"
    namespace rig = mkwvk::vr::rig;
    ''' + math + change + r'''
    static void logf(const char*, ...) {}
    struct LinkBody { bool child = true; };
    struct Entry { std::vector<float> raw; std::shared_ptr<const std::vector<float>> out; V3 origin{}; bool adult = false; uint64_t frame = 0; };
    static Entry cache[4];
    static std::mutex cache_mu;
    static Entry* vr_entry = nullptr;
    static Entry* canonical_rig(uint64_t, const LinkBody&) { return vr_entry; }
    ''' + bracelet + r'''
    static std::mt19937 rng(11);
    static float u(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }
    static M3 turn_by(float spread) { return axis_angle(unit({u(-1, 1), u(-1, 1), u(-1, 1)}, {0, 1, 0}), u(-spread, spread)); }
    static float diff(const Aff& a, const Aff& b) {
        float e = length(sub(a.t, b.t));
        for (int i = 0; i < 3; i++) e = std::max(e, length(sub(column(a.r, i), column(b.r, i))));
        return e;
    }
    static Aff translate(V3 t) { return {identity(), t}; }
    // One game frame of child Link: each palette slot's world transform (palette = world x inverse bind; with these, a bone's
    // world joint is W x its bind joint). The left forearm swings, the left wrist turns against it; the right arm is far off.
    struct Frame { std::vector<Aff> W, D; };
    static Frame pose(int f, float wrist_turn) {
        Frame F;
        const int n = rig::child.slots;
        F.W.assign(n, translate({5, 5, 5})), F.D.assign(n, translate({0, 0, 0}));
        const Aff fore = {axis_angle({0, 0, 1}, .3f * std::sin(f * .4f)), {100.2f, 21, -30}};
        const M3 bend = axis_angle({1, 0, 0}, wrist_turn * std::sin(f * 1.3f));   // (the wrist turning about its own joint)
        const V3 j = v3(rig::bracelet_bones[1].joint);
        const Aff wrist = mul(fore, Aff{bend, sub(j, apply(bend, j))});
        F.W[rig::bracelet_bones[0].slot] = fore, F.W[rig::bracelet_bones[1].slot] = wrist;
        F.W[rig::bracelet_bones[2].slot] = translate({99, 21, -30}), F.W[rig::bracelet_bones[3].slot] = translate({99, 21, -30});
        for (int b = 0; b < n; b++) F.D[b] = {turn_by(1.0f), {u(-.3f, .3f), u(-.3f, .3f), u(-.3f, .3f)}};   // (the VR pose: anything)
        return F;
    }
    static void upload(Entry& e, const Frame& F, V3 origin, uint64_t frame) {
        const int n = rig::child.slots;
        e.raw.assign(n * 12, 0.0f);
        auto vr = std::make_shared<std::vector<float>>(n * 12);
        for (int b = 0; b < n; b++) {
            store(e.raw.data() + b * 12, shifted(F.W[b], mul(origin, -1)));
            store(vr->data() + b * 12, shifted(mul(F.D[b], F.W[b]), mul(origin, -1)));
        }
        e.out = vr, e.origin = origin, e.adult = false, e.frame = frame;
    }
    // The bracelet on bone k (0 left forearm, 1 left wrist) with attachment A; the game's draw of it (one rigid matrix)
    static std::vector<float> drawn(const Frame& F, int k, const Aff& A, V3 o) {
        std::vector<float> raw(rig::bracelet_matrices * 12);
        for (uint32_t i = 0; i < rig::bracelet_matrices; i++)
            store(raw.data() + i * 12, shifted(mul(mul(F.W[rig::bracelet_bones[k].slot], A), translate({0, .001f * i, 0})), mul(o, -1)));
        return raw;
    }
    // On bone k, the attachment placed `along` metres past the bone's joint toward its end, run for some frames
    static void run(int k, float along, float wrist_turn) {
        bracelet_state = BraceletState{};
        const rig::BraceletBone& B = rig::bracelet_bones[k];
        const V3 dir = unit(sub(v3(B.end), v3(B.joint)), {1, 0, 0});
        const Aff A = {turn_by(.5f), add(v3(B.joint), mul(dir, along))};
        const V3 body_origin{100, 20, -30}, vr_origin{100.4f, 20.1f, -29.5f}, o{99.5f, 21, -31};
        LinkBody body;
        int checked = 0;
        for (int f = 1; f <= 80; f++) {
            const Frame F = pose(f, wrist_turn);
            for (Entry& e : cache) e = {};
            upload(cache[0], F, body_origin, f);   // (the game's body upload of this frame)
            upload(cache[1], pose(f + 17, wrist_turn), body_origin, f - 1);   // (an older upload: another animation snapshot)
            upload(cache[2], F, vr_origin, f);     // (the frame's one VR pose, about another floating origin)
            vr_entry = &cache[2];
            // after the attachment is taken, the game's own bracelet jiggles on its bone (a swing): the VR one does not
            const Aff jiggle = f > 45 ? Aff{turn_by(.05f), {u(-.01f, .01f), u(-.01f, .01f), u(-.01f, .01f)}} : translate({0, 0, 0});
            const std::vector<float> raw = drawn(F, k, mul(A, jiggle), o);
            auto out = bracelet_skin(raw, o, f, body);
            assert(out && out->size() == raw.size());
            auto again = bracelet_skin(raw, o, f, body);   // (another pass of the same frame: the same palette)
            assert(again && *again == *out);
            std::vector<float> other = raw;   // (a second bracelet in the same frame, 1 m off: a cutscene's, the game's)
            for (uint32_t i = 0; i < rig::bracelet_matrices; i++) other[i * 12 + 3] += 1.0f;
            assert(!bracelet_skin(other, o, f, body));
            if (bracelet_state.fixed_have) {   // (a later pass of the frame from another snapshot of a swinging arm, 0.3 m
                                               // off: the worn one still, on the VR arm where the first pass put it)
                std::vector<float> swung = raw;
                for (uint32_t i = 0; i < rig::bracelet_matrices; i++) swung[i * 12 + 3] += .3f;
                auto late = bracelet_skin(swung, o, f, body);
                assert(late && diff(shifted(aff(late->data()), o), shifted(aff(out->data()), o)) < 2e-3f);
            }
            if (f < 35) continue;   // (the bone and the attachment are found in the first frames)
            if (bracelet_state.bone != k || !bracelet_state.fixed_have)
                std::fprintf(stderr, "frame %d: bone %d (want %d), attachment %s\n", f, bracelet_state.bone, k,
                             bracelet_state.fixed_have ? "taken" : "not taken");
            assert(bracelet_state.bone == k && bracelet_state.fixed_have);
            const int slot = rig::bracelet_bones[k].slot;
            const Aff want = mul(mul(F.D[slot], F.W[slot]), A);   // (on the VR bone, at the game's attachment)
            const float e = diff(shifted(aff(out->data()), o), want);
            if (!(e < 2e-3f)) std::fprintf(stderr, "bone %d frame %d error %g\n", k, f, e);
            assert(e < 2e-3f);
            checked++;
            // every matrix of the model moved alike (its own offsets kept)
            for (uint32_t i = 1; i < rig::bracelet_matrices; i++) {
                Aff g0inv;
                assert(invert(shifted(aff(out->data()), o), &g0inv));
                assert(diff(mul(g0inv, shifted(aff(out->data() + i * 12), o)), translate({0, .001f * i, 0})) < 1e-3f);
            }
            assert(bracelet_worn_lately(f));
        }
        assert(checked > 40);
        {   // a sword swing: the game's fast arm puts its bracelet draw 0.3 m off the body upload's bone: kept on the VR bone
            const Frame F = pose(81, wrist_turn);
            for (Entry& e : cache) e = {};
            upload(cache[0], F, body_origin, 81), upload(cache[2], F, vr_origin, 81);
            vr_entry = &cache[2];
            std::vector<float> raw = drawn(F, k, A, o);
            for (uint32_t i = 0; i < rig::bracelet_matrices; i++) raw[i * 12 + 3] += .3f;
            auto out = bracelet_skin(raw, o, 81, body);
            assert(out && bracelet_state.bone == k);
            const int slot = rig::bracelet_bones[k].slot;
            assert(diff(shifted(aff(out->data()), o), mul(mul(F.D[slot], F.W[slot]), A)) < 2e-3f);
        }
        assert(!bracelet_worn_lately(81 + kBraceletWornFrames));
        // a bracelet near no arm bone (a cutscene's): not taken
        bracelet_state = BraceletState{};
        const Frame F = pose(1, wrist_turn);
        upload(cache[0], F, body_origin, 70), upload(cache[2], F, vr_origin, 70);
        cache[1] = {};
        std::vector<float> away = drawn(F, k, translate({1.2f, 0, 0}), o);
        assert(!bracelet_skin(away, o, 70, body) && !bracelet_worn_lately(70));
        // adult Link (gauntlets, not this), or no VR pose: the game's
        const std::vector<float> raw = drawn(F, k, A, o);
        body.child = false;
        assert(!bracelet_skin(raw, o, 71, body));
        body.child = true;
        vr_entry = nullptr;
        assert(!bracelet_skin(raw, o, 72, body));
    }
    int main() {
        assert(rig::bracelet_matrices >= 1);
        for (uint64_t v : rig::bracelet_vb0) assert(bracelet_model(v, rig::bracelet_matrices * 48ull) && !bracelet_model(v, 0));
        assert(!bracelet_model(86880, 48) && !bracelet_model(rig::bracelet_vb0[0], (rig::bracelet_matrices + 1) * 48ull));
        const float forearm = length(sub(v3(rig::bracelet_bones[0].end), v3(rig::bracelet_bones[0].joint)));
        run(1, .03f, .6f);   // on the left wrist, the wrist turning against the forearm
        run(0, .05f, .6f);   // on the left forearm, mid-forearm
        // on the forearm, just past its end (nearest the wrist bone at first): the wrist turns against it, so it is found
        // to hold still on the forearm
        run(0, 1.05f * forearm, .8f);
        std::fprintf(stderr, "bracelet: wrist, forearm and forearm-near-the-wrist attachments kept on the VR arm\n");
    }
    ''', cpp=True)

def check_ocarina_belt():
    """the ocarina and its holder sit on the VR belt at the front left, the ocarina in its holder, kept there whatever the
    game does with its own afterwards (takes it to his hands to play); held, the ocarina is in that VR hand (the Weapon_L
    mount until the game's own grip is seen, then that grip), its holder still on the belt"""
    src = read('body')
    math = src[src.index('constexpr float kPi'):src.index('// ---- body state kept between frames')]
    change = src[src.index('float aff_change('):src.index('std::shared_ptr<const std::vector<float>> anchored_skin(')]
    belt = src[src.index('// Hookshot VR: the ocarina on the VR belt'):src.index('// Hookshot VR: the Goron and Zora tunics')]
    compile_run(r'''
    #include <algorithm>
    #include <atomic>
    #include <cassert>
    #include <cmath>
    #include <cstdint>
    #include <cstdio>
    #include <cstring>
    #include <memory>
    #include <mutex>
    #include <vector>
    #include "''' + str(generated('vk_vr_rig_data.h')) + r'''"
    #include "''' + str(generated('vk_vr_ocarina.h')) + r'''"
    #include "''' + str(generated('vk_vr_hookshot.h')) + r'''"
    namespace rig = mkwvk::vr::rig;
    ''' + math + change + r'''
    static void logf(const char*, ...) {}
    static struct { int vr_ocarina = 1; } opts;
    struct LinkBody { bool child = true; };
    struct Entry { std::vector<float> raw; std::shared_ptr<const std::vector<float>> out; V3 origin{}; bool adult = false; uint64_t frame = 0; };
    static std::mutex cache_mu;
    static Entry vr_entry_store, *vr_entry = nullptr;
    static Entry* canonical_rig(uint64_t, const LinkBody&) { return vr_entry; }
    static int held_in = -1;   // (the VR hand the game side says holds the ocarina)
    static int ocarina_hand() { return held_in; }
    static float belt_at[3]; static int belt_told = 0;
    static void ocarina_belt_drawn(const float at[3]) { for (int k = 0; k < 3; k++) belt_at[k] = at[k]; belt_told++; }
    ''' + belt + r'''
    static Aff translate(V3 t) { return {identity(), t}; }
    static float diff(const Aff& a, const Aff& b) {
        float e = length(sub(a.t, b.t));
        for (int i = 0; i < 3; i++) e = std::max(e, length(sub(column(a.r, i), column(b.r, i))));
        return e;
    }
    // A frame of child Link: the game's waist (world palette matrix) and the VR one (another place and turn), about the
    // floating origin; every other slot anywhere
    static void body(const Aff& game_waist, const Aff& vr_waist, V3 origin, bool adult) {
        const rig::Model& M = adult ? rig::adult : rig::child;
        Entry& e = vr_entry_store;
        e.raw.assign(M.slots * 12, 0.0f);
        auto out = std::make_shared<std::vector<float>>(M.slots * 12, 0.0f);
        for (int b = 0; b < M.slots; b++) store(e.raw.data() + b * 12, translate({0, 0, 0})), store(out->data() + b * 12, translate({0, 0, 0}));
        store(e.raw.data() + M.waist * 12, shifted(game_waist, mul(origin, -1)));
        store(out->data() + M.waist * 12, shifted(vr_waist, mul(origin, -1)));
        e.out = out, e.origin = origin, e.adult = adult;
        vr_entry = &e;
    }
    // A model's draw: its matrices (the holder's second 4 cm along its z) at `place` in the world, about the draw's origin o
    static std::vector<float> drawn(const Aff& place, unsigned matrices, V3 o) {
        std::vector<float> raw(matrices * 12);
        for (unsigned i = 0; i < matrices; i++) store(raw.data() + i * 12, shifted(mul(place, translate({0, 0, .04f * i})), mul(o, -1)));
        return raw;
    }
    int main() {
        for (uint64_t v : rig::ocarina_shapes)
            assert(!v || (ocarina_model(v, 48) && !ocarina_model(v, 96) && !ocarina_holder_model(v, 96) && held_ocarina_draw(v)));
        for (uint64_t v : rig::ocarina_holder_shapes)
            assert(!v || (ocarina_holder_model(v, 96) && !ocarina_holder_model(v, 48) && !ocarina_model(v, 48) && held_ocarina_draw(v)));
        assert(!ocarina_model(0, 48) && !held_ocarina_draw(0) && !held_ocarina_draw(86880));
        const V3 waist = v3(rig::child.waist_pos), origin{200, 30, -40}, o{199.5f, 30.5f, -41};
        // worn at the back, a little to the right, turned with the belt (Link's model space: +z his front, +x his left)
        const M3 worn_turn = axis_angle({0, 1, 0}, 3.0f);
        const Aff holder_rel = {worn_turn, add(waist, {-.04f, .01f, -.12f})};
        const Aff ocarina_rel = mul(holder_rel, Aff{axis_angle({1, 0, 0}, .2f), {0, .03f, .02f}});
        LinkBody child;
        int placed = 0;
        for (uint64_t f = 1; f <= 60; f++) {
            const Aff gw = {axis_angle({0, 1, 0}, .4f + .01f * f), {200.3f + .02f * f, 30.6f, -39.8f}};   // (Link walks and turns)
            const Aff vw = {axis_angle(unit({.1f, 1, 0}, {0, 1, 0}), -.8f + .01f * f), {200.1f + .02f * f, 30.55f, -39.6f}};
            body(gw, vw, origin, false);
            // after the attachments are taken the game takes its ocarina to his hands (half a metre up and ahead): the VR
            // one stays on the belt
            const Aff game_ocarina = f > 40 ? mul(gw, Aff{identity(), add(waist, {0, .5f, .3f})}) : mul(gw, ocarina_rel);
            const std::vector<float> h = drawn(mul(gw, holder_rel), 2, o), oc = drawn(game_ocarina, 1, o);
            auto hp = held_ocarina(h, o, f, child, true);
            auto op = held_ocarina(oc, o, f, child, false);
            auto again = held_ocarina(h, o, f, child, true);   // (another pass of the same frame: the same palette)
            assert(!hp == !again && (!hp || *hp == *again));
            if (f < kOcarinaCalmFrames) { assert(!hp && !op); continue; }   // (not taken yet: left out near Link)
            if (f < kOcarinaCalmFrames + 3) continue;
            assert(hp && op && hp->size() == h.size() && op->size() == oc.size());
            const Aff H = shifted(aff(hp->data()), o), Oc = shifted(aff(op->data()), o);
            // on the VR waist: at the front left (kOcarinaBeltAngle from his front toward his left), as far from the
            // waist joint and as high as the game wore it, turned with it
            Aff vinv;
            assert(invert(vw, &vinv));
            const Aff in_model = mul(vinv, H);
            const V3 d = sub(in_model.t, waist), was = sub(holder_rel.t, waist);
            assert(std::fabs(std::atan2(d.x, d.z) - kOcarinaBeltAngle) < 1e-3f);
            assert(std::fabs(std::hypot(d.x, d.z) - std::hypot(was.x, was.z)) < 1e-4f && std::fabs(d.y - was.y) < 1e-4f);
            const Aff turn = {axis_angle({0, 1, 0}, kOcarinaBeltAngle - std::atan2(was.x, was.z)), {}};
            assert(diff(Aff{in_model.r, {}}, Aff{mul(turn.r, worn_turn), {}}) < 1e-4f);
            // the holder's own second matrix moved alike, and the ocarina still in its holder
            Aff hinv;
            assert(invert(H, &hinv));
            assert(diff(mul(hinv, shifted(aff(hp->data() + 12), o)), translate({0, 0, .04f})) < 1e-4f);
            Aff hrinv;
            assert(invert(holder_rel, &hrinv));
            assert(diff(mul(hinv, Oc), mul(hrinv, ocarina_rel)) < 1e-4f);
            placed++;
        }
        assert(placed > 30 && belt_told > 30);
        {   // the game side says the left VR hand holds it: in that hand, on the Weapon_L mount until the game's grip is seen
            const rig::Model& M = rig::child;
            const Aff gw = translate({200.3f, 30.6f, -39.8f}), vw = translate({200.1f, 30.55f, -39.6f});
            const Aff game_wrist[2] = {{axis_angle({0, 0, 1}, .3f), {200.4f, 31.0f, -39.6f}}, {axis_angle({0, 0, 1}, -.3f), {200.2f, 31.0f, -39.6f}}};
            const Aff vr_wrist[2] = {{axis_angle({1, 0, 0}, .7f), {200.5f, 31.2f, -39.3f}}, {axis_angle({0, 1, 0}, .5f), {199.9f, 31.1f, -39.4f}}};
            const auto wrists = [&] {
                body(gw, vw, origin, false);
                for (int k = 0; k < 2; k++) {
                    store(vr_entry_store.raw.data() + M.arm[k].wrist * 12, shifted(game_wrist[k], mul(origin, -1)));
                    store(const_cast<float*>(vr_entry_store.out->data()) + M.arm[k].wrist * 12, shifted(vr_wrist[k], mul(origin, -1)));
                }
            };
            held_in = 0;
            uint64_t f = 61;
            wrists();
            auto op = held_ocarina(drawn(mul(gw, ocarina_rel), 1, o), o, f, child, false);
            assert(op && diff(shifted(aff(op->data()), o), mul(vr_wrist[0], aff(rig::hookshot_left_child))) < 1e-4f);
            // the game plays it: its ocarina in Link's two game hands, away from the holder: its grip against each wrist
            // is taken and used from then on, in either VR hand
            const Aff in_hands = {axis_angle({0, 1, 0}, 1.2f), {200.3f, 31.05f, -39.5f}};
            Aff gi[2];
            for (int k = 0; k < 2; k++) assert(invert(game_wrist[k], &gi[k]));
            for (; f < 61 + kOcarinaCalmFrames + 3; f++) {
                wrists();
                held_ocarina(drawn(mul(gw, holder_rel), 2, o), o, f, child, true);
                op = held_ocarina(drawn(in_hands, 1, o), o, f, child, false);
                assert(op);
            }
            assert(ocarina_grip[0][0].have && ocarina_grip[0][1].have);
            for (int k = 0; k < 2; k++) {
                held_in = k;
                wrists();
                op = held_ocarina(drawn(mul(gw, ocarina_rel), 1, o), o, f++, child, false);
                assert(op && diff(shifted(aff(op->data()), o), mul(vr_wrist[k], mul(gi[k], in_hands))) < 1e-4f);
            }
            // the holder stays on the belt meanwhile
            held_in = 1;
            wrists();
            auto hp = held_ocarina(drawn(mul(gw, holder_rel), 2, o), o, f, child, true);
            Aff vinv;
            assert(hp && invert(vw, &vinv) && std::fabs(std::atan2(sub(mul(vinv, shifted(aff(hp->data()), o)).t, waist).x,
                                                                   sub(mul(vinv, shifted(aff(hp->data()), o)).t, waist).z) - kOcarinaBeltAngle) < 1e-3f);
            held_in = -1;
        }
        // the row off: left out (link_worn_hidden), never moved
        opts.vr_ocarina = 0;
        assert(!held_ocarina(drawn(holder_rel, 2, o), o, 61, child, true));
        opts.vr_ocarina = 1;
        // adult Link next: the child's attachments are forgotten, taken again on the adult waist
        body(translate({200, 30, -40}), translate({200, 30, -40}), origin, true);
        assert(!held_ocarina(drawn(holder_rel, 2, o), o, 62, child, true) && !ocarina_belt.holder.have && ocarina_belt.adult);
        // a holder far from the waist (a shop's, a cutscene's): never taken
        ocarina_belt = OcarinaBelt{};
        body(translate({200, 30, -40}), translate({200, 30, -40}), origin, false);
        for (uint64_t f = 100; f < 140; f++)
            assert(!held_ocarina(drawn(translate({201, 30.6f, -40}), 2, o), o, f, child, true));
        assert(!ocarina_belt.holder.have);
        // no VR pose this frame: not placed
        vr_entry = nullptr;
        assert(!held_ocarina(drawn(holder_rel, 2, o), o, 141, child, true));
        std::fprintf(stderr, "ocarina: holder and ocarina kept at the front left of the VR belt; in a VR hand by the game's grip\n");
    }
    ''', cpp=True)


def check_item_get():
    """first person, an item get: the item's model (a small one near the spot over Link's game head) is drawn in front of
    the headset, a little below the eyes, its front (the side it showed Link's front) turned to the player; Navi (13
    matrices), models elsewhere, the row off and no item get playing are left as the game draws them"""
    src = read('body')
    math = src[src.index('constexpr float kPi'):src.index('// ---- body state kept between frames')]
    item = src[src.index('// Hookshot VR: an item get'):src.index('\n}\n', src.index('item_get_skin(uint64_t bones')) + 3]
    compile_run(r'''
    #include <algorithm>
    #include <atomic>
    #include <chrono>
    #include <cassert>
    #include <cmath>
    #include <cstdint>
    #include <cstdio>
    #include <cstring>
    #include <memory>
    #include <mutex>
    #include <thread>
    #include <vector>
    #include "''' + str(generated('vk_vr_rig_data.h')) + r'''"
    namespace rig = mkwvk::vr::rig;
    ''' + math + r'''
    static void logf(const char*, ...) {}
    static struct { int vr_item_get = 1; } opts;
    static bool live = true;
    static bool item_get_live() { return live; }
    static int drawn;
    static void item_get_drawn() { drawn++; }
    static unsigned gets = 1;
    static unsigned item_get_number() { return gets; }
    struct LinkBody { bool child = true; float eye[3]{}, head_back[3]{}; };
    struct Entry { std::vector<float> raw; std::shared_ptr<const std::vector<float>> out; V3 origin{}; bool adult = false; };
    static std::mutex cache_mu;
    static Entry rig_now;
    static LinkBody told;
    static Entry* canonical_rig(uint64_t, const LinkBody&) { return &rig_now; }
    static bool frame_body(uint64_t, LinkBody& out) { out = told; return true; }
    // the draw's buffers: its palette at 0x1000, its object (origin at +0x40) at 0x2000
    static std::vector<float> palette;
    static float origin[3];
    static bool ub_read(uint64_t addr, void* dst, uint64_t n) {
        if (addr == 0x1000) { if (n > palette.size() * 4) return false; std::memcpy(dst, palette.data(), n); return true; }
        if (addr == 0x2040) { std::memcpy(dst, origin, n); return true; }
        return false;
    }
    ''' + item + r'''
    static Aff translate(V3 t) { return {identity(), t}; }
    static void put(const Aff& m) { for (size_t i = 0; i + 12 <= palette.size(); i += 12) store(palette.data() + i, m); }   // (every matrix)
    static float diff(const Aff& a, const Aff& b) {
        float e = length(sub(a.t, b.t));
        for (int i = 0; i < 3; i++) e = std::max(e, length(sub(column(a.r, i), column(b.r, i))));
        return e;
    }
    int main() {
        item_get_glide_frames = 0;   // (placed at once; the glide in is checked at the end)
        const rig::Model& M = rig::child;
        // child Link's game body: turned to face -z (round, to the game's camera), about a floating origin
        const V3 o_rig{300, 40, -20};
        const Aff body = {axis_angle({0, 1, 0}, 3.14159265f), {300.2f, 40.1f, -20.3f}};
        rig_now.raw.assign(M.slots * 12, 0.0f);
        for (int b = 0; b < M.slots; b++) store(rig_now.raw.data() + b * 12, shifted(body, mul(o_rig, -1)));
        rig_now.origin = o_rig;
        const V3 head = add(o_rig, point(rig_now.raw.data() + M.neck * 12, v3(M.neck_pos)));
        // the headset: its eyes near his head, looking +z (where the player faced before the get)
        const V3 eye = add(head, {0, .08f, .05f});
        told.eye[0] = eye.x, told.eye[1] = eye.y, told.eye[2] = eye.z, told.head_back[2] = -1;
        // the item: 2 matrices, over his head, its front (+z of its model) to his front (-z in the world), spinning
        origin[0] = 299.5f, origin[1] = 40.5f, origin[2] = -21;
        const Aff item_at = {axis_angle({0, 1, 0}, 3.14159265f), add(head, {.03f, kItemGetAbove + .15f, -.02f})};
        palette.assign(24, 0.0f);
        put(shifted(item_at, mul(v3(origin), -1)));
        store(palette.data() + 12, shifted(mul(item_at, translate({0, .1f, 0})), mul(v3(origin), -1)));
        auto out = item_get_skin(0x1000, 96, 0x2000, 0x100, 12345, 1);
        assert(out && out->size() == palette.size() && drawn == 1);   // (the game side told: its heading keeps)
        const Aff placed = shifted(aff(out->data()), v3(origin));
        // in front of the eyes, a little below, and its front toward them (+z of its model now points -z: back at the eyes)
        assert(length(sub(placed.t, add(eye, {0, -kItemGetBelow, kItemGetAhead}))) < 1e-4f);
        const V3 its_front = apply(placed.r, {0, 0, 1});
        assert(its_front.z < -.99f);
        // its second matrix moved alike
        Aff pinv;
        assert(invert(placed, &pinv) && diff(mul(pinv, shifted(aff(out->data() + 12), v3(origin))), translate({0, .1f, 0})) < 1e-4f);
        // not the item: far from the spot over his head (the chest, at his feet), Navi's 13 matrices, the row off, no get
        palette.assign(24, 0.0f);
        put(shifted(translate(add(head, {0, -.8f, .6f})), mul(v3(origin), -1)));
        assert(!item_get_skin(0x1000, 96, 0x2000, 0x100, 1, 1));
        palette.assign(13 * 12, 0.0f);
        for (int i = 0; i < 13; i++) store(palette.data() + i * 12, shifted(item_at, mul(v3(origin), -1)));
        assert(!item_get_skin(0x1000, 13 * 48, 0x2000, 0x100, 19908, 1));
        assert(drawn == 1);   // (nothing taken: nothing told)
        palette.assign(24, 0.0f);
        put(shifted(item_at, mul(v3(origin), -1)));
        const int was = drawn;   // (Over your head: left where the game has it, the game side told all the same)
        opts.vr_item_get = 0; assert(!item_get_skin(0x1000, 96, 0x2000, 0x100, 12345, 1) && drawn == was + 1); opts.vr_item_get = 1;
        live = false; assert(!item_get_skin(0x1000, 96, 0x2000, 0x100, 12345, 1)); live = true;
        assert(item_get_skin(0x1000, 96, 0x2000, 0x100, 12345, 1));
        // one of Link's gear models (a chest's Kokiri Sword): taken well over his head, never near his head joint (his
        // sword's sheath on his back is within reach of the spot)
        assert(item_get_skin(0x1000, 96, 0x2000, 0x100, 12345, 1, true));
        palette.assign(24, 0.0f);
        put(shifted(translate(add(head, {0, -.05f, .2f})), mul(v3(origin), -1)));
        assert(item_get_skin(0x1000, 96, 0x2000, 0x100, 12345, 1) && !item_get_skin(0x1000, 96, 0x2000, 0x100, 12345, 1, true));
        // taken, then refused (the game took it down into his hand): let go and hidden for the rest of this item get, never
        // flipping back, even raised again (the game's own animation swings the sword round the face); the next item get
        // takes it afresh
        palette.assign(24, 0.0f);
        put(shifted(item_at, mul(v3(origin), -1)));
        auto hidden = [](const std::shared_ptr<const std::vector<float>>& d) {   // (let go: drawn as nothing)
            return d && length(column(shifted(aff(d->data()), v3(origin)).r, 0)) < 1e-6f;
        };
        assert(hidden(item_get_skin(0x1000, 96, 0x2000, 0x100, 12345, 1)));
        gets++;
        assert(item_get_skin(0x1000, 96, 0x2000, 0x100, 12345, 1));
        for (float y : {.25f, .1f}) {   // (brought down frame by frame)
            put(shifted(translate(add(head, {0, y, .1f})), mul(v3(origin), -1)));
            assert(!item_get_skin(0x1000, 96, 0x2000, 0x100, 12345, 1, true));
        }
        put(shifted(translate(add(head, {0, -.14f, .2f})), mul(v3(origin), -1)));
        assert(!item_get_skin(0x1000, 96, 0x2000, 0x100, 12345, 1, true));
        put(shifted(item_at, mul(v3(origin), -1)));
        assert(hidden(item_get_skin(0x1000, 96, 0x2000, 0x100, 12345, 1, true)) && hidden(item_get_skin(0x1000, 96, 0x2000, 0x100, 12345, 1)));
        assert(item_get_skin(0x1000, 96, 0x2000, 0x100, 777, 1));   // (another model in the same get: still taken)
        // world-locked: the head turned to face +x in the same get, the item stays where it was put; the next get puts it
        // where the head faces then
        const V3 ahead = add(eye, {0, -kItemGetBelow, kItemGetAhead}), right = add(eye, {kItemGetAhead, -kItemGetBelow, 0});
        told.head_back[2] = 0, told.head_back[0] = -1;
        auto at = [](const std::shared_ptr<const std::vector<float>>& d) { return shifted(aff(d->data()), v3(origin)); };
        auto still = item_get_skin(0x1000, 96, 0x2000, 0x100, 777, 1);
        assert(still && length(sub(at(still).t, ahead)) < 1e-4f);
        gets++;
        auto anew = item_get_skin(0x1000, 96, 0x2000, 0x100, 777, 1);
        assert(anew && length(sub(at(anew).t, right)) < 1e-4f);
        // a shop's item (seen on its shelf first): drawn where the game has it, its close-up at the eyes too (headset 22 Nov)
        gets++, told.head_back[0] = 0, told.head_back[2] = -1;
        palette.assign(12, 0.0f);
        put(shifted(translate(add(eye, {0, .3f, 2})), mul(v3(origin), -1)));   // (a shelf, up high)
        assert(!item_get_skin(0x1000, 48, 0x2000, 0x100, 555, 1));
        const M3 big = {{{3, 0, 0}, {0, 3, 0}, {0, 0, 3}}};
        put(shifted(Aff{big, add(eye, {0, .03f, .2f})}, mul(v3(origin), -1)));
        assert(!item_get_skin(0x1000, 48, 0x2000, 0x100, 555, 1));
        // a shop's Deku Shield (a gear model): flown from the shelf in front of Link to near the eyes, taken at the spot;
        // the purchase cancelled, flown back to the shelf (the game's), and at the next confirm taken again; Link's own
        // sheath (on his back, about at his head joint) never; taken down into his hand, let go; bought again later
        // (not drawn for a while), taken afresh
        gets++;
        auto shield_at = [&](V3 d) {
            put(shifted(translate(add(head, d)), mul(v3(origin), -1)));
            return item_get_skin(0x1000, 48, 0x2000, 0x100, 19824, 1, true);
        };
        auto shield = shield_at({0, .08f, -.3f});
        assert(shield && length(sub(at(shield).t, ahead)) < 1e-4f);
        assert(!shield_at({0, .2f, .8f}) && !shield_at({0, .1f, -2}));   // (on its way back, on the shelf: the game's)
        shield = shield_at({0, .08f, -.3f});
        assert(shield && length(sub(at(shield).t, ahead)) < 1e-4f);      // (confirmed again)
        put(shifted(translate(add(head, {0, .04f, .2f})), mul(v3(origin), -1)));
        assert(!item_get_skin(0x1000, 48, 0x2000, 0x100, 484288, 1, true));
        put(shifted(translate(add(head, {0, .11f, .2f})), mul(v3(origin), -1)));   // (Link bent over a chest)
        assert(!item_get_skin(0x1000, 48, 0x2000, 0x100, 484288, 1, true));
        assert(!shield_at({0, 0, -.15f}) && !shield_at({0, -.14f, .1f}) && hidden(shield_at({0, .08f, -.3f})));   // (brought
                                                                          // down into his hand: let go, hidden this purchase)
        // flown to the camera: at the eyes, about level with his head joint, taken (headset 22 Nov: refused, full size)
        gets++;
        assert(shield_at({0, .08f, -.04f}));
        std::this_thread::sleep_for(std::chrono::milliseconds(kItemGetGoneMs + 100));
        assert(shield_at({0, .08f, -.3f}));   // (the next purchase)
        // the shop's Deku Shield (Link's own shield model, a gear model): given the game's own pose wherever it is, so the
        // gear code does not take it onto the VR body's shield place ("flew off to the right", headset 22 Nov)
        gets++;
        palette.assign(24, 0.0f);
        put(shifted(translate(add(head, {0, .3f, -1.2f})), mul(v3(origin), -1)));   // (on the shelf)
        for (V3 d : {V3{0, .3f, -1.2f}, V3{0, .35f, -.3f}, V3{0, .1f, .2f}}) {   // (shelf, counter, close-up)
            put(shifted(translate(add(head, d)), mul(v3(origin), -1)));
            auto own = item_get_skin(0x1000, 96, 0x2000, 0x100, 4444, 300, true);
            assert(own && own->size() == palette.size() && std::equal(own->begin(), own->end(), palette.begin(),
                                                                         [](float a, float b) { return std::fabs(a - b) < 1e-4f; }));
        }
        // a world model far away (a tree tens of metres off) is never limited, whatever it does
        gets++;
        put(Aff{identity(), add(eye, {0, 3, 20})});
        assert(!item_get_skin(0x1000, 48, 0x2000, 0x100, 888, 1));
        put(Aff{big, add(eye, {0, 3, 20})});
        assert(!item_get_skin(0x1000, 48, 0x2000, 0x100, 888, 1));
        // a chest's item: first seen low in the chest, a metre away and small, then over his head at full size: not limited
        gets++;
        palette.assign(12, 0.0f);
        const M3 small = {{{.2f, 0, 0}, {0, .2f, 0}, {0, 0, .2f}}};
        put(shifted(Aff{small, add(eye, {0, -.8f, .7f})}, mul(v3(origin), -1)));
        assert(!item_get_skin(0x1000, 48, 0x2000, 0x100, 666, 1));
        put(shifted(translate(add(head, {0, kItemGetAbove + .3f, 0})), mul(v3(origin), -1)));
        auto chest = item_get_skin(0x1000, 48, 0x2000, 0x100, 666, 1);
        assert(chest && std::fabs(length(column(at(chest).r, 0)) - 1) < 1e-3f);
        // the Kokiri Sword: taken over his head, then in his hand the next frame (0.84 m in one frame, as the game binds it):
        // not taken for another copy for long, let go a frame later and hidden
        gets++;
        put(shifted(translate(add(head, {0, .7f, 0})), mul(v3(origin), -1)));
        assert(item_get_skin(0x1000, 48, 0x2000, 0x100, 86880, 100, true));
        put(shifted(translate(add(head, {0, -.14f, .1f})), mul(v3(origin), -1)));
        assert(!item_get_skin(0x1000, 48, 0x2000, 0x100, 86880, 101, true));   // (the frame after: may be another copy)
        assert(!item_get_skin(0x1000, 48, 0x2000, 0x100, 86880, 102, true));   // (it moved: let go)
        assert(hidden(item_get_skin(0x1000, 48, 0x2000, 0x100, 86880, 103, true)));
        // the glide in: taken, it starts where the game had it, not at the spot
        item_get_glide_frames = 20, gets++;
        const V3 over_head = add(head, {0, kItemGetAbove, 0});
        palette.assign(24, 0.0f);
        put(shifted(translate(over_head), mul(v3(origin), -1)));
        auto glide = item_get_skin(0x1000, 96, 0x2000, 0x100, 4242, 1);
        assert(glide && length(sub(at(glide).t, over_head)) < 1e-3f);
        // another copy of the same model (a rupee on the ground) refused in the same frame changes nothing: in the next
        // frames the taken one goes on gliding, the same in every pass of a frame
        put(shifted(translate(add(eye, {1.5f, -.9f, .5f})), mul(v3(origin), -1)));
        assert(!item_get_skin(0x1000, 96, 0x2000, 0x100, 4242, 1));
        put(shifted(translate(over_head), mul(v3(origin), -1)));
        auto later = item_get_skin(0x1000, 96, 0x2000, 0x100, 4242, 11), again = item_get_skin(0x1000, 96, 0x2000, 0x100, 4242, 11);
        const V3 half = add(over_head, mul(sub(ahead, over_head), .5f));   // (10 of 20 frames: half way, smoothstep)
        assert(later && again && length(sub(at(later).t, half)) < 1e-3f && length(sub(at(again).t, half)) < 1e-3f);
        // many other small models drawn in the same frames (grass, props: more than the models kept) push nothing out
        for (int frame = 12; frame < 16; frame++) {
            put(shifted(translate(add(eye, {3, -1, 3})), mul(v3(origin), -1)));
            for (uint64_t other = 50000; other < 50100; other++) assert(!item_get_skin(0x1000, 96, 0x2000, 0x100, other, frame));
            put(shifted(translate(over_head), mul(v3(origin), -1)));
            assert(item_get_skin(0x1000, 96, 0x2000, 0x100, 4242, frame));
        }
        auto crowd = item_get_skin(0x1000, 96, 0x2000, 0x100, 4242, 16);   // (15 of 20 frames in: not begun again)
        const float e15 = .75f * .75f * (3 - 2 * .75f);
        assert(crowd && length(sub(at(crowd).t, add(over_head, mul(sub(ahead, over_head), e15)))) < 1e-3f);
        auto done = item_get_skin(0x1000, 96, 0x2000, 0x100, 4242, 40);
        assert(done && length(sub(at(done).t, ahead)) < 1e-4f);
        item_get_glide_frames = 0;
        std::fprintf(stderr, "item get: the item in front of the headset, facing it\n");
    }
    ''', cpp=True)


# ======================================================================================================================
# The bow and arrows
# ======================================================================================================================

def check_bow_patch():
    """the bow's game patch adds its three calls once, however often it runs"""
    sys.path.insert(0, str(path('bow_patch').parent))
    from vr_bow_patch import patch_source
    sample = '#include "a64.h"\nvoid f_82e1f2d0(Ctx *__restrict A64_NOESC c) {\nL_82e235f0:;\nL_82e236d0:;\n}\n'
    patched=patch_source(sample)
    assert patch_source(patched)==patched
    assert patched.count('/* VR bow:')==3

def check_bow_input():
    """ZR, grips and triggers drive the bow as the hands say, never stuck; letting go of its grip asks the game's own
    put-away and never presses B (withdrawn if taken again first, ZR takes it out again if the game put it away all the
    same; calls that never take are given up, still no B)"""
    host = read('bow_host')
    start = host.index('static volatile int oot_vr_bow_grips_owned;')
    end = host.index('\n}\n', host.index('uint64_t hos_oot_vr_bow_controls(')) + 3   # (put_away_stow.inc is inlined before it)
    compile_run(r'''
    #include <stdint.h>
    #include <string.h>
    #include <assert.h>
    static int64_t clock_ns = 5000000000LL;
    static int64_t hos_now_ns(void) { return clock_ns; }
    static void hos_log(const char *f, ...) { (void)f; }
    static volatile int64_t oot_vr_bow_out_seen;
    static volatile uint64_t oot_vr_bow_mapped_buttons;
    static volatile int oot_vr_selected_bow = -1, oot_vr_selection;
    static volatile int oot_vr_quick_open;
    static float packet[12];
    static int solution = 1;
    static int rx_vr_bow(float out[12]) { memcpy(out, packet, sizeof packet); return solution; }
    typedef struct { unsigned seq; int active, mode; } HookshotVrHead;
    static int rx_vr_head(HookshotVrHead *h) { h->active = 1, h->mode = 1; return 1; }
    static int grip_option = 0;
    static int rx_option(int id, int otherwise) { return id == 900 ? grip_option : otherwise; }
    static float squeeze[2];
    static int rx_vr_hands(void *out) { uint32_t *w = (uint32_t *)out; memcpy(&w[1 + 22], &squeeze[0], 4); memcpy(&w[1 + 26 + 22], &squeeze[1], 4); return 1; }
    uint64_t hos_vr_bomb_buttons(uint64_t b) { return b; }   // (bombs not selected: vr/items/bomb/bomb_host.c leaves them)
    uint64_t hos_vr_bomb_test_buttons(uint64_t b) { return b; }
    uint64_t hos_vr_hook_buttons(uint64_t b) { return b; }   // (the hookshot not selected: vr/items/hookshot)
    uint64_t hos_vr_boomerang_buttons(uint64_t b) { return b; }   // (the boomerang not selected: vr/items/boomerang)
    int hos_vr_bomb_in_reach(int hand) { (void)hand; return 0; }   // (no bomb near either hand)
    int hos_vr_ocarina_claims(int hand) { (void)hand; return 0; }   // (no hand at the belt ocarina)
    static int sword_claim = -1;
    int hos_vr_sword_claims(int hand) { return hand == sword_claim; }   // (a grip behind the shoulder is the sword's)
    static int asked, cancelled;
    int hos_vr_put_away_held(void) { asked++; return 1; }   // (vr/items/gear/put_away_host.c: the game's put-away asked)
    void hos_vr_put_away_cancel(void) { cancelled++; }
    ''' + host[start:end] + r'''
    static const uint64_t ZR = 1ULL << 9, GRIP = 1ULL << 8, BB = 1ULL << 1, PLUS = 1ULL << 10;
    static uint64_t frame(uint64_t buttons, int bow_out, int flags) {
        clock_ns += 16000000;
        oot_vr_bow_out_seen = bow_out ? clock_ns : 0;
        packet[10] = (float)flags;
        return hos_oot_vr_bow_controls(buttons);
    }
    int main(void) {
        assert(frame(ZR | GRIP, 0, 8) == (ZR | GRIP));            // no bow out: the player's ZR (e.g. another item, or taking the bow out)
        assert(frame(ZR, 1, 8) == ZR);                            // the bow comes out with ZR held: still the player's
        assert(frame(ZR, 1, 8 | 2) == ZR);
        assert(frame(0, 1, 8) == 0);                              // ... until let go (the game fires as it always did)
        assert(frame(ZR | GRIP, 1, 8) == GRIP);                   // from then on the real ZR does nothing while the bow is out
        assert(frame(GRIP, 1, 8 | 1) == GRIP);                    // string held but not committed: no ZR
        assert(frame(GRIP, 1, 8 | 1 | 2) == (GRIP | ZR));         // committed draw: the game's ZR held
        assert(frame(0, 1, 8 | 2) == ZR);
        solution = 0;                                             // a tracking dropout or skipped frame: still held
        for (int i = 0; i < 40; i++) assert(frame(0, 1, 0) == ZR);
        solution = 1;
        assert(frame(0, 1, 8) == 0);                              // the string's let-go: ZR let go, the game's shot
        assert(frame(ZR, 1, 8 | 2) == ZR);
        solution = 0;
        for (int i = 0; i < 200; i++) frame(0, 1, 0);             // gone for well over 1.5 s: the buttons are the player's again
        assert(frame(0, 1, 0) == 0 && frame(ZR, 1, 0) == ZR);
        solution = 1;
        assert(frame(ZR, 1, 2) == ZR && frame(0, 1, 2) == 0);     // string firing off (no flag 8): the player's ZR as before
        assert(frame(ZR, 0, 8 | 2) == ZR && frame(0, 0, 8 | 2) == 0);   // bow put away: the player's again

        // ---- "Grip holds the bow"
        grip_option = 1;
        frame(PLUS, 0, 0);   // (the first half taught it already: the pause menu forgets)
        for (int i = 0; i < 50; i++) frame(0, 0, 0);
        squeeze[0] = 1; assert(frame(0, 0, 0) == 0 && !hos_oot_vr_bow_grips());   // ZR's item not known yet: nothing, grips the game's
        squeeze[0] = 0; frame(0, 0, 0);
        assert(frame(ZR, 0, 0) == ZR);                                           // the player's ZR brings the bow out ...
        assert(frame(ZR, 1, 8) == ZR && hos_oot_vr_bow_grips());                // ... and teaches it: grips owned from now on
        assert(frame(ZR, 1, 8) == ZR);                                           // (that press stays the player's until let go)
        assert(frame(0, 1, 8) == 0);
        for (int i = 0; i < 20; i++) frame(0, 0, 8);                             // put away (the game)
        assert(frame(ZR, 0, 8) == 0);                                            // the real ZR does nothing now
        squeeze[0] = 1; assert(frame(0, 0, 8) == ZR);                            // the left grip takes it out: ZR held ...
        assert(frame(0, 0, 8) == ZR);
        for (int i = 0; i < 10; i++) assert(frame(0, 1, 8 | 0) == 0);            // ... until it is out: it rests, not aimed
        assert(frame(0, 1, 8 | 1) == 0);                                         // string held, not yet committed: still resting
        assert(frame(0, 1, 8 | 1 | 2) == ZR);                                    // drawn past the commit: ZR held, the game aims
        assert(frame(0, 1, 8 | 1 | 2) == ZR);
        assert(frame(0, 1, 8 | 16) == 0);                                        // the string's firing let-go: ZR let go, the shot
        for (int i = 0; i < 20; i++) assert(frame(0, 1, 8 | 16) == 0);           // ... and the bow rests again after it
        assert(frame(0, 1, 8 | 1 | 2 | 16) == ZR);                               // the next draw
        assert(frame(0, 1, 8) == 0);
        squeeze[0] = 0; assert(frame(0, 1, 8 | 1) == 0);                         // bow grip let go while the string is held: kept
        asked = 0;
        uint64_t r = frame(0, 1, 8); assert(r == 0 && asked == 1);               // ... then let go: the game's put-away, no B, no ZR
        for (int i = 0; i < 10; i++) r |= frame(0, 1, 8);
        assert(r == 0 && asked == 1);                                            // (never B; asked again only after 300 ms)
        assert(frame(0, 0, 8) == 0);                                             // the bow is away
        // taken again while being put away: withdrawn; the game's request put it away all the same: ZR takes it out again
        squeeze[0] = 1; assert(frame(0, 0, 8) == ZR);
        for (int i = 0; i < 10; i++) frame(0, 1, 8);                             // out, resting
        squeeze[0] = 0; asked = cancelled = 0; assert(frame(0, 1, 8) == 0 && asked == 1);
        squeeze[0] = 1; assert(frame(0, 1, 8) == 0 && cancelled == 1);           // held again, still out: nothing pressed
        assert(frame(0, 0, 8) == ZR);                                            // gone just after: ZR takes it out again
        for (int i = 0; i < 10; i++) assert(frame(0, 1, 8) == 0);
        for (int i = 0; i < 30; i++) frame(0, 1, 8);
        assert(asked == 1);                                                      // held: nothing more asked
        // the game's call never taking: asked kVrStowTries times, then given up (logged), never B
        squeeze[0] = 0; asked = 0; r = 0;
        for (int i = 0; i < 120; i++) r |= frame(0, 1, 8);                       // (1.9 s)
        assert(asked == kVrStowTries && !(r & BB));
        for (int i = 0; i < 20; i++) frame(0, 0, 8);
        sword_claim = 1; squeeze[1] = 1; assert(frame(0, 0, 8) == 0);            // a grip behind the shoulder: the sword's
        squeeze[1] = 0; frame(0, 0, 8); sword_claim = -1;
        squeeze[1] = 1; assert(frame(0, 0, 8) == ZR); squeeze[1] = 0;            // the right grip takes it out as well
        for (int i = 0; i < 50; i++) r = frame(0, 0, 8);                         // ... but nothing came out (another item on ZR)
        assert(r == 0 && !hos_oot_vr_bow_grips());
        assert(frame(ZR, 0, 8) == ZR);                                           // the player's ZR again
        frame(0, 0, 8); frame(ZR, 0, 8); frame(ZR, 1, 8); frame(0, 1, 8);        // learned again
        assert(hos_oot_vr_bow_grips());
        frame(PLUS, 1, 8); for (int i = 0; i < 20; i++) frame(0, 0, 8);          // the pause menu: asked again
        assert(!hos_oot_vr_bow_grips() && frame(ZR, 0, 8) == ZR);
        frame(0, 0, 8);
        // the selected item read from the game: the bow selected, the grips take it at once (no ZR press to learn from)
        oot_vr_selected_bow = 1, oot_vr_selection++;
        assert(frame(0, 0, 8) == 0 && hos_oot_vr_bow_grips());
        squeeze[0] = 1; assert(frame(0, 0, 8) == ZR);                            // a grip takes it out
        for (int i = 0; i < 5; i++) frame(0, 1, 8);
        squeeze[0] = 0; frame(0, 1, 8); for (int i = 0; i < 10; i++) frame(0, 1, 8); frame(0, 0, 8);   // put away
        oot_vr_selected_bow = 0, oot_vr_selection++;                             // another item selected: the grips are the game's
        assert(frame(0, 0, 8) == 0 && !hos_oot_vr_bow_grips() && frame(ZR, 0, 8) == ZR);
        frame(0, 0, 8);
        oot_vr_selected_bow = 1, oot_vr_selection++;                             // "the bow", but ZR brings nothing out:
        squeeze[1] = 1; frame(0, 0, 8); squeeze[1] = 0;
        for (int i = 0; i < 60; i++) frame(0, 0, 8);
        assert(hos_oot_vr_bow_grips());                                          // valid bow selection survives a take-out timeout
        oot_vr_selection++; frame(0, 0, 8); assert(hos_oot_vr_bow_grips());
        // Grip held while selecting: defer take-out until the quick menu actually closes, with no new squeeze/trigger.
        frame(PLUS,0,8); for(int i=0;i<60;i++) frame(0,0,8);
        squeeze[0]=0;squeeze[1]=0;oot_vr_selected_bow=0;oot_vr_selection++;frame(0,0,8);
        oot_vr_quick_open=1;squeeze[1]=1;oot_vr_selected_bow=1;oot_vr_selection++;
        for(int i=0;i<60;i++) assert(frame(0,0,8)==0);
        oot_vr_quick_open=0;assert(frame(0,0,8)==ZR);
        return 0;
    }
    ''')

def check_bow_launch():
    """the arrow leaves the VR bow in the hands' direction"""
    host=read('bow_host')
    start=host.index('void hos_oot_vr_bow_probe(Ctx *c, uint64_t address, int phase) {')
    end=host.index('\n}\n',start)+3
    callback=host[start:end]
    fixture=r'''
    #include <stdint.h>
    #include <math.h>
    #include <string.h>
    #include <assert.h>
    #include <stddef.h>
    typedef struct { unsigned char mem[32768]; } Ctx;
    typedef struct { int active,mode; unsigned seq; } HookshotVrHead;
    static HookshotVrHead head={1,1,10};
    static uint64_t oot_vr_player_actor=0x80,oot_vr_player_object=0x100;
    static _Thread_local Ctx *oot_vr_bow_context;
    static _Thread_local int oot_vr_bow_player, oot_vr_bow_rumbled;
    static int rumbles, rumble_hand=-1;
    static void rx_vr_rumble(int h,float a,float s) { rumbles++; rumble_hand=h; (void)a; (void)s; }
    static volatile uint64_t oot_vr_bow_mapped_buttons;
    static float packet[12]={0,0,0,1,0,0,0,0,0,.4f,1|4,10};   /* (flags: held, bow in the right hand) */
    static int enabled=1, quick=0;
    static int rx_vr_head(HookshotVrHead *h) { *h=head; return 1; }
    static int rx_vr_bow(float out[12]) { memcpy(out,packet,sizeof packet); return enabled; }
    static uint32_t recomp_load32(Ctx*c,uint64_t a) { uint32_t v;memcpy(&v,c->mem+a,4);return v; }
    static uint64_t recomp_load64(Ctx*c,uint64_t a) { uint64_t v;memcpy(&v,c->mem+a,8);return v; }
    static float vr_load(Ctx*c,uint64_t a) { float v;memcpy(&v,c->mem+a,4);return v; }
    static void vr_store(Ctx*c,uint64_t a,float v) { memcpy(c->mem+a,&v,4); }
    static void hos_log(const char *s,...) {}
    static void oot_vr_tp_aim_shot(Ctx *c,uint64_t a,int p) { (void)c; (void)a; (void)p; }   /* (third person's: vr/items/aim) */
    static void prepare(Ctx*c) {
        memset(c,0,sizeof *c);
        uint32_t state=0x2000446;memcpy(c->mem+0x100+0x1f60,&state,4);
        if (quick) state=0x446, memcpy(c->mem+0x100+0x1f60,&state,4);
        uint64_t owner=0x80;memcpy(c->mem+0x800+0x18,&owner,8);
        vr_store(c,0x2000+0x1948+8,1.875f);
        vr_store(c,0x2000+0x1954+8,10);
    }
    '''
    checks=r'''
    int main(void) {
        Ctx c,other; prepare(&c);prepare(&other);
        hos_oot_vr_bow_probe(&c,0x800,0);hos_oot_vr_bow_probe(&c,0x2000,1);
        assert(fabsf(vr_load(&c,0x3948)-1.875f)<1e-6f && vr_load(&c,0x3950)==0);
        assert(vr_load(&c,0x3954)==10 && vr_load(&c,0x395c)==0);
        assert(vr_load(&c,0x393c)==0 && vr_load(&c,0x3940)==0 && vr_load(&c,0x3944)==0);
        hos_oot_vr_bow_probe(&c,0,3);prepare(&c);
        assert(rumbles==1 && rumble_hand==1);   // one strong buzz in the bow hand per shot
        vr_store(&c,0x3940,.5f); // native muzzle half a metre above the palm ray: within reach, so the shot starts at the VR bow
        hos_oot_vr_bow_probe(&c,0x800,0);hos_oot_vr_bow_probe(&c,0x2000,1);hos_oot_vr_bow_probe(&c,0x2000,2);
        float vx=vr_load(&c,0x3948),vy=vr_load(&c,0x394c),vz=vr_load(&c,0x3950);
        assert(fabsf(vx-1.875f)<1e-6f && vy==0 && vz==0);   // along the reticle's line itself, the game's speed
        assert(vr_load(&c,0x393c)==0 && vr_load(&c,0x3940)==0 && vr_load(&c,0x3944)==0);   // started at the VR handle
        assert(fabsf(vr_load(&c,0x3954)-sqrtf(100.25f))<1e-4f && vr_load(&c,0x3958)==0);   // the game's target distance along it
        assert(rumbles==2);   // (the second launch basis of the same shot: no second buzz)
        hos_oot_vr_bow_probe(&c,0,3);prepare(&c);
        vr_store(&c,0x3940,2.5f); // the game's muzzle 2.5 m away (beyond the 2 m limit): the old convergence on the 10 m point
        hos_oot_vr_bow_probe(&c,0x800,0);hos_oot_vr_bow_probe(&c,0x2000,1);
        vx=vr_load(&c,0x3948),vy=vr_load(&c,0x394c),vz=vr_load(&c,0x3950);
        float t=10.0f/vx;
        assert(fabsf(vx*t-10)<1e-5f && fabsf(2.5f+vy*t)<1e-5f && fabsf(vz*t)<1e-5f);
        assert(vr_load(&c,0x3940)==2.5f && fabsf(sqrtf(vx*vx+vy*vy+vz*vz)-1.875f)<1e-6f);
        hos_oot_vr_bow_probe(&c,0,3);prepare(&c);hos_oot_vr_bow_probe(&c,0x2000,1);
        assert(vr_load(&c,0x3950)==1.875f); // outside the shoot scope
        hos_oot_vr_bow_probe(&c,0x800,0);hos_oot_vr_bow_probe(&other,0x2000,1);
        assert(vr_load(&other,0x3950)==1.875f); // another guest context cannot use its token
        hos_oot_vr_bow_probe(&c,0,3);
        head.mode=0;prepare(&c);hos_oot_vr_bow_probe(&c,0x800,0);hos_oot_vr_bow_probe(&c,0x2000,1);
        assert(vr_load(&c,0x3950)==1.875f);
        head.mode=1;hos_oot_vr_bow_probe(&c,0x900,0);hos_oot_vr_bow_probe(&c,0x2000,1);
        assert(vr_load(&c,0x3950)==1.875f); // non-player owner
        enabled=0;hos_oot_vr_bow_probe(&c,0x800,0);hos_oot_vr_bow_probe(&c,0x2000,1);
        assert(vr_load(&c,0x3950)==1.875f); // expired/missing tracking
        enabled=1;packet[10]=0;prepare(&c);hos_oot_vr_bow_probe(&c,0x800,0);hos_oot_vr_bow_probe(&c,0x2000,1);
        assert(fabsf(vr_load(&c,0x3948)-1.875f)<1e-6f); // remapped fire needs no hard-coded physical grip
        prepare(&c);
        enabled=1;packet[3]=NAN;hos_oot_vr_bow_probe(&c,0x2000,1);
        assert(vr_load(&c,0x3950)==1.875f);
        // a shot drawn without the aim camera (0x446) is hand-aimed too
        packet[3]=1;quick=1;prepare(&c);hos_oot_vr_bow_probe(&c,0,3);hos_oot_vr_bow_probe(&c,0x800,0);hos_oot_vr_bow_probe(&c,0x2000,1);
        assert(fabsf(vr_load(&c,0x3948)-1.875f)<1e-6f && vr_load(&c,0x3950)==0);
        return 0;
    }
    '''
    with tempfile.TemporaryDirectory(prefix='oot-bow-launch-') as d:
        exe=str(Path(d)/'check')
        subprocess.run(['cc','-x','c','-std=c11','-','-lm','-o',exe],input=fixture+callback+checks,text=True,check=True)
        subprocess.run([exe],check=True)

def check_tp_aim():
    """third person (package B): the slingshot's and bow's reticle where the game's own launch would fly (a fake here), only
    while they aim (the aim camera's states); the Boomerang raised, 10 m beyond Link on the camera's line (its TargetPos);
    never for the hookshot (its laser shows it), nor when the item is not out, put away, at rest, or the answer is off the
    camera's line, behind it, past its reach or missing"""
    host = read('tp_aim_host')
    defines = host[host.index('#define kVrTpAimReach'):host.index('\n', host.index('#define kVrTpBoomReach')) + 1]
    start = host.index('typedef int (*VrTpAimCast)')
    end = host.index('\n}\n', host.index('static int oot_vr_tp_aim_point(')) + 3
    fixture = r"""
    #include <stdint.h>
    #include <string.h>
    #include <math.h>
    #include <assert.h>
    typedef struct { unsigned char mem[65536]; } Ctx;
    static float vr_load(Ctx *c, uint64_t a) { float f; memcpy(&f, c->mem + a, 4); return f; }
    static void vr_store(Ctx *c, uint64_t a, float f) { memcpy(c->mem + a, &f, 4); }
    static volatile uint64_t oot_vr_player_actor = 0x100;
    static volatile int oot_vr_selected_bow = -1;
    static int hos_vr_selected_is_slingshot, hos_vr_selected_is_hookshot, hos_vr_selected_is_boomerang;
    static void hos_log(const char *f, ...) { (void)f; }
    static float answer[3], asked_reach;
    static int answers = 1, casts;
    static int fake_cast(Ctx *c, uint64_t actor, float reach, float out[3]) {   /* (the game's line, f_82bb4c50) */
        (void)c, (void)actor;
        casts++, asked_reach = reach;
        memcpy(out, answer, sizeof answer);
        return answers;
    }
    """
    checks = r"""
    static Ctx g;
    static void camera(float x, float y, float z, float lx, float ly, float lz) {   /* (the game camera: view +0x38 / +0x44) */
        const float v[6] = {x, y, z, lx, ly, lz};
        for (int k = 0; k < 6; k++) vr_store(&g, 0x1000 + 0x38 + 4 * k, v[k]);
    }
    static int point(uint32_t state, float at[3]) { return oot_vr_tp_aim_point(&g, 0x1000, state, at, fake_cast); }
    int main(void) {
        float at[3] = {0, 0, 0};
        camera(0, 2, 0, 0, 2, -1);   /* (looking along -z from 2 m up) */
        answer[0] = 0, answer[1] = 2, answer[2] = -6;
        hos_vr_selected_is_slingshot = 1;
        assert(point(0x2000446, at) == 1 && at[2] == -6 && asked_reach == kVrTpAimReach);   /* drawn: where the line ends */
        assert(point(0x2000442, at) == 1);   /* raising */
        casts = 0;
        assert(point(0x441, at) == 0 && point(0x446, at) == 0 && casts == 0);   /* out, not aiming: no reticle, no line asked */
        assert(point(0x2000041, at) == 0 && point(0x82000446, at) == 0);   /* being taken out; being put away */
        answers = 0;
        assert(point(0x2000446, at) == 0);   /* the line could not be asked */
        answers = 1, answer[0] = 3;
        assert(point(0x2000446, at) == 0);   /* 3 m off the camera's line: not believed */
        answer[0] = 0, answer[2] = 5;
        assert(point(0x2000446, at) == 0);   /* behind the camera */
        answer[2] = -200;
        assert(point(0x2000446, at) == 0);   /* far past its reach */
        answer[2] = -6;
        hos_vr_selected_is_slingshot = 0, oot_vr_selected_bow = 1;
        assert(point(0x2000446, at) == 1);   /* the bow, the same */
        oot_vr_selected_bow = 0, hos_vr_selected_is_hookshot = 1;
        assert(point(0x600480, at) == 0 && point(0x2000446, at) == 0);   /* the hookshot: its own laser, no reticle */
        hos_vr_selected_is_hookshot = 0, hos_vr_selected_is_boomerang = 1;
        vr_store(&g, 0x100 + 796, 0.4f), vr_store(&g, 0x100 + 800, 1), vr_store(&g, 0x100 + 804, -3);   /* (Link 3 m ahead) */
        casts = 0;
        assert(point(0x180400, at) == 1 && casts == 0);   /* the Boomerang raised: its TargetPos, no line asked */
        assert(fabsf(at[0]) < 1e-5f && fabsf(at[1] - 2) < 1e-5f && fabsf(at[2] + 13) < 1e-4f);   /* 10 m beyond Link, on the line */
        assert(point(0x100400, at) == 0 && point(0x400, at) == 0);   /* at rest in his hand; in flight */
        camera(0, 2, 0, 0, 2, 0);
        assert(point(0x180400, at) == 0);   /* a camera with no direction */
        return 0;
    }
    """
    compile_run(fixture + defines + host[start:end] + checks)


def check_bow_string():
    """the string grab is proximity-gated with hysteresis; the draw is measured from the bow"""
    src = read('body')
    math = src[src.index('constexpr float kPi'):src.index('// ---- body state kept between frames')]
    bow = src[src.index('constexpr V3 kBowGrip'):src.index('// The game\'s standalone normal arrow')]

    compile_run(r'''
    #include <algorithm>
    #include <atomic>
    #include <cassert>
    #include <chrono>
    #include <cmath>
    #include <cstdint>
    #include <cstdio>
    #include <cstring>
    #include <memory>
    #include <mutex>
    #include <vector>
    ''' + math + r'''
    static int logs = 0, buzzes = 0;
    static void logf(const char* f, ...) { logs++; (void)f; }
    static void hookshot_vr_rumble(int, float, float) { buzzes++; }
    struct { int vr_bow_aim_yaw = 0, vr_bow_aim_pitch = 0, vr_bow_string_trigger = 0, vr_bow_string_fires = 1, vr_bow_grip_hold = 0; } opts;
    struct LinkHand { bool valid = true; float position[3]{}; float basis[9]{}; float trigger = 0, squeeze = 0; };
    struct LinkBody { uint32_t pose_seq = 1; int bow_hand = 0; float bow_scale = 1.3f; float bow_grip[3]{}; bool bow_equipped = true;
                      bool sling = false; bool menu = false; float wrist_deg[2][3]{}; LinkHand hand[2]; };
    #include "''' + str(generated('vk_vr_bow_rest.h')) + r'''"   // (the bow's rest pose, from your game)
    namespace rig { using mkwvk::vr::rig::bow_rest; struct Arm {}; struct Model {}; inline constexpr float sling_fork[3]{}, sling_pouch[3]{}; }
    static bool sling_place(uint64_t, const LinkBody&, Aff*) { return false; }   // (the slingshot: its own test, sling_pose)
    static V3 sling_point(const Aff&, const float (&)[3]) { return {}; }
    struct Entry { bool adult = true; };
    static std::mutex cache_mu;
    static Entry* canonical_rig(uint64_t, const LinkBody&) { return nullptr; }   // (the palm then is the controller's)
    static std::mutex bow_solution_mu;
    static float latest_bow[12];
    static int64_t latest_bow_at = 0;
    ''' + bow.replace('''        if (const Entry* source = canonical_rig(frame, body)) {''', '''        if (canonical_rig(frame, body)) {''').replace(
        src[src.index('            const rig::Model& model = source->adult'):src.index('            palm = add(source->origin, point(wrist, add(socket, surface)));')], '').replace(
        '            palm = add(source->origin, point(wrist, add(socket, surface)));\n', '') + r'''
    static LinkBody body;
    static uint64_t frame = 100;
    static BowPose step(float input, V3 string_at, int passes = 3) {
        frame++;
        body.pose_seq++;
        body.hand[1].squeeze = input;
        body.hand[1].position[0] = string_at.x, body.hand[1].position[1] = string_at.y, body.hand[1].position[2] = string_at.z;
        BowPose b;
        for (int i = 0; i < passes; i++) b = bow_pose(frame, body);   // (shadow, depth, colour, both eyes: one state change)
        return b;
    }
    int main() {
        // the bow hand at the origin; its grip frame: fingers wrap -y, so the aim (-y of the grip) is world -z
        body.hand[0].position[0] = 0, body.hand[0].position[1] = 1.4f, body.hand[0].position[2] = 0;
        const float basis[9] = {1, 0, 0, 0, 0, 1, 0, -1, 0};   // (columns: grip x, y, z in world)
        std::memcpy(body.hand[0].basis, basis, sizeof basis);
        BowPose b = step(0, {0, 1.4f, .5f});
        assert(b.ok && !b.held && b.draw == 0 && std::fabs(b.forward.z + 1) < 1e-3f);
        const V3 rest = b.rest;
        assert(rest.z > .1f && rest.z < .25f);   // (the string's centre behind the handle, Bow size 1.3)
        const auto behind = [&](float d) { return add(rest, {0, 0, d}); };
        const auto zr = [] { return ((int)latest_bow[10] & 2) != 0; };   // (what the host holds the game's ZR by)
        // pressed far from the string: no grab, even when then brought to it still pressed
        b = step(.9f, {0, 1.4f, .6f}); assert(!b.held);
        b = step(.9f, behind(.02f)); assert(!b.held);
        b = step(.1f, behind(.02f)); assert(!b.held);
        // pressed near it: held; between the thresholds still held (hysteresis); the draw from the rest
        b = step(.9f, behind(.03f)); assert(b.held && b.draw < .05f && !zr());
        b = step(.45f, behind(.30f)); assert(b.held && std::fabs(b.draw - .30f) < .01f && zr());   // (past 12 cm: the game's ZR)
        assert(((int)latest_bow[10] & 1) && std::fabs(latest_bow[9] - b.draw) < 1e-5f);
        assert(std::fabs(b.nock.z - behind(.30f).z) < 1e-4f);   // (the arrow's nock on the drawing hand)
        assert(std::fabs(b.forward.z + 1) < 1e-3f);
        // let go: one release (ZR let go: the game's shot), counted once however many passes ask
        b = step(.1f, behind(.30f), 6); assert(!b.held && !zr() && bow_string.releases == 1 && bow_string.cancels == 0);
        // the shot follows the line it was drawn along, not the bow hand's own pointing, for a moment after the let-go
        const V3 drawn{latest_bow[3], latest_bow[4], latest_bow[5]};
        { const float tilt[9] = {1, 0, 0, 0, .6f, .8f, 0, -.8f, .6f}; std::memcpy(body.hand[0].basis, tilt, sizeof tilt); }
        b = step(.1f, behind(.30f)); assert(dot(V3{latest_bow[3], latest_bow[4], latest_bow[5]}, drawn) > .9999f);
        for (int i = 0; i < 25; i++) b = step(.1f, behind(.30f));
        assert(dot(V3{latest_bow[3], latest_bow[4], latest_bow[5]}, drawn) < .99f);   // (then the hand's pointing again)
        std::memcpy(body.hand[0].basis, basis, sizeof basis);
        b = step(.1f, behind(.30f)); assert(bow_string.releases == 1);
        assert(b.nock.z == b.rest.z);
        // the trigger works as well as the grip (VR tab: grip or trigger)
        body.hand[1].trigger = .9f; b = step(0, behind(0)); assert(b.held); body.hand[1].trigger = 0;
        // a short draw let go: ZR was never held, no shot
        b = step(.1f, behind(.05f)); assert(!b.held && !zr() && bow_string.releases == 1 && bow_string.cancels == 1);
        // the menu before the commit: out of the hand, no shot
        b = step(.9f, behind(0)); assert(b.held);
        body.menu = true; b = step(.9f, behind(.05f)); assert(!b.held && !zr() && bow_string.cancels == 2); body.menu = false;
        // ... after the commit: the game's draw stays held (a ZR release would fire), the next real let-go fires
        b = step(.1f, behind(0)); b = step(.9f, behind(0)); b = step(.9f, behind(.3f)); assert(b.held && zr());
        body.menu = true; b = step(.9f, behind(.3f)); assert(!b.held && zr());
        b = step(.1f, behind(.3f)); assert(zr() && bow_string.releases == 1);   // (let go during the menu: nothing yet)
        body.menu = false; b = step(.1f, behind(.3f)); assert(!zr() && bow_string.releases == 2);
        // the string hand's tracking lost after the commit: not a let-go; its next real let-go fires
        b = step(.9f, behind(0)); b = step(.9f, behind(.3f)); assert(zr());
        body.hand[1].valid = false; b = step(.1f, behind(.3f)); assert(!b.held && zr() && bow_string.releases == 2);
        body.hand[1].valid = true; b = step(.9f, behind(.3f)); assert(zr());
        b = step(.1f, behind(.3f)); assert(!zr() && bow_string.releases == 3);
        // the string hand lost before the commit: no shot
        b = step(.9f, behind(0)); assert(b.held);
        body.hand[1].valid = false; b = step(.9f, behind(.05f)); assert(!b.held && !zr() && bow_string.cancels == 3); body.hand[1].valid = true;
        // the bow hand lost: the string drops, no shot
        b = step(.1f, behind(0)); b = step(.9f, behind(0)); assert(b.held);
        body.hand[0].valid = false; b = step(.9f, behind(.05f)); assert(!b.ok && bow_string.cancels == 4); body.hand[0].valid = true;
        b = step(.9f, behind(.05f)); assert(!b.held);   // (still pressed: no re-grab until let go and pressed near the string)
        // a handedness change before the commit: no shot
        b = step(.1f, behind(0)); b = step(.9f, behind(0)); assert(b.held);
        const LinkHand bow_hand = body.hand[0];
        body.bow_hand = 1;   // (step() moves hand 1, now the bow hand)
        b = step(.9f, behind(.05f)); assert(!b.held && bow_string.cancels == 5);
        body.bow_hand = 0, body.hand[0] = bow_hand;
        // the bow put away, or first person interrupted, even after the commit: ZR let go WITHOUT counting a release
        b = step(.1f, behind(0)); b = step(.9f, behind(0)); b = step(.9f, behind(.3f)); assert(zr());
        body.bow_equipped = false; b = step(.9f, behind(.3f)); assert(!b.held && !zr() && bow_string.cancels == 6);
        bow_model_frame = frame + 1;   // (the bow out but not aiming: the bow model drawn this frame)
        b = step(.1f, behind(0)); b = step(.9f, behind(0)); assert(b.held);
        frame += 20; b = step(.9f, behind(.3f)); assert(!b.held && !zr() && bow_string.cancels == 7);
        body.bow_equipped = true;
        // string firing off: the string is only visual, never the game's ZR
        opts.vr_bow_string_fires = 0;
        b = step(.1f, behind(0)); b = step(.9f, behind(0)); b = step(.9f, behind(.3f)); assert(b.held && !zr() && !((int)latest_bow[10] & 8));
        b = step(.1f, behind(.3f)); assert(!zr());
        opts.vr_bow_string_fires = 1;
        assert(bow_string.releases == 4);
        // the string hand's rumble: none at rest, more with the draw
        b = step(.9f, behind(0)); int before = buzzes; b = step(.9f, behind(.01f)); assert(buzzes == before);
        b = step(.9f, behind(.2f)); assert(buzzes == before + 1);
        b = step(.9f, behind(.2f)); b = step(.9f, behind(.21f)); assert(buzzes == before + 1);   // (not every frame: ~10 a second)
        b = step(.9f, behind(.45f)); assert(buzzes == before + 2);   // (a real change of draw: at once)
        // "Grip holds the bow": the bow hand's grip let go hides bow and arrow at once (not while the string is held)
        opts.vr_bow_grip_hold = 1;
        b = step(.1f, behind(0)); body.hand[0].squeeze = 1; b = step(.1f, behind(0)); assert(!b.hidden);
        body.hand[0].squeeze = 0; b = step(.1f, behind(0)); assert(b.hidden);
        body.hand[0].squeeze = 1; b = step(.1f, behind(0)); assert(!b.hidden);
        b = step(.9f, behind(0)); assert(b.held);
        body.hand[0].squeeze = 0; b = step(.9f, behind(.05f)); assert(!b.hidden);   // (the string is held: the host keeps it out)
        b = step(.1f, behind(.05f)); b = step(.1f, behind(.05f)); assert(b.hidden);
        opts.vr_bow_grip_hold = 0; body.hand[0].squeeze = 0;
        b = step(.1f, behind(0)); assert(!b.hidden);
        // the right-hand bow is turned half a turn about its string axis, never mirrored (faces would turn inside out)
        { const float d0 = det(b.target); body.bow_hand = 1; const LinkHand keep = body.hand[1]; body.hand[1] = body.hand[0];
          b = step(.1f, behind(0)); assert(det(b.target) > 0 && d0 > 0);
          body.bow_hand = 0; body.hand[1] = keep; b = step(.1f, behind(0)); }
        // the solution is committed without any bow draw: link_skin publishes it from the body
        assert(latest_bow_at > 0 && latest_bow[11] == (float)body.pose_seq);
    }
    ''', cpp=True)

def check_sling_pouch():
    """the slingshot aims and shoots from its pouch through its fork; the pouch follows the drawing hand, the straps stay on the fork"""
    src = read('body')
    math = src[src.index('constexpr float kPi'):src.index('// ---- body state kept between frames')]
    bow = src[src.index('constexpr V3 kBowGrip'):src.index('// The game\'s standalone normal arrow')]
    point_fn = src[src.index('V3 sling_point('):]
    point_fn = point_fn[:point_fn.index('\n') + 1]
    start = src.index('std::shared_ptr<const std::vector<float>> held_slingshot(')
    held = src[start:src.index('\n}\n', start) + 3]
    bow = bow.replace('        if (const Entry* source = canonical_rig(frame, body)) {', '        if (canonical_rig(frame, body)) {').replace(
        src[src.index('            const rig::Model& model = source->adult'):src.index('            palm = add(source->origin, point(wrist, add(socket, surface)));')], '').replace(
        '            palm = add(source->origin, point(wrist, add(socket, surface)));\n', '')
    compile_run(r'''
    #include <algorithm>
    #include <atomic>
    #include <cassert>
    #include <chrono>
    #include <cmath>
    #include <cstdint>
    #include <cstring>
    #include <memory>
    #include <mutex>
    #include <vector>
    ''' + math + r'''
    static void logf(const char*, ...) {}
    static void hookshot_vr_rumble(int, float, float) {}
    struct { int vr_bow_aim_yaw = 0, vr_bow_aim_pitch = 0, vr_bow_string_trigger = 0, vr_bow_string_fires = 1, vr_bow_grip_hold = 1; } opts;
    struct LinkHand { bool valid = true; float position[3]{}; float basis[9]{}; float trigger = 0, squeeze = 0; };
    struct LinkBody { uint32_t pose_seq = 1; int bow_hand = 0; float bow_scale = 1.3f; float bow_grip[3]{}; bool bow_equipped = true;
                      bool sling = true; bool menu = false; float root[3]{}; float wrist_deg[2][3]{}; LinkHand hand[2]; };
    #include "''' + str(generated('vk_vr_bow_rest.h')) + r'''"
    #include "''' + str(generated('vk_vr_slingshot.h')) + r'''"   // (the slingshot, from your game)
    namespace rig { using namespace mkwvk::vr::rig; struct Arm {}; struct Model {}; }
    struct Entry { bool adult = true; };
    static std::mutex cache_mu;
    static Entry* canonical_rig(uint64_t, const LinkBody&) { return nullptr; }
    static std::mutex bow_solution_mu;
    static float latest_bow[12];
    static int64_t latest_bow_at = 0;
    // (held as a hand would: the shot, model -x, ahead along world -z; the fork, model +z, up)
    static const Aff kHeld{columns({0, 0, 1}, {1, 0, 0}, {0, 1, 0}), {0, 1.2f, 0}};
    static bool sling_place(uint64_t, const LinkBody& body, Aff* out) {
        if (!body.hand[body.bow_hand].valid) return false;
        *out = kHeld;
        return true;
    }
    ''' + point_fn + bow + held + r'''
    static LinkBody body;
    static uint64_t frame = 100;
    static BowPose step(float input, V3 at) {
        frame++, body.pose_seq++;
        body.hand[1].squeeze = input;
        body.hand[1].position[0] = at.x, body.hand[1].position[1] = at.y, body.hand[1].position[2] = at.z;
        return bow_pose(frame, body);
    }
    static V3 moved(const std::vector<float>& before, const std::vector<float>& after, int slot, V3 p) {   // (where p goes)
        Aff inv;
        assert(invert(aff(before.data() + slot * 12), &inv));
        const Aff a = aff(after.data() + slot * 12);
        return add(apply(a.r, add(apply(inv.r, p), inv.t)), a.t);
    }
    int main() {
        body.hand[0].position[1] = 1.2f, body.hand[0].squeeze = 1;
        const float basis[9] = {1, 0, 0, 0, 0, 1, 0, -1, 0};
        std::memcpy(body.hand[0].basis, basis, sizeof basis);
        const V3 fork = sling_point(kHeld, rig::sling_fork), pouch = sling_point(kHeld, rig::sling_pouch);
        BowPose b = step(0, {0, 1.2f, .8f});
        // at rest: the shot leaves the fork, along pouch -> fork; the pouch is the string's centre
        assert(b.ok && !b.held && length(sub(b.palm, fork)) < 1e-5f && length(sub(b.rest, pouch)) < 1e-5f);
        assert(dot(b.forward, unit(sub(fork, pouch), {})) > .9999f && b.forward.z < -.9f);
        // grabbed at the pouch, pulled back and off to the side: aimed from the hand through the fork
        b = step(.9f, add(pouch, {0, 0, .02f})); assert(b.held);
        const V3 hand = add(pouch, {.08f, -.05f, .25f});
        b = step(.9f, hand); assert(b.held && b.draw > .2f && ((int)latest_bow[10] & 3) == 3);
        const V3 line = unit(sub(fork, hand), {});
        assert(dot(b.forward, line) > .9999f && length(sub(V3{latest_bow[0], latest_bow[1], latest_bow[2]}, fork)) < 1e-5f);
        assert(dot(V3{latest_bow[3], latest_bow[4], latest_bow[5]}, line) > .9999f);
        // the held model: the pouch on the drawing hand, each strap still on its fork tip and on the pouch
        std::vector<float> raw(rig::sling_matrices * 12, 0.0f);
        for (unsigned i = 0; i < rig::sling_matrices; i++) store(raw.data() + i * 12, Aff{identity(), {0, 1.2f, 0}});
        std::vector<float> rest(raw.size());
        for (unsigned i = 0; i < rig::sling_matrices; i++) store(rest.data() + i * 12, mul(kHeld, aff(rig::sling_rest[i])));
        auto out = held_slingshot(raw, {0, 0, 0}, frame, body);
        assert(out && out->size() == raw.size());
        for (int16_t s : rig::sling_pouch_slots) assert(length(sub(moved(rest, *out, s, pouch), hand)) < 1e-4f);
        const V3 tip_l = sling_point(kHeld, rig::sling_tip_l), tip_r = sling_point(kHeld, rig::sling_tip_r);
        for (int16_t s : rig::sling_strap_l) assert(length(sub(moved(rest, *out, s, tip_l), tip_l)) < 1e-4f);
        for (int16_t s : rig::sling_strap_r) assert(length(sub(moved(rest, *out, s, tip_r), tip_r)) < 1e-4f);
        const V3 anchor = sling_point(kHeld, rig::sling_anchor_l);
        assert(length(sub(moved(rest, *out, rig::sling_strap_l[0], anchor), moved(rest, *out, rig::sling_pouch_slots[0], anchor))) < 1e-4f);
        // let go: the shot (the game's ZR let go); the model back at rest
        b = step(.1f, hand); assert(!b.held && ((int)latest_bow[10] & 2) == 0 && bow_string.releases == 1);
        out = held_slingshot(raw, {0, 0, 0}, frame, body);
        for (size_t i = 0; i < raw.size(); i++) assert(std::fabs((*out)[i] - rest[i]) < 1e-5f);
        // the slingshot's grip let go: gone at once; another's slingshot, far from Link, and a bow: left alone
        body.hand[0].squeeze = 0; b = step(.1f, hand); assert(b.hidden);
        out = held_slingshot(raw, {0, 0, 0}, frame, body); assert(out && (*out)[3] == 0 && (*out)[12 * 12 + 7] == 0);
        body.hand[0].squeeze = 1; b = step(.1f, hand); body.root[0] = 10; assert(!held_slingshot(raw, {0, 0, 0}, frame, body));
        body.root[0] = 0; body.sling = false; assert(!held_slingshot(raw, {0, 0, 0}, frame, body));
    }
    ''', cpp=True)


def check_arrow_carry():
    """the carried arrow's tail stays on the VR string through the game's carry rotations"""
    s=read('body')
    math=s[s.index('constexpr float kPi'):s.index('// ---- body state kept between frames')]
    held=s[s.index('std::shared_ptr<const std::vector<float>> held_arrow('):s.index('// Hookshot VR: a bottle in the VR hand')]   # (held_bottle.inc follows)
    consts=s[s.index('constexpr V3 kBowGrip'):s.index('// The string: a proximity-gated grab')]
    hands=s[s.index('struct NativeHands {'):s.index('std::shared_ptr<const std::vector<float>> recent;')]
    compile_run(r'''
    #include <algorithm>
    #include <atomic>
    #include <cassert>
    #include <cmath>
    #include <cstdint>
    #include <cstring>
    #include <memory>
    #include <mutex>
    #include <vector>
    ''' + math + r'''
    static void logf(const char*,...){}
    struct LinkBody { float bow_scale=1;bool bow_equipped=true,child=false;float root[3]{}; };
    #include "''' + str(generated('vk_vr_bow_rest.h')) + r'''"   // (the bow's rest pose, from your game)
    namespace rig { using mkwvk::vr::rig::bow_rest; struct Arm {int wrist;float wrist_pos[3];};struct Model {Arm arm[2];};static Model adult,child; }
    struct Entry {bool adult=true;V3 origin{};std::vector<float>raw;};
    static Entry source;static bool available=true;
    static std::mutex cache_mu;
    static Entry* canonical_rig(uint64_t,const LinkBody&){return available?&source:nullptr;}
    ''' + hands + consts + r'''
    struct BowPose {bool ok=true;V3 palm{},forward{};M3 target{};V3 nock{};bool hidden=false,held=false;float draw=0;};
    static BowPose current;
    static BowPose bow_pose(uint64_t,const LinkBody&){return current;}
    static std::mutex bow_mu;
    static struct {uint64_t frame=~0ull;Aff world{};} native_bow;
    struct ArrowPlaced {Aff game{},vr{};uint64_t frame=~0ull;}; static std::mutex arrow_placed_mu; static ArrowPlaced arrow_placed;
    static std::atomic<uint64_t> arrow_placed_frame{~0ull};   // (held_arrow.inc: for the arrow's effects)
    ''' + held + r'''
    int main(){
     LinkBody body;rig::adult.arm[0].wrist=0;rig::adult.arm[1].wrist=1;
     source.raw.resize(24);const M3 I=axis_angle({0,1,0},0);
     store(source.raw.data(),{I,{0,0,0}});store(source.raw.data()+12,{I,{3,0,0}});
     current.target=I;current.palm={4,1.5f,2};current.nock={4.1f,1.5f,2};
     for(int origins=0;origins<2;origins++) {
      source.origin=origins?V3{100,20,-50}:V3{};const V3 o=origins?V3{90,10,-40}:V3{};
      body.root[0]=source.origin.x;body.root[1]=source.origin.y;body.root[2]=source.origin.z;
      remember_native_hands(rig::adult,source.raw,source.origin,v3(body.root),7);
      for(int k=0;k<100;k++) {
       const M3 r=axis_angle({.2f,.8f,.3f},k*.12f);
       const Aff arrow{r,add(source.origin,apply(r,{0,0,.41f}))};
       float raw[12];store(raw,shifted(arrow,mul(o,-1)));
       // No bow submitted yet, or fresh but a completely different native orientation during a roll.
       native_bow.frame=k%2?7:~0ull;native_bow.world={I,add(source.origin,{-3,0,0})};
       auto out=held_arrow(raw,o,7,body);assert(out);
       const Aff got=shifted(aff(out->data()),o);
       const V3 tail=add(got.t,apply(got.r,{0,0,-.41f}));
       assert(length(sub(tail,add(current.nock,{0,.034f,.011f})))<2e-5f);
       current.hidden=true;out=held_arrow(raw,o,7,body);assert(out);
       for(float f:*out) assert(f==0);current.hidden=false;
      }
     }
     source.origin={};body.root[0]=body.root[1]=body.root[2]=0;
     float raw[12];native_bow.frame=7;native_bow.world={I,{-3,0,0}};
     store(raw,{I,{4,2,3}});assert(!held_arrow(raw,{},7,body)); // flying or stuck elsewhere: never moved
     store(raw,{I,{0,0,.41f}});native_bow.frame=~0ull;body.bow_equipped=false;
     assert(!held_arrow(raw,{},7,body)); // no active/recent bow: no broad hand-item claiming
     native_bow.frame=5;current.hidden=true;assert(held_arrow(raw,{},7,body)); // put-away grace, hidden
     native_bow.frame=0;assert(!held_arrow(raw,{},7,body)); // grace remains finite
     body.bow_equipped=true;available=false;
     for(auto& h:native_hands)h.frame=~0ull;
     assert(!held_arrow(raw,{},7,body));
     // Canonical VR cache may still exist, but cannot make old native uploads fresh.
     available=true;remember_native_hands(rig::adult,source.raw,{},v3(body.root),3);
     assert(!held_arrow(raw,{},7,body));
     // Actual capture 184339756730725: previous frame 3746 left wrist,
     // next frame 3747 arrow tail. First color pass lacked correction while later pass had it.
     for(auto& h:native_hands)h.frame=~0ull;
     store(source.raw.data(),{I,{2.151f,91.934f,-46.201f}});
     store(source.raw.data()+12,{I,{2.211f,92.015f,-46.925f}});
     body.root[0]=2;body.root[1]=90;body.root[2]=-46;
     remember_native_hands(rig::adult,source.raw,{},v3(body.root),3746);
     store(raw,{I,{1.967f,91.993f,-46.352f+.41f}});native_bow.frame=~0ull;
     assert(held_arrow(raw,{},3747,body));
     assert(!held_arrow(raw,{},3750,body)); // actual-upload age, never canonical rig's advanced frame
     body.child=true;assert(!held_arrow(raw,{},3747,body));body.child=false;
     body.root[0]+=10;assert(!held_arrow(raw,{},3747,body)); // scene/actor displacement
    }
    ''',cpp=True)

def check_reticle_view():
    """only the bow reticle's own textures are hidden (needs a file from your game)"""
    source=read('reticle_draw')
    start=source.index('static VkImageView vr_reticle_view(Texture* tex, VkImageView original) {')
    end=source.index('\n}\n',start)+3
    game_tools()
    import bow_reticle
    data=game_file(bow_reticle.COMMON_TEXTURES)
    brti=dict(bow_reticle.textures(data))['BowPointer_00']
    table=struct.unpack_from('<Q',data,brti+0x70)[0]
    offset=struct.unpack_from('<Q',data,table)[0]
    mip=data[offset:offset+1024]
    fixture=r'''
    #include <vulkan/vulkan.h>
    #include <cstdint>
    #include <cstring>
    #include <unordered_map>
    #include <cassert>
    namespace vr { bool hidden=true;bool native_bow_reticle_hidden(){return hidden;} }
    struct Texture { VkFormat format=VK_FORMAT_BC4_UNORM_BLOCK; unsigned width=32,height=64,levels=1,layers=1,id=1;
        uint64_t gpu=1;VkImage image=(VkImage)8;std::unordered_map<uint64_t,VkImageView> views; };
    struct ParLock {};
    struct { VkDevice device=nullptr; } dev;
    static unsigned created=0;
    extern "C" VkResult vkCreateImageView(VkDevice,const VkImageViewCreateInfo* c,const VkAllocationCallbacks*,VkImageView* v) {
        assert(c->components.r==VK_COMPONENT_SWIZZLE_ZERO && c->components.a==VK_COMPONENT_SWIZZLE_ZERO);
        created++;*v=(VkImageView)99;return VK_SUCCESS;
    }
    static void logf(const char*,...) {}
    '''+ '\nstatic uint8_t mip[]={' + ','.join(map(str,mip)) + '};\n' + r'''
    static bool ub_read(uint64_t,void* out,size_t n) { memcpy(out,mip,n);return true; }
    '''
    checks=r'''
    int main() {
        Texture t;VkImageView original=(VkImageView)77;
        assert(vr_reticle_view(&t,original)==(VkImageView)99 && created==1);
        assert(vr_reticle_view(&t,original)==(VkImageView)99 && created==1);
        vr::hidden=false;assert(vr_reticle_view(&t,original)==original);
        vr::hidden=true;Texture other;other.id=2;mip[500]^=1;
        assert(vr_reticle_view(&other,original)==original && created==1);
        other.width=64;assert(vr_reticle_view(&other,original)==original);
    }
    '''
    compile_run(fixture+source[start:end]+checks,cpp=True)

# ======================================================================================================================
# The sword
# ======================================================================================================================

def check_sword_controls():
    """the sword from the back: a grip behind the shoulder takes it (not in the bow's hand), the first swing attacks, a held
    trigger keeps attacking while the blade moves, letting go puts it away with the game's own call (moving or not, beside
    someone or not), never SOUTH: hidden until the game has it away, withdrawn if taken again first, and after
    kVrStowTries calls that do not take, given up and logged, still no button"""
    host = read('controllers_host')
    hand = host[host.index('typedef struct {\n    int valid;'):host.index('int rx_vr_hands(HookshotVrHands *out);')]
    start = host.index('/* (OoT VR, first person, VR tab\'s Sword from the back)')
    end = host.index('\n}\n', host.index('static int vr_sword(const HookshotVrHand')) + 3
    compile_run(r'''
    #include <stdint.h>
    #include <string.h>
    #include <math.h>
    #include <assert.h>
    #include <stdio.h>
    #include <stdlib.h>
    enum { HOS_PAD_SOUTH = 1, HOS_PAD_WEST = 3 };
    static void hos_log(const char *f, ...) { (void)f; }
    static int mode = 0, drawn = 0, told = -1, asked = 0, cancelled = 0, guard = 1, by_blade = 1, sensitivity = 1, hits = 0;
    static int draws = 0;
    static int rx_option(int id, int otherwise) {
        return id == 915 ? mode : id == 1000 ? guard : id == 1001 ? by_blade : id == 1003 ? sensitivity : id == 1005 ? hits :
               otherwise;
    }
    int hos_vr_put_away_held(void) { asked++; return 1; }   /* (vr/items/gear/put_away_host.c: the game's call asked) */
    void hos_vr_put_away_cancel(void) { cancelled++; }
    int hos_vr_sword_draw(void) { draws++; return 1; }      /* (vr/items/sword/blade_hits.inc: the game's own draw asked) */
    void hos_vr_sword_trigger(int held) { (void)held; }   /* (blade_hits.inc: the spin's charge) */
    void rx_vr_rumble(int hand, float a, float t) { (void)hand, (void)a, (void)t; }
    ''' + hand + host[start:end] + r'''
    int rx_vr_head(VrSwordHead *h) { memset(h, 0, sizeof *h); h->active = 1, h->q[3] = 1; return 1; }   /* (at the origin, ahead -z) */
    void rx_vr_sword_hand(int hand) { told = hand; }
    int hos_vr_sword_drawn(void) { return drawn; }
    static int64_t t = 1000000000LL;
    static HookshotVrHand hands[2];
    static uint64_t step(int busy, float dx) {   /* (one read 11 ms later, the right hand moved dx metres along x) */
        uint64_t raw = 0;
        t += 11000000LL, hands[1].grip_p[0] += dx;
        vr_sword(hands, 1, 1, busy, &raw, t);
        return raw;
    }
    static int attacks(int n, int busy, float speed) {   /* attack presses begun over n reads at this speed (m/s) */
        int count = 0, was = 0;
        for (int i = 0; i < n; i++) { const int now = (step(busy, speed * .011f * ((i / 20) % 2 ? -1 : 1)) >> HOS_PAD_WEST) & 1; count += now && !was; was = now; }
        return count;
    }
    int main(void) {
        hands[0].valid = hands[1].valid = 1;
        hands[0].grip_p[0] = -.5f, hands[0].grip_p[2] = -.4f;              /* (the left hand ahead, out of the way) */
        hands[1].grip_p[0] = .2f, hands[1].grip_p[2] = .1f;                /* (the right hand behind the right shoulder) */
        step(1, 0);
        assert(!hos_vr_sword_claims(1));                                   /* the bow is in that hand: not the sword's */
        hands[1].squeeze = 1; step(1, 0); assert(told == -1);
        hands[1].squeeze = 0; step(-1, 0);
        assert(hos_vr_sword_claims(1) && !hos_vr_sword_claims(0));         /* behind the shoulder: the sword's grip */
        for (int i = 0; i < 8; i++) step(-1, .022f * (i % 2 ? -1 : 1));   /* (the hand already moving, 2 m/s, at the take) */
        hands[1].squeeze = 1; step(-1, .022f);
        assert(told == 1 && hos_vr_sword_holder() == 1);                   /* taken */
        assert(hos_vr_sword_claims(0));                                    /* the other hand holds the shield: it takes no item */
        guard = 0; step(-1, 0); assert(!hos_vr_sword_claims(0));           /* (Guard with the sword out: Off) */
        guard = 1; step(-1, 0); assert(hos_vr_sword_claims(0));
        assert(attacks(20, -1, 2.4f) == 1);                                /* the pull straight from the shoulder attacks */
        for (int i = 0; i < 40; i++) step(-1, 0);
        assert(attacks(60, -1, 1.2f) == 0);                                /* a slow move does not */
        hands[1].trigger = 1; assert(attacks(60, -1, 1.2f) == 0);          /* Swing: the trigger does nothing */
        mode = 1; assert(attacks(100, -1, 1.2f) >= 3);                     /* Hold trigger: attacks again and again as it moves */
        for (int i = 0; i < 40; i++) step(-1, 0);
        assert(attacks(40, -1, 0) == 0);                                   /* ... never while the hand holds still */
        hands[1].trigger = 0; assert(attacks(60, -1, 3.0f) == 0);          /* ... and a swing alone does not attack */
        mode = 2; assert(attacks(20, -1, 3.0f) == 1);                      /* Swing or trigger: a swing does */
        /* Swing detection = Blade: a long swing keeps the attack going (every kVrSwingGapNs); Hand: one press a swing */
        for (int i = 0; i < 40; i++) step(-1, 0);
        assert(attacks(60, -1, 2.4f) >= 2);
        for (int i = 0; i < 40; i++) step(-1, 0);
        assert(attacks(200, -1, 2.4f) == 1 + kVrSwingRepeats);             /* ... at most kVrSwingRepeats times a swing */
        for (int i = 0; i < 40; i++) step(-1, 0);
        {   /* a swing that slows to a drift (above rest, under kVrSwingOnFraction of the swing speed): no repeats */
            int count = 0, was = 0;
            for (int i = 0; i < 72; i++) {
                const int now = (step(-1, (i < 12 ? 2.4f : 1.15f) * .011f) >> HOS_PAD_WEST) & 1;
                count += now && !was, was = now;
            }
            assert(count == 1);
        }
        /* Swing sensitivity: 2.1 m/s attacks on High only, 2.4 on Medium, 2.4 not on Low */
        for (int i = 0; i < 40; i++) step(-1, 0);
        by_blade = 0;
        sensitivity = 2; assert(attacks(20, -1, 2.1f) == 1); for (int i = 0; i < 40; i++) step(-1, 0);
        sensitivity = 1; assert(attacks(20, -1, 2.1f) == 0); for (int i = 0; i < 40; i++) step(-1, 0);
        sensitivity = 0; assert(attacks(20, -1, 2.4f) == 0); for (int i = 0; i < 40; i++) step(-1, 0);
        sensitivity = 1; by_blade = 1;
        for (int i = 0; i < 40; i++) step(-1, 0);
        by_blade = 0; assert(attacks(60, -1, 2.4f) == 1); by_blade = 1;
        /* a wrist flick: the hand still, the blade swung (12 rad/s about the grip's x: its middle at about 4 m/s) */
        for (int pass = 0; pass < 2; pass++) {
            by_blade = !pass;
            for (int i = 0; i < 40; i++) step(-1, 0);
            int count = 0, was = 0;
            for (int i = 0; i < 25; i++) {
                const float a = 12.0f * .011f * (float)i;
                hands[1].grip_q[0] = sinf(a / 2), hands[1].grip_q[3] = cosf(a / 2);
                const int now = (step(-1, 0) >> HOS_PAD_WEST) & 1;
                count += now && !was, was = now;
            }
            assert(pass ? count == 0 : count >= 1);
            hands[1].grip_q[0] = 0, hands[1].grip_q[3] = 1;
        }
        by_blade = 1;
        /* let go (moving or beside someone: the call does not care): the game's own call at once, never SOUTH; hidden until
           the game has it away (headset 18 Nov: SOUTH while running rolled) */
        drawn = 1; hands[1].squeeze = 0; asked = 0;
        int south = 0;
        for (int i = 0; i < 20; i++) south |= (int)(step(-1, 0) >> HOS_PAD_SOUTH & 1);   /* (220 ms) */
        assert(hos_vr_sword_holder() == -1 && told == -2 && !south && asked == 1);
        assert(!hos_vr_sword_claims(0));                                   /* let go: the shield hand is free again */
        drawn = 0; step(-1, 0); assert(told == -1);                       /* the game has it away */
        for (int i = 0; i < 100; i++) step(-1, 0);
        assert(asked == 1);                                                /* away: nothing more asked */
        /* taken again before it was away: the request withdrawn, nothing more asked while it is held */
        hands[1].grip_p[0] = .2f, hands[1].grip_p[2] = .1f;   /* (back behind the shoulder) */
        hands[1].squeeze = 1; step(-1, 0); assert(told == 1);
        drawn = 1; hands[1].squeeze = 0; asked = 0; step(-1, 0); assert(asked == 1 && told == -2);
        hands[1].squeeze = 1; step(-1, 0); assert(told == 1 && cancelled == 1);
        for (int i = 0; i < 100; i++) step(-1, 0);
        assert(asked == 1);
        /* the game's call never taking: asked kVrStowTries times, then given up, shown in the game's hand again (told -1);
           never SOUTH */
        hands[1].squeeze = 0; asked = 0; south = 0;
        for (int i = 0; i < 400; i++) south |= (int)(step(-1, 0) >> HOS_PAD_SOUTH & 1);   /* (4.4 s) */
        assert(asked == kVrStowTries && !south && told == -1);
        /* Sword hits = Blade: no attack button, however it swings or the trigger is held, and none at the take (the
           player's rule: no button made in the background); taken while the game has it away, the game's own draw is asked,
           every kVrDrawGapNs, at most kVrDrawTries times a take; none while the game has it out */
        assert(draws == 0);                                                /* (Button: never asked) */
        hits = 1, drawn = 0, mode = 2;
        hands[1].grip_p[0] = .2f, hands[1].grip_p[2] = .1f;
        hands[1].squeeze = 1;
        int west = 0;
        for (int i = 0; i < 14; i++) west += (int)(step(-1, 0) >> HOS_PAD_WEST & 1);
        assert(hos_vr_sword_holder() == 1 && west == 0 && draws == 1);
        assert(attacks(60, -1, 3.0f) == 0 && attacks(60, -1, 1.2f) == 0);   /* swings: no button */
        hands[1].trigger = 1;                                              /* the trigger held: the spin's charge, the game's */
        int held = 0;                                                      /* attack button held while it is, the player's own */
        for (int i = 0; i < 20; i++) held += (int)(step(-1, 0) >> HOS_PAD_WEST & 1);
        assert(held == 20);
        hands[1].trigger = 0; step(-1, 0); assert(!(step(-1, 0) >> HOS_PAD_WEST & 1));   /* let go: let go */
        for (int i = 0; i < 400; i++) west += (int)(step(-1, 0) >> HOS_PAD_WEST & 1);
        assert(west == 0 && draws == kVrDrawTries);                        /* a few tries a take, never a button */
        hands[1].squeeze = 0; step(-1, 0); hands[1].grip_p[0] = .2f, hands[1].grip_p[2] = .1f;
        drawn = 1, draws = 0; hands[1].squeeze = 1;
        for (int i = 0; i < 100; i++) step(-1, 0);
        assert(hos_vr_sword_holder() == 1 && draws == 0);                  /* taken while it is out: nothing asked */
        return 0;
    }
    ''')

def check_stick_controls():
    """Deku Sticks in the hands: with the stick selected a grip takes it (the game's ZR until it is out, never the real
    right trigger), a swing attacks with ZR only once the game has it out, let go between, never WEST (it puts the stick
    away for the sword), letting go puts it away like the sword (the game's own call, never SOUTH, moving or not): hidden
    until the game has it away; taken again first, the request withdrawn, and if the game put it away all the same, ZR
    takes it out again; calls that never take are given up and logged, no button; the renderer is told the holding hand"""
    host = read('controllers_host')
    hand = host[host.index('typedef struct {\n    int valid;'):host.index('int rx_vr_hands(HookshotVrHands *out);')]
    start = host.index('/* (OoT VR, first person, VR tab\'s Sword from the back)')
    end = host.index('\n}\n', host.index('static int vr_stick(const HookshotVrHand')) + 3
    compile_run(r"""
    #include <stdint.h>
    #include <string.h>
    #include <math.h>
    #include <assert.h>
    enum { HOS_PAD_SOUTH = 1, HOS_PAD_WEST = 3, HOS_PAD_LTRIGGER = 8, HOS_PAD_RTRIGGER = 9 };
    static void hos_log(const char *f, ...) { (void)f; }
    static int mode = 2, out = 0, told = -100, claims = 0, asked = 0, cancelled = 0, hits = 0;
    static int rx_option(int id, int otherwise) { return id == 915 ? mode : id == 1005 ? hits : otherwise; }
    int hos_vr_sword_draw(void) { return 1; }
    void hos_vr_sword_trigger(int held) { (void)held; }   /* (blade_hits.inc: the spin's charge) */
    void rx_vr_rumble(int hand, float a, float t) { (void)hand, (void)a, (void)t; }
    static int stick_draws;
    int hos_vr_stick_draw(void) { stick_draws++; return 1; }   /* (vr/items/sword/blade_hits.inc: the stick's slot drawn) */
    int hos_vr_put_away_held(void) { asked++; return 1; }   /* (vr/items/gear/put_away_host.c: the game's call asked) */
    void hos_vr_put_away_cancel(void) { cancelled++; }
    """ + hand + host[start:end] + r"""
    int rx_vr_head(VrSwordHead *h) { memset(h, 0, sizeof *h); return 0; }
    void rx_vr_sword_hand(int hand) { (void)hand; }
    int hos_vr_sword_drawn(void) { return 0; }
    void rx_vr_stick_hand(int hand) { told = hand; }
    int hos_vr_stick_out(void) { return out; }
    int hos_vr_bomb_in_reach(int hand) { (void)hand; return 0; }
    static int ocarina = 0;
    int hos_vr_ocarina_claims(int hand) { return ocarina && hand == 1; }   /* (the right hand at the belt ocarina) */
    static int64_t t = 1000000000LL;
    static HookshotVrHand hands[2];
    static int on = 1;
    static uint64_t step(uint64_t raw, float dx) {   /* (one read 11 ms later, the right hand moved dx metres along x) */
        t += 11000000LL, hands[1].grip_p[0] += dx;
        vr_sword_shoulder[1] = claims;   /* (the hand behind the shoulder with Sword from the back: the sword's grip) */
        vr_stick(hands, on, 1, &raw, t);
        return raw;
    }
    #define BIT(raw, b) ((int)((raw) >> (b) & 1))
    int main(void) {
        const uint64_t real_zr = 1ULL << HOS_PAD_RTRIGGER;
        hands[0].valid = hands[1].valid = 1;
        /* selected: the real right trigger does not reach the game (the grips take the stick) */
        assert(!BIT(step(real_zr, 0), HOS_PAD_RTRIGGER) && told == -1);
        on = 0; assert(BIT(step(real_zr, 0), HOS_PAD_RTRIGGER));          /* not selected: the trigger is the game's */
        hands[1].squeeze = 1; step(0, 0); assert(hos_vr_stick_holder() == -1);   /* ... and a grip takes nothing */
        hands[1].squeeze = 0; step(0, 0); on = 1;
        claims = 1; hands[1].squeeze = 1; step(0, 0); assert(hos_vr_stick_holder() == -1);   /* the sword's grip behind the shoulder */
        hands[1].squeeze = 0; step(0, 0); claims = 0;
        ocarina = 1; hands[1].squeeze = 1; step(0, 0); assert(hos_vr_stick_holder() == -1);   /* the ocarina's grip at the belt */
        hands[1].squeeze = 0; step(0, 0); ocarina = 0;
        /* a grip takes it: the game's ZR held until it is out */
        hands[1].squeeze = 1;
        uint64_t raw = step(0, 0);
        assert(hos_vr_stick_holder() == 1 && told == 1 && BIT(raw, HOS_PAD_RTRIGGER));
        int west = 0, zr = 1;
        for (int i = 0; i < 20; i++) {   /* (3 m/s swings) */
            raw = step(0, .033f * (i % 2 ? -1 : 1));
            west |= BIT(raw, HOS_PAD_WEST), zr &= BIT(raw, HOS_PAD_RTRIGGER);
        }
        assert(!west && zr);                                                 /* not out yet: only the take's ZR, held */
        out = 1; raw = step(0, .033f); assert(!BIT(raw, HOS_PAD_RTRIGGER));  /* out: ZR let go, even mid-swing */
        assert(!BIT(step(0, -.033f), HOS_PAD_RTRIGGER));                     /* ... for a swing gap: the next is a new press */
        for (int i = 0; i < 40; i++) step(0, 0);
        int attacks = 0, was = 0;
        for (int i = 0; i < 20; i++) {
            raw = step(0, .033f);
            const int a = BIT(raw, HOS_PAD_RTRIGGER);
            attacks += a && !was, was = a, west |= BIT(raw, HOS_PAD_WEST);
        }
        assert(attacks == 1 && !west);                                       /* a swing: one ZR press, never WEST */
        hits = 1; for (int i = 0; i < 40; i++) step(0, 0);                   /* Sword hits = Blade: no ZR for a swing */
        for (int i = 0; i < 20; i++) attacks += BIT(step(0, .033f), HOS_PAD_RTRIGGER), west |= BIT(raw, HOS_PAD_WEST);
        assert(attacks == 1 && !west);
        hits = 0; for (int i = 0; i < 40; i++) step(0, 0);
        {   /* Sword hits = Blade: taken while the game has it away, its slot drawn by the game's call, never ZR */
            hits = 1; hands[1].squeeze = 0; step(0, 0); out = 0; step(0, 0);
            stick_draws = 0; hands[1].squeeze = 1;
            int zr = 0;
            for (int i = 0; i < 20; i++) zr |= BIT(step(0, 0), HOS_PAD_RTRIGGER);
            assert(hos_vr_stick_holder() == 1 && !zr && stick_draws >= 1);
            out = 1; step(0, 0); for (int i = 0; i < 40; i++) step(0, 0);
            hits = 0;
        }
        hands[1].trigger = 1;                                                /* Hold trigger: the holding hand's trigger is the attack's */
        assert(!BIT(step(real_zr, 0), HOS_PAD_RTRIGGER));
        hands[1].trigger = 0;
        /* let go (running too: a burning stick let go in Goron City waited for SOUTH and kept its fire in Link's game hand,
           headset 18 Nov): the game's own call at once, never SOUTH; hidden (-2) until the game has it away (-1) */
        hands[1].squeeze = 0; asked = 0;
        int south = 0;
        for (int i = 0; i < 20; i++) south |= BIT(step(0, 0), HOS_PAD_SOUTH);   /* (220 ms) */
        assert(asked == 1 && !south && told == -2 && hos_vr_stick_holder() == -1);
        out = 0; step(0, 0); assert(told == -1);
        /* taken again before it was away: withdrawn; the game put it away all the same just after: ZR takes it out again */
        hands[1].squeeze = 1; out = 1; step(0, 0); assert(hos_vr_stick_holder() == 1);
        hands[1].squeeze = 0; asked = 0; step(0, 0); assert(asked == 1 && told == -2);
        hands[1].squeeze = 1; raw = step(0, 0);
        assert(hos_vr_stick_holder() == 1 && cancelled == 1 && !BIT(raw, HOS_PAD_RTRIGGER));   /* (still out: nothing to take) */
        out = 0; raw = step(0, 0); assert(BIT(raw, HOS_PAD_RTRIGGER));      /* the game's last request took it: ZR again */
        out = 1; step(0, 0); for (int i = 0; i < 40; i++) step(0, 0);
        assert(asked == 1);                                                  /* held: nothing more asked */
        /* ... but gone later than that (burnt up, an event): not taken out again by itself */
        hands[1].squeeze = 0; step(0, 0); out = 0; step(0, 0); assert(told == -1);
        /* the game's call never taking: asked kVrStowTries times, then given up and shown in the game's hand (-1), no SOUTH */
        hands[1].squeeze = 1; out = 1; step(0, 0); assert(hos_vr_stick_holder() == 1);
        hands[1].squeeze = 0; asked = 0; south = 0;
        for (int i = 0; i < 400; i++) south |= BIT(step(0, 0), HOS_PAD_SOUTH);   /* (4.4 s) */
        assert(asked == kVrStowTries && !south && told == -1);
        out = 0; step(0, 0);
        /* none comes out (none left): ZR given up after its time */
        hands[1].squeeze = 1; assert(BIT(step(0, 0), HOS_PAD_RTRIGGER));
        t += kVrStickTakeNs; assert(!BIT(step(0, 0), HOS_PAD_RTRIGGER));
        hands[1].squeeze = 0; step(0, 0);
        assert(told == -1);                                                  /* let go with none out: nothing to put away */
        return 0;
    }
    """)


def check_ocarina_controls():
    """the ocarina in the VR hands: a grip at the belt ocarina takes it (and claims that grip from other items), held to the
    mouth it presses the game's D-pad Left once, while the game waits for notes the triggers, the right stick and the left
    controller's buttons are the game's note buttons (and nothing else), held away or let go it presses the game's B once
    (not in a song lesson), and it starts again only after leaving the mouth. Its presses are the game's own buttons, added
    after the controller mapping, never pad positions (the player's mapping moves those), and let go at once when the
    controllers cannot be read; when they are back, a grip closed meanwhile takes nothing"""
    host = read('controllers_host')
    hand = host[host.index('enum {\n    VR_BTN_LOWER'):host.index('int rx_vr_hands(HookshotVrHands *out);')]
    head = host[host.index('typedef struct { uint32_t seq; int active, mode, move_head'):host.index('int rx_vr_head(VrSwordHead *out);')]
    ocarina = (REPO / 'vr/items/ocarina/ocarina_host.c').read_text(encoding='utf-8')
    hid = (REPO / 'host/nn_hid.c').read_text(encoding='utf-8')
    assert 'buttons = hos_vr_ocarina_controls(buttons);' in hid, 'host/nn_hid.c no longer adds the ocarina\'s buttons'
    read_fn = host[host.index('static void vr_pad_read('):]
    assert read_fn.index('vr_ocarina_hands_lost();') < read_fn.index('return;'), \
        'vr_pad_read must let the ocarina\'s buttons go when the controllers cannot be read (review finding 9)'
    compile_run(r"""
    #include <stdint.h>
    #include <stdlib.h>
    #include <string.h>
    #include <math.h>
    #include <assert.h>
    enum { HOS_SW_A, HOS_SW_B, HOS_SW_X, HOS_SW_Y, HOS_SW_LS, HOS_SW_RS, HOS_SW_L, HOS_SW_R, HOS_SW_ZL, HOS_SW_ZR,
           HOS_SW_PLUS, HOS_SW_MINUS, HOS_SW_LEFT, HOS_SW_UP, HOS_SW_RIGHT, HOS_SW_DOWN, HOS_SW_N };
    enum { HOS_PAD_SOUTH, HOS_PAD_EAST, HOS_PAD_WEST, HOS_PAD_NORTH, HOS_PAD_BACK, HOS_PAD_GUIDE, HOS_PAD_START,
           HOS_PAD_LSTICK, HOS_PAD_RSTICK, HOS_PAD_LSHOULDER, HOS_PAD_RSHOULDER, HOS_PAD_UP, HOS_PAD_DOWN, HOS_PAD_LEFT,
           HOS_PAD_RIGHT, HOS_PAD_LTRIGGER, HOS_PAD_RTRIGGER };
    #define kVrPressNs 100000000LL
    static void hos_log(const char *f, ...) { (void)f; }
    static int64_t t = 1000000000LL;
    static int64_t hos_now_ns(void) { return t; }
    volatile int hos_oc_wait, hos_oc_last_kind;
    """ + hand + head + r"""
    int rx_vr_head(VrSwordHead *out);
    int hos_vr_sword_holder(void) { return -1; }
    static int rx_option(int id, int otherwise);
    """ + ocarina + r"""
    static int have_belt = 1, told = -100, event = 0;
    static float belt[3] = {0.1f, 0.6f, -0.1f}, world[2][3] = {{-1, 0, 0}, {1, 0, 0}};
    int rx_vr_head(VrSwordHead *h) {
        memset(h, 0, sizeof *h);
        h->active = 1, h->q[3] = 1, h->p[1] = 1.6f;   /* (looking straight ahead; the mouth at 0 1.52 -0.09) */
        return 1;
    }
    int rx_vr_ocarina_belt(float out[3]) { memcpy(out, belt, sizeof belt); return have_belt; }
    int rx_vr_hands_world(float out[42]) {
        memset(out, 0, 42 * sizeof *out);
        for (int h = 0; h < 2; h++) out[21 * h] = 1, memcpy(out + 21 * h + 1, world[h], sizeof world[h]);
        return 1;
    }
    void rx_vr_ocarina_hand(int hand) { told = hand; }
    static int song = -1, menu_row = 1;
    void rx_vr_ocarina_song(int playing) { song = playing; }
    static int rx_option(int id, int otherwise) { return id == 981 ? menu_row : otherwise; }
    int hos_vr_stick_holder(void) { return -1; }
    int hos_vr_event_playing(void) { return event; }
    static HookshotVrHand hands[2];
    static int playing;
    static uint64_t pads;   /* (what is left of the pad positions after a read) */
    static uint64_t step(uint64_t raw) {   /* (one read 11 ms later: the game's buttons it adds to none of the player's) */
        t += 11000000LL;
        vr_ocarina(hands, 1, 1, &raw, &playing, t);
        pads = raw;
        return hos_vr_ocarina_controls(0);
    }
    static uint64_t steps(int n, uint64_t raw, uint64_t *any) {   /* (n reads; the game's buttons pressed in any of them) */
        uint64_t last = 0;
        *any = 0;
        for (int i = 0; i < n; i++) last = step(raw), *any |= last;
        return last;
    }
    #define BIT(w, b) ((int)((w) >> (b) & 1))
    #define SW(b) (1ULL << HOS_SW_##b)
    static void at(int h, float x, float y, float z) { hands[h].grip_p[0] = x, hands[h].grip_p[1] = y, hands[h].grip_p[2] = z; }
    int main(void) {
        uint64_t any;
        hands[0].valid = hands[1].valid = 1;
        at(0, -.3f, 1.0f, 0), at(1, .3f, 1.0f, 0);
        /* a grip away from the belt ocarina takes nothing, and claims nothing */
        hands[1].squeeze = 1; step(0);
        assert(vr_ocarina_holder == -1 && !hos_vr_ocarina_claims(1) && told == -1);
        hands[1].squeeze = 0; step(0);
        /* the right hand at the belt ocarina: its grip is the ocarina's (other items leave it), and takes it */
        memcpy(world[1], belt, sizeof belt);
        step(0);
        assert(hos_vr_ocarina_claims(1) && !hos_vr_ocarina_claims(0));
        hands[1].squeeze = 1; step(0);
        assert(vr_ocarina_holder == 1 && told == 1 && hos_vr_ocarina_claims(1));
        /* held below the chin: not played, nothing pressed, the triggers are the player's */
        at(1, 0, 1.25f, -.1f);
        assert(steps(10, 1ULL << HOS_PAD_LTRIGGER, &any) == 0 && !any && pads == 1ULL << HOS_PAD_LTRIGGER && !playing);
        /* to the mouth: D-pad Left once */
        at(1, .02f, 1.5f, -.12f);
        steps(30, 0, &any);
        assert(any == SW(LEFT) && !playing);
        steps(30, 0, &any);
        assert(!any);   /* (once) */
        /* the game waits for notes: the controls are the notes, as the game's own buttons */
        hos_oc_wait = 1, hos_oc_last_kind = 1;
        assert(step((1ULL << HOS_PAD_LTRIGGER) | (1ULL << HOS_PAD_RTRIGGER) | (1ULL << HOS_PAD_WEST) | (1ULL << HOS_PAD_NORTH)) == 0);
        assert(playing && pads == 0);   /* (the controllers' own buttons do not reach the mapping) */
        hands[0].trigger = 1; assert(step(0) == SW(L)); hands[0].trigger = 0;
        hands[1].trigger = 1; assert(step(0) == SW(R)); hands[1].trigger = 0;
        hands[1].stick[1] = 1; assert(step(0) == SW(X));
        hands[1].stick[1] = .5f; assert(step(0) == SW(X));   /* (held while it stays out) */
        hands[1].stick[1] = 0; assert(step(0) == 0);
        hands[1].stick[0] = -1; assert(step(0) == SW(Y));
        hands[1].stick[0] = 1; assert(step(0) == SW(A));   /* (straight across: the new note) */
        hands[1].stick[0] = 0; hands[1].stick[1] = -1; assert(step(0) == 0);   /* (down plays nothing) */
        hands[1].stick[1] = 0; step(0);
        hands[0].trigger = 1; hands[0].buttons = VR_BTN_UPPER; assert(step(0) == (SW(L) | SW(UP)));    /* half step up */
        hands[0].buttons = VR_BTN_LOWER; assert(step(0) == (SW(L) | SW(DOWN)));                     /* half step down */
        hands[0].buttons = 0; hands[0].trigger = 0; step(0);
        assert(hos_vr_ocarina_controls(SW(PLUS)) == SW(PLUS));   /* (the player's own buttons pass) */
        /* a short wobble away from the mouth does not stop it; held away, the game's B once */
        at(1, .02f, 1.5f, -.45f);
        steps(10, 0, &any);
        assert(!any && !playing);
        steps(40, 0, &any);
        assert(any == SW(B));
        steps(40, 0, &any);
        assert(!any);   /* (once) */
        /* the game stopped; back to the mouth: it starts again */
        hos_oc_wait = 0;
        step(0);
        at(1, .02f, 1.5f, -.12f);
        steps(20, 0, &any);
        assert(any == SW(LEFT));
        hos_oc_wait = 1;
        step(0);
        assert(playing);
        /* the song menu: the other hand's grip is the game's ZL; the game's note wait stops meanwhile, the menu stays, and A
           (the right stick right) plays the chosen song */
        hands[0].squeeze = 1;
        assert(step(0) == SW(ZL) && playing);
        hos_oc_wait = 0, event = 1;
        assert(step(0) == SW(ZL) && playing && song == 0);
        hands[1].stick[0] = 1; assert(step(0) == (SW(ZL) | SW(A))); hands[1].stick[0] = 0;
        /* let go while the menu is open: the menu closes; B only once the game waits for notes again */
        hands[1].squeeze = 0;
        steps(5, 0, &any);
        assert(vr_ocarina_holder == -1 && !any);
        hos_oc_wait = 1;
        steps(5, 0, &any);
        assert(any == SW(B));
        assert(song == 0);
        hos_oc_wait = 0, event = 0;
        steps(20, 0, &any);
        hands[0].squeeze = 0;
        /* without the row: the other grip opens no menu */
        menu_row = 0;
        at(1, .3f, 1.0f, 0);
        hands[1].squeeze = 1; memcpy(world[1], belt, sizeof belt); step(0);
        at(1, .02f, 1.5f, -.12f);
        steps(20, 0, &any);
        assert(any == SW(LEFT));
        hos_oc_wait = 1;
        hands[0].squeeze = 1;
        assert(step(0) == 0 && playing);
        hands[0].squeeze = 0; menu_row = 1;
        /* a song played: the game ends its note wait and plays its song's event (the renderer is told: its effects); held
           at the mouth it does not start again */
        hos_oc_wait = 0, event = 1;
        steps(60, 0, &any);
        assert(!any && song == 1);
        event = 0;
        steps(60, 0, &any);
        assert(!any && song == 0);   /* (still at the mouth: it has to leave it first) */
        /* in a song lesson (the game's own wait, kind 2) the notes play, and letting go presses no B: the lesson's own */
        hos_oc_wait = 1, hos_oc_last_kind = 2;
        hands[0].trigger = 1; assert(step(0) == SW(L)); hands[0].trigger = 0;
        hands[1].squeeze = 0;
        steps(20, 0, &any);
        assert(vr_ocarina_holder == -1 && told == -1 && !any && !playing);
        /* on the field, let go while playing: B */
        hos_oc_last_kind = 1;
        hands[1].squeeze = 1; memcpy(world[1], belt, sizeof belt); step(0);
        assert(vr_ocarina_holder == 1);
        hands[1].squeeze = 0;
        steps(20, 0, &any);
        assert(any == SW(B) && vr_ocarina_holder == -1);
        /* no ocarina on the belt (none owned, or the row off): nothing taken */
        have_belt = 0;
        hands[1].squeeze = 1; step(0);
        assert(vr_ocarina_holder == -1 && !hos_vr_ocarina_claims(1));
        hands[1].squeeze = 0; step(0);
        /* chosen on the radial wheel: in the right hand; not gripped in time, back on the belt; gripped, held as usual */
        hos_vr_ocarina_take_from_radial(); step(0);
        assert(vr_ocarina_holder == 1 && told == 1 && hos_vr_ocarina_held());
        steps(150, 0, &any);
        assert(vr_ocarina_holder == 1);
        steps(50, 0, &any);
        assert(vr_ocarina_holder == -1 && !hos_vr_ocarina_held());
        hos_vr_ocarina_take_from_radial(); step(0);
        hands[1].squeeze = 1; steps(250, 0, &any);
        assert(vr_ocarina_holder == 1);
        hands[1].squeeze = 0; step(0);
        assert(vr_ocarina_holder == -1);
        /* the controllers cannot be read (vr_pad_read returns early) while a note and the song menu are held: the game's
           buttons let go at once, not held until they are back (review finding 9) */
        have_belt = 1; memcpy(world[1], belt, sizeof belt);
        at(1, .3f, 1.0f, 0); hands[1].squeeze = 1; step(0);
        at(1, .02f, 1.5f, -.12f); steps(20, 0, &any);
        hos_oc_wait = 1, hos_oc_last_kind = 1;
        hands[0].trigger = 1, hands[0].squeeze = 1;
        assert(step(0) == (SW(L) | SW(ZL)) && hos_vr_ocarina_controls(0) == (SW(L) | SW(ZL)));
        vr_ocarina_hands_lost();
        assert(hos_vr_ocarina_controls(0) == 0 && hos_vr_ocarina_controls(SW(PLUS)) == SW(PLUS));
        /* back with the holding grip still closed: it keeps the ocarina, and the controls are read afresh */
        assert(step(0) == (SW(L) | SW(ZL)) && vr_ocarina_holder == 1);
        hands[0].trigger = 0, hands[0].squeeze = 0;
        assert(step(0) == 0);
        /* let go while they are gone: back on the belt when they are back (the game's B, its ocarina was open) */
        vr_ocarina_hands_lost();
        hands[1].squeeze = 0;
        steps(20, 0, &any);
        assert(vr_ocarina_holder == -1 && any == SW(B));
        hos_oc_wait = 0; steps(20, 0, &any);
        /* a grip closed at the belt while they were gone is not a new squeeze: it takes nothing */
        vr_ocarina_hands_lost();
        at(1, .3f, 1.0f, 0); hands[1].squeeze = 1;
        step(0);
        assert(vr_ocarina_holder == -1);
        hands[1].squeeze = 0; step(0);
        hands[1].squeeze = 1; step(0);
        assert(vr_ocarina_holder == 1);   /* (a new squeeze after it takes it as ever) */
        return 0;
    }
    """)



def check_hook_controls():
    """the hookshot in a VR hand: a grip takes it out (ZR until it is out), letting go puts it away with the game's own call
    and never B, also while Link moves; let go mid-shot, only once the shot or pull is over; taken again before it is away,
    the request withdrawn, and if the game put it away all the same, ZR takes it out again; calls that never take are given
    up and logged, still no B"""
    host = read('hookshot_host')
    start = host.index('/* (host/nn_hid.c, through hos_oot_vr_bow_controls) the game\'s buttons while the hookshot is in a VR hand')
    end = host.index('\n}\n', host.index('uint64_t hos_vr_hook_buttons(')) + 3
    compile_run(r'''
    #include <stdint.h>
    #include <string.h>
    #include <assert.h>
    static int64_t clock_ns = 5000000000LL;
    static int64_t hos_now_ns(void) { return clock_ns; }
    static void hos_log(const char *f, ...) { (void)f; }
    static volatile int oot_vr_hook_hand = -1, oot_vr_hook_owned, oot_vr_quick_open;
    static volatile int64_t oot_vr_hook_out_at;
    static volatile uint32_t oot_vr_hook_state;
    static int oot_vr_hook_on(void) { return 1; }
    static float squeeze[2], trigger[2];
    static int rx_vr_hands(void *out) {
        uint32_t *w = (uint32_t *)out;
        for (int k = 0; k < 2; k++) memcpy(&w[1 + 26 * k + 21], &trigger[k], 4), memcpy(&w[1 + 26 * k + 22], &squeeze[k], 4);
        return 1;
    }
    int hos_vr_bomb_in_reach(int hand) { (void)hand; return 0; }
    int hos_vr_ocarina_claims(int hand) { (void)hand; return 0; }
    static int asked, cancelled;
    int hos_vr_put_away_held(void) { asked++; return 1; }   /* (vr/items/gear/put_away_host.c: the game's put-away asked) */
    void hos_vr_put_away_cancel(void) { cancelled++; }
    ''' + host[start:end] + r'''
    static const uint64_t ZR = 1ULL << 9, BB = 1ULL << 1;
    static int out = 0;
    static uint64_t frame(void) {   /* (one read 16 ms on; the game's frame sees it out, or not, meanwhile) */
        clock_ns += 16000000;
        if (out) oot_vr_hook_out_at = clock_ns - 8000000;
        return hos_vr_hook_buttons(0);
    }
    int main(void) {
        uint64_t r = 0;
        squeeze[1] = 1; assert(frame() & ZR);                       /* a grip takes it out: ZR held ... */
        out = 1, oot_vr_hook_state = 0x480; frame(); assert(!(frame() & ZR));   /* ... until it is out: it rests */
        /* let go (Link running or not: nothing here looks): the game's own call at once, never B; away, over */
        squeeze[1] = 0; asked = 0; r = frame();
        assert(asked == 1 && !(r & BB));
        for (int i = 0; i < 10; i++) r |= frame();
        assert(asked == 1 && !(r & BB));
        out = 0, oot_vr_hook_state = 0;
        for (int i = 0; i < 40; i++) r |= frame();
        assert(asked == 1 && !(r & (BB | ZR)));                     /* away: nothing more asked, nothing pressed */
        /* taken again before it was away: withdrawn; the game's request put it away all the same: ZR takes it out again */
        squeeze[1] = 1; frame(); out = 1, oot_vr_hook_state = 0x480; for (int i = 0; i < 5; i++) frame();
        squeeze[1] = 0; asked = cancelled = 0; frame(); assert(asked == 1);
        squeeze[1] = 1; assert(!(frame() & ZR) && cancelled == 1);  /* held again, still out: nothing to take */
        out = 0, oot_vr_hook_state = 0; r = 0;
        for (int i = 0; i < 12; i++) r |= frame();
        assert(r & ZR);                                             /* gone just after: ZR takes it out again */
        out = 1, oot_vr_hook_state = 0x480; for (int i = 0; i < 30; i++) frame();
        assert(asked == 1);                                         /* held: nothing more asked */
        /* let go mid-shot: kept until the shot is over, then the game's call */
        trigger[1] = 1; frame(); oot_vr_hook_state = 0x600480; frame();
        oot_vr_hook_state = 0x200000; trigger[1] = 0; frame();      /* (the trigger let go: the shot) */
        squeeze[1] = 0; asked = 0;
        for (int i = 0; i < 20; i++) frame();
        assert(asked == 0 && oot_vr_hook_hand == 1);                /* mid-shot: still held, nothing asked */
        oot_vr_hook_state = 0x480; frame(); frame();
        assert(asked == 1 && oot_vr_hook_hand == -1);               /* the shot over: put away */
        out = 0, oot_vr_hook_state = 0; for (int i = 0; i < 20; i++) frame();
        /* the game's call never taking: asked kVrStowTries times, then given up (logged), never B */
        squeeze[1] = 1; frame(); out = 1, oot_vr_hook_state = 0x480; for (int i = 0; i < 5; i++) frame();
        squeeze[1] = 0; asked = 0; r = 0;
        for (int i = 0; i < 120; i++) r |= frame();                 /* (1.9 s) */
        assert(asked == kVrStowTries && !(r & BB));
        return 0;
    }
    ''')


def check_boomerang_controls():
    """The Boomerang in the hands, each of the three throw styles, against a model of the game's states (headless 18 Nov:
    ZR raises it, let go throws it, B lowers a raised one to rest without a throw, caught it is at rest in Link's hand): a
    grip takes it out to rest (ZR until raised, then B) and never throws it; the style's trigger raises and aims it (the
    reticle 10 m along its line: the holding hand's, the empty hand's, the eyes') and letting go throws, the line frozen
    for the game hook, which puts the throw's TargetPos far along the line from the hand through the reticle (so the game's
    orbit camera does not bend it: headset 19 Nov, "down and to the right of the reticle"); back with the grip held it is in
    the hand again; letting go puts it away with the game's own call (raised: lowered first, never thrown; asked again while
    it is still out, then the game's B); a style saved when there were six (3-5) is the first; the model stays in the VR
    hand until the game lets it go"""
    host = read('boomerang_host')
    controls = host[host.index('#define kVrBoomActorName'):host.index('\n}\n', host.index('uint64_t hos_vr_boomerang_buttons(')) + 3]
    hook = host[host.index('static void vr_boom_target('):host.index('\n}\n', host.index('void hos_vr_boomerang_target(')) + 3]
    compile_run(r"""
    #include <stdint.h>
    #include <string.h>
    #include <math.h>
    #include <assert.h>
    typedef struct { uint64_t x[32]; } Ctx;
    typedef struct { uint32_t seq; int active, mode, move_head, level, height_cm, body_depth_cm; float q[4], p[3]; float lean,
                     tan_half_x, tan_half_y; int detail, foliage; } HookshotVrHead;
    static int64_t t = 1000000000LL;
    static int64_t hos_now_ns(void) { return t; }
    static void hos_log(const char *f, ...) { (void)f; }
    static int style = 0;
    static volatile int oot_vr_quick_open;
    static int rx_option(int id, int otherwise) { return id == 960 ? 1 : id == 961 ? style : otherwise; }
    static int rx_vr_head(HookshotVrHead *h) { memset(h, 0, sizeof *h); h->active = 1, h->mode = 1; return 1; }
    static float trigger[2], squeeze[2], pos[2][3];
    static int rx_vr_hands(void *out) {
        uint32_t *hw = out;
        for (int k = 0; k < 2; k++) memcpy(&hw[1 + 26 * k + 21], &trigger[k], 4), memcpy(&hw[1 + 26 * k + 22], &squeeze[k], 4);
        return 1;
    }
    int rx_vr_hands_world(float out[42]) {
        memset(out, 0, 42 * sizeof *out);
        for (int k = 0; k < 2; k++) out[21 * k] = 1, memcpy(out + 21 * k + 1, pos[k], 12);
        return 1;
    }
    /* (the pointing lines: the left hand's toward -z, the right's toward +x, from the hand) */
    int rx_vr_hook_ray(int hand, float out[6]) {
        if (hand < 0 || hand > 1) return 0;
        memcpy(out, pos[hand], 12);
        out[3] = hand ? 1.0f : 0, out[4] = 0, out[5] = hand ? 0 : -1.0f;
        return 1;
    }
    static int told = -100, told_reticle;
    static float told_at[3];
    void rx_vr_boomerang(int hand, int reticle, const float at[3]) { told = hand, told_reticle = reticle; memcpy(told_at, at, 12); }
    int hos_vr_sword_claims(int hand) { (void)hand; return 0; }
    int hos_vr_bomb_in_reach(int hand) { (void)hand; return 0; }
    int hos_vr_selected_is_boomerang = 1;
    static int asked, button_mode, call_ignored;   /* (the game's put-away call: asked; VR tab Put away: Button; it fails) */
    int hos_vr_put_away_held(void) { if (button_mode) return 0; asked++; return 1; }
    /* (the hook's guest memory: the stack and the actor, as floats by address from 0x1000) */
    static float memory[2048];
    static float vr_load(Ctx *c, uint64_t a) { (void)c; return memory[(a - 0x1000) / 4]; }
    static void vr_store(Ctx *c, uint64_t a, float f) { (void)c; memory[(a - 0x1000) / 4] = f; }
    static volatile uint64_t oot_vr_player_actor;
    static int oot_vr_equipment_ptr(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; }
    static void oot_vr_equipment_name(Ctx *c, uint64_t a, uint64_t o, char name[64]) { (void)c; (void)a; (void)o; name[0] = 0; }
    """ + controls + hook + r"""
    #define ZR (1ULL << 9)
    #define ZL (1ULL << 8)
    #define B (1ULL << 1)
    /* the game, as seen headless: Link's action state from the buttons it gets */
    static uint32_t game;
    static int zr_was, lowered, flight, throws, asked_seen, viewing;
    static void game_step(uint64_t b) {
        const int zr = (b & ZR) != 0, press = zr && !zr_was;
        if (game == 0 && press) game = 0x2180400, lowered = 0;                         /* taken out, raised */
        else if ((game & 0x80000) && (b & B)) game = 0x100400, lowered = 1;              /* lowered to rest */
        else if ((game & 0x80000) && !zr && !lowered) game = 0x400, flight = 80, throws++;   /* thrown */
        else if (game == 0x100400 && press) game = 0x180400, lowered = 0;                /* raised from rest */
        else if (game == 0x100400 && (b & B)) game = 0;                                  /* (B at rest: put away) */
        else if (game == 0x400 && --flight <= 0) game = 0x100400;                         /* back, caught: at rest */
        if (asked != asked_seen && !call_ignored && game == 0x100400) game = 0;           /* the put-away call */
        asked_seen = asked, zr_was = zr;
        oot_vr_boom_state = game;
    }
    static const float eye[3] = {0, 1, 0}, back[3] = {0, 0, 1};   /* (the headset: at 0 1 0, looking along -z) */
    static uint64_t step(uint64_t in) {
        t += 11000000LL;
        if (viewing) oot_vr_boom_note_view(eye, back);   /* (view_host.c tells it every view) */
        const uint64_t b = hos_vr_boomerang_buttons(in);
        game_step(b);
        return b;
    }
    static uint64_t wait(int ms) { uint64_t b = 0; for (int i = 0; i < ms / 11; i++) b = step(0); return b; }
    static void hook(float out[3]) {   /* (the game lets it go: the hook at its aim, the game's own point 1 1 1 at sp+0x14) */
        Ctx c = {{0}};
        c.x[21] = 0x2000, c.x[31] = 0x1000 - 0x14;
        oot_vr_boom_actor = 0x2000;
        memory[0] = memory[1] = memory[2] = 1;
        hos_vr_boomerang_target(&c);
        memcpy(out, memory, 12);
    }
    /* TargetPos on the line from the hand through the reticle, kVrBoomFar from the hand */
    static void on_line(const float at[3], const float hand[3], const float reticle[3]) {
        float d[3], e[3], n = 0, m = 0, dot = 0;
        for (int k = 0; k < 3; k++) d[k] = reticle[k] - hand[k], e[k] = at[k] - hand[k], n += d[k] * d[k], m += e[k] * e[k];
        n = sqrtf(n), m = sqrtf(m);
        for (int k = 0; k < 3; k++) dot += d[k] / n * e[k] / m;
        assert(fabsf(m - kVrBoomFar) < 1e-2f && dot > 0.99999f);
    }
    static void take(int hand) {   /* (a grip takes it out to rest, never thrown) */
        const int before = throws;
        squeeze[hand] = 1;
        for (int i = 0; i < 60; i++)
            if (!(step(0) & ZR) && game == 0x100400) break;
        assert(game == 0x100400 && throws == before && told == hand);
    }
    static void put_away(int hand) {   /* (let go: the game's call; away, over) */
        squeeze[hand] = 0;
        wait(200);
        assert(game == 0 && told == -1);
    }
    static void fly(void) { for (int i = 0; i < 120 && game != 0x100400; i++) step(0); }
    int main(void) {
        float at[3];
        pos[0][0] = -.3f, pos[1][0] = .3f;
        /* 0 Aim with Boomerang Hand */
        assert(!(step(ZR) & ZR));                                 /* (selected: the real ZR does not reach the game) */
        take(1);
        assert(hos_vr_boomerang_grips() == 2 && !told_reticle);   /* (the other grip is the game's again) */
        trigger[1] = 1; step(0); uint64_t b = step(0);
        assert((b & ZR) && game == 0x180400 && told_reticle && fabsf(told_at[0] - 10.3f) < 1e-3f);   /* raised, aimed */
        const float reticle0[3] = {told_at[0], told_at[1], told_at[2]};
        trigger[1] = 0; b = step(0);
        assert(!(b & ZR) && game == 0x400 && throws == 1 && told == 1);   /* thrown; the model in the hand until it leaves */
        hook(at); on_line(at, pos[1], reticle0);                 /* the game's aim: far along hand -> reticle */
        step(0); assert(told == -1);                              /* (left: the game's own flight) */
        fly(); step(0); assert(game == 0x100400 && told == 1);    /* back with the grip held: in the hand again */
        wait(1600); hook(at); assert(at[0] == 1 && at[1] == 1);   /* (a throw's aim holds only so long: later, the game's own) */
        put_away(1); assert(asked == 1 && throws == 1);
        /* let go while raised: lowered first (B, ZR held), then put away: never thrown */
        take(1); trigger[1] = 1; step(0); step(0); assert(game == 0x180400);
        squeeze[1] = 0; b = step(0); assert((b & ZR) && (b & B) && told == -2);
        trigger[1] = 0; wait(300); assert(game == 0 && throws == 1 && told == -1);
        /* 1 Aim with Empty Hand: held left, the right hand points, its trigger aims; the left trigger (ZL) is not the game's */
        style = 1; take(0); assert(!(step(ZL) & ZL));
        trigger[1] = 1; step(0); step(0); assert(told_reticle && fabsf(told_at[0] - 10.3f) < 1e-3f);
        const float reticle1[3] = {told_at[0], told_at[1], told_at[2]};
        trigger[1] = 0; step(0); assert(throws == 2);
        hook(at); on_line(at, pos[0], reticle1);                 /* (from the holding hand, the left, through the reticle) */
        fly(); put_away(0);
        /* 2 Aim with Headset: the reticle 10 m along the eyes' line, as view_host.c built the view; the throw through it */
        style = 2; viewing = 1;
        take(1); trigger[1] = 1; step(0);
        assert(told_reticle && fabsf(told_at[0]) < 1e-3f && fabsf(told_at[1] - 1) < 1e-3f && fabsf(told_at[2] + 10) < 1e-3f);
        const float reticle2[3] = {told_at[0], told_at[1], told_at[2]};
        step(0); trigger[1] = 0; step(0); assert(throws == 3);
        hook(at); on_line(at, pos[1], reticle2);
        fly(); put_away(1);
        viewing = 0; t += 300000000LL;                            /* (the view not told: no headset line, no reticle) */
        take(1); trigger[1] = 1; step(0); assert(!told_reticle);
        trigger[1] = 0; step(0); hook(at); assert(at[0] == 1);    /* (thrown with the game's own aim) */
        fly(); put_away(1);
        /* a style saved when there were six (3-5): the first */
        style = 4; take(1); trigger[1] = 1; step(0); step(0);
        assert(told_reticle && fabsf(told_at[0] - 10.3f) < 1e-3f);
        trigger[1] = 0; step(0); fly(); put_away(1); style = 0;
        /* the game's call does nothing: asked again every 300 ms, 4 times, then the game's B */
        call_ignored = 1; take(1); asked = 0; squeeze[1] = 0;
        int pressed = 0;
        for (int i = 0; i < 160; i++) pressed |= (step(0) & B) != 0;
        assert(asked == 4 && pressed && game == 0 && told == -1);
        /* VR tab Put away: Button: the game's B at once */
        call_ignored = 0; button_mode = 1; take(1); squeeze[1] = 0;
        assert(step(0) & B);
        wait(100); assert(game == 0);
        /* not first person or not selected: the buttons are the game's */
        hos_vr_selected_is_boomerang = 0; assert(step(ZR) & ZR);
        return 0;
    }
    """)


def check_stick_socket():
    """the held Deku Stick sits in the VR hand as the game holds a sword (wrist x Weapon_L x its stick bone), its leaf's
    cloth matrices moved with it, mirrored in the right hand; a stick lying in the world is left as the game draws it; let go
    it is hidden; held near its butt (STICK_GRIP), with the game side's actor placed as far up the fist; its flame drawn on
    the VR stick"""
    grip = re.search(r'^STICK_GRIP = \(([^)]*)\)', (REPO / 'vr/tools/generate/hand_sockets.py').read_text(), re.M)
    actor = re.search(r'#define kVrStickGripZ ([0-9.]+)f', (REPO / 'vr/items/deku_stick/deku_stick_host.c').read_text())
    assert grip and actor and abs(float(grip.group(1).split(',')[2]) + float(actor.group(1))) < 1e-6, \
        'the game side places the stick actor as far up the fist as the drawn stick is held (kVrStickGripZ = -STICK_GRIP[2])'
    src = read('body')
    math = src[src.index('constexpr float kPi'):src.index('// ---- body state kept between frames')]
    fn = src[src.index('// Hookshot VR: the Deku Stick in the VR hand that holds it'):src.index('struct Anchored { uint64_t vb0;')]
    compile_run(r"""
    #include <algorithm>
    #include <atomic>
    #include <cassert>
    #include <cmath>
    #include <cstdint>
    #include <cstring>
    #include <memory>
    #include <mutex>
    #include <vector>
    #include <chrono>
    #include "@RIG@"
    #include "@SOCKET@"
    #include "@FX@"
    namespace rig=mkwvk::vr::rig;
    namespace arrow_fx=mkwvk::vr::arrow_fx;
    """.replace('@RIG@', str(generated('vk_vr_rig_data.h'))).replace('@SOCKET@', str(generated('vk_vr_sword_socket.h'))).replace(
        '@FX@', str(REPO / 'vr/items/bow/arrow_effect_math.h')) +
    math + r"""
    static void logf(const char*, ...) {}
    struct LinkHand {bool valid=true;};
    struct LinkBody {float root[3]{};bool child=true;LinkHand hand[2];};
    struct Entry {std::vector<float>raw;std::shared_ptr<const std::vector<float>>out;V3 origin{};bool adult=false;uint64_t frame=0;};
    static Entry cache[2];static std::mutex cache_mu;
    static Entry* canonical_rig(uint64_t,const LinkBody&) {return &cache[1];}
    struct NativeHands {uint64_t frame=~0ull;bool adult=false;V3 root{},wrist[2]{};};static NativeHands native_hands[32];
    static int held=-1;
    static bool bottle_model(uint64_t,uint64_t) {return false;}   // (a bottle: vr/items/bottle, its own check)
    static std::shared_ptr<const std::vector<float>> held_bottle(uint64_t,const std::vector<float>&,V3,uint64_t,const LinkBody&) {return nullptr;}
    static int stick_hand() {return held;}
    """ + fn + r"""
    static float error(const Aff& a,const Aff& b) {
        float e=length(sub(a.t,b.t));
        for(int k=0;k<3;k++) e=std::max(e,length(sub(column(a.r,k),column(b.r,k))));
        return e;
    }
    int main() {
        static_assert(rig::deku_stick_matrices == 4 && rig::deku_stick_rigid_slot < rig::deku_stick_matrices, "layout");
        const rig::Model& m=rig::child;
        cache[1].adult=false;cache[1].origin={80,20,-40};
        cache[1].out=std::make_shared<std::vector<float>>(m.slots*12);
        const V3 o{79.5f,20.1f,-40.4f};   // (the draw's origin)
        LinkBody body;body.root[0]=80;body.root[1]=20;body.root[2]=-40;
        for(int f=0;f<40;f++) {
            const Aff wrist[2]={{axis_angle({.4f,.8f,.2f},.2f+f*.05f),{80.1f,21.1f,-39.8f}},
                                {axis_angle({-.3f,.9f,.1f},.4f-f*.03f),{79.7f,21.0f,-39.9f}}};
            for(int k=0;k<2;k++)
                store(const_cast<float*>(cache[1].out->data())+m.arm[k].wrist*12,shifted(wrist[k],mul(cache[1].origin,-1)));
            // (the game's stick: in Link's game left hand, swinging; its leaf matrices somewhere relative to it)
            const Aff game_model{axis_angle({0,1,0},f*.3f),{80.3f+.1f*std::sin(f*.4f),20.7f,-40.f}};
            const Aff game_rigid=mul(game_model,aff(rig::deku_stick_root));
            std::vector<float>raw(rig::deku_stick_matrices*12+12);   // (a pass that binds a larger buffer: the rest kept)
            for(size_t i=0;i<rig::deku_stick_matrices;i++) {
                const Aff leaf=i==rig::deku_stick_rigid_slot?game_rigid:
                    mul(game_rigid,Aff{axis_angle({1,0,0},.1f*(i+1)),{.01f*i,.02f,.03f}});
                store(raw.data()+i*12,shifted(leaf,mul(o,-1)));
            }
            raw[rig::deku_stick_matrices*12]=7;
            native_hands[0].frame=100+f;native_hands[0].adult=false;native_hands[0].root=v3(body.root);
            native_hands[0].wrist[0]=add(game_model.t,{.05f,0,0});native_hands[0].wrist[1]={81.5f,20.7f,-40.f};
            for(int side:{0,1}) {
                held=side;
                const auto got=held_stick(raw,o,100+f,body);assert(got && got->size()==raw.size());
                const Aff want=mul(wrist[side],aff(side?rig::deku_stick_right_child:rig::deku_stick_child));
                const Aff rigid=shifted(aff(got->data()+rig::deku_stick_rigid_slot*12),o);
                assert(error(rigid,want)<5e-5f);   // (float rounding 80 m from the origin)
                for(int k=0;k<3;k++) assert(std::fabs(length(column(rigid.r,k))-1)<2e-5f);
                assert(dot(cross(column(rigid.r,0),column(rigid.r,1)),column(rigid.r,2))>0);   // (a rotation, not a mirror)
                Aff inv,ginv;invert(rigid,&inv);invert(game_rigid,&ginv);
                for(size_t i=0;i<rig::deku_stick_matrices;i++) {   // (every matrix keeps its place against the stick)
                    const Aff now_rel=mul(inv,shifted(aff(got->data()+i*12),o));
                    const Aff was_rel=mul(ginv,shifted(aff(raw.data()+i*12),o));
                    assert(error(now_rel,was_rel)<5e-5f);
                }
                assert((*got)[rig::deku_stick_matrices*12]==7);
                // (its flame: an effect on the game's stick head is moved onto the VR stick's head; one further off is not)
                Aff root_inv;invert(aff(rig::deku_stick_root),&root_inv);
                const Aff vr_model=mul(rigid,root_inv);
                const V3 game_head=add(apply(game_model.r,v3(rig::deku_stick_head)),game_model.t);
                const V3 vr_head=add(apply(vr_model.r,v3(rig::deku_stick_head)),vr_model.t);
                float fx[16]={1,0,0,0,0,1,0,0,0,0,1,0,game_head.x,game_head.y+.05f,game_head.z,1},move[12];
                const float eye[3]={game_head.x,game_head.y+1.5f,game_head.z+.3f};
                assert(held_stick_effect_move(fx,eye,move));
                assert(length(sub(point(move,game_head),vr_head))<1e-4f);
                {   // (its embers halfway down the stick: moved with it too)
                    const V3 mid=add(apply(game_model.r,V3{0,0,.3f}),game_model.t);
                    float em[16];std::memcpy(em,fx,sizeof em);em[12]=mid.x,em[13]=mid.y,em[14]=mid.z;
                    assert(held_stick_effect_move(em,eye,move));
                }
                fx[13]+=.4f;assert(!held_stick_effect_move(fx,eye,move));   // (a torch's flame 0.4 m off the stick: left alone)
            }
            held=-1;   // (no VR grip: in the VR hand the game holds it in, never the game's pose)
            const auto own=held_stick(raw,o,100+f,body);
            assert(own && error(shifted(aff(own->data()+rig::deku_stick_rigid_slot*12),o),mul(wrist[0],aff(rig::deku_stick_child)))<5e-5f);
            held=-2;   // (let go, being put away: hidden)
            const auto hidden=held_stick(raw,o,100+f,body);
            assert(hidden && std::all_of(hidden->begin(),hidden->end(),[](float v){return v==0;}));
            held=1;   // (a stick lying in the world, a metre from Link's hands: the game's own)
            native_hands[0].wrist[0]=add(game_model.t,{1,0,0});native_hands[0].wrist[1]=add(game_model.t,{0,0,1});
            const auto world=held_stick(raw,o,100+f,body);
            assert(world && *world==raw);
        }
    }
    """, cpp=True)


def check_sword_socket():
    """the held Master Sword and Kokiri Sword stay fixed in the VR palm through draw, idle and swings"""
    src=read('body')
    math=src[src.index('constexpr float kPi'):src.index('// ---- body state kept between frames')]
    fn=src[src.index('// Hookshot VR: the Deku Stick in the VR hand that holds it'):src.index('const rig::Tunic* tunic_model(')]
    fn=fn[:fn.index('// Hookshot VR: the hookshot or longshot in the VR hand')] if '// Hookshot VR: the hookshot or longshot in the VR hand' in fn else fn
    # (Shield when guarding: Arm by default, the shield following the hand, since 22 Nov: the player's choice)
    assert 'int vr_shield_pose = 0;' in (REPO / 'vr/menu/vr_options.inc').read_text(encoding='utf-8')
    compile_run(r'''
    #include <algorithm>
    #include <atomic>
    #include <cassert>
    #include <cmath>
    #include <cstdint>
    #include <cstring>
    #include <memory>
    #include <mutex>
    #include <vector>
    #include "'''+str(generated('vk_vr_rig_data.h'))+r'''"
    #include "'''+str(generated('vk_vr_sword_socket.h'))+r'''"
    #include "'''+str(generated('vk_vr_shield_grip.h'))+r'''"
    #include <chrono>
    #include "'''+str(REPO / 'vr/items/bow/arrow_effect_math.h')+r'''"
    namespace rig=mkwvk::vr::rig;
    namespace arrow_fx=mkwvk::vr::arrow_fx;
    '''+math+r'''
    static void logf(const char*, ...) {}
    static struct { int vr_show_stowed=0; int vr_shield_pose=1; } opts;
    static bool guarding=false;
    static bool shield_guarding() {return guarding;}   // (the game's ZL held: vr::shield_guarding)
    struct LinkHand {bool valid=true;};
    struct LinkBody {float root[3]{};uint32_t equipment_known=1,equipment_drawn=1;uint64_t equipment_owner[2]{123,0};int hook_hand=-1;bool hook_out=false;bool shield_away=false;bool child=false;LinkHand hand[2];};
    struct Entry {std::vector<float>raw;std::shared_ptr<const std::vector<float>>out;V3 origin{};bool adult=true;uint64_t frame=0;};
    static Entry cache[2];static std::mutex cache_mu;
    static Entry* canonical_rig(uint64_t,const LinkBody&) {return &cache[1];}
    static int sword_hand() {return -1;}   // (no sword taken from the back: the game's own draw)
    struct NativeHands {uint64_t frame=~0ull;bool adult=false;V3 root{},wrist[2]{};};static NativeHands native_hands[32];
    static int stick_hand() {return -1;}
    static bool bottle_model(uint64_t,uint64_t) {return false;}   // (a bottle: vr/items/bottle, its own check)
    static std::shared_ptr<const std::vector<float>> held_bottle(uint64_t,const std::vector<float>&,V3,uint64_t,const LinkBody&) {return nullptr;}
    static std::atomic<int64_t> kept_shield_at{-1000};
    static int64_t test_ms=1000;
    static int64_t now_ms() {return test_ms;}
    '''+fn+r'''
    static float error(const Aff& a,const Aff& b) {
        float e=length(sub(a.t,b.t));
        for(int k=0;k<3;k++) e=std::max(e,length(sub(column(a.r,k),column(b.r,k))));
        return e;
    }
    int main() {
        // the shield's guard pose: a quarter turn about the grip (the mount, the shield's origin: it stays in the palm), its
        // face (+y) to where the fist points (-x); the left hand's the mirror image of the right's (its placement is
        // mirrored in x)
        {
            const Aff right=shield_guard_pose(false,1),left=shield_guard_pose(true,1);
            assert(length(right.t)<1e-6f && length(left.t)<1e-6f);   // (the grip stays where the fist holds it)
            assert(length(sub(apply(right.r,V3{0,1,0}),V3{-1,0,0}))<1e-5f);   // (the face to the fist)
            assert(length(sub(apply(right.r,V3{0,0,1}),V3{0,0,1}))<1e-5f);    // (its long side still up)
            const M3 X{{{-1,0,0},{0,1,0},{0,0,1}}};
            for(V3 p:{V3{.1f,.2f,.3f},V3{-.2f,.05f,.4f}}) {
                const V3 a=add(apply(left.r,p),left.t), b=apply(X,add(apply(right.r,apply(X,p)),right.t));
                assert(length(sub(a,b))<1e-5f);   // (left = X right X)
            }
            const Aff none=shield_guard_pose(false,0);
            assert(length(none.t)<1e-6f && length(sub(apply(none.r,V3{1,2,3}),V3{1,2,3}))<1e-5f);
        }
        {   // eased in while the game guards (Guard turn), out after; never (Arm, the default); always (Fist)
            assert(shield_turn_amount()==0);
            guarding=true; test_ms+=60; const float half=shield_turn_amount(); assert(half>.45f && half<.55f);
            test_ms+=100; assert(shield_turn_amount()==1);
            guarding=false; test_ms+=200; assert(shield_turn_amount()==0);
            opts.vr_shield_pose=0; guarding=true; test_ms+=200; assert(shield_turn_amount()==0);
            opts.vr_shield_pose=2; guarding=false; test_ms+=200; assert(shield_turn_amount()==1);
            opts.vr_shield_pose=1; test_ms+=200; assert(shield_turn_amount()==0);
        }
        for(int age=0;age<2;age++) {
            const rig::Model& m=age?rig::child:rig::adult;
            const Aff master=aff(age?rig::master_sword_child:rig::master_sword_adult);
            const Aff kokiri=aff(age?rig::kokiri_sword_child:rig::kokiri_sword_adult);
            cache[1].adult=!age;cache[1].origin={80,20,-40};
            cache[1].out=std::make_shared<std::vector<float>>(m.slots*12);
            for(int f=0;f<160;f++) {
                const Aff wrist{axis_angle({.4f,.8f,.2f},.2f+f*.01f),{80.1f,21.1f,-39.8f}};
                store(const_cast<float*>(cache[1].out->data())+m.arm[0].wrist*12,shifted(wrist,mul(cache[1].origin,-1)));
                const V3 item_origin{79.5f,20.1f,-40.4f};
                // Deliberate native draw/swing motion, slowly changing even in idle. Previous fitting/refinement must not leak in.
                const Aff native{axis_angle({0,1,0},f*.3f),{80.2f+.1f*std::sin(f*.4f),20.7f,-40.f}};
                std::vector<float>raw(12);store(raw.data(),shifted(native,mul(item_origin,-1)));
                LinkBody body;body.root[0]=80;body.root[1]=20;body.root[2]=-40;
                for(uint64_t vb:{56688ull,87120ull,86376ull,86880ull}) for(int pass=0;pass<3;pass++) {
                    const Aff want=mul(wrist,vb==86880ull?kokiri:master);
                    const auto got=anchored_skin(vb,raw,item_origin,100+f,body);assert(got);
                    assert(error(shifted(aff(got->data()),item_origin),want)<2e-5f);
                    const M3 r=aff(got->data()).r;
                    for(int k=0;k<3;k++) assert(std::fabs(length(column(r,k))-1)<2e-5f);
                }
            }
        }
    }
    ''',cpp=True)

    # Compile the exact equipped-grip block rather than assuming a raw button is always pressed.
    start=src.index('            if ((body.bow_equipped && side == body.bow_hand) ||')
    end=src.index('\n',src.index('h.trigger = .85f, h.squeeze = 1;',start))+1
    grip=src[start:end]
    compile_run(r'''
    #include <cstdint>
    #include <cassert>
    enum class Grip { buttons, game, ball };
    struct { int vr_held_grip=0; } opts;
    struct Body {bool bow_equipped=false;int bow_hand=0;uint32_t equipment_known=0,equipment_drawn=0;uint64_t equipment_owner[2]{};int hook_hand=-1;bool hook_out=false;bool shield_away=false;};
    struct Hand {float trigger=0,squeeze=0;Grip grip=Grip::buttons;};
    static int vr_sword=-1;
    static int sword_hand(){return vr_sword;}   // (-2: the sword taken from the back let go, the game still putting it away)
    static Hand pose_grip(const Body&body,int side){Hand h;
    '''+grip+r'''
    return h;}
    int main(){Body b;
    assert(pose_grip(b,0).squeeze==0 && pose_grip(b,0).grip==Grip::buttons);
    b.equipment_drawn=1;b.equipment_owner[0]=123;assert(pose_grip(b,0).squeeze==0);
    b.equipment_known=1;assert(pose_grip(b,0).squeeze==1 && pose_grip(b,0).trigger==.85f && pose_grip(b,0).grip==Grip::game);
    assert(pose_grip(b,1).squeeze==0);
    opts.vr_held_grip=1;assert(pose_grip(b,0).squeeze==1 && pose_grip(b,0).grip==Grip::buttons);   // (Held gear grip: Fist)
    vr_sword=-2;assert(pose_grip(b,0).squeeze==0 && pose_grip(b,0).grip==Grip::buttons);vr_sword=-1;   // (let go: empty at once)
    opts.vr_held_grip=0;b.equipment_drawn=0;assert(pose_grip(b,0).squeeze==0);
    b.bow_equipped=true;b.bow_hand=1;assert(pose_grip(b,1).grip==Grip::game && pose_grip(b,0).grip==Grip::buttons);
    }
    ''',cpp=True)

def check_shield_grip():
    """the shield in the left VR hand: its grip moved across its back in every level of detail, so the grip stays in the
    fingers and the plate and strap lie over the forearm, nothing mirrored; only for the left hand and a model the renderer
    recognises (else as before); needs --romfs"""
    if not ARGS.romfs:
        raise Skip('needs --romfs <folder>: the shields\' own vertices')
    _, game_files = game_tools()
    import numpy as np
    import shield_grip
    game = game_files.GameFiles(ARGS.romfs)
    header = generated('vk_vr_shield_grip.h')
    with tempfile.TemporaryDirectory(prefix='hookshot-vr-grip-') as folder:
        for name, rel in zip(shield_grip.MODELS, shield_grip.INPUTS):
            found = [(s, shield_grip.grip(s)) for s in shield_grip.shapes(game.read(rel))]
            s, G = next((s, g) for s, g in found if g)
            moved, shift, place, nudge = G['moved'], G['shift'], G['place'], G['nudge']
            assert len([g for _, g in found if g]) == 1, name
            assert 0 <= nudge <= .015 and (nudge > .01) == (name == 'Weapon_Shield_Deku'), (name, nudge)   # (headset 22 Nov)
            lods, n, stride = s['lods'], s['vertices'], s['stride']
            assert stride == (24 if name in ('Weapon_Shield_Deku', 'Weapon_Shield_Hylian_Dark') else 28), (name, stride)
            assert len(lods) == (3 if name == 'Weapon_Shield_Mirror' else 10), (name, len(lods))   # (past the 8 bfres_skeleton reads)
            firsts = [f for _, f in lods] + [n]
            for k in range(len(lods)):   # (a grip in every level of detail)
                assert any(firsts[k] <= v < firsts[k + 1] for v in moved), (name, k)
            raw = np.frombuffer(s['bytes'], np.uint8)[:n * stride].reshape(n, stride)
            pos = raw[:, :8].copy().view('<f2')[:, :3].astype(np.float64)
            parts = shield_grip.pieces([bytes(r) for r in raw[:, :6]], lods[0][0].reshape(-1, 3))
            grip0 = [v for v in moved if v < firsts[1]]
            plate = parts[0]
            strap = next(p for p in parts if (raw[p, s['attrs']['_w0'][1]] < 255).any())

            def left(v, moves):   # (into the right hand's frame: the left hand's placement is that frame mirrored in x)
                q = pos[v].copy()
                if moves:
                    q[:, 0] += shift
                q[:, 0] = -(q[:, 0] + place)
                return q

            def miss(a, b):
                return np.sqrt(((a[:, None, :] - b[None, :, :]) ** 2).sum(-1).min(1)).mean()
            # the grip where the right hand holds it (in the fingers); the plate where the right hand's is, its mirror image
            # (over the forearm: its extent to 5 mm, past the nudge toward the elbow; its vertices to 2 cm, the Deku
            # Shield's sampled unevenly); the strap over the forearm too (the Deku Shield's 2.8 cm nearer the wrist than on
            # the right arm, 1.3 cm after its nudge)
            assert np.abs(left(grip0, True).mean(0) - pos[grip0].mean(0)).max() < .005, name
            mirrored = left(plate, False)
            mirrored[:, 0] -= nudge
            assert np.abs(np.sort(mirrored[:, 0])[[0, -1]] - np.sort(pos[plate][:, 0])[[0, -1]]).max() < .005, name
            assert miss(mirrored, pos[plate]) < .02, (name, miss(mirrored, pos[plate]))
            assert abs(left(strap, False)[:, 0].mean() - pos[strap][:, 0].mean()) < .035, name
            # the moved mounts meet the shell as they did
            shell = pos[plate]
            near = lambda g: np.sqrt(((g[:, None, :] - shell[None, :, :]) ** 2).sum(-1).min(1)).min()
            g = pos[grip0].copy()
            g[:, 0] += shift
            assert abs(near(g) - near(pos[grip0])) < .004, (name, near(g), near(pos[grip0]))
            # the plate's back with its texture mirrored: under each moved mount foot, the texture that was under the old
            # mounts (their holes), to 1 cm; none of the front's islands (the emblem) changed
            a = s['attrs']
            normal = shield_grip.snorm10(raw[:, a['_n0'][1]:a['_n0'][1] + 4].copy().view('<u4')[:, 0])
            uv = raw[:, a['_u0'][1]:a['_u0'][1] + 4].copy().view('<u2').reshape(-1, 2).astype(np.float64) / 65535
            edited = uv.copy()
            for v, (u, w, _) in G['backs'].items():
                edited[v] = (u / 65535, w / 65535)
            tris0 = lods[0][0].reshape(-1, 3)
            isl = shield_grip.islands(tris0[np.isin(tris0[:, 0], plate)])
            back = np.concatenate([t for t in isl if normal[np.unique(t), 1].mean() < 0])
            for t in isl:
                if normal[np.unique(t), 1].mean() >= 0:
                    assert not set(np.unique(t).tolist()) & set(G['backs']), name
            A, B, C = pos[back[:, 0]], pos[back[:, 1]], pos[back[:, 2]]
            vs = np.unique(back)
            J = np.linalg.lstsq(np.c_[pos[vs][:, 0], pos[vs][:, 2], np.ones(len(vs))], uv[vs], rcond=None)[0][:2]
            per_m = np.sqrt(abs(np.linalg.det(J)))   # (texture per metre on the back)
            for f in sorted(grip0, key=lambda v: -pos[v, 1])[:40]:   # (the feet: nearest the plate)
                was = pos[f].copy()
                was[0] = 2 * G['middle'] - was[0]   # (the footprint mirrored about the grip's middle: the same, it is symmetric)
                now = pos[f].copy()
                now[0] += shift
                o, m = shield_grip.under_in_xz(was, A, B, C), shield_grip.under_in_xz(now, A, B, C)
                assert o and m, name
                hole = (o[1][:, None] * uv[back[o[0]]]).sum(0)
                assert np.linalg.norm((m[1][:, None] * edited[back[m[0]]]).sum(0) - hole) / per_m < .01, name
                assert np.linalg.norm((m[1][:, None] * uv[back[m[0]]]).sum(0) - hole) / per_m > .05, name   # (not so before)
            Path(folder, '%d.bin' % len(s['bytes'])).write_bytes(s['bytes'])
        src = read('body')
        math = src[src.index('constexpr float kPi'):src.index('// ---- body state kept between frames')]
        fn = src[src.index('// Hookshot VR: the Deku Stick in the VR hand that holds it'):src.index('const rig::Tunic* tunic_model(')]
        fn = fn[:fn.index('// Hookshot VR: the hookshot or longshot in the VR hand')] if '// Hookshot VR: the hookshot or longshot in the VR hand' in fn else fn
        compile_run(r'''
        #include <algorithm>
        #include <atomic>
        #include <cassert>
        #include <cmath>
        #include <cstdint>
        #include <cstdio>
        #include <cstring>
        #include <memory>
        #include <mutex>
        #include <string>
        #include <vector>
        #include "'''+str(generated('vk_vr_rig_data.h'))+r'''"
        #include "'''+str(generated('vk_vr_sword_socket.h'))+r'''"
        #include "'''+str(header)+r'''"
        #include <chrono>
        #include "'''+str(REPO / 'vr/items/bow/arrow_effect_math.h')+r'''"
        namespace rig=mkwvk::vr::rig;
        namespace arrow_fx=mkwvk::vr::arrow_fx;
        '''+math+r'''
        static void logf(const char*, ...) {}
        static struct { int vr_show_stowed=0; int vr_shield_pose=0; } opts;
        static bool shield_guarding() {return false;}
        struct LinkHand {bool valid=true;};
        struct LinkBody {float root[3]{};uint32_t equipment_known=2,equipment_drawn=2;uint64_t equipment_owner[2]{0,7};int hook_hand=-1;bool hook_out=false;bool shield_away=false;bool child=false;LinkHand hand[2];};
        struct Entry {std::vector<float>raw;std::shared_ptr<const std::vector<float>>out;V3 origin{};bool adult=true;uint64_t frame=0;};
        static Entry cache[2];static std::mutex cache_mu;
        static Entry* canonical_rig(uint64_t,const LinkBody&) {return &cache[1];}
        static int test_sword=-1;
        static int sword_hand() {return test_sword;}   // (1: the sword in the right VR hand, the shield in the left)
        struct NativeHands {uint64_t frame=~0ull;bool adult=false;V3 root{},wrist[2]{};};static NativeHands native_hands[32];
        static int stick_hand() {return -1;}
        static bool bottle_model(uint64_t,uint64_t) {return false;}
        static std::shared_ptr<const std::vector<float>> held_bottle(uint64_t,const std::vector<float>&,V3,uint64_t,const LinkBody&) {return nullptr;}
        static std::atomic<int64_t> kept_shield_at{-1000};
        static int64_t test_ms=1000;
        static int64_t now_ms() {return test_ms;}
        '''+fn+r'''
        static float error(const Aff& a,const Aff& b) {
            float e=length(sub(a.t,b.t));
            for(int k=0;k<3;k++) e=std::max(e,length(sub(column(a.r,k),column(b.r,k))));
            return e;
        }
        int main() {
            for(uint32_t h=0;h<0x7c00;h++) {   // (every finite half, both signs, through the edit's conversions)
                assert(to_half(from_half((uint16_t)h))==h);
                assert(to_half(from_half((uint16_t)(h|0x8000)))==(h|0x8000) || h==0);
            }
            int grips=0;
            for(const rig::ShieldGrip& g:rig::shield_grips) {
                if(!g.vb0) continue;
                grips++;
                FILE* f=std::fopen((std::string("'''+folder+r'''/")+std::to_string(g.vb0)+".bin").c_str(),"rb");assert(f);
                std::vector<uint8_t> game(g.vb0),out(g.vb0,0xEE);
                assert(std::fread(game.data(),1,g.vb0,f)==g.vb0);std::fclose(f);
                // not your game's model (one byte off): nothing written, held as before
                std::vector<uint8_t> other=game;other[g.vb0/2]^=1;
                assert(!shield_grip_edit(g.vb0,other.data(),g.vb0,out.data()) && !grip_recognised(&g));
                for(uint8_t b:out) assert(b==0xEE);
                assert(!shield_grip_edit(g.vb0,nullptr,0,nullptr) && !grip_recognised(&g));   // (not read: not known)
                assert(shield_grip_edit(g.vb0,game.data(),g.vb0,out.data()) && grip_recognised(&g));
                // only the grip's x moved, by the shift, and the back's texture coordinates and tangents set as listed; every
                // other byte (normals, colours, skin, the front's texture, the rest) the game's
                std::vector<bool> listed(g.vertices),back(g.vertices);
                for(uint32_t r=g.first;r<g.first+g.runs;r++)
                    for(uint32_t v=rig::shield_grip_runs[r][0];v<rig::shield_grip_runs[r][0]+rig::shield_grip_runs[r][1];v++) listed[v]=true;
                assert(g.backs>0);
                for(uint32_t b=g.back_first;b<g.back_first+g.backs;b++) {
                    const uint32_t* e=rig::shield_grip_backs[b];assert(e[0]<g.vertices && !listed[e[0]]);
                    back[e[0]]=true;
                    uint32_t uvw,t;std::memcpy(&uvw,&out[(uint64_t)e[0]*g.stride+g.uv_at],4);std::memcpy(&t,&out[(uint64_t)e[0]*g.stride+g.tangent_at],4);
                    assert(uvw==e[1] && t==e[2]);
                }
                for(uint64_t i=0;i<g.vb0;i++) {
                    const uint64_t v=i/g.stride,at=i%g.stride;
                    if(v<g.vertices && listed[v] && at<2) continue;
                    if(v<g.vertices && back[v] && ((at>=g.uv_at && at<g.uv_at+4) || (at>=g.tangent_at && at<g.tangent_at+4))) continue;
                    assert(out[i]==game[i]);
                }
                for(uint32_t v=0;v<g.vertices;v++) if(listed[v]) {
                    uint16_t a,b;std::memcpy(&a,&game[(uint64_t)v*g.stride],2);std::memcpy(&b,&out[(uint64_t)v*g.stride],2);
                    assert(std::fabs(from_half(b)-(from_half(a)+g.shift))<1.3e-4f);
                }
            }
            assert(grips==4);
            // the placement: the left hand's moved back by place (grip recognised), its palette listed for the replay; the
            // right hand's, and an unrecognised shield's, as before and not listed
            const struct { uint64_t vb0; bool adult; } held[]={{228696,false},{138320,true},{125916,true},{4872,true}};
            for(const auto& c:held) {
                const rig::Model& m=c.adult?rig::adult:rig::child;
                const rig::ShieldGrip* g=shield_grip_model(c.vb0);assert(g);
                cache[1].adult=c.adult;cache[1].origin={80,20,-40};
                auto out=std::make_shared<std::vector<float>>(m.slots*12);
                const Aff wl{axis_angle({.3f,.9f,.1f},.7f),{80.3f,21.f,-39.7f}},wr{axis_angle({.1f,.2f,.9f},-.4f),{79.7f,21.f,-39.9f}};
                store(out->data()+m.arm[0].wrist*12,shifted(wl,mul(cache[1].origin,-1)));
                store(out->data()+m.arm[1].wrist*12,shifted(wr,mul(cache[1].origin,-1)));
                cache[1].out=out;
                LinkBody body;body.root[0]=80;body.root[1]=20;body.root[2]=-40;body.child=!c.adult;
                const V3 o{79.9f,20.2f,-40.1f};
                std::vector<float> raw(24,0.0f);
                for(int k:{0,5,10,12,17,22}) raw[k]=1;
                const Aff left=aff(c.adult?rig::shield_left_adult:rig::shield_left_child),right=aff(c.adult?rig::shield_adult:rig::shield_child);
                for(int known:{1,2}) {
                    grip_known[g-rig::shield_grips].store(known);
                    for(int sword:{1,0}) {
                        test_sword=sword;
                        const auto got=anchored_skin(c.vb0,raw,o,100,body);assert(got && got->size()==24);
                        const bool moved=sword==1 && known==1;
                        const Aff want=sword==1?mul(wl,moved?mul(left,Aff{identity(),{g->place,0,0}}):left):mul(wr,right);
                        assert(error(shifted(aff(got->data()),o),want)<2e-5f);
                        assert(error(shifted(aff(got->data()+12),o),want)<2e-5f);   // (both palette entries: plate and strap)
                        assert(det(aff(got->data()).r)>.999f);   // (turned, never mirrored: nothing inside out)
                        assert(grip_palette_listed(got.get())==moved);
                        // the replay binds the moved grip for that palette's draws, of the shape with the grip only
                        assert((moved && shield_grip_of(c.vb0)!=nullptr)==(moved && c.vb0!=4872));
                    }
                }
                // Fist (turned all the time): the turn about the grip, so with the grip moved the palm's point of the moved
                // shield (model x = -place) is where it was unturned, and the face points where the fist does
                grip_known[g-rig::shield_grips].store(1);
                test_sword=1;
                const auto flat=anchored_skin(c.vb0,raw,o,101,body);
                opts.vr_shield_pose=2;test_ms+=200;   // (past the turn's easing)
                const auto turned=anchored_skin(c.vb0,raw,o,102,body);
                opts.vr_shield_pose=0;test_ms+=200;
                assert(flat && turned);
                const V3 palm{-g->place,0,0};
                const Aff a=shifted(aff(flat->data()),o),b=shifted(aff(turned->data()),o);
                assert(length(sub(add(apply(a.r,palm),a.t),add(apply(b.r,palm),b.t)))<1e-5f);
                assert(length(sub(apply(b.r,V3{0,1,0}),apply(a.r,V3{1,0,0})))<1e-5f);   // (its face (+y) to the fist (-x left: mirrored))
                grip_known[g-rig::shield_grips].store(0);
            }
        }
        ''', cpp=True)


def check_shield_beside_items():
    """the shield the game draws beside an item a VR hand holds (the Deku Stick, the Boomerang: drawn with them; a bottle,
    the ocarina, a bomb, the bow or slingshot) stays out of the VR hands while it is held and 0.6 s after, as with the
    hookshot; beside the sword taken from the back it is the other hand's, as before; the right hand does not close on it"""
    headset = read('headset')
    start = headset.index('    {   // The shield the game draws beside an item a VR hand holds')
    end = headset.index('\n    }\n', start) + 7
    block = headset[start:end]
    compile_run(r'''
    #include <atomic>
    #include <cassert>
    #include <cstdint>
    struct { std::atomic<bool> menu_visible{false}; } s;
    static int sword = -1, stick = -1, boomerang = -1, bottle = -1, ocarina = -1, bomb[2];
    static int sword_hand() { return sword; }
    static int stick_hand() { return stick; }
    static int boomerang_hand() { return boomerang; }
    static int bottle_hand() { return bottle; }
    static int ocarina_hand() { return ocarina; }
    static int held_bomb_kind(int hand) { return bomb[hand]; }
    struct Out { bool hook_out = false, shield_away = false, bow_equipped = false; };
    static int64_t now = 100000;
    static bool away(bool hook = false, bool bow = false) {
        Out out; out.hook_out = hook, out.bow_equipped = bow;
    ''' + block + r'''
        return out.shield_away;
    }
    int main() {
        assert(!away());                                   // (nothing held: the game's shield as the game draws it)
        assert(away(true));                                // (the hookshot's, as before)
        for (int item = 0; item < 6; item++) {             // (stick, boomerang, bottle, ocarina, bomb, bow or slingshot)
            for (int hand = 0; hand < 2; hand++) {
                now += 10000;
                int* held = item == 0 ? &stick : item == 1 ? &boomerang : item == 2 ? &bottle : item == 3 ? &ocarina : nullptr;
                if (held) *held = hand;
                if (item == 4) bomb[hand] = 1;
                assert(away(false, item == 5));
                if (held && item != 3) { *held = -2; now += 300; assert(away()); }   // (let go, the game putting it away)
                if (held) *held = -1;
                bomb[0] = bomb[1] = 0;
                now += 550; assert(away());                // (the game puts the shield away a moment after)
                now += 100; assert(!away());               // (then the game's own again)
            }
        }
        now += 10000; stick = 1; sword = 0; assert(!away());   // (beside the sword: the other hand's shield, held_gear.inc)
        sword = -1; s.menu_visible = true; assert(!away()); s.menu_visible = false; stick = -1;
    }
    ''', cpp=True)

    # the shield placement leaves the shield out while shield_away, before it is put in a VR hand
    body = read('body')
    hide = body.index('if (shield && body.shield_away) return std::make_shared<std::vector<float>>(raw.size(), 0.0f);')
    assert hide < body.index('if (semantic && shield && ((body.equipment_drawn & 2u) || vr_sword >= 0) && raw.size() == 24) {')
    assert 'body.hook_out' not in body
    # and the right hand's fingers do not close on a shield left out (the equipped-grip block, compiled)
    start = body.index('            if ((body.bow_equipped && side == body.bow_hand) ||')
    end = body.index('\n', body.index('h.trigger = .85f, h.squeeze = 1;', start)) + 1
    compile_run(r'''
    #include <cstdint>
    #include <cassert>
    enum class Grip { buttons, game, ball };
    struct { int vr_held_grip=0; } opts;
    struct Body {bool bow_equipped=false;int bow_hand=0;uint32_t equipment_known=2,equipment_drawn=2;uint64_t equipment_owner[2]{0,7};bool shield_away=false;};
    struct Hand {float trigger=0,squeeze=0;Grip grip=Grip::buttons;};
    static int sword_hand(){return -1;}
    static Hand pose_grip(const Body&body,int side){Hand h;
    ''' + body[start:end] + r'''
    return h;}
    int main(){Body b;
    assert(pose_grip(b,1).squeeze==1 && pose_grip(b,1).grip==Grip::game);   // (the game's shield out: its strap held)
    b.shield_away=true;assert(pose_grip(b,1).squeeze==0 && pose_grip(b,1).grip==Grip::buttons);   // (left out: the hand free)
    }
    ''', cpp=True)


# ======================================================================================================================
# Link's equipment (sword, shield, bow)
# ======================================================================================================================

def check_equipment_state():
    """Link's equipment state is read from the game's memory, never guessed"""
    source = read('equipment_host')
    source = source[:source.index('/* (OoT VR) main+0x2dfe260')]   # (the show/hide hook needs the guest register context: test_equipment_visible.py)
    compile_run(r'''
    #include <assert.h>
    #include <stdint.h>
    #include <stdio.h>
    static volatile int64_t oot_vr_equip_dump_t0;
    #include <string.h>
    #include <stddef.h>
    #include <stdlib.h>
    #define HOS_HEAP_BASE 0x1000000000ULL
    typedef struct { int unused; } Ctx;
    static unsigned char memory[0x20000];
    static uint64_t oot_vr_player_actor = HOS_HEAP_BASE + 0x1000;
    static uint64_t oot_vr_player_object = HOS_HEAP_BASE + 0x12000;
    static void *hos_guest_range(uint64_t a, uint64_t n) {
        return a >= HOS_HEAP_BASE && a-HOS_HEAP_BASE <= sizeof memory && n <= sizeof memory-(a-HOS_HEAP_BASE)
            ? memory + (a-HOS_HEAP_BASE) : NULL;
    }
    static uint64_t readn(uint64_t a, size_t n) { uint64_t v=0; void *p=hos_guest_range(a,n); assert(p); memcpy(&v,p,n); return v; }
    static uint64_t recomp_load64(Ctx *c,uint64_t a) { (void)c; return readn(a,8); }
    static uint32_t recomp_load32(Ctx *c,uint64_t a) { (void)c; return (uint32_t)readn(a,4); }
    static uint8_t recomp_load8(Ctx *c,uint64_t a) { (void)c; return (uint8_t)readn(a,1); }
    static void put(uint64_t a, uint64_t v, size_t n) { void *p=hos_guest_range(a,n); assert(p); memcpy(p,&v,n); }
    static int is_player=1;
    static void hos_log(const char *format, ...) { (void)format; }
    static int64_t hos_now_ns(void) { return 0; }
    static void oot_vr_gear_cull_publish(Ctx*c,uint32_t k,uint32_t d,const uint64_t ids[3]) {
        (void)c;(void)k;(void)d;(void)ids;
    }
    static uint64_t got_bracelet=~0ull;   // (the Goron Bracelet's actor, for the near test)
    static void oot_vr_gear_worn_publish(Ctx*c,uint64_t actor) { (void)c; got_bracelet=actor; }
    static uint64_t got_held=0;   // (the bow or slingshot out: kept from the near test, vr/items/gear/gear_near_host.c)
    void hos_vr_held_keep(Ctx*c,uint64_t actor) { (void)c; got_held=actor; }
    static uint64_t bracelet_on_link=0;   // (vr/items/sword/sword_host.c: the bracelet found by its own update on Link)
    uint64_t hos_vr_bracelet_on_link(void) { return bracelet_on_link; }
    static void oot_vr_visibility_equipment(Ctx*c,uint32_t s,uint32_t k,uint32_t d,const uint64_t ids[3]) {
        (void)c;(void)s;(void)k;(void)d;(void)ids;
    }
    static int hos_oot_is_player(Ctx *c,uint64_t a) {
        return is_player && a==oot_vr_player_object && recomp_load64(c,a)==0x84b6c460ULL;
    }
    static uint32_t got_seq,got_known,got_drawn;
    static uint64_t got_owner[2];
    static void rx_vr_equipment(uint32_t s,uint32_t k,uint32_t d,const uint64_t ids[2]) {
        got_seq=s;got_known=k;got_drawn=d;memcpy(got_owner,ids,sizeof got_owner);
    }
    ''' + source + r'''
    static uint64_t add(int slot, const char *name, int state) {
        uint64_t user=HOS_HEAP_BASE+0x3000, entry=HOS_HEAP_BASE+0x5000+0x100*slot;
        uint64_t actor=HOS_HEAP_BASE+0x6000+0x2000*slot, parts=actor+0x500, equipment=actor+0x800, resource=actor+0xc00;
        put(user+0x20+0x18*slot+8,entry,8);put(user+0x20+0x18*slot+0x10,123,4);
        put(entry+0x48,123,4);put(entry+0x40,actor,8);
        put(actor+0x2c,3,4);put(actor+0x10,resource,8);put(actor+0x270,parts,8);put(parts+0x1d8,equipment,8);
        memcpy(hos_guest_range(resource+0x58,64),name,strlen(name)+1);
        put(actor+0x260,resource+0x58,8);put(actor+0x20,resource+0x58,8);
        put(equipment+0x98,1,1);put(equipment+0x9c,state,4);
        return equipment;
    }
    int main(void) {
        Ctx c={0};uint64_t comp=HOS_HEAP_BASE+0x2000,user=HOS_HEAP_BASE+0x3000;
        put(oot_vr_player_actor,0x84b45e18ULL,8);put(oot_vr_player_object,0x84b6c460ULL,8);
        put(oot_vr_player_object+0x1d50+0x18,oot_vr_player_actor,8);
        put(oot_vr_player_actor+0x270,comp,8);put(comp+0x200,user,8);
        for(int i=0;i<8;i++)put(user+0x20+0x18*i+0x10,UINT32_MAX,4);
        const uint64_t sword=add(0,"Weapon_Sword_Master",1),shield=add(1,"Weapon_Shield_Hylian",0);
        oot_vr_equipment_snapshot(&c,5,1);assert(got_seq==5 && got_known==3 && got_drawn==1 && got_bracelet==0);
        put(sword+0x9c,0,4);put(shield+0x9c,1,4);
        oot_vr_equipment_snapshot(&c,6,1);assert(got_known==3 && got_drawn==2);
        oot_vr_equipment_snapshot(&c,7,0);assert(!got_known && !got_drawn && !got_owner[0] && !got_owner[1]);
        is_player=0;oot_vr_equipment_snapshot(&c,8,1);assert(!got_known);is_player=1;
        put(HOS_HEAP_BASE+0x5100+0x48,124,4);oot_vr_equipment_snapshot(&c,9,1);assert(got_known==1);
        put(HOS_HEAP_BASE+0x5100+0x48,123,4);put(shield+0x98,0,1);
        oot_vr_equipment_snapshot(&c,10,1);assert(got_known==1);put(shield+0x98,1,1);
        put(shield+0x9c,99,4);oot_vr_equipment_snapshot(&c,11,1);assert(got_known==1);put(shield+0x9c,0,4);
        put(user+0x13bc+1,2,1);oot_vr_equipment_snapshot(&c,12,1);assert(got_known==3 && got_drawn==2);
        add(2,"Weapon_Sheath_Master",1);oot_vr_equipment_snapshot(&c,13,1);assert(got_known==3);
        add(3,"Weapon_Shield_Mirror",1);oot_vr_equipment_snapshot(&c,14,1);assert(got_known==1);
        add(4,"Weapon_Shield_Hylian",1);oot_vr_equipment_snapshot(&c,15,1);assert(got_known==1);
        assert(!got_held);                                   // (no bow out: nothing held kept)
        add(5,"Weapon_Range_Bow",1);
        const uint32_t out=oot_vr_equipment_snapshot(&c,16,1);
        assert((out & 0x404u)==0x404u && got_bracelet==0);
        assert(got_held==HOS_HEAP_BASE+0x6000+0x2000*5);    // the bow out: kept from the near test (gear_near_host.c)
        // not among the slots (the 18 Nov headset run): the one found by its own update on Link, only while active
        bracelet_on_link=HOS_HEAP_BASE+0x7700;oot_vr_equipment_snapshot(&c,16,1);assert(got_bracelet==HOS_HEAP_BASE+0x7700);
        oot_vr_equipment_snapshot(&c,16,0);assert(got_bracelet==0);bracelet_on_link=0;
        // child Link's Goron Bracelet: found by its actor's name (worn: no drawn state), not taken for a sword/shield/bow
        add(7,"Armor_Child_Gauntlet_Goron",0);   // (slot 7: slot 6's actor would overlap the player object here)
        assert(oot_vr_equipment_snapshot(&c,16,1)==out && got_bracelet==HOS_HEAP_BASE+0x6000+0x2000*7);
        oot_vr_equipment_snapshot(&c,16,0);assert(got_bracelet==0);   // (not active: none)
        oot_vr_equipment_snapshot(&c,16,1);
        assert(oot_vr_bow_state_owned(0x2000446,out));
        assert(!oot_vr_bow_state_owned(0x2000446,1u|0x100u)); // sword drawing must not turn on the bow reticle
        assert(!oot_vr_bow_state_owned(0x2000446,4u)); // known holstered bow
        assert(!oot_vr_bow_state_owned(0,0x404u));
        assert(oot_vr_bow_state_owned(0x446,0)); // compatibility path if there is no authoritative equipment snapshot
        put(oot_vr_player_object+0x1d50+0x18,0,8);
        oot_vr_equipment_snapshot(&c,17,1);assert(!got_known); // mismatched actor/player pair must be rejected
        return 0;
    }
    ''')

    header = read('vr_h')
    body_types = header[header.index('struct LinkHand {'):header.index('/// The replay-side body state.')]
    compile_run('#include <cstdint>\n#include <cstddef>\n' + body_types + r'''
    // (each hand's world velocity and angular velocity added 16 November, for bombs in the hands: 24 bytes a hand; its aim
    // pose, 36 bytes, and the hookshot's hand added 17 November; the crawl and how far the view came down for it, 8 bytes,
    // 18 November)
    static_assert(sizeof(LinkBody) == 424, "tracking/equipment state layout remains unchanged");
    static_assert(sizeof(LinkBody)-offsetof(LinkBody,hand) == 240, "two tracked hands occupy the final 240 bytes");
    int main() { return 0; }
    ''', cpp=True)

def check_equipment_visible():
    """first person keeps a drawn sword/shield visible"""
    source = read('equipment_host')
    helpers = source[:source.index('static uint32_t oot_vr_equipment_snapshot(')]
    hook = source[source.index('/* (OoT VR) main+0x2dfe260'):]
    compile_run(r'''
    #include <assert.h>
    #include <stdint.h>
    #include <string.h>
    #define HOS_HEAP_BASE 0x1000000000ULL
    typedef struct { uint64_t x[32]; } Ctx;
    typedef struct { unsigned seq; int active, mode; } HookshotVrHead;
    static unsigned char memory[0x20000];
    static void *hos_guest_range(uint64_t a, uint64_t n) {
        return a >= HOS_HEAP_BASE && a - HOS_HEAP_BASE <= sizeof memory && n <= sizeof memory - (a - HOS_HEAP_BASE) ? memory + (a - HOS_HEAP_BASE) : 0;
    }
    static uint64_t rd(uint64_t a, int n) { uint64_t v = 0; void *p = hos_guest_range(a, n); assert(p); memcpy(&v, p, n); return v; }
    static uint64_t recomp_load64(Ctx *c, uint64_t a) { (void)c; return rd(a, 8); }
    static uint32_t recomp_load32(Ctx *c, uint64_t a) { (void)c; return (uint32_t)rd(a, 4); }
    static uint8_t recomp_load8(Ctx *c, uint64_t a) { (void)c; return (uint8_t)rd(a, 1); }
    static void recomp_store8(Ctx *c, uint64_t a, uint64_t v) { (void)c; unsigned char *p = hos_guest_range(a, 1); assert(p); *p = (unsigned char)v; }
    static void put(uint64_t a, uint64_t v, int n) { void *p = hos_guest_range(a, n); assert(p); memcpy(p, &v, n); }
    static void hos_log(const char *f, ...) { (void)f; }
    static int option = 1, mode = 1;
    static int rx_option(int id, int d) { return id == 902 ? option : d; }
    static int rx_vr_head(HookshotVrHead *h) { h->active = 1, h->mode = mode; return 1; }
    #define B HOS_HEAP_BASE
    static uint64_t oot_vr_player_actor = B + 0x1000;
    ''' + helpers + hook + r'''
    static uint64_t user = B + 0x3000, flag = B + 0x9000;
    static void item(const char *name, int drawn) {   // slot 2 -> entry -> actor (name) -> parts -> Equipment state
        const uint64_t handle = user + 0x20 + 0x18 * 2, entry = B + 0x5000, actor = B + 0x6000, parts = B + 0x7000, eq = B + 0x8000;
        put(handle + 8, entry, 8), put(handle + 0x10, 7, 4), put(entry + 0x48, 7, 4), put(entry + 0x40, actor, 8);
        strcpy((char *)hos_guest_range(B + 0xa000, 64), name), put(actor + 0x260, B + 0xa000, 8);
        put(actor + 0x270, parts, 8), put(parts + 0x1d8, eq, 8), put(eq + 0x9c, drawn ? 1 : 2, 4), put(user + 0x13bc + 2, 0, 1);
    }
    static int call(uint64_t u, int visible) {
        Ctx c = {{0}};
        put(flag, visible, 1);
        c.x[0] = u, c.x[1] = flag, c.x[2] = 2;
        hos_vr_equipment_visible(&c);
        return (int)rd(flag, 1);
    }
    int main(void) {
        put(oot_vr_player_actor + 0x270, B + 0x2000, 8), put(B + 0x2000 + 0x200, user, 8);
        item("Weapon_Sword_Master", 1);
        assert(call(user, 0) == 1);            // a drawn sword's hide request: kept visible
        assert(call(user, 1) == 1);            // show: untouched
        item("Weapon_Shield_Hylian", 1); assert(call(user, 0) == 1);
        item("Weapon_Sword_Master", 0); assert(call(user, 0) == 0);    // sheathed: untouched
        item("Weapon_Range_Bow", 1); assert(call(user, 0) == 0);       // another item: untouched
        item("Weapon_Sword_Master", 1);
        assert(call(B + 0x4000, 0) == 0);      // not Link's EquipmentUser (an enemy's): untouched
        mode = 0; assert(call(user, 0) == 0); mode = 1;               // third person
        option = 0; assert(call(user, 0) == 0); option = 1;           // option off
        assert(call(user, 0) == 1);
        return 0;
    }
    ''')

def check_put_away():
    """putting away what Link holds: asked from any thread, made on Link's own update (never another actor's) with the game's
    call (OneShotPlayerRequestAllEquipmentSheathed's, f_804c8690) on his actor, only while the game has something drawn in his
    EquipmentUser slots (its own drawn test: enabled, state 1), once per request; a request withdrawn before then is not made"""
    equipment = read('equipment_host')
    helpers = equipment[equipment.index('static int oot_vr_equipment_ptr('):equipment.index('static uint32_t oot_vr_equipment_snapshot(')]
    source = read('put_away_host')
    call = re.search(r'#define kVrPutAwayCall (0x[0-9a-f]+)ULL', source)
    if not call or call.group(1) != '0x804c8690':
        raise AssertionError('the put-away call is not f_804c8690 (OneShotPlayerRequestAllEquipmentSheathed\'s)')
    compile_run(r'''
    #include <assert.h>
    #include <stdint.h>
    #include <string.h>
    #include <stddef.h>
    #define HOS_HEAP_BASE 0x1000000000ULL
    typedef struct { int unused; } Ctx;
    static unsigned char memory[0x20000];
    static uint64_t oot_vr_player_actor = HOS_HEAP_BASE + 0x1000;
    static uint64_t oot_vr_player_object = HOS_HEAP_BASE + 0x12000;
    static void *hos_guest_range(uint64_t a, uint64_t n) {
        return a >= HOS_HEAP_BASE && a-HOS_HEAP_BASE <= sizeof memory && n <= sizeof memory-(a-HOS_HEAP_BASE)
            ? memory + (a-HOS_HEAP_BASE) : NULL;
    }
    static uint64_t readn(uint64_t a, size_t n) { uint64_t v=0; void *p=hos_guest_range(a,n); assert(p); memcpy(&v,p,n); return v; }
    static uint64_t recomp_load64(Ctx *c,uint64_t a) { (void)c; return readn(a,8); }
    static uint32_t recomp_load32(Ctx *c,uint64_t a) { (void)c; return (uint32_t)readn(a,4); }
    static uint8_t recomp_load8(Ctx *c,uint64_t a) { (void)c; return (uint8_t)readn(a,1); }
    static void put(uint64_t a, uint64_t v, size_t n) { void *p=hos_guest_range(a,n); assert(p); memcpy(p,&v,n); }
    static void hos_log(const char *format, ...) { (void)format; }
    static int64_t now_ns = 1000000000LL;
    static int64_t hos_now_ns(void) { return now_ns; }
    static int rx_option(int id, int otherwise) { (void)id; return otherwise; }   /* (env_int: the tester's, the test unset) */
    static int calls = 0;
    static uint64_t called_fn, called_x0;
    static void vr_bomb_call(Ctx *c, uint64_t fn, uint64_t x0, uint64_t x2, uint64_t x3, float s0, const float *a, const float *b9) {
        (void)c; (void)x2; (void)x3; (void)s0; (void)a; (void)b9;
        calls++, called_fn = fn, called_x0 = x0;
    }
    typedef struct { int item; const char *icon, *own; int age; } VrRadialItem;
    static const VrRadialItem vr_radial_items[] = {{0, "Item_DekuStick", "Item_DekuStick", 2}};
    #define VR_RADIAL_ITEMS 1
    static volatile int oot_vr_radial_pending = -1;
    ''' + helpers + source + r'''
    static uint64_t add(int slot, const char *name, int state) {   /* (as check_equipment_state's) */
        uint64_t user=HOS_HEAP_BASE+0x3000, entry=HOS_HEAP_BASE+0x5000+0x100*slot;
        uint64_t actor=HOS_HEAP_BASE+0x6000+0x1000*slot, parts=actor+0x500, equipment=actor+0x800, resource=actor+0xc00;
        put(user+0x20+0x18*slot+8,entry,8);put(user+0x20+0x18*slot+0x10,123,4);
        put(entry+0x48,123,4);put(entry+0x40,actor,8);
        put(actor+0x270,parts,8);put(parts+0x1d8,equipment,8);
        memcpy(hos_guest_range(resource+0x58,64),name,strlen(name)+1);
        put(actor+0x260,resource+0x58,8);put(actor+0x20,resource+0x58,8);
        put(equipment+0x98,1,1);put(equipment+0x9c,state,4);
        return equipment;
    }
    int main(void) {
        Ctx c={0};
        const uint64_t link=oot_vr_player_actor, comp=HOS_HEAP_BASE+0x2000, user=HOS_HEAP_BASE+0x3000;
        put(link+0x270,comp,8);put(comp+0x200,user,8);
        for(int i=0;i<8;i++)put(user+0x20+0x18*i+0x10,UINT32_MAX,4);
        put(oot_vr_player_object+0x1f60,0x408,4);
        const uint64_t stick=add(1,"Weapon_DekuStick",1);   /* (the slots as headless, 18 Nov: the stick 1, the shield 4) */
        add(4,"Weapon_Shield_Deku",0);
        char name[64];
        assert(vr_held_slots(&c,link,name)==2 && !strcmp(name,"Weapon_DekuStick"));   /* the stick drawn, the shield not */
        /* asked: nothing until Link's own update, then the game's call on his actor, once */
        assert(hos_vr_put_away_held());
        hos_vr_put_away_actor_update(&c,HOS_HEAP_BASE+0x9000); assert(calls==0);       /* another actor's update */
        hos_vr_put_away_actor_update(&c,link);
        assert(calls==1 && called_fn==0x804c8690ULL && called_x0==link);
        now_ns+=16000000; hos_vr_put_away_actor_update(&c,link); assert(calls==1);    /* asked once: called once */
        /* nothing drawn: asked, but nothing to call */
        put(stick+0x9c,2,4);
        assert(vr_held_slots(&c,link,name)==0);
        assert(hos_vr_put_away_held()); hos_vr_put_away_actor_update(&c,link); assert(calls==1);
        put(stick+0x9c,1,4);hos_vr_put_away_actor_update(&c,link); assert(calls==1);   /* (that request is spent) */
        put(stick+0x98,0,1); assert(vr_held_slots(&c,link,name)==0);                   /* not enabled: not drawn */
        put(stick+0x98,1,1);
        /* Put away = Button: nothing asked (the caller presses B) */
        /* asked, then withdrawn before Link's update (the item taken again): no call */
        assert(hos_vr_put_away_held()); hos_vr_put_away_cancel(); hos_vr_put_away_actor_update(&c,link); assert(calls==1);
        return 0;
    }
    ''')


def check_blade_hits():
    """Sword hits = Blade (vr/items/sword/blade_hits.inc): a stroke is a fast blade that went far enough one way (a fast
    swing and a quick wrist cut count; a slow wave and a fast wiggle never do), a backhand right after is a stroke of its
    own; a stab is a thrust; the contact is the blade as drawn, swept between two updates (a fast swing does not pass a
    stalk unseen), held at the swept pose closest to it; the weapons and the targets by name"""
    source = read('sword_host')
    consts = source[source.index('#define kVrSetupAttack'):source.index('/* ---- the swung weapons')]
    start = source.index('/* ---- the swung weapons')
    end = source.index('/* ---- the targets near Link')
    compile_run(r"""
    #include <stdint.h>
    #include <string.h>
    #include <math.h>
    #include <assert.h>
    #include <stdio.h>
    static int rx_option(int id, int otherwise) { (void)id; return otherwise; }
    int rx_vr_hands(void *out) { (void)out; return 0; }   /* (vr/headset/bridges_host.c: no headset here) */
    """ + consts + source[start:end] + r"""
    static int64_t t = 1000000000LL;
    static VrStroke s;
    static int begun, ended;
    static const float *axis;   /* (the blade's way in the room: NULL not known) */
    static void at(float x, float y, float z, int64_t dt) {   /* (the blade's point there, dt ns later) */
        const float p[3] = {x, y, z};
        t += dt;
        const int r = vr_stroke_step(&s, p, axis, t, 2.6f, .15f);
        begun += r > 0, ended += r < 0;
    }
    static void rest(int n) { for (int i = 0; i < n; i++) at(s.was[0], s.was[1], s.was[2], 16000000LL); }
    int main(void) {
        /* a fast swing (5 m/s along x, 16 ms updates): one stroke, begun once it has gone kVrStrokeTravel, ended at rest */
        at(0, 0, 0, 0); rest(5);
        for (int i = 1; i <= 20; i++) { at(.08f * i, 0, 0, 16000000LL); if (i == 1) assert(!begun); }
        assert(begun == 1 && s.active && s.number == 1);
        rest(3); assert(ended == 1 && !s.active);
        /* a slow wave (0.8 m/s, a metre): never */
        begun = ended = 0;
        for (int i = 1; i <= 80; i++) at(1.6f - .0128f * i, 0, 0, 16000000LL);
        assert(!begun);
        /* fast, but along a path no longer than kVrStrokeTravel: never; nor a fast wiggle (4 m/s, 6 cm each way) */
        rest(5); for (int i = 1; i <= 2; i++) at(s.was[0] + .06f, 0, 0, 16000000LL);
        rest(5); for (int i = 0; i < 40; i++) at(s.was[0] + (i % 2 ? -.064f : .064f), 0, 0, 16000000LL);
        assert(!begun);
        /* a quick wrist cut: the point 0.35 m from a still wrist, 100 degrees in 110 ms: a stroke */
        rest(5);
        for (int i = 0; i <= 8; i++) { const float a = 1.745f * i / 8; at(.35f * cosf(a), .35f * sinf(a), 0, i ? 14000000LL : 16000000LL); }
        assert(begun == 1);
        /* a forehand then at once a backhand: two strokes */
        rest(5); begun = ended = 0;
        const int was = s.number;
        for (int i = 1; i <= 8; i++) at(s.was[0] + .08f, 0, 0, 16000000LL);
        for (int i = 1; i <= 8; i++) at(s.was[0] - .08f, 0, 0, 16000000LL);
        assert(begun == 2 && ended == 1 && s.number == was + 2);
        /* a stab (the blade along +z, tip first) is one stroke; pulling it back out (along -z, tip trailing) is none, and
           the next stab after it is a stroke again (headset 22 Nov: one thrust counted twice) */
        {
            const float z[3] = {0, 0, 1};
            axis = z, rest(5), begun = ended = 0;
            for (int i = 1; i <= 8; i++) at(0, 0, s.was[2] + .06f, 16000000LL);
            assert(begun == 1);
            for (int i = 1; i <= 8; i++) at(0, 0, s.was[2] - .06f, 16000000LL);
            assert(begun == 1 && ended == 1);
            for (int i = 1; i <= 8; i++) at(0, 0, s.was[2] + .06f, 16000000LL);
            assert(begun == 2);
            rest(5);
            /* Swing arc: a 20 cm sweep is a stroke at 15 cm, not at 25 */
            begun = 0, axis = NULL;
            for (int i = 1; i <= 4; i++) at(s.was[0] + .05f, 0, 0, 16000000LL);
            assert(begun == 1);
            rest(5), begun = 0;
            for (int i = 1; i <= 4; i++) {
                const float p[3] = {s.was[0] + .05f, 0, 0};
                t += 16000000LL, begun += vr_stroke_step(&s, p, NULL, t, 2.6f, .25f) > 0;
            }
            assert(begun == 0);
            rest(5);
        }
        /* the sweep: the Kokiri Sword level along +z, swept 1.4 m across in one update (0.2 m at the tip per sub-step would
           pass a stalk): touching a Deku Baba's stalk whose axis is 0.45 m ahead, held where it crossed */
        const VrBladeWeapon *w = &kVrBladeWeapons[vr_blade_weapon_of("Weapon_Sword_Kokiri")];
        const VrTargetShape *baba = &kVrTargetShapes[vr_target_shape_of("Enemy_Dekubaba")];
        const float r[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        const float o0[3] = {-.7f, .35f, -.45f}, o1[3] = {.7f, .35f, -.45f}, e[3] = {0, 0, 0};
        float o[3], rr[9];
        int step;
        assert(vr_blade_sweep(w, o0, r, o1, r, e, baba, o, rr, &step) < -.25f && fabsf(o[0]) < .05f && step > 0);
        /* ... the drawn blade's reach, not the game's longer sensor: the stalk 0.7 m along it is out of reach */
        const float far0[3] = {-.7f, .35f, -.7f - w->thick - baba->radius}, far1[3] = {.7f, .35f, -.7f - w->thick - baba->radius};
        assert(vr_blade_sweep(w, far0, r, far1, r, e, baba, o, rr, &step) > 0);
        /* the same 0.5 m over its top: no contact */
        const float h0[3] = {-.7f, 2.3f, -.45f}, h1[3] = {.7f, 2.3f, -.45f};
        assert(vr_blade_sweep(w, h0, r, h1, r, e, baba, o, rr, &step) > .2f);
        /* turned (the blade's way mixed between two poses, kept square): a chop from up to level crosses it too */
        const float up[9] = {1, 0, 0, 0, 0, 1, 0, -1, 0};   /* (z up) */
        const float c0[3] = {0, .35f, -.45f};
        assert(vr_blade_sweep(w, c0, up, c0, r, e, baba, o, rr, &step) < 0);
        /* a stab (the point going along the blade, tip first) is a thrust; a cut across it, a slash */
        {
            VrStroke k = {0};
            const float z[3] = {0, 0, 1};
            k.way[2] = 1; assert(vr_blade_kind(&k, z));
            k.way[2] = 0, k.way[0] = 1; assert(!vr_blade_kind(&k, z));
            k.way[0] = 0, k.way[2] = -1; assert(!vr_blade_kind(&k, z));   /* (pulled back, pommel first: not a thrust) */
        }
        /* the rows and the shapes by name */
        assert(vr_blade_weapon_of("Weapon_DekuStick") >= 0 && vr_blade_weapon_of("Weapon_Sword_Biggoron") >= 0);
        assert(vr_blade_weapon_of("Weapon_Boomerang") < 0 && vr_blade_weapon_of("Weapon_Shield_Deku") < 0);
        assert(vr_target_shape_of("Enemy_Skulltula") >= 0 && vr_target_shape_of("Obj_BrokenBox_A_01") >= 0);
        assert(vr_target_shape_of("Weapon_Sword_Kokiri") < 0 && vr_target_shape_of("Obj_Door_ShopBombChu") < 0);
        assert(vr_target_shape_of("Npc_Saria") < 0 && vr_target_shape_of("Land_Kokiri_A") < 0);
        return 0;
    }
    """)


def check_bracelet_found():
    """child Link's worn Goron Bracelet is found by its own update on Link (it is not among his EquipmentUser slots: headset
    18 Nov), by name, each other actor's name read once; it counts as worn only while it keeps updating"""
    source = read('sword_host')
    start = source.index("/* ---- Child Link's worn Goron Bracelet")
    end = source.index('\n}\n', source.index('uint64_t hos_vr_bracelet_on_link(void)')) + 3
    finder = source[start:end]
    compile_run(r'''
    #include <stdint.h>
    #include <string.h>
    #include <assert.h>
    typedef struct { int unused; } Ctx;
    static int64_t now = 1000000000LL;
    static int64_t hos_now_ns(void) { return now; }
    static int reads;
    static void oot_vr_equipment_name(Ctx *c, uint64_t actor, uint64_t offset, char name[64]) {
        (void)c;
        reads++;
        strcpy(name, offset != 0x260 ? "" : actor == 7 ? "Armor_Child_Gauntlet_Goron" : actor == 8 ? "Item_Gauntlet_Goron" :
                                            actor == 3 ? "Weapon_Sword_Kokiri" : actor == 9 ? "Item_Ocarina_Fairy" : "Weapon_Shield_Deku");
    }
    static void vr_on_link_trace(Ctx *c, uint64_t actor, float dist) { (void)c, (void)actor, (void)dist; }
    static void hos_log(const char *f, ...) { (void)f; }
    #define kVrBesideNear 0.45f
    static uint64_t kept;
    void hos_vr_held_keep(Ctx *c, uint64_t actor) { (void)c; kept = actor; }
    ''' + finder + r'''
    int main(void) {
        Ctx c;
        assert(!hos_vr_bracelet_on_link());
        assert(!vr_on_link_note(&c, 3, .2f) && !vr_on_link_note(&c, 4, .3f));
        assert(!hos_vr_bracelet_on_link());                    /* the sword and the shield: not it */
        const int was = reads;
        for (int i = 0; i < 10; i++) vr_on_link_note(&c, 3, .2f);
        assert(reads == was);                                  /* a name read once, not every update */
        assert(vr_on_link_note(&c, 7, .66f));                  /* by its name, on Link's arm (not beside him) */
        assert(hos_vr_bracelet_on_link() == 7);
        now += 200000000LL; vr_on_link_note(&c, 7, .1f);
        now += 200000000LL; assert(hos_vr_bracelet_on_link() == 7);   /* still updating: still worn */
        now += 200000000LL; assert(!hos_vr_bracelet_on_link());       /* no longer updating (taken off, a map change) */
        vr_on_link_note(&c, 8, .1f); assert(hos_vr_bracelet_on_link() == 8);   /* the other name it may have */
        assert(vr_on_link_note(&c, 9, .5f) && kept == 9);       /* the ocarina: Link's own, kept from the near test */
        kept = 0; assert(vr_on_link_note(&c, 9, .5f) && kept == 9);   /* ... at each of its updates */
        return 0;
    }
    ''')


def check_gear_near():
    """held gear near the eye (and the worn Goron Bracelet, and whatever a VR hand holds: hos_vr_held_keep) isn't rejected by
    the game's near-plane test"""
    source=read('gear_near_host')
    compile_run(r'''
    #include <stdint.h>
    #include <string.h>
    #include <math.h>
    #include <assert.h>
    #define HOS_HEAP_BASE 0x1000000000ULL
    #define B HOS_HEAP_BASE
    typedef struct { uint64_t x[32]; unsigned char vreg[32][16]; } Ctx;
    typedef struct { int active,mode; } HookshotVrHead;
    static unsigned char memory[0x10000], before[0x10000];
    static int64_t now=1000000000;
    static int ours=1, mode=1, active=1, hos_vr_cull_first_person=1;
    static float hos_vr_cull_eye[3];
    static void *hos_guest_range(uint64_t p,uint64_t n) { return p>=B && p-B<=sizeof memory && n<=sizeof memory-(p-B) ? memory+p-B : 0; }
    static uint64_t recomp_load64(Ctx*c,uint64_t p) { (void)c; uint64_t v; assert(hos_guest_range(p,8)); memcpy(&v,hos_guest_range(p,8),8); return v; }
    static void ptr(uint64_t p,uint64_t v) { assert(hos_guest_range(p,8)); memcpy(hos_guest_range(p,8),&v,8); }
    static float vr_load(Ctx*c,uint64_t p) { (void)c; float v; assert(hos_guest_range(p,4)); memcpy(&v,hos_guest_range(p,4),4); return v; }
    static void flt(uint64_t p,float v) { assert(hos_guest_range(p,4)); memcpy(hos_guest_range(p,4),&v,4); }
    static int64_t hos_now_ns(void) { return now; }
    static int hos_vr_snapshot_ours(Ctx*c,uint64_t p) { (void)c;return ours && p==B+0x4000; }
    static int rx_vr_head(HookshotVrHead*h) { h->active=active;h->mode=mode;return 1; }
    static void hos_log(const char*f,...) { (void)f; }
    static int holder=-1;
    int hos_vr_sword_holder(void) { return holder; }   // (the VR hand holding the sword taken from the back, -1 none)
    static int census;   // (the world-culling watch's census, vr/camera/culling_host.c: told of every model's sphere)
    static uint64_t census_model;
    static void hos_vr_cull_model_seen(Ctx*c,uint64_t snap,uint64_t sphere,uint64_t model) { (void)c;(void)snap;(void)sphere; census_model=model; census++; }
    static int held_on=1;   // (VR tab Held items near the face, option 1002)
    static int rx_option(int id,int otherwise) { return id==1002 ? held_on : otherwise; }
    static void oot_vr_equipment_name(Ctx*c,uint64_t a,uint64_t o,char name[64]) { (void)c;(void)a;(void)o; strcpy(name,"Item_Bottle"); }
    ''' + source + r'''
    static void reg(Ctx*c,int i,float f) { memcpy(c->vreg[i],&f,4); }
    static float get(Ctx*c,int i) { float f;memcpy(&f,c->vreg[i],4);return f; }
    static void call(Ctx*c,int rescue) {
        reg(c,3,0.1f); Ctx original=*c;
        memcpy(before,memory,sizeof memory);
        const int counted=census;
        hos_vr_gear_near(c);
        assert(census==counted+1 && census_model==c->x[19]);   /* (every model goes to the watch's census, gear or not, as itself) */
        assert(!memcmp(before,memory,sizeof memory));
        assert(get(c,3)==(rescue?-1.5f:0.1f));
        reg(c,3,0.1f);assert(!memcmp(&original,c,sizeof original));
    }
    int main(void) {
        Ctx c;memset(&c,0x55,sizeof c);
        uint64_t ids[3]={B+0x1000,0,0};
        ptr(ids[0]+0x270,B+0x2000);ptr(B+0x2008,B+0x2100);
        ptr(B+0x2100,0x84c81020);ptr(B+0x2118,B+0x2200);
        ptr(B+0x2200,0x84c288a0);ptr(B+0x2218,B+0x2300);
        ptr(B+0x2300,B+0x3000);ptr(B+0x3000,0x84c28cc8);
        c.x[19]=B+0x3000;c.x[9]=B+0x4000;c.x[25]=B+0x5000;
        flt(c.x[25]+12,0.2f);reg(&c,1,0.6f);
        oot_vr_gear_cull_publish(&c,1,1,ids);call(&c,1);
        mode=0;call(&c,0);mode=1; active=0;call(&c,0);active=1;
        hos_vr_cull_first_person=0;call(&c,0);hos_vr_cull_first_person=1;
        ours=0;call(&c,0);ours=1;
        flt(c.x[9]+0xe4,2);call(&c,0);flt(c.x[9]+0xe4,0);
        now+=300000000;call(&c,0);oot_vr_gear_cull_publish(&c,1,1,ids);
        c.x[19]+=8;call(&c,0);c.x[19]-=8;
        reg(&c,1,2);call(&c,0);reg(&c,1,-2);call(&c,0);reg(&c,1,0.6f);
        oot_vr_gear_cull_publish(&c,1,0,ids);call(&c,0);
        holder=1;oot_vr_gear_cull_publish(&c,1,0,ids);call(&c,1);holder=-1;   // (sheathed in the game, held by a VR hand)
        oot_vr_gear_cull_publish(&c,0,0,ids);call(&c,0);
        oot_vr_gear_cull_publish(&c,1,1,ids);call(&c,1);
        // child Link's worn Goron Bracelet (no sword/shield published): kept while its actor is published, then not
        oot_vr_gear_cull_publish(&c,0,0,ids);oot_vr_gear_worn_publish(&c,ids[0]);call(&c,1);
        mode=0;call(&c,0);mode=1;
        now+=300000000;call(&c,0);oot_vr_gear_worn_publish(&c,ids[0]);call(&c,1);
        oot_vr_gear_worn_publish(&c,0);call(&c,0);
        ptr(B+0x2200,0);oot_vr_gear_worn_publish(&c,ids[0]);call(&c,0);   // (its model not resolved: not kept)
        oot_vr_gear_cull_publish(&c,1,1,ids);call(&c,0);
        // whatever a VR hand holds (a bottle, a bomb, the slingshot...): kept while its code keeps it, not after, not when off
        ptr(B+0x2200,0x84c288a0);oot_vr_gear_cull_publish(&c,0,0,ids);oot_vr_gear_worn_publish(&c,0);call(&c,0);
        hos_vr_held_keep(&c,ids[0]);call(&c,1);
        mode=0;call(&c,0);mode=1; hos_vr_cull_first_person=0;call(&c,0);hos_vr_cull_first_person=1;
        reg(&c,1,-2);call(&c,0);reg(&c,1,0.6f);                 // (past the rescue's band, as for the sword)
        now+=300000000;call(&c,0);hos_vr_held_keep(&c,ids[0]);call(&c,1);
        now+=300000000;held_on=0;hos_vr_held_keep(&c,ids[0]);call(&c,0);held_on=1;
        ptr(B+0x2200,0);hos_vr_held_keep(&c,ids[0]);call(&c,0);  // (its model not resolved: nothing kept)
        // an item get's item, over Link's head (above and behind the eye looking down): kept while an item get may play,
        // not below the eye, not big, not far, not after
        ptr(B+0x2200,0x84c288a0);now+=300000000;call(&c,0);
        flt(c.x[25]+4,0.7f);oot_vr_item_get_time=now;call(&c,1);
        flt(c.x[25]+4,0.05f);call(&c,0);flt(c.x[25]+4,1.6f);call(&c,0);flt(c.x[25]+4,0.7f);
        flt(c.x[25]+12,0.8f);call(&c,0);flt(c.x[25]+12,0.2f);
        mode=0;call(&c,0);mode=1;
        now+=300000000;call(&c,0);
        return 0;
    }
    ''')

# ======================================================================================================================
# Bombs in the hands (vr/items/bomb): the throw arc, the gravity pull, quiet unlit bombs, Bomb Flowers
# ======================================================================================================================

def check_bomb_math():
    """the throw arc lands where it says, a throw onto a target reaches it, and the pull picks only what the hand points at"""
    compile_run(read('bomb_math') + r'''
    #include <assert.h>
    static int near(float a, float b, float e) { return fabsf(a - b) <= e; }
    int main(void) {
        const float g = 9.81f, from[3] = {0, 1, 0};
        /* a 45-degree throw at sqrt(g R) from the ground lands R away (here from 1 m up: a little further, on the way down) */
        const float R = 8, s = sqrtf(g * R), v45[3] = {s * 0.70710678f, s * 0.70710678f, 0}, ground[3] = {0, 0, 0};
        float t = bomb_arc_land_time(ground, v45, g, 0), at[3];
        assert(t > 0); bomb_arc_at(ground, v45, g, t, at); assert(near(at[0], R, 0.01f) && near(at[1], 0, 1e-3f));
        t = bomb_arc_land_time(from, v45, g, 0); bomb_arc_at(from, v45, g, t, at);
        assert(at[0] > R && near(at[1], 0, 1e-3f));
        /* thrown down from 1 m: lands on the way down, before 1 s; thrown up from below a ledge too slowly: never reaches it */
        const float down[3] = {1, -2, 0}, weak[3] = {0, 1, 0};
        t = bomb_arc_land_time(from, down, g, 0); assert(t > 0 && t < 1);
        assert(bomb_arc_land_time(ground, weak, g, 2) < 0);
        /* onto a target: 6 m ahead and 1 m lower, at 30 degrees; it passes through the target */
        const float target[3] = {0, 0, 6};
        float v[3];
        assert(bomb_arc_onto(from, target, 0.5236f, g, 30, v));
        const float level = sqrtf(v[0] * v[0] + v[2] * v[2]);
        assert(near(atan2f(v[1], level), 0.5236f, 0.01f));            /* (the angle asked for reaches it) */
        bomb_arc_at(from, v, g, 6 / level, at);
        assert(near(at[0], 0, 1e-3f) && near(at[1], 0, 0.01f) && near(at[2], 6, 0.01f));
        /* aimed below a high target: a steeper throw is taken instead, still through it */
        const float high[3] = {4, 3, 0};
        assert(bomb_arc_onto(from, high, 0.0f, g, 30, v));
        assert(atan2f(v[1], sqrtf(v[0] * v[0] + v[2] * v[2])) > atan2f(2, 4));
        bomb_arc_at(from, v, g, 4 / sqrtf(v[0] * v[0] + v[2] * v[2]), at);
        assert(near(at[0], 4, 0.01f) && near(at[1], 3, 0.02f));
        /* out of reach at the speed allowed: none */
        const float far[3] = {40, 0, 0};
        assert(!bomb_arc_onto(from, far, 0.785f, g, 8, v));
        /* straight below: a drop */
        const float below[3] = {0.02f, 0, 0};
        assert(bomb_arc_onto(from, below, 0.5f, g, 8, v) && v[0] == 0 && v[1] == 0 && v[2] == 0);
        /* the pull: dead ahead 3 m, in a 10-degree cone; 20 degrees off, not; behind, too near or too far, not */
        const float o[3] = {0, 1, 0}, dir[3] = {0, 0, 1}, c10 = 10 * 0.0174533f;
        const float ahead[3] = {0, 1, 3}, off20[3] = {3 * sinf(0.349f), 1, 3 * cosf(0.349f)}, behind[3] = {0, 1, -3};
        const float close[3] = {0, 1, 0.2f}, distant[3] = {0, 1, 9};
        assert(bomb_pull_aim(o, dir, ahead, 0.22f, 0.37f, 4, c10) == 0);
        assert(bomb_pull_aim(o, dir, off20, 0.22f, 0.37f, 4, c10) < 0);
        assert(bomb_pull_aim(o, dir, behind, 0.22f, 0.37f, 4, c10) < 0);
        assert(bomb_pull_aim(o, dir, close, 0.22f, 0.37f, 4, c10) < 0);
        assert(bomb_pull_aim(o, dir, distant, 0.22f, 0.37f, 4, c10) < 0);
        /* a bomb's own size counts: 12 degrees off at 1.2 m (the ball covers about 10 of them) is in a 3-degree cone */
        const float big[3] = {1.2f * sinf(0.21f), 1, 1.2f * cosf(0.21f)};
        assert(bomb_pull_aim(o, dir, big, 0.22f, 0.37f, 4, 3 * 0.0174533f) >= 0);
        /* the better aimed of two scores lower */
        const float off5[3] = {3 * sinf(0.087f) + 0.22f * 2, 1, 3};
        assert(bomb_pull_aim(o, dir, ahead, 0.22f, 0.37f, 4, c10) <= bomb_pull_aim(o, dir, off5, 0.22f, 0.37f, 4, c10));
        return 0;
    }
    ''')


BOMB_GAME = r'''
    /* ---- a small fake of the game for vr/items/bomb/bomb_host.c: its memory, the calls it makes, the hands ---- */
    #include <stdint.h>
    #include <stddef.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
    #include <math.h>
    #include <assert.h>
    #define HOS_HEAP_BASE 0x1000000000ULL
    typedef struct { uint64_t x[32]; unsigned char vreg[32][16]; } Ctx;
    typedef void (*A64Fn)(Ctx *);
    #include <pthread.h>   /* (the SRW lock as host/posix/windows.h makes it) */
    typedef pthread_rwlock_t SRWLOCK;
    #define SRWLOCK_INIT PTHREAD_RWLOCK_INITIALIZER
    static void AcquireSRWLockExclusive(SRWLOCK *l) { pthread_rwlock_wrlock(l); }
    static void ReleaseSRWLockExclusive(SRWLOCK *l) { pthread_rwlock_unlock(l); }
    static int TryAcquireSRWLockExclusive(SRWLOCK *l) { return pthread_rwlock_trywrlock(l) == 0; }
    static unsigned char memory[0x100000];
    static void *hos_guest_range(uint64_t a, uint64_t n) {
        return a >= HOS_HEAP_BASE && a - HOS_HEAP_BASE <= sizeof memory && n <= sizeof memory - (a - HOS_HEAP_BASE)
            ? memory + (a - HOS_HEAP_BASE) : NULL;
    }
    static uint64_t get(uint64_t a, size_t n) { uint64_t v = 0; void *p = hos_guest_range(a, n); if (p) memcpy(&v, p, n); return v; }
    static void put(uint64_t a, uint64_t v, size_t n) { void *p = hos_guest_range(a, n); assert(p); memcpy(p, &v, n); }
    static float getf(uint64_t a) { uint32_t w = (uint32_t)get(a, 4); float f; memcpy(&f, &w, 4); return f; }
    static void putf(uint64_t a, float f) { uint32_t w; memcpy(&w, &f, 4); put(a, w, 4); }
    static uint64_t recomp_load64(Ctx *c, uint64_t a) { (void)c; return get(a, 8); }
    static uint32_t recomp_load32(Ctx *c, uint64_t a) { (void)c; return (uint32_t)get(a, 4); }
    static void recomp_store32(Ctx *c, uint64_t a, uint32_t v) { (void)c; put(a, v, 4); }
    static void a64_vset64(unsigned char *r, uint64_t v) { memcpy(r, &v, 8); }
    static int64_t clock_ns = 1000000000LL;
    static int64_t hos_now_ns(void) { return clock_ns; }
    static int logs;
    static char last_log[256];
    static void hos_log(const char *f, ...) {
        va_list a; va_start(a, f); vsnprintf(last_log, sizeof last_log, f, a); va_end(a); logs++;
        if (getenv("BOMB_CHECK_LOG")) fprintf(stderr, "%s\n", last_log);
    }
    static int options[1000];
    static int rx_option(int id, int otherwise) { return id >= 0 && id < 1000 && options[id] >= 0 ? options[id] : otherwise; }
    static int bracelet;
    static int oot_item_count(Ctx *c, const char *name) { (void)c; return bracelet && !strcmp(name, "Armor_Child_Gauntlet_Goron"); }
    static void oot_vr_note_selected_item(Ctx *c) { (void)c; }
    static uint64_t oot_vr_player_actor, oot_vr_player_object;
    int hos_vr_selected_is_bomb;
    static uint64_t flower_actor, flower_actor2, plant_actor;   /* (fruits: Obj_BombFlower_A_01 / _A_04; the plant: _A_03) */
    static void oot_vr_equipment_name(Ctx *c, uint64_t actor, uint64_t offset, char name[64]) {
        (void)c; (void)offset; memset(name, 0, 64);
        strcpy(name, actor == flower_actor ? "Obj_BombFlower_A_01" : actor == flower_actor2 ? "Obj_BombFlower_A_04" :
                     actor == plant_actor ? "Obj_BombFlower_A_03" : "Obj_Something_Else");
    }
    static int locked; static float lock_at[3];
    static int oot_vr_lock_point(Ctx *c, uint64_t actor, float point[3]) {
        (void)c; (void)actor; if (locked && point) memcpy(point, lock_at, 12); return locked;
    }
    /* the hands, as hookshot_vr_hands_world sends them (21 floats a hand) and their pointing lines */
    typedef struct { int valid; float p[3], v[3], trigger, grip, aim[3], palm[3]; } Hand;
    static Hand hand[2];
    static int rx_vr_hands_world(float out[42]) {
        memset(out, 0, 42 * sizeof *out);
        for (int h = 0; h < 2; h++) {
            float *o = out + 21 * h;
            o[0] = hand[h].valid;
            for (int k = 0; k < 3; k++) o[1 + k] = hand[h].p[k], o[4 + k] = hand[h].v[k];
            o[7] = hand[h].trigger, o[8] = hand[h].grip;
            /* (grip axes: the world's x, y, z, but x turned so the palm faces palm[] when one is given: +x is the left
               palm's way, -x the right's) */
            const float n = sqrtf(hand[h].palm[0] * hand[h].palm[0] + hand[h].palm[1] * hand[h].palm[1] + hand[h].palm[2] * hand[h].palm[2]);
            o[9] = 1, o[13] = 1, o[17] = 1;
            if (n > 0.5f) for (int k = 0; k < 3; k++) o[9 + k] = hand[h].palm[k] / n * (h ? -1.0f : 1.0f);
        }
        return 1;
    }
    static int rx_vr_hand_ray(int h, float out[6]) {
        if (!hand[h].valid) return 0;
        for (int k = 0; k < 3; k++) out[k] = hand[h].p[k], out[3 + k] = hand[h].aim[k];
        return 1;
    }
    static int rx_vr_body_heading(float *yaw) { (void)yaw; return 0; }
    static float held_radius[2];
    static void rx_vr_hand_holds(int h, float radius, const float centre[3]) { (void)centre; held_radius[h] = radius; }
    static float marks[64 * 5]; static int marks_n;
    static void rx_vr_bomb_marks(const float *m, int n) { marks_n = n; memcpy(marks, m, n * 5 * sizeof *m); }
    static int mark_count(int kind) { int k = 0; for (int i = 0; i < marks_n; i++) k += (int)marks[5 * i] == kind; return k; }
    static float state[8];   /* (what the renderer is told: held kinds, lifted, the pull's target) */
    static void rx_vr_bomb_state(const float s[8]) { memcpy(state, s, sizeof state); }
    static int rx_vr_flower_shape(float out[4]) { out[0] = 0, out[1] = 0.2226f, out[2] = 0, out[3] = 0.2638f; return 1; }

    /* the game's functions the bombs call: the actors' memory as the game keeps it */
    #define ACTOR_SIZE 0x4000
    static uint64_t actor_of(uint64_t part) { return HOS_HEAP_BASE + ((part - HOS_HEAP_BASE) / ACTOR_SIZE) * ACTOR_SIZE; }
    static float velocity[3]; static int layer = -1, sleeps, wakes; static uint32_t command = 0x1111; static float held_frame = -1;
    static void g_place(Ctx *c) {   /* f_804e6630(physics, &origin, &rot, 1) */
        const uint64_t a = actor_of(c->x[0]);
        for (int k = 0; k < 3; k++) putf(a + 796 + 4 * k, getf(c->x[1] + 4 * k));
    }
    static void g_velocity(Ctx *c) { for (int k = 0; k < 3; k++) velocity[k] = getf(c->x[1] + 4 * k); }
    static void g_nothing(Ctx *c) { (void)c; }
    static void g_layer(Ctx *c) { layer = (int)c->x[1]; }
    static void g_frame(Ctx *c) { memcpy(&held_frame, c->vreg[0], 4); }
    static void g_command(Ctx *c) { c->x[0] = command; }
    static void g_sleep(Ctx *c) { if (!get(c->x[0] + 200, 4) && c->x[1]) sleeps++; put(c->x[0] + 200, get(c->x[0] + 200, 4) | c->x[1], 4); }
    static void g_wake(Ctx *c) { if (get(c->x[0] + 200, 4) && c->x[1]) wakes++; put(c->x[0] + 200, get(c->x[0] + 200, 4) & ~c->x[1], 4); }
    static int falls, fall_args_ok;   /* f_82e70db0(lifter, 1, 1.0, 0.0): OneShotLifterTryRequestFall's, on Link's Lifter */
    static void g_fall(Ctx *c) {
        float s0, s1; memcpy(&s0, c->vreg[0], 4); memcpy(&s1, c->vreg[1], 4);
        falls++, fall_args_ok = c->x[1] == 1 && s0 == 1.0f && s1 == 0.0f;
        const uint32_t st = (uint32_t)get(c->x[0] + 0x2b0, 4);
        c->x[0] = (st & ~1u) == 2;
    }
    static A64Fn a64_lookup(uint64_t fn) {
        switch (fn) {
        case 0x804e6630ULL: return g_place;
        case 0x8014dbe0ULL: return g_velocity;
        case 0x8014b790ULL: return g_nothing;
        case 0x80147840ULL: return g_layer;
        case 0x837d5d70ULL: return g_frame;
        case 0x837d5d00ULL: return g_command;
        case 0x80994100ULL: return g_sleep;
        case 0x80994060ULL: return g_wake;
        case 0x82e70db0ULL: return g_fall;
        }
        return NULL;
    }
    /* an actor at a slot of fake memory: its components (physics, body, animation, XLink) inside its own block */
    static uint64_t make_actor(int slot, float x, float y, float z) {
        const uint64_t a = HOS_HEAP_BASE + (uint64_t)slot * ACTOR_SIZE, comp = a + 0x1000;
        put(a + 624, comp, 8);
        put(comp + 80, a + 0x1100, 8); put(a + 0x1100 + 32, a + 0x1200, 8); put(a + 0x1200 + 400, a + 0x1300, 8);   /* physics, body */
        put(comp + 64, a + 0x1400, 8); put(a + 0x1400 + 24, a + 0x1500, 8); put(a + 0x1500 + 24, a + 0x1600, 8);  /* animation */
        put(comp + 104, a + 0x1700, 8);                                                                              /* XLink */
        putf(a + 796, x), putf(a + 800, y), putf(a + 804, z);
        putf(a + 0x328, 1), putf(a + 0x328 + 16, 1), putf(a + 0x328 + 32, 1);                                      /* upright */
        return a;
    }
    static uint32_t sleep_reasons(uint64_t a) { return (uint32_t)get(a + 0x1700 + 200, 4); }
'''

def check_bomb_hands():
    """bombs in the VR hands, with the game faked: an unlit bomb's effects sleep (the mod's own reason) until lit or set off;
    the gravity pull rings what an open hand points at and flies it into the hand; Arc to target shows the arc and throws
    along it (a slow let-go drops); Bomb Flowers are found by name and need the Goron's Bracelet; a held bomb keeps that
    hand's trigger from the game whatever is selected"""
    source = read('bomb_host').replace('#include "bomb_math.h"', read('bomb_math'))
    compile_run('#include <stdarg.h>\n' + BOMB_GAME +
                'int hos_vr_ocarina_claims(int hand) { (void)hand; return 0; }   /* (no hand at the belt ocarina) */\n' +
                'static int sword_claims = -1;   /* (the hand the sword has: vr/input/controllers_host.c) */\n' +
                'int hos_vr_sword_claims(int hand) { return hand == sword_claims; }\n' +
                'static uint64_t held_kept;   /* (vr/items/gear/gear_near_host.c: the last actor kept from the near test) */\n' +
                'void hos_vr_held_keep(Ctx *c, uint64_t actor) { (void)c; held_kept = actor; }\n' +
                source + r'''
    static Ctx c;
    static uint64_t player;
    static void frame(uint64_t *bombs, int n) {   /* one game frame (90 a second): Link's update, then each bomb's */
        clock_ns += 11111111;
        hos_vr_bomb_actor_update(&c, player);
        for (int i = 0; i < n; i++) hos_vr_bomb_actor_update(&c, bombs[i]);
    }
    static void frames(uint64_t *bombs, int n, int count) { for (int i = 0; i < count; i++) frame(bombs, n); }
    static float dist(uint64_t a, const float p[3]) {
        float d = 0; for (int k = 0; k < 3; k++) { const float e = getf(a + 796 + 4 * k) + (k == 1 ? BOMB_CENTRE : 0) - p[k]; d += e * e; }
        return sqrtf(d);
    }
    int main(void) {
        c.x[31] = HOS_HEAP_BASE + sizeof memory - 0x1000;   /* (a stack for the calls) */
        for (int i = 0; i < 1000; i++) options[i] = -1;
        options[904] = 1; options[905] = 100; options[907] = 100; options[908] = 0;
        options[950] = 1; options[951] = 0; options[952] = 80; options[953] = 1; options[954] = 40; options[955] = 20; options[956] = 1;
        player = make_actor(1, 0, 0, 0);
        oot_vr_player_actor = player;
        const uint64_t lifter = player + 0x1800;   /* (Link's Lifter: actor +0x270 -> +0x2c8) */
        put(player + 0x1000 + 712, lifter, 8);
        hand[1].valid = 1;
        const float rest[3] = {0.3f, 1.2f, 0.3f};
        memcpy(hand[1].p, rest, 12); hand[1].aim[2] = 1; hand[1].palm[2] = 1;   /* (the palm toward +z) */

        /* ---- a bomb seen by its AI, 2.5 m ahead (on a ledge level with the hand): the open hand pointing at it rings it ---- */
        uint64_t bombs[2] = {make_actor(2, 0.3f, 1.2f - BOMB_CENTRE, 2.8f), 0};
        { uint64_t node = HOS_HEAP_BASE + 0x80000, a = node + 0x100, b = node + 0x200;   /* (node +8 -> +8 -> +496: the actor) */
          put(node + 8, a, 8); put(a + 8, b, 8); put(b + 496, bombs[0], 8); c.x[0] = node; hos_vr_bomb_ai(&c); }
        VrBomb *b = vr_bomb_find(bombs[0]);
        assert(b && !b->owned && !b->flower && b->pull_hand < 0);
        frames(bombs, 1, 3);
        assert(state[4] == 0);                             /* (a bomb the hands never had needs the Goron's Bracelet) */
        b->owned = 1;                                      /* (one the hands threw earlier) */
        frames(bombs, 1, 3);
        assert(state[4] == 1 && fabsf(state[7] - 2.8f) < 0.01f && mark_count(3) == 0);   /* (the pull's target, told the
                                                           renderer: it pulses; no ring in the headset) */
        assert(hos_vr_bomb_in_reach(1));                   /* (other items leave this grip to the bombs) */
        hand[1].aim[0] = 0.5f, hand[1].aim[2] = 0.866f;    /* (the controller's aim 30 degrees off, the palm still on it: kept) */
        frames(bombs, 1, 2);
        assert(state[4] == 1);
        hand[1].palm[0] = 0.7f, hand[1].palm[2] = 0.7f;    /* (the palm 45 degrees off: none) */
        frames(bombs, 1, 2);
        assert(state[4] == 0 && !hos_vr_bomb_in_reach(1));
        hand[1].palm[0] = 0.26f, hand[1].palm[2] = 0.97f;  /* (15 degrees off: in the 20-degree cone) */
        frames(bombs, 1, 2);
        assert(state[4] == 1);
        hand[1].aim[0] = 0, hand[1].aim[2] = 1, hand[1].palm[0] = 0, hand[1].palm[2] = 1;
        /* ---- squeezed: pulled, placed toward the hand each frame, held on arrival; quiet once the hands' ---- */
        hand[1].grip = 1;
        frame(bombs, 1);
        assert(b->pull_hand == 1);
        frames(bombs, 1, 60);
        assert(b->holder == 1 && b->owned && b->pull_hand < 0);
        assert(held_kept == bombs[0]);                                    /* (held: kept from the near test at the face) */
        assert(layer == LAYER_NO_HIT);
        assert(held_radius[1] > 0 && state[1] == 1 && state[3] == 0);   /* (a bomb in the right hand, not in Link's lift) */
        /* unlit: its fuse held at the Debug tab's frame, its XLink asleep for the mod's own reason (not the game's bit 0) */
        assert(sleeps == 1 && sleep_reasons(bombs[0]) == BOMB_SLEEP_REASON && !(BOMB_SLEEP_REASON & 1));
        assert(held_frame == BOMB_UNLIT_FRAME);
        frames(bombs, 1, 10);
        assert(sleeps == 1);                               /* (once, not every frame) */
        /* VR tab Unlit bomb fire: the game's: awake, though unlit */
        options[950] = 0; frame(bombs, 1); assert(wakes == 1 && sleep_reasons(bombs[0]) == 0 && !b->lit);
        options[950] = 1; frame(bombs, 1); assert(sleeps == 2);
        /* the trigger lights it: awake, the fuse runs on (no more frame held) */
        hand[1].trigger = 1; frame(bombs, 1); hand[1].trigger = 0; frame(bombs, 1);
        assert(b->lit && wakes == 2 && sleep_reasons(bombs[0]) == 0);
        held_frame = -1; frames(bombs, 1, 3); assert(held_frame == -1);

        /* ---- a second bomb, held unlit, set off by something else: its animation changes, so it wakes for its explosion ---- */
        hand[1].grip = 0; frames(bombs, 1, 3);   /* (the lit one dropped) */
        assert(b->holder < 0);
        b->actor = 0;   /* (the lit one is forgotten: exploded) */
        bombs[1] = make_actor(3, rest[0], rest[1] - BOMB_CENTRE, rest[2]);
        VrBomb *b2 = vr_bomb_track(&c, bombs[1], 0);
        b2->owned = 1;   /* (from the bag) */
        hand[1].grip = 1; frames(bombs, 2, 2);
        assert(b2->holder == 1 && sleep_reasons(bombs[1]) == BOMB_SLEEP_REASON);
        command = 0x2222; frames(bombs, 2, 2);
        assert(b2->lit && sleep_reasons(bombs[1]) == 0);
        command = 0x1111;

        /* ---- Arc to target: the arc while held (dots and a ring), a throw along it, a slow let-go a drop ---- */
        hand[1].grip = 0; frames(bombs, 2, 3);
        b2->actor = 0;
        bombs[0] = make_actor(4, rest[0], rest[1] - BOMB_CENTRE, rest[2]);
        b = vr_bomb_track(&c, bombs[0], 0);
        b->owned = 1;
        options[951] = 1;
        hand[1].aim[0] = 0, hand[1].aim[1] = 0.5f, hand[1].aim[2] = 0.866f;   /* (30 degrees up) */
        hand[1].grip = 1; frames(bombs, 1, 3);
        assert(b->holder == 1 && mark_count(1) == BOMB_ARC_DOTS && mark_count(2) == 1);
        float land[3]; for (int i = 0; i < marks_n; i++) if ((int)marks[5 * i] == 2) memcpy(land, marks + 5 * i + 1, 12);
        assert(fabsf(land[1] - 0) < 0.01f && land[2] > 4);   /* (on the ground at Link's feet, metres ahead) */
        hand[1].aim[1] = -0.9f; frame(bombs, 1); assert(mark_count(1) == 0);   /* (pointing at the feet: no arc) */
        hand[1].aim[1] = 0.5f; frame(bombs, 1);
        for (int i = 0; i < 4; i++) { hand[1].v[2] = 4; frame(bombs, 1); }   /* (a throw) */
        hand[1].grip = 0; frame(bombs, 1); hand[1].v[2] = 0;
        const float speed = sqrtf(BOMB_REAL_G * 8);
        assert(b->holder < 0 && fabsf(b->release_v[1] - speed * 0.5f) < 0.05f && fabsf(b->release_v[2] - speed * 0.866f) < 0.05f);
        /* a slow let-go: dropped with the hand's own velocity, not along the arc */
        hand[1].grip = 0; frames(bombs, 1, 2);
        for (int k = 0; k < 3; k++) putf(bombs[0] + 796 + 4 * k, rest[k] - (k == 1 ? BOMB_CENTRE : 0));
        frames(bombs, 1, 2);
        hand[1].grip = 1; frames(bombs, 1, 3); assert(b->holder == 1);
        hand[1].v[2] = 0.5f; frames(bombs, 1, 3);
        hand[1].grip = 0; frame(bombs, 1);
        assert(b->holder < 0 && fabsf(b->release_v[2] - 0.5f) < 0.05f && fabsf(b->release_v[1]) < 0.05f);
        hand[1].v[2] = 0;
        /* a Z-target: the arc ends on it */
        locked = 1; lock_at[0] = 0.3f, lock_at[1] = 0.5f, lock_at[2] = 5;
        for (int k = 0; k < 3; k++) putf(bombs[0] + 796 + 4 * k, rest[k] - (k == 1 ? BOMB_CENTRE : 0));
        frames(bombs, 1, 2);
        hand[1].grip = 1; frames(bombs, 1, 3);
        for (int i = 0; i < marks_n; i++) if ((int)marks[5 * i] == 2) memcpy(land, marks + 5 * i + 1, 12);
        assert(fabsf(land[0] - 0.3f) < 1e-3f && fabsf(land[1] - 0.5f) < 1e-3f && fabsf(land[2] - 5) < 1e-3f);
        locked = 0; options[951] = 0;
        frames(bombs, 1, 2);
        assert(mark_count(1) == 0);   /* (Free throw: no arc) */
        /* whatever is selected (here nothing), a hand holding a bomb keeps its trigger from the game: ZR for the right */
        hos_vr_selected_is_bomb = 0;
        assert(!(hos_vr_bomb_buttons((1ULL << 9) | (1ULL << 8)) & (1ULL << 9)));
        assert(hos_vr_bomb_buttons(1ULL << 8) & (1ULL << 8));   /* (the left trigger: the left hand holds nothing) */
        hand[1].grip = 0; frames(bombs, 1, 3);
        b->actor = 0;

        /* ---- bombs selected: a grip anywhere takes one out (the game's ZR), shown in the hand while Link's lift has it,
           and the game's lifter call (not ZR, Link's throw) ends his hold. Nearest first: a bomb within 25 cm of the hand
           is taken instead (a catch), and the sword's hand takes none ---- */
        hos_vr_selected_is_bomb = 1;
        {   /* (a loose bomb 20 cm from the hand's surface reach: that one, no new one) */
            uint64_t loose[1] = {make_actor(9, rest[0], rest[1] - BOMB_CENTRE, rest[2] + 0.2f + BOMB_RADIUS)};
            uint64_t node = HOS_HEAP_BASE + 0x80000, a = node + 0x100, nb = node + 0x200;
            put(node + 8, a, 8); put(a + 8, nb, 8); put(nb + 496, loose[0], 8); c.x[0] = node; hos_vr_bomb_ai(&c);
            VrBomb *l = vr_bomb_find(loose[0]);
            l->owned = 1;
            frames(loose, 1, 2);
            hand[1].grip = 1; frames(loose, 1, 2);
            assert(vr_bomb_pull == PULL_IDLE && l->holder == 1);
            hand[1].grip = 0; frames(loose, 1, 2);
            l->actor = 0;
        }
        sword_claims = 1;
        hand[1].grip = 1; frames(bombs, 0, 2);
        assert(vr_bomb_pull == PULL_IDLE);                 /* (the sword's hand: no bomb) */
        hand[1].grip = 0; frames(bombs, 0, 2);
        sword_claims = -1;
        hand[1].p[0] = 0.3f, hand[1].p[1] = 1.4f, hand[1].p[2] = 0.4f;   /* (out in front, nowhere near the hips) */
        hand[1].grip = 1; frame(bombs, 0);
        assert(vr_bomb_pull == PULL_TAKE && (hos_vr_bomb_buttons(0) & (1ULL << 9)));
        put(lifter + 0x2b0, 2, 4);                         /* (Link's Lifter holds it) */
        frames(bombs, 0, 3);
        bombs[0] = make_actor(6, 0, 1.5f, 0);   /* (it appears over Link's head) */
        { uint64_t node = HOS_HEAP_BASE + 0x80000, a = node + 0x100, nb = node + 0x200;
          put(node + 8, a, 8); put(a + 8, nb, 8); put(nb + 496, bombs[0], 8); c.x[0] = node; hos_vr_bomb_ai(&c); }
        frame(bombs, 1);
        b = vr_bomb_find(bombs[0]);
        assert(vr_bomb_pull == PULL_THROW && b->holder == 1 && b->owned);
        assert(state[1] == 1 && state[3] == 1);            /* (the renderer: in the right hand, Link's lift has it) */
        frames(bombs, 1, 80);
        assert(vr_bomb_pull == PULL_IDLE && b->holder == 1 && state[3] == 0);
        assert(falls == 1 && fall_args_ok && !(hos_vr_bomb_buttons(0) & (1ULL << 9)));   /* (the call, no second ZR) */
        hand[1].grip = 0; frames(bombs, 1, 3);
        memcpy(hand[1].p, rest, 12);
        b->actor = 0;
        hos_vr_selected_is_bomb = 0;

        /* ---- a Bomb Flower beside Link, found by its name: without the Goron's Bracelet, not the hands' ---- */
        flower_actor = make_actor(5, rest[0], rest[1] - BOMB_CENTRE, rest[2] + 0.05f);
        plant_actor = make_actor(7, rest[0], rest[1] - BOMB_CENTRE - 0.1f, rest[2] + 0.05f);   /* (its plant, the leaves) */
        uint64_t flowers[2] = {flower_actor, plant_actor};
        bracelet = 0; clock_ns += 600000000LL;
        frames(flowers, 2, 3);
        assert(!vr_bomb_find(flower_actor));             /* (not looked for without the bracelet) */
        bracelet = 1; clock_ns += 600000000LL;
        frames(flowers, 2, 3);
        assert(!vr_bomb_find(plant_actor));              /* (the plant is never the hands') */
        VrBomb *f = vr_bomb_find(flower_actor);
        assert(f && f->flower && !f->owned);
        options[956] = 0; hand[1].grip = 1; frames(flowers, 1, 3);
        assert(f->holder < 0 && !hos_vr_bomb_in_reach(1));   /* (VR tab Bomb Flowers in the hands: Off) */
        hand[1].grip = 0; frames(flowers, 1, 2);
        /* on its plant, a grip asks the game to pick it (its A); not picked in time (Link not beside it): given up */
        assert(f->on_plant);
        options[956] = 1; hand[1].grip = 1; frame(flowers, 1);
        options[951] = 1; frame(flowers, 1);
        assert(f->holder == 1 && f->picking_at && (hos_vr_bomb_buttons(0) & 1));
        assert(mark_count(1) == 0);                        /* (no throw arc until it is in the hand) */
        options[951] = 0;
        const float plant[3] = {getf(flower_actor + 796), getf(flower_actor + 800), getf(flower_actor + 804)};
        frames(flowers, 1, 20);
        assert(getf(flower_actor + 796) == plant[0] && getf(flower_actor + 804) == plant[2]);   /* (left on its plant meanwhile) */
        frames(flowers, 1, 140);
        assert(f->holder < 0 && !f->owned);
        hand[1].grip = 0; frames(flowers, 1, 2);
        /* again, and the game picks it: Link's carry stance; the hand has it then (an A tap ends his carry), quiet and unlit */
        const uint64_t object = HOS_HEAP_BASE + 0x90000;
        oot_vr_player_object = object;
        flower_actor2 = make_actor(8, 0.1f, 2.0f, 0.1f);   /* (another fruit, the one the game lifts: over Link's head) */
        uint64_t three[3] = {flower_actor, plant_actor, flower_actor2};
        frames(three, 3, 2);
        VrBomb *f2 = vr_bomb_find(flower_actor2);
        assert(f2 && f2->on_plant);
        hand[1].grip = 1; frames(three, 3, 5);
        assert(f->holder == 1 && f2->holder < 0);
        put(object + 0x1f60, 0x17060000, 4); frames(three, 3, 2);
        assert(f2->holder == 1 && f2->owned && !f2->on_plant && f->holder < 0 && !f->owned && f->on_plant);
        assert(state[1] == 2 && state[3] == 1);            /* (a Bomb Flower in the right hand, in Link's lift) */
        /* (back to the first, for what follows: the game lifts that one) */
        put(object + 0x1f60, 0, 4); hand[1].grip = 0; frames(three, 3, 3);
        f2->actor = 0; flower_actor2 = 0;
        f->on_plant = 1; f->owned = 0; f->holder = -1;
        hand[1].grip = 1; frames(flowers, 2, 5);
        const int falls_before = falls;
        put(object + 0x1f60, 0x17060000, 4); frames(flowers, 2, 2);
        assert(!f->on_plant && f->holder == 1 && f->owned && sleep_reasons(flower_actor) == BOMB_SLEEP_REASON);
        clock_ns += 200000000LL; frame(flowers, 2);
        assert(falls > falls_before && !(hos_vr_bomb_buttons(0) & 1));   /* (his carry ended by the lifter call, not A) */
        assert(fabsf(held_radius[1] - 0.2638f) < 1e-4f);   /* (the fingers on the fruit's own size, from its model) */
        put(object + 0x1f60, 0, 4); command = 0x3333; frames(flowers, 2, 5);   /* (its animation changes as it is picked) */
        assert(!f->lit);                                   /* (not taken for being set off) */
        frames(flowers, 2, 80); command = 0x3333; frame(flowers, 2);
        {   /* (placed with the hand now: the fruit's middle, its own height above its origin, at the palm's seat) */
            const float seat_z = rest[2] + (BOMB_PALM + 0.2638f);   /* (this hand's palm faces +z) */
            if (getenv("BOMB_CHECK_LOG")) fprintf(stderr, "fruit at %.3f %.3f %.3f\n", getf(flower_actor + 796), getf(flower_actor + 800), getf(flower_actor + 804));
            assert(fabsf(getf(flower_actor + 800) + 0.2226f - rest[1]) < 0.02f && fabsf(getf(flower_actor + 804) - seat_z) < 0.02f &&
                   fabsf(getf(flower_actor + 796) - rest[0]) < 0.02f);
        }
        hand[1].trigger = 1; frame(flowers, 2); hand[1].trigger = 0; frame(flowers, 2);
        assert(f->lit && sleep_reasons(flower_actor) == 0);
        return 0;
    }
    ''', flags=('-D_DEFAULT_SOURCE', '-pthread'))


def check_bomb_input_snapshot():
    """the pad read sees the bombs' state as one snapshot (review finding 6): published by Link's update while other threads
    read it, a read is always one whole publish, never a mix; the game side's records lock lets one update in at a time,
    is taken again by the same thread without waiting, and counts the entries that had to wait"""
    source = read('bomb_host').replace('#include "bomb_math.h"', read('bomb_math'))
    compile_run('#include <stdarg.h>\n' + BOMB_GAME +
                'int hos_vr_ocarina_claims(int hand) { (void)hand; return 0; }\n' +
                'int hos_vr_sword_claims(int hand) { (void)hand; return 0; }\n' +
                'void hos_vr_held_keep(Ctx *c, uint64_t actor) { (void)c; (void)actor; }\n' + source + r'''
    #include <stdatomic.h>
    static const VrBombInput A = {1, {1, 0}, {1, 0}, 1000, 1000, 5}, B = {0, {0, 1}, {0, 1}, 2000, 2000, 6}, Z = {0, {0, 0}, {0, 0}, 0, 0, 0};
    static atomic_int stop;
    static int same(const VrBombInput *x, const VrBombInput *y) {
        return x->owned == y->owned && x->holds[0] == y->holds[0] && x->holds[1] == y->holds[1] && x->claims[0] == y->claims[0] &&
               x->claims[1] == y->claims[1] && x->zr_until == y->zr_until && x->a_until == y->a_until && x->at == y->at;
    }
    static void *writer(void *arg) {
        (void)arg;
        for (int i = 0; i < 2000000; i++) vr_bomb_publish(i & 1 ? &B : &A);
        atomic_store(&stop, 1);
        return NULL;
    }
    static atomic_long mixes, reads;
    static void *reader(void *arg) {
        (void)arg;
        VrBombInput in;
        while (!atomic_load(&stop)) {
            vr_bomb_read_input(&in);
            if (!same(&in, &A) && !same(&in, &B) && !same(&in, &Z)) atomic_fetch_add(&mixes, 1);
            atomic_fetch_add(&reads, 1);
        }
        return NULL;
    }
    static long counted;   /* (changed only under vr_bomb_lock: no atomics, so a missed exclusion loses counts) */
    static void *updater(void *arg) {
        (void)arg;
        for (int i = 0; i < 200000; i++) {
            vr_bomb_enter();
            vr_bomb_enter();   /* (again on the same thread: no wait, no deadlock) */
            const long was = counted;
            counted = was + 1;
            vr_bomb_leave();
            vr_bomb_leave();
        }
        return NULL;
    }
    int main(void) {
        pthread_t w, r[3];
        pthread_create(&w, NULL, writer, NULL);
        for (int i = 0; i < 3; i++) pthread_create(&r[i], NULL, reader, NULL);
        pthread_join(w, NULL);
        for (int i = 0; i < 3; i++) pthread_join(r[i], NULL);
        assert(atomic_load(&reads) > 1000 && atomic_load(&mixes) == 0);
        /* the input side's answers come from the snapshot: A says the left hand holds a bomb and its grip is the bombs' */
        for (int i = 0; i < 1000; i++) options[i] = -1;
        options[904] = 1;
        VrBombInput now_a = A;
        now_a.at = clock_ns;
        vr_bomb_publish(&now_a);
        assert(hos_vr_bomb_in_reach(0) && !hos_vr_bomb_in_reach(1) && !hos_vr_bomb_in_reach(2) && !hos_vr_bomb_in_reach(-1));
        assert(hos_vr_bomb_grips() == 1);
        assert(!(hos_vr_bomb_buttons(1ULL << 8) & (1ULL << 8)));   /* (the left trigger kept from the game) */
        /* an old snapshot (Link not updated for half a second: a map load) or the feature off: nothing held, nothing claimed */
        clock_ns += 600000000LL;
        assert(!hos_vr_bomb_in_reach(0) && !hos_vr_bomb_grips() && (hos_vr_bomb_buttons(1ULL << 8) & (1ULL << 8)));
        now_a.at = clock_ns;
        vr_bomb_publish(&now_a);
        assert(hos_vr_bomb_in_reach(0));
        options[904] = 0;
        assert(!hos_vr_bomb_in_reach(0) && !hos_vr_bomb_grips());
        /* the lock: four updaters, 200000 entries each, all counted; at least one had to wait */
        pthread_t u[4];
        for (int i = 0; i < 4; i++) pthread_create(&u[i], NULL, updater, NULL);
        for (int i = 0; i < 4; i++) pthread_join(u[i], NULL);
        assert(counted == 800000 && vr_bomb_lock_depth == 0);
        assert(__atomic_load_n(&vr_bomb_lock_waits, __ATOMIC_RELAXED) > 0);
        return 0;
    }
    ''', flags=('-D_DEFAULT_SOURCE', '-pthread', '-O2'))


def check_bomb_marks():
    """the bombs' marks in the headset: what the game side sends is kept (bad marks dropped), the target lies flat on the
    world's up, the target has a centre and the pull's old ring (kind 3) is refused"""
    source = read('bomb_marks')
    start = source.index('constexpr int kBombMarkPx')
    end = source.index('bool bomb_mark_make(int which)')
    flat_at = source.index('XrQuaternionf bomb_mark_flat(')
    flat_end = source.index('/// The bombs\' marks as quad layers')
    compile_run(r'''
    #include <algorithm>
    #include <cassert>
    #include <chrono>
    #include <cmath>
    #include <mutex>
    #include <vector>
    struct XrSwapchainImageVulkanKHR { int image; };
    struct Eye { int chain = 0; unsigned w = 0, h = 0; std::vector<XrSwapchainImageVulkanKHR> images; };
    struct XrQuaternionf { float x, y, z, w; };
    ''' + source[start:end] + source[flat_at:flat_end] + r'''
    static void rotate(XrQuaternionf q, const float v[3], float out[3]) {   /* (q v q*) */
        const float t[3] = {2 * (q.y * v[2] - q.z * v[1]), 2 * (q.z * v[0] - q.x * v[2]), 2 * (q.x * v[1] - q.y * v[0])};
        out[0] = v[0] + q.w * t[0] + (q.y * t[2] - q.z * t[1]);
        out[1] = v[1] + q.w * t[1] + (q.z * t[0] - q.x * t[2]);
        out[2] = v[2] + q.w * t[2] + (q.x * t[1] - q.y * t[0]);
    }
    int main() {
        const float m[5 * 5] = {1, 0, 1, 2, .04f,   2, 0, 0, 5, .8f,   7, 0, 0, 0, .1f,   2, NAN, 0, 0, .5f,
                                3, 0, 0, 0, .5f};   /* (3: the pull's ring, gone: refused) */
        bomb_marks_set(m, 5);
        assert(bomb_marks_n == 2 && bomb_marks_in[0].kind == kBombMarkDot && bomb_marks_in[1].kind == kBombMarkTarget);
        bomb_marks_set(nullptr, 0);
        assert(bomb_marks_n == 0);
        const float ups[3][3] = {{0, 1, 0}, {0.1f, 0.99f, 0}, {0, 0.98f, -0.2f}};
        for (const auto& up : ups) {   /* the target's face (+z) along the world's up */
            const float z[3] = {0, 0, 1};
            float out[3];
            rotate(bomb_mark_flat(up), z, out);
            const float n = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
            for (int k = 0; k < 3; k++) assert(std::fabs(out[k] - up[k] / n) < 1e-4f);
        }
        assert(bomb_mark_alpha(3, .88f, 0) == 0);   /* (no third picture) */
        assert(bomb_mark_alpha(kBombMarkTarget, 0, 0) > .9f && bomb_mark_alpha(kBombMarkTarget, .5f, 0) == 0 &&
               bomb_mark_alpha(kBombMarkTarget, .82f, 0) > .9f);
        assert(bomb_mark_alpha(kBombMarkDot, 0, 0) > .9f && bomb_mark_alpha(kBombMarkDot, .9f, 0) == 0);
        for (int k = 1; k <= 2; k++) assert(bomb_mark_alpha(k, 1, 1) == 0);   /* (the corners clear) */
        return 0;
    }
    ''', cpp=True)


def check_bomb_flower_model():
    """a Bomb Flower's fruit is told apart from its plant and from a bomb by its draws (vk_vr_bomb_flower.h, made from your
    game's files), with a fruit's size; the renderer moves only those (needs --romfs or --work)"""
    header = generated('vk_vr_bomb_flower.h').read_text()
    shapes = [int(v) for v in re.search(r'flower_shapes\[\d+\] = \{([^}]*)\}', header).group(1).split(',')]
    radius = float(re.search(r'flower_radius = ([0-9.]+)f', header).group(1))
    assert len(shapes) >= 2 and not {120288, 110448} & set(shapes), f'the fruit\'s draws {shapes} are a bomb\'s'
    assert 0.15 < radius < 0.4, f'the fruit\'s radius {radius} m is not a fruit\'s'
    if ARGS.romfs:   # (the plant's model must not be taken for the fruit: the dungeon bug, 19 Nov)
        _, game_files = game_tools()
        import models
        plant = models.Models(game_files.GameFiles(ARGS.romfs))['Model/Obj_BombFlower_A.Obj_BombFlower_A_03.bfres.zs']
        assert not {s['vb0'] for s in plant['shapes']} & set(shapes), 'the plant\'s draws are taken for the fruit\'s'
    centre = float(re.search(r'flower_centre\[3\] = \{[-0-9.]+f, ([-0-9.]+)f', header).group(1))
    assert 0.15 < centre < 0.35, f'the fruit\'s middle {centre} m above its origin is not where its collision has it'
    # (the renderer leaves the fruit's draws alone: its body is rigid, outside the palette; moving the palette left the
    # stem floating off it, 19 Nov) and hands the game side its size
    skin = (REPO / 'vr/items/gear/equipment_skin.inc').read_text(encoding='utf-8')
    assert 'flower_model' not in skin and 'bool flower_shape(float out[4])' in skin


def check_bomb_options():
    """the bombs' VR tab rows reach the game side: options 950 and 953-956 (vr/items/bomb/bomb_host.c) in fill_replay_table,
    each the value it should be, and every row named in the VR tab with a default; the parked arc's options (951, 952) gone"""
    replay = read('vk_replay')
    wanted = {950: 'vr_bomb_quiet', 953: 'vr_bomb_pull', 954: 'vr_bomb_pull_reach', 955: 'vr_bomb_pull_cone', 956: 'vr_bomb_flowers'}
    found = {id: re.search(rf'if \(id == {id}\) return opts\.(\w+);', replay) for id in wanted}
    found = {id: m.group(1) if m else None for id, m in found.items()}
    assert found == wanted, f'options 950-956 are {found}, want {wanted}'
    rows = (REPO / 'vr/menu/vr_tab_rows.inc').read_text(encoding='utf-8')
    defaults = (REPO / 'vr/menu/vr_options.inc').read_text(encoding='utf-8')
    host = read('bomb_host')
    built_in = ('vr_bomb_quiet', 'vr_bomb_flowers')   # (Unlit bomb fire None, Bomb Flowers in the hands: built in, 19 Nov)
    assert 'vr_bomb_flower_scale' not in defaults, 'Bomb Flower size is back: it sized only the fuse (19 Nov)'
    for name in wanted.values():
        if name in built_in:
            assert f'&opts.{name},' not in rows and re.search(rf'static constexpr int {name} = 1;', defaults), f'{name}: built in'
            continue
        assert f'&opts.{name},' in rows, f'{name}: a row missing'
        assert re.search(rf'int {name} = ', defaults), f'{name} has no default'
    # (Arc to target, parked 19 Nov, out for the release: no option, no default, no value words; a player's throw is free)
    for gone in ('vr_bomb_aim', 'vr_bomb_arc_reach'):
        assert gone not in defaults and gone not in replay and gone not in rows, f'{gone} is back'
    assert 'id == 951' not in replay and 'id == 952' not in replay
    assert 'tx_vr_bomb_aim' not in (REPO / 'vr/menu/vr_values_menu.inc').read_text(encoding='utf-8')
    for id in (950, 953, 954, 955, 956):
        assert f'rx_option({id}' in host, f'option {id} is not read by bomb_host.c'
    # the marks' limit is the same on both sides
    assert re.search(r'#define BOMB_MARKS_MAX 12\b', host) and 'kBombMarksMax = 12;' in read('bomb_marks')


# ======================================================================================================================
# The shield
# ======================================================================================================================

def check_guard_heading():
    """the untargeted guard faces the way the stick moves Link: the view's heading (with the head's yaw in Headset mode)"""
    source=read('guard_host')
    compile_run(r'''
    #include <stdint.h>
    #include <stddef.h>
    #include <assert.h>
    #include <string.h>
    #include <math.h>
    #define HOS_HEAP_BASE 0x1000000000ULL
    typedef struct { uint64_t x[31]; unsigned char vreg[32][16]; } Ctx;
    typedef struct { int active,mode,move_head; float q[4]; } HookshotVrHead;
    static HookshotVrHead head={1,1,0,{0,0,0,1}};
    static unsigned char memory[0x10000];
    static uint64_t oot_vr_player_actor=HOS_HEAP_BASE+0x1000;
    static volatile int oot_vr_quick_open;
    static float oot_vr_turn_yaw;
    static int64_t clock_ns=1000000000;
    static int enabled=1;
    static int64_t hos_now_ns(void) { return clock_ns; }
    static int rx_option(int id,int fallback) { return id==903?enabled:fallback; }
    static int rx_vr_head(HookshotVrHead *h) { *h=head;return 1; }
    static void hos_log(const char *s,...) { (void)s; }
    static void *hos_guest_range(uint64_t a,uint64_t n) {
        return a>=HOS_HEAP_BASE && a-HOS_HEAP_BASE<=sizeof memory && n<=sizeof memory-(a-HOS_HEAP_BASE)
            ? memory+(a-HOS_HEAP_BASE) : NULL;
    }
    static uint64_t get(uint64_t a,size_t n) { uint64_t v=0;void *p=hos_guest_range(a,n);assert(p);memcpy(&v,p,n);return v; }
    static void put(uint64_t a,uint64_t v,size_t n) { void *p=hos_guest_range(a,n);assert(p);memcpy(p,&v,n); }
    static uint64_t recomp_load64(Ctx*c,uint64_t a) { (void)c;return get(a,8); }
    static uint32_t recomp_load32(Ctx*c,uint64_t a) { (void)c;return get(a,4); }
    static uint16_t recomp_load16(Ctx*c,uint64_t a) { (void)c;return get(a,2); }
    static void vr_store(Ctx*c,uint64_t a,float v) { (void)c;void *p=hos_guest_range(a,4);assert(p);memcpy(p,&v,4); }
    static float regf(Ctx *c,int r) { float f;memcpy(&f,c->vreg[r],4);return f; }
    static void setf(Ctx *c,int r,float f) { memcpy(c->vreg[r],&f,4); }
    ''' + source + r'''
    static int close_angle(float a,float b) { return fabsf(remainderf(a-b,6.283185307f))<1e-5f; }
    int main(void) {
        Ctx c={0};
        const uint64_t actor=oot_vr_player_actor,table=HOS_HEAP_BASE+0x2000,
                       combat=HOS_HEAP_BASE+0x4000,guard=HOS_HEAP_BASE+0x8000,vec=HOS_HEAP_BASE+0x9000;
        put(actor,0x84b45e18ULL,8);put(actor+0x270,table,8);
        put(table+0x358,combat,8);put(table+0x248,guard,8);
        put(combat+0x18,actor,8);put(guard,0x84b6a258ULL,8);put(guard+0x18,actor,8);
        put(guard+0x34,0x100,2);put(combat+0x1c20,UINT32_MAX,4);put(combat+0x1c28,UINT32_MAX,4);
        put(combat+0x20,0x408,4);
        uint64_t resolved=0;assert(oot_vr_guard_native(&c,actor,&resolved) && resolved==combat);
        put(guard+0x34,2,2);assert(oot_vr_guard_native(&c,actor,NULL)); // confirmed 408 survives transient query clears
        oot_vr_guard_forget_native();assert(!oot_vr_guard_native(&c,actor,NULL)); // 408 without prior native confirmation is NOT enough
        put(guard+0x34,0x100,2);assert(oot_vr_guard_native(&c,actor,NULL));
        put(guard+0x34,0,2);put(combat+0x20,0,4);assert(!oot_vr_guard_native(&c,actor,NULL)); // release revokes, no timed sticky guard
        put(combat+0x20,0x408,4);assert(!oot_vr_guard_native(&c,actor,NULL)); // later same action needs fresh confirmation
        put(guard+0x34,0x100,2);
        put(combat+0x1c38,1,4);put(combat+0x1c14,0x42c80000,4);   // locked on: the flag and the target's point (x 100)
        assert(!oot_vr_guard_native(&c,actor,NULL)); // a target: the game's own guard
        float point[3];assert(oot_vr_lock_point(&c,actor,point)==1 && point[0]==100);
        put(combat+0x1c14,0x7fc00000,4);assert(oot_vr_lock_point(&c,actor,point)==0);   // its x a NaN: no target
        put(combat+0x1c14,0x42c80000,4);put(combat+0x1c38,0x40,4);assert(oot_vr_lock_point(&c,actor,point)==0);   // flag clear
        put(combat+0x1c20,123,4);assert(oot_vr_lock_point(&c,actor,point)==0);put(combat+0x1c20,UINT32_MAX,4);   // (the link is not it)
        put(combat+0x18,0,8);assert(!oot_vr_guard_native(&c,actor,NULL));put(combat+0x18,actor,8);
        put(guard,0,8);assert(!oot_vr_guard_native(&c,actor,NULL));put(guard,0x84b6a258ULL,8);
        assert(!oot_vr_guard_native(&c,actor+1,NULL));

        OotVrGuardView s={0};float offset=0;
        assert(close_angle(oot_vr_guard_yaw(&s,actor,0,.3f,0,.4f,&offset),.3f));
        // Native camera has snapped to 2 rad on guard entry: rendered base stays at prior .3 rad.
        assert(close_angle(oot_vr_guard_yaw(&s,actor,1,2,0,.4f,&offset),.3f));
        assert(close_angle(s.facing,.3f+.4f+3.141592654f));
        assert(close_angle(oot_vr_guard_yaw(&s,actor,1,-2,0,1.1f,&offset),.3f)); // free head look, latched guard
        assert(close_angle(s.facing,.3f+.4f+3.141592654f));
        assert(close_angle(oot_vr_guard_yaw(&s,actor,1,-2,.2f,1.1f,&offset),.5f)); // explicit stick turn
        assert(close_angle(s.facing,.5f+.4f+3.141592654f));
        assert(close_angle(oot_vr_guard_yaw(&s,actor,0,1.7f,.2f,0,&offset),.5f));
        assert(close_angle(offset,-1.2f)); // preserve release frame by rebasing old camera offset
        assert(close_angle(oot_vr_guard_yaw(&s,actor+8,1,-1,0,0,&offset),-1)); // new actor never inherits old heading

        oot_vr_guard_reset();float back[3]={0,0,1};oot_vr_guard_camera(&c,&head,0,back);
        head.q[1]=sinf(.25f);head.q[3]=cosf(.25f); // headset yaw .5 radians at raising
        back[0]=1;back[2]=0;oot_vr_guard_camera(&c,&head,1,back);
        assert(fabsf(back[0])<1e-5f && fabsf(back[2]-1)<1e-5f);
        c.x[0]=combat;c.x[1]=vec;
        memset(c.vreg,0xa5,sizeof c.vreg);setf(&c,0,1.234f);
        Ctx before=c;
        unsigned char saved[sizeof memory];memcpy(saved,memory,sizeof memory);
        hos_vr_guard_direction(&c);
        // Camera mode: the game moves Link relative to the view's heading alone (base pi/2: back (1,0,0)), so the guard
        // faces it (the opposite of back), whatever the head does
        const float cam=1.570796327f+3.141592654f;
        assert(fabsf(regf(&c,3)-sinf(cam))<1e-5f && regf(&c,2)==0 && fabsf(regf(&c,1)-cosf(cam))<1e-5f);
        assert(fabsf(regf(&c,4)-sinf(cam)*sinf(cam))<1e-5f); // native partial norm must be coherent
        for(int r=1;r<=4;r++)memcpy(before.vreg[r],c.vreg[r],4);
        assert(!memcmp(&before,&c,sizeof c)); // ONLY low 4 bytes of s1/s2/s3/s4, not s0 or other registers
        assert(!memcmp(saved,memory,sizeof memory)); // read-only caller direction and all guest memory unchanged

        // Replay the failure pattern: no-target guard action remains 408 while the query bit clears and the native
        // facing request turns during strafing; Headset mode (the head's yaw .5 counts), the view turned by the right
        // stick meanwhile (base .02 a frame): the guard follows the view every frame, so forward never veers.
        head.move_head=1;
        for(int frame=0;frame<30;frame++) {
            put(guard+0x34,frame%3==0?0x100:0,2);
            assert(oot_vr_guard_native(&c,actor,NULL));
            const float native_yaw=frame*.13f, base=frame*.02f, want=base+.5f+3.141592654f;
            back[0]=sinf(base);back[2]=cosf(base);
            oot_vr_guard_camera(&c,&head,1,back);
            setf(&c,3,sinf(native_yaw));setf(&c,2,.2f);setf(&c,1,cosf(native_yaw));setf(&c,4,99);
            hos_vr_guard_direction(&c);
            assert(fabsf(regf(&c,3)-sinf(want))<1e-4f && regf(&c,2)==0 && fabsf(regf(&c,1)-cosf(want))<1e-4f);
            assert(fabsf(regf(&c,4)+regf(&c,1)*regf(&c,1)-1)<1e-5f); // native remaining z*z yields unit norm
            clock_ns+=16000000LL;
        }
        head.move_head=0;

        setf(&c,3,99);head.mode=0;hos_vr_guard_direction(&c);assert(regf(&c,3)==99);
        head.mode=1;head.active=0;hos_vr_guard_direction(&c);assert(regf(&c,3)==99);head.active=1;
        enabled=0;hos_vr_guard_direction(&c);assert(regf(&c,3)==99);enabled=1;
        oot_vr_quick_open=1;hos_vr_guard_direction(&c);assert(regf(&c,3)==99);oot_vr_quick_open=0;
        c.x[0]=combat+8;hos_vr_guard_direction(&c);assert(regf(&c,3)==99);c.x[0]=combat;
        put(combat+0x1c38,0x41,4);Ctx targeted=c;hos_vr_guard_direction(&c);
        assert(!memcmp(&targeted,&c,sizeof c)); // targeted call remains bit-for-bit native
        assert(oot_vr_lock_target(&c,oot_vr_player_actor)==1);   // (the view turns to the target: view_host.c)
        put(combat+0x1c38,0x40,4);
        assert(oot_vr_lock_target(&c,oot_vr_player_actor)==0 && oot_vr_lock_target(&c,0)==-1);
        clock_ns+=200000001LL;hos_vr_guard_direction(&c);assert(regf(&c,3)==99);
        oot_vr_guard_camera(&c,&head,1,back);oot_vr_guard_reset();hos_vr_guard_direction(&c);assert(regf(&c,3)==99);

        // Headset-directed movement is opt-in: with it on, guard direction follows new head yaw.
        put(guard+0x34,0x100,2); // a new guard session needs fresh native confirmation
        back[0]=0;back[1]=0;back[2]=1;
        head.move_head=1;oot_vr_guard_camera(&c,&head,0,back);oot_vr_guard_camera(&c,&head,1,back);
        head.q[1]=sinf(.5f);head.q[3]=cosf(.5f);oot_vr_guard_camera(&c,&head,1,back);
        hos_vr_guard_direction(&c);assert(fabsf(regf(&c,3)+sinf(1))<1e-5f);
        // Looking aside while guarding: the stick's forward goes where the head looks, so the guard turns with it.
        head.q[1]=sinf(.9f);head.q[3]=cosf(.9f);oot_vr_guard_camera(&c,&head,1,back);
        hos_vr_guard_direction(&c);assert(fabsf(regf(&c,3)+sinf(1.8f))<1e-5f && fabsf(regf(&c,1)+cosf(1.8f))<1e-5f);
        head.mode=0;oot_vr_guard_camera(&c,&head,1,back);assert(!oot_vr_guard_seen);
        return 0;
    }
    ''')

    patch=read('game_hooks')+read('view_host')
    assert '(0x93533c, 0xbd400821,' in patch
    assert '(0x935288, 0x1e210800,' not in patch
    assert 'oot_vr_guard_camera(c, &h, guarding, guard_back);' in patch   # (the guard's facing only: the view is not changed)
    assert '(aiming || guarding)' in patch

def check_heading_realign():
    """first person: an event that turns Link (a talk, a warp, a map change) turns the view's heading by as much, so the
    stick still moves him straight ahead: at its end as a quick turn or a snap, at once after a flat stretch or at a warp,
    or following him; never for turns the player's own stick made, small ones, while locked on, or with the option off"""
    view = read('view_host')
    start = view.index('#define kVrRealignMinTurn')
    end = view.index('\n}\n', view.index('static float oot_vr_realign_turn(')) + 3
    compile_run(r'''
    #include <stdint.h>
    #include <stdio.h>
    #include <stdarg.h>
    #include <string.h>
    #include <math.h>
    #include <assert.h>
    static char said[4096];
    static void hos_log(const char *f, ...) {
        char line[512]; va_list a; va_start(a, f); vsnprintf(line, sizeof line, f, a); va_end(a);
        if (strlen(said) + strlen(line) + 2 < sizeof said) strcat(said, line), strcat(said, "\n");
    }
    static int item_get_view = 1;   /* (VR tab, Item get view: 1 keep your view, 0 turn with Link) */
    static int rx_option(int id, int otherwise) { return id == 982 ? item_get_view : otherwise; }
    ''' + view[start:end] + r'''
    static const float D = 0.0174532925f, DT = 1 / 60.0f;
    static int64_t t = 1000000000LL;
    static float base;   /* the view's heading (its back), as view_host.c keeps it */
    static float link_facing, link_pos[3];
    static OotVrRealign s;
    static int close(float a, float b, float tol) { return fabsf(remainderf(a - b, 6.283185307f)) < tol; }
    /* one game update: what the watcher reads, then the heading's turn (shown: the view was in the headset) */
    static float frame(int style, const char *event, int link_ok, int stick, int shown, int locked) {
        t += 16666667LL;
        oot_vr_realign_watch(&s, t, event, link_ok, remainderf(link_facing, 6.283185307f), link_pos, stick,   /* (as atan2) */
                             base + 3.141592654f);
        if (!shown) return 0;   /* (a flat update: hos_oot_vr_view returns before the heading) */
        const float turn = oot_vr_realign_turn(&s, style, shown, locked, DT, base);
        base += turn;
        return turn;
    }
    static void play(int style, int n) { for (int i = 0; i < n; i++) frame(style, "", 1, 0, 1, 0); }
    static void reset(void) { memset(&s, 0, sizeof s); base = 0; link_facing = 3.141592654f; said[0] = 0; play(1, 5); }
    int main(void) {
        /* a talk shown in VR turns Link 40 degrees (no stick): at its end the view turns 40 degrees, never faster than
           kVrRealignSpeed, and the log names the event */
        for (int style = 1; style <= 2; style++) {
            reset();
            for (int i = 0; i < 20; i++) { link_facing += 2 * D; assert(frame(style, "Talk_Event", 1, 0, 1, 0) == 0); }
            float total = 0, most = 0;
            for (int i = 0; i < 60; i++) { const float d = frame(style, "", 1, 0, 1, 0); total += d; most = fmaxf(most, fabsf(d)); }
            assert(close(total, 40 * D, 1e-3f));
            assert(style == 2 ? close(most, 40 * D, 1e-3f) : most <= kVrRealignSpeed * DT + 1e-5f);
            assert(strstr(said, "heading re-aligned after Talk_Event: from") && strstr(said, style == 2 ? "(snap)" : "(quick turn)"));
            play(style, 30); assert(close(base, 40 * D, 1e-3f));   /* (and stays: once only) */
        }
        /* a wrap past 180 degrees is a 20 degree turn, not 340 */
        reset(); link_facing = 170 * D; play(1, 2);
        for (int i = 0; i < 10; i++) { link_facing += 2 * D; frame(1, "Talk_Event", 1, 0, 1, 0); }
        for (int i = 0; i < 30; i++) frame(1, "", 1, 0, 1, 0);
        assert(close(base, 20 * D, 1e-3f));
        /* a small turn (3 degrees) is left alone, and is not carried into the next event */
        reset();
        for (int i = 0; i < 3; i++) { link_facing += D; frame(1, "Chest", 1, 0, 1, 0); }
        play(1, 30); assert(base == 0);
        for (int i = 0; i < 3; i++) { link_facing += D; frame(1, "Chest", 1, 0, 1, 0); }
        play(1, 30); assert(base == 0);
        /* the player's own stick turning Link during an event is his, not the event's */
        reset();
        for (int i = 0; i < 30; i++) { link_facing += 2 * D; frame(1, "Navi_Talk", 1, 1, 1, 0); }
        play(1, 60); assert(base == 0);
        /* ordinary play: Link's turns never move the view (the stick moves Link relative to it) */
        reset();
        for (int i = 0; i < 60; i++) { link_facing += 3 * D; frame(1, "", 1, 0, 1, 0); }
        assert(base == 0);
        /* a flat cinematic turns Link 90 degrees: the view comes back already turned (at once: nothing was shown) */
        reset(); said[0] = 0;
        for (int i = 0; i < 30; i++) { link_facing -= 3 * D; frame(1, "Dm_Kakariko_0000", 1, 0, 0, 0); }
        frame(1, "", 1, 0, 0, 0);
        t += 16666667LL;
        oot_vr_realign_watch(&s, t, "", 1, link_facing, link_pos, 0, base);
        base += oot_vr_realign_turn(&s, 1, 0, 0, 0, base);   /* (the first update shown again: a gap, dt 0) */
        assert(close(base, -90 * D, 1e-3f) && strstr(said, "as the view came back"));
        /* a map change: Link is gone for a second (held stick or not), then faces the new map's way: the whole change
           counts, and the view's relation to Link is kept */
        reset(); link_facing = 10 * D; play(1, 3);
        frame(1, "Dm_SY_BancChangeGo", 1, 1, 1, 0);
        for (int i = 0; i < 60; i++) frame(1, "", 0, 1, 1, 0);
        link_facing = 100 * D, link_pos[0] = 500;
        said[0] = 0;
        assert(close(frame(1, "DeathMountainTrail_Arrive", 1, 1, 1, 0), 90 * D, 1e-3f));   /* (at once: still dark) */
        assert(strstr(said, "as Link came back"));
        for (int i = 0; i < 60; i++) frame(1, i < 30 ? "DeathMountainTrail_Arrive" : "", 1, 1, 1, 0);
        assert(close(base, 90 * D, 1e-3f));
        link_pos[0] = 0;
        /* a warp during an event shown in VR: the turn is made in the warp's update, with the event still playing */
        reset(); said[0] = 0;
        frame(1, "Warp_Event", 1, 0, 1, 0);
        link_facing += 120 * D, link_pos[2] += 6;
        assert(close(frame(1, "Warp_Event", 1, 0, 1, 0), 120 * D, 1e-3f) && strstr(said, "at the warp"));
        link_pos[2] = 0;
        play(1, 60); assert(close(base, 120 * D, 1e-3f));   /* (not twice at the end) */
        /* locked on: the lock-on turn owns the heading, nothing is added (then or later) */
        reset();
        for (int i = 0; i < 20; i++) { link_facing += 2 * D; frame(1, "Talk_Event", 1, 0, 1, 1); }
        for (int i = 0; i < 5; i++) frame(1, "", 1, 0, 1, 1);
        play(1, 60); assert(base == 0);
        /* off: the heading is never turned */
        reset();
        for (int i = 0; i < 20; i++) { link_facing += 2 * D; frame(0, "Talk_Event", 1, 0, 1, 0); }
        for (int i = 0; i < 60; i++) frame(0, "", 1, 0, 1, 0);
        assert(base == 0);
        /* Follow Link: the view turns with him during the event, at most kVrRealignSpeed, and ends where he does */
        reset();
        float during = 0;
        for (int i = 0; i < 20; i++) {
            link_facing += 4 * D;
            const float d = frame(3, "Talk_Event", 1, 0, 1, 0);
            during += d; assert(fabsf(d) <= kVrRealignSpeed * DT + 1e-5f);
        }
        assert(during > 40 * D);
        play(3, 60); assert(close(base, 80 * D, 1e-3f));
        /* an item get (a chest's, or one picked up) turns Link round for the game's camera: the view keeps its heading,
           in every style, and the chest's warp to its front turns nothing either; with Item get view off, as any event */
        static const char *const gets[] = {"Dm_SY_Treasurebox", "Dm_SY_Get", "Dm_SY_TreasureboxSmall", "Dm_SY_GetInEvent",
                                           "Dm_SY_ShopGet"};
        for (int style = 1; style <= 3; style++)
            for (int k = 0; k < 5; k++) {
                reset();
                frame(style, gets[k], 1, 0, 1, 0);
                link_pos[2] += 2;   /* (the chest's warp) */
                for (int i = 0; i < 60; i++) { link_facing += 2 * D; frame(style, gets[k], 1, 0, 1, 0); }
                link_pos[2] = 0;
                play(style, 60); assert(base == 0);
            }
        /* a chest with an event of its own, or an NPC's gift: once the renderer shows the item, all Link's turns in that
           event are the get's, those before it too (the game turns him round before he raises it: headset 21 Nov, the view
           snapped round at the get's end), and the next event is ordinary again */
        reset();
        for (int i = 0; i < 15; i++) { link_facing += 2 * D; frame(1, "KokiriForest_TreasureBox_NaviChatOff", 1, 0, 1, 0); }
        s.item_shown = 1;
        for (int i = 0; i < 60; i++) { link_facing += 2 * D; frame(1, "KokiriForest_TreasureBox_NaviChatOff", 1, 0, 1, 0); }
        s.item_shown = 0;
        for (int i = 0; i < 10; i++) frame(1, "KokiriForest_TreasureBox_NaviChatOff", 1, 0, 1, 0);
        play(1, 120); assert(base == 0);
        for (int i = 0; i < 10; i++) { link_facing += 2 * D; frame(1, "Talk_Event", 1, 0, 1, 0); }
        play(1, 120); assert(close(base, 20 * D, 1e-3f));
        item_get_view = 0;
        reset();
        for (int i = 0; i < 60; i++) { link_facing += 2 * D; frame(1, "Dm_SY_Get", 1, 0, 1, 0); }
        play(1, 120); assert(close(base, 120 * D, 1e-3f));
        item_get_view = 1;
        return 0;
    }
    ''')
    replay = (REPO / FILES['vk_replay']).read_text()
    assert 'if (id == 927) return opts.vr && opts.vr_mode == 1 ? opts.vr_realign : 0;' in replay
    flat = view.index('if (!oot_vr_player_camera(c, view)')
    assert -1 < view.index('oot_vr_realign_frame(c, &h);') < flat   # (events shown flat count too)
    assert 'base_yaw += oot_vr_realign_turn(&oot_vr_realign, realign, !gap, target == 1, dt, base_yaw);' in view
    assert view.index('oot_vr_realign_turn(&oot_vr_realign') < view.index('const float kVrLockTurn')   # (lock-on after it)
    assert '(gap && (!realign || !related))' in view   # (off, or nothing to relate: after a gap, the game camera, as before)


def check_comfort():
    """first person, Comfort: Smooth turning is as before by default (a full stick 1.6 rad/s), its speed rows scale it; a
    snap turns once a push by its angle and needs the stick back first; the vignette's causes (moving, turning, in the air,
    the game moving Link) read right from Link's position, a stair step or a warp is no fall or motion, a row Off tells
    nothing; every Comfort row Off or unchanged by default, labels and values only; the layer hooked under the menus"""
    view = read('view_host')
    start = view.index('#define kVrTurnDead')
    end = view.index('/* (vr/comfort/comfort_host.c, after this file')
    comfort = read('comfort_host')
    cstart = comfort.index('#define kVrComfortMove')
    cend = comfort.index('/* (view_host.c, each first-person update of the view')
    compile_run(r"""
    #include <stdint.h>
    #include <stdarg.h>
    #include <stdio.h>
    #include <string.h>
    #include <math.h>
    #include <assert.h>
    static int lines;
    static void hos_log(const char *f, ...) { (void)f; lines++; }
    """ + view[start:end] + comfort[cstart:cend] + r"""
    static OotVrComfort st;
    static int64_t now = 1000000000LL, seen;
    static float link[3];
    static float cause[4];
    static void step(float dt, float turned, int64_t stick, int on) {   /* (a game frame: Link read, then the view) */
        now += (int64_t)(dt * 1e9f), seen = now;
        oot_vr_comfort_step(&st, now, turned, stick, seen, link, on, cause);
    }
    int main(void) {
        OotVrTurning t = {0};
        /* Smooth, Medium: today's turn exactly; the dead zone; the speeds */
        assert(fabsf(oot_vr_stick_turn(&t, 0, 1, 1, 1, .1f) - .16f) < 1e-6f);
        assert(fabsf(oot_vr_stick_turn(&t, 0, 1, 1, -.5f, .1f) + .08f) < 1e-6f);
        assert(oot_vr_stick_turn(&t, 0, 1, 1, .1f, .1f) == 0);
        assert(fabsf(oot_vr_stick_turn(&t, 0, 1, 0, 1, .1f) - .08f) < 1e-6f && fabsf(oot_vr_stick_turn(&t, 0, 1, 3, 1, .1f) - .32f) < 1e-6f);
        /* Snap: one turn a push, by its angle, the same way as smooth; back to the middle before the next */
        float total = 0;
        for (int i = 0; i < 30; i++) total += oot_vr_stick_turn(&t, 1, 1, 1, .9f, .016f);
        assert(fabsf(total - 45 * 0.0174532925f) < 1e-5f);
        assert(oot_vr_stick_turn(&t, 1, 1, 1, .5f, .016f) == 0 && oot_vr_stick_turn(&t, 1, 1, 1, .9f, .016f) == 0);   /* (not back yet) */
        assert(oot_vr_stick_turn(&t, 1, 1, 1, .1f, .016f) == 0);
        assert(fabsf(oot_vr_stick_turn(&t, 1, 3, 1, -.8f, .016f) + 90 * 0.0174532925f) < 1e-5f);   /* (90, to the left) */
        oot_vr_stick_turn(&t, 1, 3, 1, 0, .016f);
        assert(fabsf(oot_vr_stick_turn(&t, 1, 0, 1, .7f, .016f) - 30 * 0.0174532925f) < 1e-5f);
        oot_vr_stick_turn(&t, 1, 0, 1, 0, .016f);
        assert(oot_vr_stick_turn(&t, 1, 1, 1, .55f, .016f) == 0);   /* (a push short of kVrSnapPush) */

        const float f = 1 / 30.0f;
        /* standing still: nothing */
        for (int i = 0; i < 20; i++) step(f, 0, 0, 15);
        for (int i = 0; i < 4; i++) assert(cause[i] == 0);
        /* the stick walking him at 3 m/s: moving, not the game moving him */
        for (int i = 0; i < 10; i++) link[0] += 3 * f, step(f, 0, now, 15);
        assert(cause[kVrComfortMoving] == 1 && cause[kVrComfortGameMoves] == 0 && cause[kVrComfortAir] == 0);
        /* the stick let go, Link slowing from 3 m/s: moving fades after the hold, and his coasting is not the game's */
        const int64_t let_go = now;
        for (int i = 0; i < 10; i++) link[0] += 3 * f * (1 - i / 10.0f), step(f, 0, let_go, 15), assert(cause[kVrComfortGameMoves] == 0);
        assert(cause[kVrComfortMoving] == 0);
        for (int i = 0; i < 10; i++) step(f, 0, 0, 15);
        /* turning: a smooth turn's speed, a snap at once; a slow drift not */
        step(f, .016f, 0, 15); assert(cause[kVrComfortTurning] == 1);
        for (int i = 0; i < 10; i++) step(f, .001f, 0, 15);
        assert(cause[kVrComfortTurning] == 0);
        step(f, .785f, 0, 15); assert(cause[kVrComfortTurning] == 1);
        for (int i = 0; i < 10; i++) step(f, 0, 0, 15);
        /* a jump off a ledge: no lift, then a fall under 30 m/s/s: in the air within a few frames */
        float vy = 0;
        int frames = 0;
        for (int i = 0; i < 30 && !cause[kVrComfortAir]; i++, frames++) vy -= 30 * f, link[1] += vy * f, step(f, 0, 0, 15);
        assert(cause[kVrComfortAir] == 1 && frames <= 7);   /* (about 0.2 s into the drop) */
        for (int i = 0; i < 10; i++) step(f, 0, 0, 15);   /* (landed) */
        assert(cause[kVrComfortAir] == 0);
        /* stairs, a flight up and a flight down, a step every few frames or every other (running): never in the air */
        for (int every = 2; every <= 4; every += 2)
            for (int dir = 1; dir >= -1; dir -= 2)
                for (int i = 0; i < 40; i++) {
                    link[0] += 2 * f;
                    if (i % every == 0) link[1] += dir * .18f;
                    step(f, 0, now, 15);
                    assert(cause[kVrComfortAir] == 0);
                }
        /* running at 5 m/s over humps (Hyrule Field: up and down at most 2.6 m/s, a hump every 0.4 s): not in the air */
        for (int i = 0; i < 90; i++) {
            link[0] += 5 * f, link[1] += 2.6f * sinf(6.2831853f * i / 12) * f, step(f, 0, now, 15);
            assert(cause[kVrComfortAir] == 0);
        }
        /* running down a 45 degree slope at 5.3 m/s: not in the air */
        for (int i = 0; i < 30; i++) {
            link[0] += 3.75f * f, link[1] -= 3.75f * f, step(f, 0, now, 15);
            assert(cause[kVrComfortAir] == 0);
        }
        for (int i = 0; i < 10; i++) step(f, 0, 0, 15);
        /* the game moving him: a hookshot pull at 12 m/s with the stick at rest; Epona at 9 m/s with it pushed */
        for (int i = 0; i < 6; i++) link[2] += 12 * f, step(f, 0, 0, 15);
        assert(cause[kVrComfortGameMoves] == 1 && cause[kVrComfortMoving] == 0);
        for (int i = 0; i < 10; i++) step(f, 0, 0, 15);
        assert(cause[kVrComfortGameMoves] == 0);
        for (int i = 0; i < 6; i++) link[2] += 9 * f, step(f, 0, now, 15);
        assert(cause[kVrComfortGameMoves] == 1 && cause[kVrComfortMoving] == 1);
        for (int i = 0; i < 10; i++) step(f, 0, 0, 15);
        /* a warp of 40 m: no motion */
        link[0] += 40, step(f, 0, 0, 15);
        for (int i = 0; i < 4; i++) assert(cause[i] == 0);
        /* the same frame read twice (the same Link update): no stop in his motion */
        for (int i = 0; i < 6; i++) {
            link[0] += 3 * f, step(f, 0, now, 15);
            oot_vr_comfort_step(&st, now + 1000000, 0, now, seen + 1000000, link, 15, cause);
            assert(i < 2 || cause[kVrComfortMoving] == 1);
        }
        /* read again unchanged in the same game frame, 20 ms on (another thread's read): no stop either */
        for (int i = 0; i < 8; i++) {
            link[0] += 5 * f, step(f, 0, now, 15);
            oot_vr_comfort_step(&st, now + 20000000, 0, now, seen + 20000000, link, 15, cause);
            assert(i < 2 || (cause[kVrComfortMoving] == 1 && st.speed > 4));
        }
        /* a row Off: its cause never told */
        for (int i = 0; i < 10; i++) step(f, 0, 0, 15);
        for (int i = 0; i < 6; i++) link[0] += 3 * f, step(f, .05f, now, 15 & ~1 & ~2);
        assert(cause[kVrComfortMoving] == 0 && cause[kVrComfortTurning] == 0);
        return 0;
    }
    """)
    # (every Comfort row Off or unchanged by default; labels and values only: no help line)
    defaults = (REPO / 'vr/menu/vr_options.inc').read_text(encoding='utf-8')
    for line in ('int vr_turn_style = 0;', 'int vr_snap_angle = 1;', 'int vr_turn_speed = 1;', 'int vr_vignette_move = 0;',
                 'int vr_vignette_turn = 0;', 'int vr_vignette_air = 0;', 'int vr_vignette_carried = 0;'):
        assert line in defaults, line
    tab_rows = (REPO / 'vr/menu/vr_tab_rows.inc').read_text(encoding='utf-8')
    rows = re.findall(r'\{TAB_STORED, "([^"]*)", &opts\.(vr_(?:turn|snap|vignette)\w*), (-?\d+), (\d+), 1, (\w+), (\w+),', tab_rows)
    assert len(rows) == 7 and all(r[5] == 'nullptr' for r in rows), rows
    values = (REPO / 'vr/menu/vr_values_menu.inc').read_text(encoding='utf-8')
    for words in ('"Smooth"', '"Snap"', '"Slow", "Medium", "Fast", "Very fast"', '"Off", "Low", "Medium", "High"'):
        assert words in values, words
    replay = read('vk_replay')
    for i, field in ((1030, 'vr_turn_style'), (1031, 'vr_snap_angle'), (1032, 'vr_turn_speed')):
        assert f'if (id == {i}) return opts.{field};' in replay
    assert 'base_yaw -= oot_vr_stick_turn(&oot_vr_turning, rx_option(1030, 0), rx_option(1031, 1), rx_option(1032, 1), x, dt);' in view
    # (the layer: over the world, under the radial and the menus, with a layer kept for it)
    loop = (REPO / 'vr/headset/frame_loop.inc').read_text(encoding='utf-8')
    assert 'kBombMarkLayersMax - 9 - (int)n_layers' in loop
    assert loop.index('bomb_mark_layers(') < loop.index('vignette_layer(vignette_quad)') < loop.index('radial_layer(radial_quad')
    layer = read('vignette')
    a = layer.index('float vignette_alpha(')
    compile_run(r"""
    #include <algorithm>
    #include <cassert>
    #include <cmath>
    """ + layer[layer.index('constexpr float kVignetteClear'):layer.index('\n', layer.index('constexpr float kVignetteClear')) + 1] +
                 layer[a:layer.index('\n}\n', a) + 3] + r"""
    int main() {
        assert(vignette_alpha(0, 0) == 0 && vignette_alpha(.29f, 0) == 0);    // (clear in the middle)
        assert(vignette_alpha(.61f, 0) == 1 && vignette_alpha(1, 1) == 1);    // (dark to the picture's edge)
        assert(vignette_alpha(.45f, 0) > .4f && vignette_alpha(.45f, 0) < .6f);
        return 0;
    }
    """, cpp=True)
    # (a cinematic in 3D: the director's camera's own motion reads as the game moving you, its turn as turning, a cut as
    # neither; wired in the director's branch of view_host.c)
    v = comfort.index('static void oot_vr_comfort_vantage(')
    compile_run(r"""
    #include <stdint.h>
    #include <stdarg.h>
    #include <stdio.h>
    #include <string.h>
    #include <math.h>
    #include <assert.h>
    static void hos_log(const char *f, ...) { (void)f; }
    static int rx_option(int id, int otherwise) { (void)otherwise; return id == 1033 ? 15 : 0; }
    static float told[4];
    static void rx_vr_comfort(const float cause[4]) { memcpy(told, cause, sizeof told); }
    """ + comfort[cstart:cend] + comfort[v:comfort.index('\n}\n', v) + 3] + r"""
    int main(void) {
        int64_t now = 1000000000LL;
        float eye[3] = {0, 2, 0}, back[3] = {0, 0, 1};
        for (int i = 0; i < 60; i++, now += 16666667) oot_vr_comfort_vantage(now, eye, back);   /* (a held shot) */
        assert(told[3] == 0 && told[1] == 0 && told[0] == 0);
        for (int i = 0; i < 60; i++, now += 16666667) eye[0] += 2.0f / 60, oot_vr_comfort_vantage(now, eye, back);   /* (a dolly) */
        assert(told[3] == 1 && told[1] == 0 && told[0] == 0);
        for (int i = 0; i < 30; i++, now += 16666667) oot_vr_comfort_vantage(now, eye, back);
        assert(told[3] == 0);
        for (int i = 0; i < 30; i++, now += 16666667) {   /* (a pan: the heading turning 0.5 rad/s, the camera still) */
            const float yaw = .5f * (i + 1) / 60;
            back[0] = sinf(yaw), back[2] = cosf(yaw), oot_vr_comfort_vantage(now, eye, back);
        }
        assert(told[1] == 1 && told[3] == 0);
        for (int i = 0; i < 30; i++, now += 16666667) oot_vr_comfort_vantage(now, eye, back);
        assert(told[1] == 0);
        eye[0] += 40, back[0] = -1, back[2] = 0, oot_vr_comfort_vantage(now, eye, back);   /* (a cut: 40 m and a half turn) */
        assert(told[1] == 0 && told[3] == 0 && told[2] == 0);
        return 0;
    }
    """)
    assert 'oot_vr_comfort_vantage(hos_now_ns(), eye, back);' in view[view.index('oot_vr_event_vantage(c, view, eye, right, up, back)'):]


def check_event_views():
    """the game's events in VR: every Dm_/DmF_ flow but the system ones is a director's cinematic (the boss intros, songs
    and awakenings an old rule missed), on the world screen with Cutscene view Screen and by 3D cutscenes
    with In 3D (the default, at the director's camera); talks, signs, Navi, item gets and arrivals are never flat but by Flat, in first person and in third (the
    user's "dialogue never puts you in 2D", 21 Nov); a talk's far shot by 3D cutscenes; the theatre seat moves only for a
    new scene; a held palette stands in for the game's per-shot ones and the game's own goes back at the end; the view's
    matrix is the old one; the wiring (options, the flat check, the director's branch, Link's spot held, the world screen
    a quad fixed in the room)"""
    events = read('events_host')
    start = events.index('enum { kVrEventNone')
    end = events.index('/* ---- The game side, each gameplay-view update')
    mstart = events.index('/* The VR view\'s matrix (rows right')
    mend = events.index('/* The director\'s camera or the theatre seat')
    compile_run(r"""
    #include <stdint.h>
    #include <stdio.h>
    #include <string.h>
    #include <math.h>
    #include <assert.h>
    """ + events[start:end] + events[mstart:mend] + r"""
    static int near3(const float *a, const float *b, float tol) {
        for (int k = 0; k < 3; k++) if (fabsf(a[k] - b[k]) > tol) return 0;
        return 1;
    }
    int main(void) {
        /* kinds: the system events, item gets and the ocarina by name; every other Dm_ and DmF_ flow is a cinematic */
        static const char *const cine[] = {"Dm_Kokiri_0030", "Dm_Deku_BossIn", "Dm_DodongoCave_BossOut", "Dm_MelodyGet_Saria",
                                           "Dm_Awake_Forest_Get", "Dm_InsideJabu_Jabu_0000", "Dm_FairyFountain_Din",
                                           "DmF_Kokiri_0010", "Dm_LostWood_0000_GoronCity"};
        for (int i = 0; i < 9; i++) assert(oot_vr_event_kind(cine[i]) == kVrEventCinematic);
        assert(oot_vr_event_kind("Dm_SY_BancChangeArrive") == kVrEventSystem);
        assert(oot_vr_event_kind("DmF_SY_GameOver") == kVrEventSystem);
        assert(oot_vr_event_kind("Dm_SY_Treasurebox") == kVrEventItemGet && oot_vr_event_kind("Dm_SY_ShopGet") == kVrEventItemGet);
        assert(oot_vr_event_kind("Dm_SY_Get_WithoutPlayer") == kVrEventItemGet);
        assert(oot_vr_event_kind("Dm_SY_Ocarina_SariaSong") == kVrEventOcarina);
        assert(oot_vr_event_kind("Npc_Kokiri_012_Child_C3_T") == kVrEventInteraction);
        assert(oot_vr_event_kind("Common_Dungeon_OpenDungeonDoorSingleMove") == kVrEventInteraction);
        assert(oot_vr_event_kind("") == kVrEventNone);
        /* styles (kind, Cutscene view, 3D cutscenes, far, ocarina in the hands, first person): play is your eyes; Flat
           flattens every event but the ocarina in the hands */
        for (int v = 0; v < 4; v++) for (int fp = 0; fp < 2; fp++) assert(oot_vr_event_style(kVrEventNone, 0, v, 0, 0, fp) == kVrViewEyes);
        assert(oot_vr_event_style(kVrEventInteraction, 0, 0, 0, 0, 1) == kVrViewFlat);
        assert(oot_vr_event_style(kVrEventInteraction, 0, 0, 0, 0, 0) == kVrViewFlat);
        assert(oot_vr_event_style(kVrEventOcarina, 0, 0, 0, 1, 1) == kVrViewEyes);
        assert(oot_vr_event_style(kVrEventOcarina, 1, 0, 0, 0, 1) == kVrViewEyes);
        /* dialogue is never flat but by Flat: every kind but a cinematic, near or far, any 3D cutscenes, either person */
        static const int kinds[] = {kVrEventInteraction, kVrEventSystem, kVrEventItemGet, kVrEventOcarina};
        for (int k = 0; k < 4; k++) for (int pol = 1; pol < 3; pol++) for (int v = 0; v < 4; v++)
            for (int f = 0; f < 2; f++) for (int fp = 0; fp < 2; fp++)
                assert(oot_vr_event_style(kinds[k], pol, v, f, 0, fp) != kVrViewFlat);
        /* Screen: cinematics on the world screen, talks and arrivals in your eyes, a far shot by 3D cutscenes */
        assert(oot_vr_event_style(kVrEventCinematic, 1, 0, 0, 0, 1) == kVrViewFlat);
        assert(oot_vr_event_style(kVrEventCinematic, 1, 0, 0, 0, 0) == kVrViewFlat);
        assert(oot_vr_event_style(kVrEventInteraction, 1, 0, 0, 0, 1) == kVrViewEyes);
        assert(oot_vr_event_style(kVrEventSystem, 1, 0, 1, 0, 1) == kVrViewEyes);   /* (not by distance: a map change's camera) */
        assert(oot_vr_event_style(kVrEventItemGet, 1, 0, 1, 0, 1) == kVrViewEyes);
        assert(oot_vr_event_style(kVrEventInteraction, 1, 0, 1, 0, 1) == kVrViewDirector);
        assert(oot_vr_event_style(kVrEventInteraction, 1, 1, 1, 0, 1) == kVrViewTheatre);
        assert(oot_vr_event_style(kVrEventInteraction, 1, 2, 1, 0, 1) == kVrViewEyes);
        assert(oot_vr_event_style(kVrEventInteraction, 1, 3, 1, 0, 1) == kVrViewEyes);
        assert(oot_vr_event_style(kVrEventInteraction, 1, 0, 1, 0, 0) == kVrViewEyes);   /* (third person: the game's camera) */
        /* In 3D (the default): a cinematic by 3D cutscenes; in third person the game's camera with your head */
        assert(oot_vr_event_style(kVrEventCinematic, 2, 0, 0, 0, 1) == kVrViewDirector);
        assert(oot_vr_event_style(kVrEventCinematic, 2, 1, 0, 0, 1) == kVrViewTheatre);
        assert(oot_vr_event_style(kVrEventCinematic, 2, 2, 0, 0, 1) == kVrViewAnchored);
        assert(oot_vr_event_style(kVrEventCinematic, 2, 3, 0, 0, 1) == kVrViewEyes);
        for (int v = 0; v < 4; v++) assert(oot_vr_event_style(kVrEventCinematic, 2, v, 0, 0, 0) == kVrViewEyes);
        /* a movie playing during any event: the world screen, whatever the rows; no event, no change (the title's) */
        for (int s = 0; s < 5; s++) {
            assert(oot_vr_movie_style(s, kVrEventCinematic, 1) == kVrViewFlat);
            assert(oot_vr_movie_style(s, kVrEventInteraction, 1) == kVrViewFlat);
            assert(oot_vr_movie_style(s, kVrEventNone, 1) == s && oot_vr_movie_style(s, kVrEventCinematic, 0) == s);
        }
        /* an interaction that sets a cinematic's demo palette is a cinematic till it ends (Opening_Introduction plays the Deku
           Tree's opening); a system event, an item get or a Dm_SY_ palette is not */
        int seen = 0;
        assert(oot_vr_palette_kind(kVrEventInteraction, "", &seen) == kVrEventInteraction && !seen);
        assert(oot_vr_palette_kind(kVrEventInteraction, "Dm_SY_Something_C01", &seen) == kVrEventInteraction && !seen);
        assert(oot_vr_palette_kind(kVrEventSystem, "Dm_Kokiri_0030_C12", &seen) == kVrEventSystem && !seen);
        assert(oot_vr_palette_kind(kVrEventInteraction, "Dm_Kokiri_0030_C12", &seen) == kVrEventCinematic && seen);
        assert(oot_vr_palette_kind(kVrEventInteraction, "", &seen) == kVrEventCinematic);   /* (between cuts: still one) */
        assert(oot_vr_palette_kind(kVrEventNone, "", &seen) == kVrEventNone);
        /* far shots: in past 8 m, back under 6 m, nothing in between flips it */
        int far = 0;
        far = oot_vr_far_shot(far, 3.2f); assert(!far);
        far = oot_vr_far_shot(far, 7.5f); assert(!far);
        far = oot_vr_far_shot(far, 9.0f); assert(far);
        far = oot_vr_far_shot(far, 7.0f); assert(far);
        far = oot_vr_far_shot(far, 5.0f); assert(!far);
        assert(!oot_vr_far_shot(0, NAN) && oot_vr_far_shot(1, NAN));   /* (a bad distance changes nothing) */
        /* lighting: a flat screen is never touched; Auto keeps the game's per shot at the director's camera, holds otherwise */
        for (int o = 0; o < 4; o++) assert(oot_vr_light_mode(o, kVrViewFlat) == kVrLightShot);
        assert(oot_vr_light_mode(kVrLightAuto, kVrViewDirector) == kVrLightShot);
        assert(oot_vr_light_mode(kVrLightAuto, kVrViewTheatre) == kVrLightHeld);
        assert(oot_vr_light_mode(kVrLightAuto, kVrViewAnchored) == kVrLightHeld);
        assert(oot_vr_light_mode(kVrLightAuto, kVrViewEyes) == kVrLightHeld);
        assert(oot_vr_light_mode(kVrLightArea, kVrViewDirector) == kVrLightArea);
        /* the theatre seat (Dm_Kokiri_0030's cameras and targets, 19 Nov): the Deku Tree's shots share one seat; a shot
           of the village across the forest is a new scene; the seat is level and faces the way its first shot looked */
        OotVrTheatre t = {0};
        const float c27[3] = {67.4f, 112.4f, -11.2f}, a27[3] = {68.1f, 112.3f, -11.8f};
        const float c29[3] = {71.9f, 116.9f, -23.1f}, a29[3] = {72.7f, 116.5f, -22.6f};
        const float c30[3] = {85.4f, 124.9f, -5.8f}, a30[3] = {85.0f, 124.7f, -6.7f};
        const float c02[3] = {22.4f, 112.2f, -0.7f}, a02[3] = {23.0f, 112.3f, 0.1f};
        assert(oot_vr_theatre_step(&t, c27, a27) && t.scenes == 1 && near3(t.seat, c27, 1e-4f));
        assert(fabsf(t.back[1]) < 1e-6f && fabsf(hypotf(t.back[0], t.back[2]) - 1) < 1e-4f);
        assert(t.back[0] * (c27[0] - a27[0]) + t.back[2] * (c27[2] - a27[2]) > 0);   /* (from the target toward the seat) */
        assert(!oot_vr_theatre_step(&t, c29, a29) && !oot_vr_theatre_step(&t, c30, a30) && near3(t.seat, c27, 1e-4f));
        assert(oot_vr_theatre_step(&t, c02, a02) && t.scenes == 2 && near3(t.seat, c02, 1e-4f));
        const float high[3] = {22.4f, 160.0f, -0.7f}, below[3] = {22.4f, 159.0f, -0.7f};
        const float keep[3] = {t.back[0], t.back[1], t.back[2]};
        assert(oot_vr_theatre_step(&t, high, below) && near3(t.back, keep, 1e-6f));   /* (looking straight down: heading kept) */
        /* Held: the event's first palette stands in for later ones; the game's own last goes back at its end */
        OotVrPalette p = {0};
        char want[48];
        assert(oot_vr_palette_step(&p, kVrLightHeld, "", 0, want) == kVrPaletteKeep);
        assert(oot_vr_palette_step(&p, kVrLightHeld, "Dm_Kokiri_0030_C27", 0, want) == kVrPaletteKeep);
        assert(oot_vr_palette_step(&p, kVrLightHeld, "Dm_Kokiri_0030_C29", 0, want) == kVrPaletteSet && !strcmp(want, "Dm_Kokiri_0030_C27"));
        assert(oot_vr_palette_step(&p, kVrLightHeld, "Dm_Kokiri_0030_C27", 0, want) == kVrPaletteKeep);   /* (ours in place) */
        assert(oot_vr_palette_step(&p, kVrLightHeld, "", 0, want) == kVrPaletteSet && !strcmp(want, "Dm_Kokiri_0030_C27"));
        assert(oot_vr_palette_step(&p, kVrLightHeld, "Dm_Kokiri_0030_C27", 1, want) == kVrPaletteReset);   /* (the game's last: none) */
        assert(!p.touched && !p.held[0]);
        /* ... the game's own palette if it set one last; nothing if the game moved on by itself */
        memset(&p, 0, sizeof p);
        oot_vr_palette_step(&p, kVrLightHeld, "A", 0, want);
        oot_vr_palette_step(&p, kVrLightHeld, "B", 0, want);
        assert(oot_vr_palette_step(&p, kVrLightHeld, "A", 1, want) == kVrPaletteSet && !strcmp(want, "B"));
        memset(&p, 0, sizeof p);
        oot_vr_palette_step(&p, kVrLightHeld, "A", 0, want);
        oot_vr_palette_step(&p, kVrLightHeld, "B", 0, want);
        assert(oot_vr_palette_step(&p, kVrLightHeld, "C", 1, want) == kVrPaletteKeep);
        /* an event that never changed its palette is left alone, at the end too */
        memset(&p, 0, sizeof p);
        oot_vr_palette_step(&p, kVrLightHeld, "A", 0, want);
        assert(oot_vr_palette_step(&p, kVrLightHeld, "A", 1, want) == kVrPaletteKeep);
        /* The area's: any palette reset; the game's last back at the end; switching to Every shot gives it back at once */
        memset(&p, 0, sizeof p);
        assert(oot_vr_palette_step(&p, kVrLightArea, "", 0, want) == kVrPaletteKeep);
        assert(oot_vr_palette_step(&p, kVrLightArea, "X", 0, want) == kVrPaletteReset);
        assert(oot_vr_palette_step(&p, kVrLightShot, "", 0, want) == kVrPaletteSet && !strcmp(want, "X"));
        assert(oot_vr_palette_step(&p, kVrLightShot, "X", 0, want) == kVrPaletteKeep);
        /* the matrix: no head turn is the frame itself, the eye at the origin of the view */
        const float right[3] = {0, 0, -1}, up[3] = {0, 1, 0}, back[3] = {1, 0, 0}, still[4] = {0, 0, 0, 1}, eye[3] = {2, 3, 4};
        float axis[3][3], m[12];
        oot_vr_view_matrix(right, up, back, still, eye, axis, m);
        assert(near3(axis[0], right, 1e-6f) && near3(axis[1], up, 1e-6f) && near3(axis[2], back, 1e-6f));
        for (int row = 0; row < 3; row++)
            assert(fabsf(m[row * 4] * eye[0] + m[row * 4 + 1] * eye[1] + m[row * 4 + 2] * eye[2] + m[row * 4 + 3]) < 1e-5f);
        const float yaw90[4] = {0, sinf(0.785398f), 0, cosf(0.785398f)};   /* (the head turned 90 degrees left) */
        oot_vr_view_matrix(right, up, back, yaw90, eye, axis, m);
        const float looks[3] = {-axis[2][0], -axis[2][1], -axis[2][2]}, want_look[3] = {0, 0, 1};
        assert(near3(looks, want_look, 1e-5f));   /* (forward -back was -x; a left turn faces +z with right = -z) */
        return 0;
    }
    """)
    view = read('view_host')
    stick = read('stick_host')
    replay = (REPO / FILES['vk_replay']).read_text()
    for opt in ('if (id == 912) return opts.vr ? opts.vr_events : 0;',   # (third person too: talks in VR there as well)
                'if (id == 1010) return opts.vr_cinematic_view;', 'if (id == 1011) return opts.vr_cinematic_light;',
                'if (id == 1012) return opts.vr_event_detail;'):
        assert opt in replay, opt
    assert 'return !oot_vr_event_flat();' in stick and 'oot_vr_event_cinematic' not in stick
    frame = view.index('oot_vr_event_frame(c, view, h.mode == 1, h.height_cm);')
    assert view.index('oot_vr_realign_frame(c, &h);') < frame < view.index('if (!oot_vr_player_camera(c, view)')
    vantage = view.index('oot_vr_event_vantage(c, view, eye, right, up, back)')
    assert view.index('if (!oot_vr_player_camera(c, view)') < vantage < view.index('float pos[3], look[3], cup[3];')
    assert 'rx_vr_body(h.seq, 0, 1, NULL, NULL, NULL);' in view[vantage:view.index('float pos[3], look[3], cup[3];')]
    assert 'if (oot_vr_event_anchor(eye)) body_ok = 0, crawl = lowered = 0;' in view
    assert 'if (oot_vr_ev.style != kVrViewAnchored)' in view
    assert 'oot_vr_event_style(kind, oot_vr_ev_option(912, 2),' in events and 'first_person);' in events
    # (World culling's event mode for every cinematic in 3D in first person, Follow Link's eyes included: the game's
    # lighting and detail from where you are, not the director's camera; the user's floating box, 22 Nov)
    assert 'style == kVrViewEyes && kind == kVrEventCinematic' in events
    assert 'vr_cull_event_mode = first_person && in_3d && !oot_vr_ev_option(1012, 0) ? 2 : 0;' in events
    # (Link whole at the director's camera: his head and hair are left out only in frames with a first-person body, which
    # the director's branch turns off; headset 21 Nov, a headless Link in cutscenes)
    video = (REPO / 'host/nn_video.c').read_text(encoding='utf-8')
    acquire = video[video.index('static void dec_acquire(Ctx *c) {'):]
    assert acquire.index('__atomic_store_n(&hos_video_acquired_ns, hos_now_ns(), __ATOMIC_RELAXED);') < acquire.index('\n}\n')
    assert 'style = oot_vr_movie_style(style, kind, movie);' in events
    assert 'if (strcmp(event, e->event)) e->palette_cine = 0;' in events
    # (the headset camera's near plane at most gameplay's 0.1 m: a cutscene's 0.5-1 m cut the floor away, headset 22 Nov)
    cull = read('culling_host') if 'culling_host' in FILES else (REPO / 'vr/camera/culling_host.c').read_text(encoding='utf-8')
    locked = cull[cull.index('static int hos_vr_projection_locked('):]
    assert locked.index('if (vr_load(c, proj + 0x98) > kVrNearM) vr_store(c, proj + 0x98, kVrNearM);') < locked.index('*tan_out = nt;')
    assert '#define kVrNearM 0.1f' in cull
    skin = read('skin_draw')
    assert skin.count('vr::link_body_shown(texture_frame_no())') == 2
    assert skin.index('vr::link_body_shown(texture_frame_no())') < skin.index('trace("hair left out", false);')
    # (the pause menu's Equipment Link keeps his hair: the hair buffers known by size, not near, only in the world's passes)
    assert '(near || ((vb0 == 2912496 || vb0 == 1577752) && world_pass()))' in skin
    assert 'return (!(at.n && at.color[0]) || vr::scene_depth_is(at.depth)) &&' in skin
    assert 'vr::link_draw_near_body(object.addr, object.size, texture_frame_no());' in skin
    tracking = (REPO / 'vr/body/tracking.inc').read_text(encoding='utf-8')
    shown = tracking[tracking.index('bool link_body_shown('):]
    assert 'return b && b->active && b->seq;' in shown[:shown.index('\n}\n')]
    assert '!first_person ? (event[0] ? kVrViewFlat' not in events   # (third person no longer flattens every event)
    hos = (REPO / 'host/hos_main.c').read_text()
    assert hos.index('#include "../vr/camera/events_host.c"') < hos.index('#include "../vr/camera/view_host.c"')
    assert hos.index('#include "../vr/camera/culling_host.c"') < hos.index('#include "../vr/camera/events_host.c"')
    # (the world screen: a flat frame's picture on a quad in the room's space, placed once where it opened)
    eyes = read('eye_rendering')
    loop = read('frame_loop')
    opened = eyes.index('if (!flat_screen.open || again) {')
    assert eyes.index('flat_pose.position = head.position;') > opened
    assert eyes.index('flat_pose.position = head.position;') < eyes.index('flat_screen.open = true;')
    assert 'if (flat) flat_place(head);\n    else flat_screen.open = false;' in eyes
    assert 'quad.space = s.local;' in eyes[eyes.index('bool flat_screen_quad('):]
    # (and its picture copied as drawn: no stereo shift for a flat frame, whose rim moved with the head, headset 22 Nov)
    assert 'const bool stereo = !sl.head_locked && opts.vr_stereo != 0 && sl.has_depth;' in eyes
    # (and its edges left out: the picture's last column and row moved with the head, headset 22 Nov)
    quad = eyes[eyes.index('bool flat_screen_quad('):]
    assert 'const uint32_t fh = sl.head_locked ? flat_picture_h(f.w, f.h) : 0;' in eyes   # (by the headset's own density)
    assert 'rec_w = v[0].recommendedImageRectWidth;' in eyes
    assert 'if (!fits && !make_eye(e, f.w, f.h)) return false;' in eyes
    # (by default a flat frame is drawn into each eye's own view; the quad, which showed a line along its edges, the user's A/B
    # 22 Nov, only by RX_VR_FLAT_LAYER=quad: check_flat_eyes)
    assert 'quad.subImage.imageRect.offset = {in, in};' in quad and 'quad.subImage.imageRect.extent = {w - 2 * in, h - 2 * in};' in quad
    assert loop.index('if (flat_mode() == kFlatQuad && flat_screen_quad(flat_quad))') < loop.index('layer.views = views;')
    defaults = (REPO / 'vr/menu/vr_options.inc').read_text(encoding='utf-8')
    assert 'int vr_events = 2;' in defaults and 'int vr_cinematic_view = 0;' in defaults and 'int vr_cutscene_screen = 70;' in defaults


def check_crawl():
    """first person, Link crawling through a tunnel (the player object's crawl bit and its low view height, as seen
    headless): the view comes down to the crawl eye height (the game's own, or the VR tab's), smoothly and never in a jump,
    and goes back to exactly the standing height when he stands, the option is turned off or the crawl is lost; neither
    field alone counts; the body comes down with the view and takes the game's crawl"""
    crawl = read('crawl_host')
    start = crawl.index('#define kVrCrawlEaseS')
    end = crawl.index('/* ---- RX_VR_CRAWL_TEST')
    compile_run(r"""
    #include <stdint.h>
    #include <stdio.h>
    #include <string.h>
    #include <math.h>
    #include <assert.h>
    typedef struct { int unused; } Ctx;
    static int64_t clock_ns = 1000000000LL;
    static int64_t hos_now_ns(void) { return clock_ns; }
    static uint64_t oot_vr_player_object = 0x10000;
    static uint32_t flags_word, view_bits;   /* the player object's +0x1f6c and +0x3c90 */
    static void *hos_guest_range(uint64_t a, uint64_t n) { (void)n; return a >= 0x10000 && a < 0x14000 ? &flags_word : 0; }
    static uint32_t recomp_load32(Ctx *c, uint64_t a) {
        (void)c;
        assert(a == 0x10000 + 0x1f6c || a == 0x10000 + 0x3c90);
        return a == 0x10000 + 0x1f6c ? flags_word : view_bits;
    }
    static int opt_on = 1, opt_eye = 0;
    static int rx_option(int id, int otherwise) { return id == 990 ? opt_on : id == 991 ? opt_eye : otherwise; }
    static int logged;
    #define hos_log(...) (logged++)
    """ + crawl[start:end] + r"""
    static const float STAND = 1.25f;   /* (child Link's standing eye above his position) */
    static float amount;
    /* one first-person view update at 90 Hz, as view_host.c makes it: the eye's height above Link's position */
    static float frame(uint32_t flags, float view_height) {
        clock_ns += 11111111LL;
        flags_word = flags;
        memcpy(&view_bits, &view_height, 4);
        return STAND - oot_vr_crawl_drop(0, STAND, &amount);
    }
    static float run(uint32_t flags, float view_height, int n, float *most_step) {   /* n updates; the largest one step */
        float eye = frame(flags, view_height), was = eye;
        for (int i = 1; i < n; i++) {
            eye = frame(flags, view_height);
            if (most_step && fabsf(eye - was) > *most_step) *most_step = fabsf(eye - was);
            was = eye;
        }
        return eye;
    }
    int main(void) {
        /* standing (as seen headless: no bit, 0.91): the standing height */
        assert(run(0, .91f, 30, 0) == STAND && amount == 0);
        /* Link goes down (the bit and 0.29): the view comes to the game's crawl height within the ease, in small steps */
        float step = 0, prev = frame(0x800, .29f);
        assert(prev < STAND && prev > STAND - .05f);   /* (it starts gently: no jump) */
        int frames = 1;
        while (fabsf(prev - .29f) > 1e-4f && frames < 200) {
            const float eye = frame(0x800, .29f);
            assert(eye <= prev + 1e-6f);   /* (only down) */
            if (prev - eye > step) step = prev - eye;
            prev = eye, frames++;
        }
        assert(fabsf(prev - .29f) < 1e-4f && frames * (1 / 90.0f) <= kVrCrawlEaseS + .03f);
        assert(step < (STAND - .29f) * 1.5f / (kVrCrawlEaseS * 90) + 1e-3f);   /* (smoothstep: at most 1.5x the even step) */
        /* crawling along: it stays there */
        assert(fabsf(run(0x800, .29f, 90, 0) - .29f) < 1e-4f && amount == 1);
        /* getting up (the bit kept, the view height back to 0.91), then standing: back to exactly the standing height */
        step = 0;
        run(0x800, .91f, 20, &step);
        assert(run(0, .91f, 30, &step) == STAND && amount == 0 && step < .06f);
        /* either field alone is no crawl (another action's bit, or a low view height without it) */
        assert(run(0x800, .91f, 60, 0) == STAND && run(0, .29f, 60, 0) == STAND && run(0x1000, .29f, 60, 0) == STAND);
        /* the VR tab's own height, 40 cm; and Auto keeps the game's within limits */
        opt_eye = 40;
        assert(fabsf(run(0x800, .29f, 60, 0) - .40f) < 1e-4f);
        opt_eye = 0;
        assert(fabsf(run(0x800, .05f, 60, 0) - kVrCrawlEyeMin) < 1e-4f);
        run(0, .91f, 60, 0);
        /* Crawl spaces off: the standing height, mid-crawl too (eased back up) */
        run(0x800, .29f, 60, 0);
        opt_on = 0;
        assert(run(0x800, .29f, 60, 0) == STAND && amount == 0);
        opt_on = 1;
        /* a pause in the updates (a menu): no jump when they come back */
        run(0x800, .29f, 60, 0);
        clock_ns += 5000000000LL;
        assert(fabsf(frame(0, .91f) - .29f) < .2f);
        assert(logged > 0);
        return 0;
    }
    """)
    # the crawl's body (vr/body/arms_and_pose.inc crawl_under_view), with Link's own rig: all the way into the crawl it is the
    # game's pose moved across only, its neck under the head's (10 cm under the eyes, 8 behind) and its height the game's (the
    # hands on the floor), the head folded to a point; halfway, halfway between; and a far slide is refused
    src = read('body')
    math = src[src.index('constexpr float kPi'):src.index('// ---- body state kept between frames')]
    at = src.index('void crawl_under_view(')
    under = src[at:src.index('\n}\n', at) + 3]
    header = read('vr_h')
    body_types = header[header.index('struct LinkHand {'):header.index('/// The replay-side body state.')]
    compile_run(r"""
    #include <algorithm>
    #include <cassert>
    #include <cmath>
    #include <cstddef>
    #include <cstdint>
    #include <cstring>
    #include <vector>
    """ + '#include "' + str(generated('vk_vr_rig_data.h')) + '"' + r"""
    namespace rig = mkwvk::vr::rig;
    """ + body_types + math + under + r"""
    int main() {
        const rig::Model& M = rig::child;
        std::vector<float> game(M.slots * 12, 0.0f), vr(M.slots * 12, 0.0f);
        for (int i = 0; i < M.slots; i++) {   // (the game's crawl: the bind pose 0.3 m up; the VR pose 2 m away)
            float* g = game.data() + i * 12;
            g[0] = g[5] = g[10] = 1, g[7] = .3f;
            float* v = vr.data() + i * 12;
            v[0] = v[5] = v[10] = 1, v[3] = 2;
        }
        const V3 origin{10, 100, 10}, U{0, 1, 0};
        const V3 game_neck = point(game.data() + M.neck * 12, v3(M.neck_pos));
        LinkBody body{};
        body.crawl = 1;
        const V3 eye = add(add(origin, game_neck), {.25f, .1f, -.15f});
        for (int k = 0; k < 3; k++) body.eye[k] = (&eye.x)[k], body.head_up[k] = k == 1, body.head_back[k] = k == 2;
        std::vector<float> p = vr;
        crawl_under_view(p.data(), M, body, origin, game);
        const V3 want = flat(sub(sub(add(add(eye, mul(U, -.10f)), {0, 0, .08f}), origin), game_neck));   // (the slide)
        bool head = false;
        for (int i = 0; i < M.slots; i++) {
            const float* m = p.data() + i * 12;
            if (M.region[i] == 2) { head = true; assert(std::fabs(m[0]) < .01f && std::fabs(m[5]) < .01f); continue; }
            assert(std::fabs(m[3] - want.x) < 1e-4f && std::fabs(m[7] - .3f) < 1e-4f && std::fabs(m[11] - want.z) < 1e-4f);
            assert(m[0] == 1 && m[5] == 1 && m[10] == 1);
        }
        assert(head);
        body.crawl = .5f;   // (halfway: between the VR pose and the crawl)
        p = vr;
        crawl_under_view(p.data(), M, body, origin, game);
        for (int i = 0; i < M.slots; i++)
            if (M.region[i] != 2) assert(std::fabs(p[i * 12 + 3] - (2 + want.x) * .5f) < 1e-4f && std::fabs(p[i * 12 + 7] - .15f) < 1e-4f);
        body.crawl = 1;   // (the head 3 m away: a warp, no slide)
        for (int k = 0; k < 3; k++) body.eye[k] += 3;
        p = vr;
        crawl_under_view(p.data(), M, body, origin, game);
        for (int i = 0; i < M.slots; i++) if (M.region[i] != 2) assert(p[i * 12 + 3] == 0 && p[i * 12 + 11] == 0);
    }
    """, cpp=True)
    view = read('view_host')
    first = view.index('if (h.mode == 1) {', view.index('float crawl = 0, lowered = 0;'))
    assert first < view.index('lowered = oot_vr_crawl_drop(c, eye[1] - body_root[1], &crawl);') < view.index('h.body_depth_cm * 0.01f')
    assert view.index('rx_vr_crawl(h.seq, crawl, lowered);') < view.index('rx_vr_body(h.seq, h.mode == 1 && body_ok')
    replay = (REPO / FILES['vk_replay']).read_text()
    assert 'if (id == 990) return opts.vr && opts.vr_mode == 1 && opts.vr_crawl;' in replay   # (first person only)
    assert 'if (id == 991) return opts.vr_crawl_eye;' in replay
    rows = (REPO / 'vr/menu/vr_tab_rows.inc').read_text()
    assert '&opts.vr_crawl,' not in rows and '"Crawl eye height", &opts.vr_crawl_eye, 0, 60, 5,' in rows   # (Crawl spaces: built in)
    assert 'static constexpr int vr_crawl = 1;' in (REPO / 'vr/menu/vr_options.inc').read_text()
    # the renderer: the rest eye comes down as the view does (no bend for it), the body's height comes down with the view and
    # is not learned from the crawling pelvis, and the crawl takes the game's pose under the view
    tracking = (REPO / 'vr/body/tracking.inc').read_text()
    assert "height_cm * 0.01f - b->lowered" in tracking
    body = read('body')
    assert 'shift.y -= y - (root.y + motion.hang - body.lowered);' in body
    assert 'if (body.crawl > 0 && motion.hang != 0) {}' in body
    assert body.index('if (body.crawl > 0) game_pose.assign(p, p + M.slots * 12);') < body.index('// Torso: upright')
    assert 'if (body.crawl > 0) crawl_under_view(p, M, body, origin, game_pose);' in body
    api = (REPO / 'vr/headset/host_api.inc').read_text()
    assert 'if (vr::s.crawl_seq == seq) b.crawl = vr::s.crawl_amount, b.lowered = vr::s.crawl_lowered;' in api


# ======================================================================================================================
# 3D: true stereo
# ======================================================================================================================

def check_stereo_projection():
    """the eyes' shift is made only from the headset's own projection"""
    source = read('stereo_eyes')
    start = source.index('void eyes_note_projection(const float proj[16]) {')
    end = source.index('\nuint64_t eyes_key()', start)
    function = source[start:end]
    fixture = r'''
    #include <algorithm>
    #include <atomic>
    #include <cassert>
    #include <cmath>
    #include <cstdint>
    #include <cstdio>
    #include <cstring>
    #include <mutex>
    static bool enabled=true, test=false;
    static float headset_p00=.705f, ipd=.064f;
    static int logged=0;
    static bool eyes_wanted(){return enabled;}
    static const char* test_dir(){return test ? "test" : nullptr;}
    static float eyes_headset_p00(){return headset_p00;}
    static float eyes_ipd(){return ipd;}
    static struct { int vr_depth_scale=100; } opts;
    static std::mutex kx_mu;
    static float kx_now[2]={0,0},p00_now=0;
    static std::atomic<uint64_t> kx_key{0};
    static std::atomic<bool> kx_known{false};
    static void logf(const char*, ...){++logged;}
    static void note(float p){float proj[16]={};proj[0]=p;eyes_note_projection(proj);}
    '''
    # Declare note after the extracted production function instead of inventing a
    # separate approximation of the math/filter under test.
    fixture = fixture.replace('static void note(float p)', 'void eyes_note_projection(const float proj[16]);\nstatic void note(float p)')
    checks = r'''
    int main(){
     enabled=false;note(.705f);assert(!kx_known);enabled=true;
     note(1.358f);note(1.206f);assert(!kx_known && !logged);
     note(.705f);assert(kx_known && logged==1);
     uint64_t key=kx_key.load();
     assert(std::fabs(kx_now[0]+.032f*.705f)<1e-7f);
     assert(std::fabs(kx_now[1]-.032f*.705f)<1e-7f);
     for(int i=0;i<100;i++){
       note(1.358f);note(.705f);note(1.206f);note(-.705f);
       assert(kx_key==key && logged==1);
     }
     ipd=.06401f;note(.705f);assert(kx_key==key); // quantized IPD noise
     note(NAN);note(INFINITY);note(0);assert(kx_key==key);
     opts.vr_depth_scale=50;note(.705f);assert(kx_key!=key && logged==2);
     test=true;note(1.358f);assert(std::fabs(p00_now-1.358f)<1e-6f);
    }
    '''
    with tempfile.TemporaryDirectory(prefix='oot-stereo-projection-') as directory:
        executable = str(Path(directory) / 'check')
        subprocess.run(['c++', '-x', 'c++', '-std=c++17', '-include', str(REPO / 'gpu/vk/compat/hookshot_env.h'),
                        '-DHOOKSHOT_VR_RESEARCH', '-', '-o', executable], input=fixture + function + checks, text=True, check=True)
        subprocess.run([executable], check=True)

def check_stereo_samples():
    """the shared-sampling shader patch makes valid SPIR-V (needs glslang and spirv-val)"""
    driver = r'''
    #include "vk_vr_spirv_samples.h"
    #include "vk_vr_spirv_eyes.h"
    #include <fstream>
    #include <iterator>
    #include <cstdio>
    int main(int argc,char**argv){
     std::ifstream f(argv[1],std::ios::binary);
     std::vector<char> bytes((std::istreambuf_iterator<char>(f)),{});
     std::vector<uint32_t> src(bytes.size()/4),out;
     memcpy(src.data(),bytes.data(),bytes.size());const char* why="";
     if(argc>3){std::vector<uint32_t> moved;float k[2]={-.02f,.02f};
      if(!mkwvk::vr::spirv_eyes_patch(src.data(),src.size(),mkwvk::vr::SpirvEyesStage::Vertex,k,moved,&why))return 3;
      src=std::move(moved);}
     if(!mkwvk::vr::spirv_eye_samples(src,1ull<<1,out,&why)){fprintf(stderr,"%s\n",why);return 2;}
     std::ofstream o(argv[2],std::ios::binary);o.write((const char*)out.data(),out.size()*4);
    }
    '''
    cases = {
        'fragment': ('.frag', '''layout(set=0,binding=1) uniform sampler2D scene;
    layout(set=0,binding=2) uniform sampler2D material;
    layout(location=0) out vec4 color;
    void main(){ color=texture(scene,vec2(.5))+texture(material,vec2(.2)); }'''),
        'fetch_size': ('.frag', '''layout(set=0,binding=1) uniform sampler2D scene;
    layout(location=0) out vec4 color;
    void main(){ ivec2 n=textureSize(scene,0); color=texelFetch(scene,n/2,0); }'''),
        'shadow': ('.frag', '''layout(set=0,binding=1) uniform sampler2DShadow scene;
    layout(location=0) out vec4 color;
    void main(){ color=vec4(texture(scene,vec3(.5,.5,.3))); }'''),
        'array': ('.frag', '''layout(set=0,binding=1) uniform sampler2D scene[2];
    layout(location=0) out vec4 color;
    void main(){color=texture(scene[0],vec2(.5))+texture(scene[1],vec2(.2));}'''),
        'vertex': ('.vert', '''layout(set=0,binding=1) uniform sampler2D scene;
    void main(){gl_Position=textureLod(scene,vec2(.5),0);}'''),
        'gather_lod': ('.frag', '''layout(set=0,binding=1) uniform sampler2D scene;
    layout(location=0) out vec4 color;
    void main(){color=textureGather(scene,vec2(.5))+vec4(textureQueryLod(scene,vec2(.5)),0,0);}'''),
        'existing_view': ('.frag', '''#extension GL_EXT_multiview : require
    layout(set=0,binding=1) uniform sampler2D scene;
    layout(location=0) out vec4 color;
    void main(){color=texture(scene,vec2(.5))+float(gl_ViewIndex);}'''),
        'existing_array_type': ('.frag', '''layout(set=0,binding=1) uniform sampler2D scene;
    layout(set=0,binding=2) uniform sampler2DArray material;
    layout(location=0) out vec4 color;
    void main(){color=texture(scene,vec2(.5))+texture(material,vec3(.5,.5,0));}'''),
        'projected_reject': ('.frag', '''layout(set=0,binding=1) uniform sampler2D scene;
    layout(location=0) out vec4 color;
    void main(){color=textureProj(scene,vec3(.5,.5,1));}'''),
    }
    with tempfile.TemporaryDirectory(prefix='oot-stereo-samples-') as directory:
        d = Path(directory)
        source = d/'driver.cpp'
        source.write_text('#include <cstring>\n'+driver)
        exe = d/'patch'
        subprocess.run(['c++','-std=c++17','-I'+str(path('spirv_eyes').parent),str(source),
                        str(path('spirv_eyes')),'-o',str(exe)],check=True)
        for name, (stage, body) in cases.items():
            glsl=d/(name+stage); raw=d/(name+'.spv'); output=d/(name+'.eyes.spv')
            glsl.write_text('#version 450\n'+body)
            subprocess.run(['glslangValidator','-V','--target-env','vulkan1.2',str(glsl),'-o',str(raw)],check=True,capture_output=True)
            result=subprocess.run([str(exe),str(raw),str(output)],capture_output=True,text=True)
            if name.endswith('_reject'):
                assert result.returncode==2,(name,result.stderr)
            else:
                assert result.returncode==0,(name,result.stderr)
                subprocess.run(['spirv-val','--target-env','vulkan1.2',str(output)],check=True)
                dis=subprocess.check_output(['spirv-dis',str(output)],text=True)
                assert 'BuiltIn ViewIndex' in dis
                assert 'OpTypeImage %float 2D 0 1 0 1 Unknown' in dis or name=='shadow'
                if name=='fragment':
                    assert 'OpTypeImage %float 2D 0 0 0 1 Unknown' in dis,'material type changed'

def check_stereo_camera_patch():
    """a fragment stage's reads of the camera copies corrected per view: valid SPIR-V, every translator layout (STE-31)"""
    for tool in ('glslangValidator', 'spirv-val', 'c++'):
        if not shutil.which(tool):
            raise Skip(f'needs {tool}')
    driver = r"""
    #include "vk_vr_spirv_camera.h"
    #include <cstdio>
    #include <fstream>
    #include <iterator>
    int main(int, char** argv) {
     std::ifstream f(argv[1], std::ios::binary);
     std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
     std::vector<uint32_t> src(bytes.size() / 4), out;
     std::memcpy(src.data(), bytes.data(), bytes.size());
     const float e = 0.032f, p00 = 0.7f;
     // (binding 12: a view at float 0, a view-projection at 12, an inverse view at 32)
     // (and STE-33: cascade 0's first row, at float 3088: its w at 3091 by its x at 3088)
     std::vector<mkwvk::vr::CameraFix> fx = {{12, 3, -1, {e, -e}}, {12, 15, -1, {p00 * e, -p00 * e}},
                                             {12, 35, 32, {-e, e}}, {12, 39, 36, {-e, e}}, {12, 43, 40, {-e, e}},
                                             {12, 3091, 3088, {-e, e}}};
     int edits = 0, dyn = 0; const char* why = "";
     if (!mkwvk::vr::spirv_camera_patch(src, fx, out, &edits, &dyn, &why)) { fprintf(stderr, "%s\n", why); return 2; }
     printf("%d %d\n", edits, dyn);
     std::ofstream o(argv[2], std::ios::binary); o.write((const char*)out.data(), out.size() * 4);
    }
    """
    head = '#version 450\n#extension GL_EXT_scalar_block_layout : require\n'
    out_ = 'layout(location=0) out vec4 color;\n'
    cases = {   # name: (shader, the reads corrected, reads at a computed place); None: refused
        'floats': (head + 'layout(set=0,binding=4,scalar) uniform B { float data[4096]; } c12;\nlayout(location=0) flat in int k;\n' + out_ +
                   'void main(){ color = vec4(c12.data[3], c12.data[15], c12.data[35] + c12.data[39], c12.data[43] + c12.data[k]); }', (5, 1)),
        'uints': (head + 'layout(set=0,binding=4,scalar) uniform B { uint data[4096]; } c12;\n' + out_ +
                  'void main(){ color = vec4(uintBitsToFloat(c12.data[3]), uintBitsToFloat(c12.data[35]), 0, 1); }', (2, 0)),
        'vectors': (head + 'layout(set=0,binding=4,scalar) uniform B { uvec4 data[1024]; } c12;\n' + out_ +
                    'void main(){ color = uintBitsToFloat(c12.data[0]) + uintBitsToFloat(c12.data[8]) + uintBitsToFloat(c12.data[3]); }', (3, 0)),
        'existing_view': (head + '#extension GL_EXT_multiview : require\nlayout(set=0,binding=4,scalar) uniform B { float data[4096]; } c12;\n' +
                          out_ + 'void main(){ color = vec4(c12.data[3], float(gl_ViewIndex), 0, 1); }', (1, 0)),
        'cascade': (head + 'layout(set=0,binding=4,scalar) uniform B { uvec4 data[1024]; } c12;\n' + out_ +
                    'void main(){ color = uintBitsToFloat(c12.data[772]); }', (1, 0)),
        'other_binding': (head + 'layout(set=0,binding=4,scalar) uniform B { float data[4096]; } c9;\n' + out_ +
                          'void main(){ color = vec4(c9.data[3]); }', None),
        'not_tight': ('#version 450\nlayout(set=0,binding=4,std140) uniform B { float data[1024]; } c12;\n' + out_ +
                      'void main(){ color = vec4(c12.data[3]); }', None),
    }
    with tempfile.TemporaryDirectory(prefix='oot-stereo-camera-') as directory:
        d = Path(directory)
        source = d/'driver.cpp'
        source.write_text('#include <cstring>\n' + driver)
        exe = d/'patch'
        subprocess.run(['c++', '-std=c++17', '-I'+str(path('spirv_eyes').parent), str(source), '-o', str(exe)], check=True)
        for name, (glsl_text, want) in cases.items():
            glsl = d/(name+'.frag'); raw = d/(name+'.spv'); output = d/(name+'.eyes.spv')
            glsl.write_text(glsl_text)
            subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.2', str(glsl), '-o', str(raw)], check=True,
                           capture_output=True)
            result = subprocess.run([str(exe), str(raw), str(output)], capture_output=True, text=True)
            if want is None:
                assert result.returncode == 2, (name, 'patched a buffer that is not the camera\'s', result.stdout)
                continue
            assert result.returncode == 0, (name, result.stderr)
            assert tuple(map(int, result.stdout.split())) == want, (name, 'reads corrected / computed', result.stdout, want)
            subprocess.run(['spirv-val', '--target-env', 'vulkan1.2', '--scalar-block-layout', str(output)], check=True)


def check_stereo_shift_gate():
    """the eye shift leaves w == 1 vertices (full-screen draws) alone and moves world ones; valid SPIR-V (spirv-as, spirv-val)"""
    driver = r'''
    #include "vk_vr_spirv_eyes.h"
    #include <cstdio>
    #include <cstring>
    #include <fstream>
    #include <iterator>
    int main(int argc,char**argv){
     std::ifstream f(argv[1],std::ios::binary);
     std::vector<char> bytes((std::istreambuf_iterator<char>(f)),{});
     std::vector<uint32_t> src(bytes.size()/4),out;
     memcpy(src.data(),bytes.data(),bytes.size());const char* why="";
     const auto stage=argv[3][0]=='g'?mkwvk::vr::SpirvEyesStage::Geometry:mkwvk::vr::SpirvEyesStage::Vertex;
     float k[2]={-.02f,.02f};
     if(!mkwvk::vr::spirv_eyes_patch(src.data(),src.size(),stage,k,out,&why)){fprintf(stderr,"%s\n",why);return 2;}
     std::ofstream o(argv[2],std::ios::binary);o.write((const char*)out.data(),out.size()*4);
    }
    '''
    # (the translator's shape: Position is a variable of its own, not glslang's gl_PerVertex block, so the stages are written
    # as SPIR-V assembly. The vertex stage's position comes from an input: w is whatever the draw gives, 1 or a depth)
    head = """OpMemoryModel Logical GLSL450
    %void = OpTypeVoid
    %fn = OpTypeFunction %void
    %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
    %out_v4ptr = OpTypePointer Output %v4float
    %pos_out = OpVariable %out_v4ptr Output
    """
    cases = {
        'vertex': """OpCapability Shader
    OpMemoryModel Logical GLSL450
    OpEntryPoint Vertex %main "main" %pos_out %pos_in
    OpDecorate %pos_out BuiltIn Position
    OpDecorate %pos_in Location 0
    """ + head.split('\n', 1)[1] + """%in_v4ptr = OpTypePointer Input %v4float
    %pos_in = OpVariable %in_v4ptr Input
    %main = OpFunction %void None %fn
    %entry = OpLabel
    %p = OpLoad %v4float %pos_in
    OpStore %pos_out %p
    OpReturn
    OpFunctionEnd
    """,
        'geometry': """OpCapability Geometry
    OpMemoryModel Logical GLSL450
    OpEntryPoint Geometry %main "main" %pos_out
    OpExecutionMode %main InputPoints
    OpExecutionMode %main Invocations 1
    OpExecutionMode %main OutputPoints
    OpExecutionMode %main OutputVertices 1
    OpDecorate %pos_out BuiltIn Position
    """ + head.split('\n', 1)[1] + """%half = OpConstant %float 0.5
    %two = OpConstant %float 2
    %p = OpConstantComposite %v4float %half %half %half %two
    %main = OpFunction %void None %fn
    %entry = OpLabel
    OpStore %pos_out %p
    OpEmitVertex
    OpReturn
    OpFunctionEnd
    """,
    }
    with tempfile.TemporaryDirectory(prefix='oot-stereo-gate-') as directory:
        d = Path(directory)
        source = d/'driver.cpp'
        source.write_text(driver)
        exe = d/'patch'
        subprocess.run(['c++','-std=c++17','-I'+str(path('spirv_eyes').parent),str(source),
                        str(path('spirv_eyes')),'-o',str(exe)],check=True)
        for name, text in cases.items():
            asm=d/(name+'.spvasm'); raw=d/(name+'.spv'); output=d/(name+'.eyes.spv')
            asm.write_text('\n'.join(line.strip() for line in text.splitlines()) + '\n')
            subprocess.run(['spirv-as','--target-env','vulkan1.2',str(asm),'-o',str(raw)],check=True,capture_output=True)
            result=subprocess.run([str(exe),str(raw),str(output),name],capture_output=True,text=True)
            assert result.returncode==0,(name,result.stderr)
            subprocess.run(['spirv-val','--target-env','vulkan1.2',str(output)],check=True)
            dis=subprocess.check_output(['spirv-dis','--raw-id',str(output)],text=True)
            assert 'BuiltIn ViewIndex' in dis, name
            # (the Position written: its w (component 3) compared with 1.0, the shift picked by that, then taken from x)
            lines = dis.splitlines()
            at = next(i for i, l in enumerate(lines) if 'OpFOrdEqual' in l)
            w_id = lines[at].split()[-2]
            assert any(l.strip().startswith(w_id + ' = OpCompositeExtract') and l.rstrip().endswith(' 3') for l in lines), (name, 'gate not on w')
            flat = lines[at].split()[0]
            pick = next(l for l in lines[at:] if 'OpSelect' in l and ' ' + flat + ' ' in l + ' ')
            shift = pick.split()[0]
            assert any('OpFSub' in l and l.rstrip().endswith(' ' + shift) for l in lines[at:]), (name, 'shift not gated')


def check_stereo_small_draws():
    """small draws: blended quads with the scene camera get each eye's shift; triangles and opaque ones stay flat (STE-06/23)"""
    source = read('stereo_inputs')
    start = source.index('bool vr_eyes_small_moved(')
    end = source.index('\n}\n', start) + 3
    fixture = r'''
    #include <cassert>
    namespace vr { int debug = 0; bool eyes_debug(int bit) { return (debug & bit) != 0; } }
    struct Targets { int eyes = 0; };
    struct NvnState { struct { unsigned blend_en = 0; } color; };
    struct DrawArgs { unsigned count = 0; };
    ''' + source[start:end] + r'''
    static bool moved(int eyes, unsigned count, unsigned blend) {
        Targets at; at.eyes = eyes; NvnState s; s.color.blend_en = blend; DrawArgs d; d.count = count;
        return vr_eyes_small_moved(at, s, d);
    }
    int main() {
        assert(!moved(2, 3, 1));   // (a full-screen triangle: flat, even blended)
        assert(!moved(2, 6, 0));   // (an opaque quad: a composite, flat)
        assert(moved(2, 4, 1) && moved(2, 6, 1));   // (a blended quad with the scene camera: a glow or a fog sheet, moved)
        assert(!moved(1, 6, 1));   // (no scene camera in its vertex stage: flat)
        vr::debug = 128;
        assert(!moved(2, 6, 1));   // (RX_VR_EYES_DEBUG 128: the old rule, every small draw flat)
    }
    '''
    with tempfile.TemporaryDirectory(prefix='oot-stereo-small-') as directory:
        d = Path(directory)
        (d/'t.cpp').write_text(fixture)
        subprocess.run(['c++','-std=c++17',str(d/'t.cpp'),'-o',str(d/'t')],check=True)
        subprocess.run([str(d/'t')],check=True)


def check_stereo_world_over_scene():
    """boxes over the scene stay one draw for both eyes (STE-35/30); moved only on RX_VR_EYES_DEBUG 524288 (STE-27); screen draws flat"""
    source = read('stereo_inputs')
    start = source.index('bool vr_eyes_world_over_scene(')
    end = source.index('\n}\n', start) + 3
    fixture = r'''
    #include <cassert>
    namespace vr { int debug = 0; bool eyes_debug(int bit) { return (debug & bit) != 0; }
                   bool eyes_actor_shadow_box(unsigned long long k) { return k == 0x1e26755d8cb22dc8ull; } }
    struct { int vr_eyes_lights = 1; } opts;   // (the VR menu's Debug row Lights per eye)
    struct Program { unsigned long long key = 0; };
    struct NvnState { bool reads_scene = false; };
    struct DrawArgs { bool mesh = false, indirect = false; unsigned count = 0; };
    bool vr_eyes_samples(const Program&, const NvnState& s) { return s.reads_scene; }
    ''' + source[start:end] + r'''
    static bool moved(unsigned count, bool reads, bool mesh = false, bool indirect = false, unsigned long long key = 0) {
        Program p; p.key = key; NvnState s; s.reads_scene = reads; DrawArgs d; d.count = count, d.mesh = mesh, d.indirect = indirect;
        return vr_eyes_world_over_scene(p, s, d);
    }
    int main() {
        assert(!moved(36, true) && !moved(360, true));  // (STE-35/30: boxes drawn once for both eyes by default)
        vr::debug = 524288;                             // (RX_VR_EYES_DEBUG 524288: moved as STE-27 had it, for A/B)
        assert(moved(36, true) && moved(360, true));   // (a light volume or decal box; many lights in one draw)
        assert(!moved(3, true) && !moved(6, true));     // (full-screen triangles and composites: their own rule, STE-06)
        assert(!moved(36, false));                      // (reads nothing of the scene: not this rule's)
        assert(!moved(36, true, true) && !moved(36, true, false, true));   // (mesh, indirect: left as they were)
        assert(!moved(36, true, false, false, 0x1e26755d8cb22dc8ull));   // (STE-34: an actor's shadow box: drawn once, patched)
        opts.vr_eyes_lights = 0;
        assert(!moved(36, true));                       // (the menu's Lights per eye Off: flat, as before)
        opts.vr_eyes_lights = 1, vr::debug = 1024 | 524288;
        assert(!moved(36, true));                       // (RX_VR_EYES_DEBUG 1024: flat, as before)
    }
    '''
    with tempfile.TemporaryDirectory(prefix='oot-stereo-world-') as directory:
        d = Path(directory)
        (d/'t.cpp').write_text(fixture)
        subprocess.run(['c++','-std=c++17',str(d/'t.cpp'),'-o',str(d/'t')],check=True)
        subprocess.run([str(d/'t')],check=True)


def check_stereo_forget_module():
    """a destroyed module's SPIR-V and patched versions forgotten, so a later module given its handle gets its own (STE-30)"""
    if not shutil.which('c++'):
        raise Skip('needs c++')
    source = read('stereo_eyes')
    start = source.index('struct Patched {')
    end = source.index('\n}\n', source.index('void stale_use(')) + 3
    # (this file's own spare modules go to the driver: through the hook they would wait on modules_mu, which they hold)
    assert source.count('vkDestroyShaderModule(') == 0, 'vk_vr_eyes.cpp destroys a module through the hook'
    assert 'vkDestroyShaderModule = forget_module;' in source
    fixture = r'''
    #include <atomic>
    #include <cassert>
    #include <cstdint>
    #include <map>
    #include <mutex>
    #include <set>
    #include <unordered_map>
    #include <unordered_set>
    #include <vector>
    #define VKAPI_CALL
    struct Module_T; using VkShaderModule = Module_T*; struct Device_T; using VkDevice = Device_T*;
    struct VkAllocationCallbacks; struct VkShaderModuleCreateInfo { const uint32_t* pCode; size_t codeSize; };
    enum VkResult { VK_SUCCESS };
    using PFN_vkDestroyShaderModule = void (*)(VkDevice, VkShaderModule, const VkAllocationCallbacks*);
    using PFN_vkCreateShaderModule = VkResult (*)(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule*);
    const VkShaderModule VK_NULL_HANDLE = nullptr;
    int debug = 0; bool eyes_debug(int bit) { return (debug & bit) != 0; }
    void logf(const char*, ...) {}
    thread_local uint64_t eyes_building_prog = 0;
    int spirv_execution_model(const uint32_t* w, size_t) { return (int)w[0]; }
    std::mutex modules_mu;
    std::unordered_map<VkShaderModule, std::vector<uint32_t>> spirv_of;
    PFN_vkCreateShaderModule original_create = nullptr;
    ''' + source[start:end] + r'''
    int destroyed = 0;
    void driver_destroy(VkDevice, VkShaderModule, const VkAllocationCallbacks*) { destroyed++; }
    int main() {
        original_destroy = driver_destroy;
        const VkShaderModule a = (VkShaderModule)0x1000, mine = (VkShaderModule)0x2000;
        for (int keep = 0; keep < 2; keep++) {
            debug = keep ? 1048576 : 0;
            spirv_of[a] = {0, 1, 2};
            patched[77] = Patched{mine, false}, note_patched_from(a, 77);
            sampled[{a, 5}] = Patched{mine, false};
            forget_module(nullptr, a, nullptr);
            assert(!spirv_of.count(a));                                 // (its words: a new module at that handle brings its own)
            assert(destroyed == keep + 1);                              // (the driver still destroys it)
            if (!keep) assert(!patched.count(77) && !sampled.count({a, 5}) && !patched_from.count(a));
            else assert(patched.count(77) && stale_ids.count(77) && stale_sampled.count({a, 5}));   // (as before, for A/B)
        }
        forget_module(nullptr, VK_NULL_HANDLE, nullptr);                // (a null handle: nothing to forget)
    }
    '''
    with tempfile.TemporaryDirectory(prefix='oot-stereo-forget-') as directory:
        d = Path(directory)
        (d/'t.cpp').write_text(fixture)
        subprocess.run(['c++','-std=c++17',str(d/'t.cpp'),'-o',str(d/'t')],check=True)
        subprocess.run([str(d/'t')],check=True)


def check_stereo_camera_per_eye():
    """the lighting and shadow passes get each eye's camera: every stage checked, a moved box's vertex stage left out (STE-28)"""
    stereo = (REPO / 'vr/stereo/stereo_draw.inc').read_text(encoding='utf-8')
    draw = (REPO / 'gpu/vk/vk_draw.cpp').read_text(encoding='utf-8')
    body = stereo[stereo.index('bool vr_reads_camera(const NvnState& s) {'):]
    body = body[:body.index('\n}\n')]
    # (every stage: the lighting passes read the camera in their fragment stage; by the camera's own size: quarter-size passes)
    assert 'st < kNvnStages' in body and 'eyes_scene_camera(u.addr, u.size)' in body, 'the camera check misses stages or sizes'
    assert 'return opts.vr_eyes_camera && !vr::eyes_debug(2048);' in stereo, 'Shadows per eye: its row and its debug bit'
    # (full-screen passes and light boxes that read it are routed per eye with their eye's copy)
    assert 'vr_fs_camera = at.eyes && (vr_screen_triangle || vr_world_box) && vr_eyes_camera_per_eye() && vr_reads_camera(s);' in draw
    assert 'vr_cam_eye = vr_eye_camera ? (int)at.eyes_mask : 0, vr_cam_w = vr_cam_h = 0;' in draw
    # (a moved box or draw: its vertex stage is already shifted, the camera copy there would move it twice)
    assert 'vr_cam_frag_only = at.eyes == 2 || vr_frag_camera;' in draw
    # (STE-31: geometry lit as it is drawn, its fragment stage reading camera copies, is routed per eye with them)
    # (only draws drawn per eye anyway: routing the two-view ones too cost 33 -> 14 fps)
    assert 'vr_frag_camera = at.eyes && !vr_fs_camera && (vr_routed_anyway || vr::eyes_debug(8192)) &&' in draw
    assert 'const bool vr_routed_anyway = vr_split == 2 || (!vr_shared_inputs && vr_has_eye_inputs) || (d.mesh && !vr::eyes_debug(32));' in draw
    assert 'const bool vr_eye_camera = vr_fs_camera || vr_frag_camera;' in draw and '|| vr_fs_camera || vr_frag_camera ||' in draw
    frag = stereo[stereo.index('uint64_t vr_frag_camera_signature(const Program& p, const NvnState& s) {'):]
    frag = frag[:frag.index('\n}\n')]
    assert 'VK_SHADER_STAGE_FRAGMENT_BIT' in frag and 'eyes_camera_copies_learn(p.key, (int)binding' in frag, \
        'the fragment-stage camera check reads other stages or no copies'
    assert 'vr::eyes_debug(4096)' in frag, 'STE-31: its debug bit'
    # (STE-33: a program decided before the cascade check passed is decided again once it does)
    assert 'it->second.cascades == cascades || !it->second.signature' in frag, 'a program drawn once keeps its cascade-less signature'
    # (option (a): the draws drawn once for both eyes get their fragment stage patched instead, keyed in the pipeline)
    assert 'at.eyes_cam = at.eyes && !vr_fs_camera && !vr_frag_camera && vr_eyes_camera_per_eye() ? vr_frag_camera_patch(p, s) : 0;' in draw
    assert 'at.eyes_cam * 0x9e3779b97f4a7c15ull' in stereo, 'the fragment patch is not in the pipeline key'
    assert 'if (const VkShaderModule m = vr::eyes_camera_module(si.module, p.key, at.eyes_cam)) si.module = m;' in draw
    assert 'vr::eyes_debug(32768) ? 0 : vr_frag_camera_signature(p, s)' in stereo, 'option (a): its debug bit'
    # (STE-33: the scene camera buffer's four view->shadow cascades moved too, as inverse-view rows, after their check)
    eyes = (REPO / 'vr/stereo/vk_vr_eyes.cpp').read_text(encoding='utf-8')
    assert 'constexpr uint32_t kCascadeAt[3] = {3088, 3104, 3120};' in eyes, 'the three cascade 4x4s (headset dump, 20 Nov)'
    add = eyes[eyes.index('static void add_cascades('):]
    add = add[:add.index('\n}\n')]
    assert "c.scene != 1 || cascades_state.load() != 1 || eyes_debug(65536)" in add, 'cascades without their check or bit'
    assert 'c.at.push_back({o, 3})' in add, 'the cascades are not moved as view-space 4x4s (kind 3: all four rows)'
    assert 'else for (int r = 0; r < 4; r++) m[r * 4 + 3] += ex * m[r * 4];' in eyes, 'kind 3 does not move all four rows'
    assert 'r < (kind == 3 ? 4u : 3u)' in eyes, 'the fragment patch does not correct all four rows of kind 3'
    # (STE-34: the shared-sampling module's words kept, so the camera patch can be made on top of it; the actors' shadow
    # boxes' two view-space 3x4s at floats 0 and 36 of binding 5, moved as kind 1)
    sample = eyes[eyes.index('VkShaderModule eyes_sample_module('):]
    sample = sample[:sample.index('\n}\n')]
    assert 'spirv_of[made.module] = out;' in sample, 'the camera patch cannot be made on a shared-sampling module'
    assert 'constexpr uint32_t kActorShadowAt[2] = {0, 36};' in eyes and 'if (!have) c.at.push_back({o, 1});' in eyes
    # (STE-32/35: a screen downsample chain's steps get the right eye's layer by default, RX_VR_EYES_MORE=0 not)
    assert '(chain && levels == 1 && eyes_scene_chain(width, height))' in eyes, 'the glow chain stays one layer'
    capture = (REPO / 'vr/headset/frame_capture.inc').read_text(encoding='utf-8')
    # (STE-36, research: the fog dump is off unless RX_VR_FOG_DUMP names a folder, and hooked at every draw and dispatch)
    dump = (REPO / 'vr/stereo/fog_dump.inc').read_text(encoding='utf-8')
    assert 'vr_research_getenv("RX_VR_FOG_DUMP")' in dump and 'if (!fogdump::folder()) return;' in dump, 'the fog dump is not off by default'
    assert 'vr_fog_dump_draw(p, s, d, at);' in draw and 'vr_fog_dump_dispatch(p, s);' in draw, 'the fog dump is not hooked'
    # (STE-30: VR edits its own copy of a uniform buffer, never the one ub_overlay keeps for later draws)
    ub_vr = draw[draw.index('auto vr_private_copy = [&]() -> uint64_t {'):]
    ub_vr = ub_vr[:ub_vr.index('vr::eyes_move_camera_copies(')]
    assert 'ub_overlay(' not in ub_vr and 'ub_overlay_onto(u.addr, n, p);' in ub_vr, 'VR edits a uniform copy others share'
    assert 'for (const uint32_t k : {3u, 6u, 8u, 12u, 16u})' in capture, 'the downsample chain steps'
    # (the scene camera buffer told by its own fields, not by the copies learned from it: the headset's shadow pass learned
    # only 3 of them, from its vertex stage, and fe51a7db left its cascades out)
    scene = eyes[eyes.index('static bool scene_camera_buffer('):]
    scene = scene[:scene.index('\n}\n')]
    assert "data[0xa8 / 4] == -1.0f" in scene and 'p00_now' in scene, 'the scene camera buffer is not told by its fields'
    learn = eyes[eyes.index('static bool learn_copies('):]
    learn = learn[:learn.index('\n}\n')]
    first = learn.index('cascades_try('); assert learn.index('c.tries >= 3') < first, 'cascades added before the copies are learned'
    ub = draw[draw.index('VkDescriptorBufferInfo uniform_range('):]
    ub = ub[:ub.index('vr::eyes_move_camera_copies(vr_log_prog, (int)index, (float*)(cur->mapped + at), n, vr_cam_eye)')]
    assert '!(vr_cam_frag_only && (st.vk_stage & (VK_SHADER_STAGE_VERTEX_BIT' in ub, 'the vertex stage gets the eye copy too'
    # (STE-29: every buffer with copies of the camera, not the scene camera's buffer alone)
    assert 'vr::eyes_camera_copies_maybe(vr_log_prog, (int)index)' in ub, 'only the scene camera buffer moved'
    # (compute run once per eye: each run its eye's camera)
    assert 'vr_dispatch_eye_camera(p, s)' in draw and 'else if (vr_split == 2 && vr_cam_eye == 1) vr_cam_eye = 2;' in draw


def check_stereo_particle_camera():
    """effects with their own particle camera are moved per eye only when its projection is the scene's (STE-26)"""
    source = read('stereo_eyes')
    start = source.index('bool eyes_particle_camera(')
    end = source.index('\n}\n', start) + 3
    fixture = r'''
    #include <atomic>
    #include <cassert>
    #include <cmath>
    #include <cstdint>
    #include <cstring>
    #include <mutex>
    #include <unordered_map>
    namespace mkwvk { namespace vr {
    std::atomic<bool> kx_known{true};
    std::mutex kx_mu, cams_mu;
    float p00_now = 0.705f;
    std::unordered_map<uint64_t, std::pair<uint64_t, bool>> cams;
    uint64_t frame = 1;
    uint64_t texture_frame_no() { return frame; }
    float mem[0xc0 / 4];
    bool memory_read_as_submitted(uint64_t, void* out, size_t n) { std::memcpy(out, mem, n); return true; }
    }}
    #include "arrow_effect_math.h"
    namespace mkwvk { namespace vr {
    ''' + source[start:end] + r'''
    }}
    using namespace mkwvk::vr;
    static void camera(float p00, bool perspective) {   // (an identity view at the origin; the projection column-major at 0x80)
        std::memset(mem, 0, sizeof mem);
        float* v = mem + 0x40 / 4;
        v[0] = v[5] = v[10] = v[15] = 1;
        float* p = mem + 0x80 / 4;
        p[0] = p00, p[5] = 0.7f;
        if (perspective) p[10] = -1.0002f, p[11] = -1, p[14] = -0.2f;
        else p[10] = -0.01f, p[14] = 0, p[15] = 1;
        cams.clear(), frame++;
    }
    int main() {
        camera(0.705f, true);  assert(eyes_particle_camera(0x1000, 0x210));    // (the scene's projection: moved)
        camera(1.931f, true);  assert(!eyes_particle_camera(0x2000, 0x210));   // (the game's own FOV: not)
        camera(0.705f, false); assert(!eyes_particle_camera(0x3000, 0x210));   // (orthographic: not a perspective camera)
        camera(0.705f, true);  assert(!eyes_particle_camera(0x4000, 0x40));    // (too small to be one)
        kx_known = false; camera(0.705f, true); assert(!eyes_particle_camera(0x5000, 0x210));   // (no eye shift yet)
    }
    '''
    with tempfile.TemporaryDirectory(prefix='oot-stereo-particles-') as directory:
        d = Path(directory)
        (d/'t.cpp').write_text(fixture)
        subprocess.run(['c++','-std=c++17','-I'+str(REPO/'vr/items/bow'),str(d/'t.cpp'),'-o',str(d/'t')],check=True)
        subprocess.run([str(d/'t')],check=True)


def check_rebar_hint():
    """the Resizable BAR hint: one line, once a run, only in VR on a discrete card without Resizable BAR; at a lost device
    (before the fatal line), an image put in system memory for want of VRAM and pipelines refused for want of memory; its
    prefix the one the crash window reads (host/hos_report_prefixes.h)"""
    hint = (REPO / 'vr/headset/rebar_hint.cpp').read_text(encoding='utf-8')
    rule = re.search(r'bool rebar_hint_wanted\(bool headset, int device_type, bool rebar\) \{.*?\n\}', hint, re.S).group()
    body = re.search(r'void rebar_hint\(const char\* what\) \{.*?\n\}', hint, re.S).group()
    assert 'HOS_REPORT_VR_REBAR_PREFIX' in body and 'told.exchange(true)' in body and 'rebar_hint_wanted(active()' in body
    prefixes = (REPO / 'host/hos_report_prefixes.h').read_text(encoding='utf-8')
    assert re.search(r'#define HOS_REPORT_VR_REBAR_PREFIX "vk: VR: [^"]+"', prefixes)
    assert 'HOS_REPORT_VR_REBAR_PREFIX' in (REPO / 'host/hos_crash_dialog.h').read_text(encoding='utf-8')
    replay = (REPO / 'gpu/vk/vk_replay.cpp').read_text(encoding='utf-8')
    lost = re.search(r'\[\[noreturn\]\] void device_lost\(const char\* why\) \{.*?\n\}', replay, re.S).group()
    assert lost.index('vr::rebar_hint(') < lost.index('replay_fatal(lost)'), 'the hint before the fatal line'
    vma = (REPO / 'gpu/vk/vk_vma.cpp').read_text(encoding='utf-8')
    assert re.search(r'logf\("vk: VRAM full: a %ux%u image[^\n]+\n\s+vr::rebar_hint\(', vma)
    texture = (REPO / 'gpu/vk/vk_texture.cpp').read_text(encoding='utf-8')
    assert re.search(r'the driver refused %lld pipeline\(s\) for want of memory.*?vr::rebar_hint\(', texture, re.S)
    compile_run(r'''
    #include <cassert>
    enum { VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU = 1, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU = 2 };
    ''' + rule + r'''
    int main() {
        assert(rebar_hint_wanted(true, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, false));
        assert(!rebar_hint_wanted(true, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, true));    /* (Resizable BAR on) */
        assert(!rebar_hint_wanted(false, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, false));  /* (no headset: a flat screen) */
        assert(!rebar_hint_wanted(true, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, false)); /* (an APU: no BAR to turn on) */
        return 0;
    }
    ''', cpp=True)


def check_stereo_capture():
    """the headless picture capture runs only when asked"""
    replay = read('vk_replay')
    calls = list(re.finditer(r'        if \(tex && vr::eyes_wanted\(\).*?\n\s*vr::eyes_present_test\([^;]+;', replay))
    assert len(calls) == 1, f'{len(calls)} final capture call sites, expected exactly one'
    route = calls[0].group()
    limit = re.search(r'int capture_pair_limit\(bool final\) \{[^\n]+\}',
                      read('stereo_eyes')).group()
    fixture = r'''
    #include <cassert>
    #include <cstdio>
    constexpr int VK_NULL_HANDLE=0;
    struct Texture {bool vr_eyes=true;int image=11;unsigned width=2880,height=2304;};
    struct Recording {int cmd=1;} r;
    namespace vr {
     bool enabled=true,hmd=false;int calls=0;
     bool eyes_wanted(){return enabled;}bool active(){return hmd;}
     void eyes_present_test(int,int image,unsigned,unsigned){assert(image==11);++calls;}
    }
    bool test=false;const char* test_dir(){return test?"test":nullptr;}
    '''
    checks = r'''
    int main(){
     Texture t;
     vr::hmd=false;capture(&t);assert(vr::calls==1);
     vr::hmd=true;capture(&t);assert(vr::calls==2);
     vr::enabled=false;capture(&t);assert(vr::calls==2);
     vr::enabled=true;capture(nullptr);assert(vr::calls==2);
     assert(capture_pair_limit(false)==3 && capture_pair_limit(true)==3);
     test=true;assert(capture_pair_limit(false)==8 && capture_pair_limit(true)==6);
    }
    '''
    with tempfile.TemporaryDirectory(prefix='oot-stereo-capture-') as directory:
        exe = str(Path(directory)/'check')
        subprocess.run(['c++','-std=c++17','-x','c++','-','-o',exe],
                       input=fixture+limit+'\nvoid capture(Texture* tex){\n'+route+'\n}\n'+checks,
                       text=True,check=True)
        subprocess.run([exe],check=True)


def check_scene_depth_any_share():
    """the scene's depth and true stereo's second eye are copied at every Render resolution: 67%/58%/50%/33% networks by shape"""
    draw = read('vk_draw')
    hook = re.search(r'if \(\(vr::active\(\) \|\| vr::eyes_wanted\(\)\) && (\w+)\(s, p\)\) \{[^\n]*\n\s*end_pass\(cmd\);\n'
                     r'\s*vr::scene_done\(cmd\);', draw)
    assert hook, 'the dispatch hook that runs vr::scene_done is gone'
    assert hook.group(1) == 'vr_scene_network', f'scene_done runs on {hook.group(1)}, not on vr_scene_network'
    known = re.search(r'bool gameplay_network\(uint64_t key\) \{[^\n]+\}', draw).group()
    body = 'static bool vr_scene_network(const NvnState& s, const Program& p) ' + function_body(draw, 'static bool vr_scene_network(')
    assert 'scene_color_is' in read('vr_h') and 'bool scene_color_is(const Texture* color) {' in read('headset')
    compile_run(r'''
    #include <cassert>
    #include <atomic>
    #include <cstdint>
    #include <vector>
    enum VkImageViewType { VK_IMAGE_VIEW_TYPE_2D };
    enum VkFormat { VK_FORMAT_UNDEFINED };
    enum class ViewUse { Sample, Storage };
    struct Texture { unsigned width = 1280, height = 720; };
    struct Descriptor { int count = 1; };
    struct Info { bool uses_cooperative_matrix = true; std::vector<Descriptor> texture_descriptors{1}, image_descriptors{1}; };
    struct Stage { Info info; };
    struct Program { uint64_t key = 0x6676619641320ba7ull; std::vector<Stage> stages{1}; };
    struct NvnState {};
    Texture scene, other;
    unsigned bound_handle = 1;
    uint32_t handle_of(const NvnState&, const Stage&, const Descriptor&, int) { return bound_handle; }
    int texture_view_of(uint32_t h, VkImageViewType, ViewUse, VkFormat, Texture** t) { *t = h == 1 ? &scene : &other; return 1; }
    namespace vr { bool scene_color_is(const Texture* t) { return t == &scene; } }
    void logf(const char*, ...) {}
    ''' + known + '\n' + body + r'''
    int main() {
        NvnState s;
        Program p;
        assert(vr_scene_network(s, p));                          // (67%: its own network, the scene's colour in)
        p.key = 0x72ba090c70fe2614ull; assert(vr_scene_network(s, p));   // (50%)
        bound_handle = 2; assert(!vr_scene_network(s, p));        // (a network of that shape on another texture)
        p.key = 0x9dc98871eb7fc101ull; assert(vr_scene_network(s, p));   // (100%: the full-size pass, by its key)
        p.key = 0xfd029a317a9a4b94ull; assert(vr_scene_network(s, p));   // (75%)
        p.key = 1; bound_handle = 1; p.stages[0].info.uses_cooperative_matrix = false; assert(!vr_scene_network(s, p));
        p.stages[0].info.uses_cooperative_matrix = true; p.stages[0].info.texture_descriptors.resize(2); assert(!vr_scene_network(s, p));
        p.stages.clear(); assert(!vr_scene_network(s, p));
    }
    ''', cpp=True)


def check_warp_near_plane():
    """From depth's warp clamps distances to the picture's real near plane (0.1 m in play), not its far plane (24672 m)"""
    eyes = read('eye_rendering')
    planes = re.search(r'        const float a = f\.projection_ok.*?\n        const float far_plane = [^\n]+\n', eyes, re.S)
    assert planes, 'make_eyes: the depth planes (a, b, near_plane, far_plane) are gone'
    assert 'p.near_plane = std::max(kWarpNearOf(opts.vr_near_depth), near_plane' in eyes, 'the warp\'s near distance: the camera\'s, eased no nearer than Near depth\'s'
    compile_run(r'''
    #include <cassert>
    #include <cmath>
    struct { bool projection_ok = true; } f;
    struct { float proj[16]; } sl;
    static void planes_of(float n, float fa, float* near_out, float* far_out) {
        for (float& v : sl.proj) v = 0;
        sl.proj[10] = -(fa + n) / (fa - n), sl.proj[11] = -2 * fa * n / (fa - n), sl.proj[14] = -1;   // (the game's, in float)
    ''' + planes.group() + r'''
        *near_out = near_plane, *far_out = far_plane;
        (void)b;
    }
    int main() {
        float n, fa;
        planes_of(0.1f, 25000.0f, &n, &fa); assert(std::fabs(n - 0.1f) < 1e-4f && fa > 20000.0f);   // (gameplay)
        planes_of(0.5f, 1000.0f, &n, &fa); assert(std::fabs(n - 0.5f) < 1e-4f && std::fabs(fa - 1000.0f) < 1.0f);   // (a cutscene)
        f.projection_ok = false; planes_of(0.1f, 25000.0f, &n, &fa); assert(n == 0.05f);
    }
    ''', cpp=True)


def check_warp_splat():
    """From depth warps in two passes: each source lands once (the nearest kept), holes filled from behind; headers rebuilt"""
    shaders = REPO / 'vr/headset/shaders'
    splat, warp = (shaders / 'vr_warp_splat.comp').read_text(), (shaders / 'vr_warp.comp').read_text()
    assert 'imageAtomicMax(landed' in splat, 'pass 1: the nearest source per pixel (the largest shift)'
    assert 'imageLoad(landed, p)' in warp and 'landed_along(p, walk' in warp, 'pass 2: the landed source, or the background'
    assert 'float tmax' not in warp, 'the fixed-step march is back (its steps matched the wrong surface: ghost copies)'
    eyes = read('eye_rendering')
    order = [eyes.index(k) for k in ('vkCmdClearColorImage(cmd, s.landed[e].image', 'VK_PIPELINE_BIND_POINT_COMPUTE, s.warp_splat)',
                                     'memory_barrier(cmd);\n        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.warp)')]
    assert order == sorted(order), 'clear, splat, barrier, resolve'
    if not shutil.which('glslangValidator'):
        skip('glslangValidator not found (the SPIR-V headers not compared)')
    with tempfile.TemporaryDirectory(prefix='hookshot-vr-warp-') as directory:
        for name, var in (('vr_warp', 'VR_WARP_COMP_SPV'), ('vr_warp_splat', 'VR_WARP_SPLAT_COMP_SPV')):
            out = Path(directory) / f'{name}.h'   # (the command that makes the header)
            subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.3', '--vn', var, str(shaders / f'{name}.comp'),
                            '-o', str(out)], check=True, capture_output=True)
            words = lambda text: re.findall(r'0x[0-9a-f]{8}', text)
            assert words(out.read_text()) == words((shaders / f'{name}_comp_spv.h').read_text()), f'{name}_comp_spv.h is stale'


def check_3d_modes():
    """3D is Mono (the default) or Stereo: a card sets its own mode, a saved From depth loads as Mono; one switch offers it again"""
    values = (REPO / 'vr/menu/vr_values_menu.inc').read_text(encoding='utf-8')
    code = values[values.index('constexpr int kVr3dMono'):values.index('const char* tx_vr_near_depth')]
    options = (REPO / 'vr/menu/vr_options.inc').read_text(encoding='utf-8')
    menu = (REPO / 'gpu/vk/vk_menu.cpp').read_text(encoding='utf-8')
    switch = 'static constexpr bool vr_from_depth = false;'
    assert options.count(switch) == 1 and 'int vr_stereo = 0;' in options
    hook = re.search(r'    if \(r\.value == &opts\.vr_stereo\) (v = [^\n;]+;)', function_body(menu, 'void set_row(Row& r, int v, int dir'))
    load = re.search(r'if \(r\.value == &opts\.vr_stereo\) (\*r\.value = [^\n;]+;)', menu)
    assert hook and load and 'r.lo), r.hi);' in menu[load.start() - 200:load.start()], 'set_row and the ini reader take the modes offered'
    vrm = path('vr_menu').read_text(encoding='utf-8')
    card = vrm[vrm.index('    {"3D", &opts.vr_stereo, VrmKind::cards'):]
    card = card[:card.index('VrmCard{}}},') + len('VrmCard{}}},')]
    assert 'if (*r.value != v) set_row(*row, v, v > *r.value ? 1 : -1);' in vrm, 'vrm_set: a card gives its value and a direction'
    assert '#include "../../menu/vr_options.inc"' in (REPO / 'vr/launcher/src/system.h').read_text(encoding='utf-8'), 'the launcher: the same switch'
    for on in (False, True):
        compile_run(r'''
        #include <cassert>
        #include <cstring>
        struct Options {
        ''' + (options.replace(switch, switch.replace('false', 'true')) if on else options) + r'''
        } opts;
        struct Row { int* value; } r{&opts.vr_stereo};
        ''' + code + r'''
        enum class VrmKind { cards };
        struct VrmCard { const char* label; const char* desc; int value; const char* icon; };
        struct VrmRow { const char* name; int* value; VrmKind kind; unsigned when; const char* a; const char* b; VrmCard cards[3]; };
        const VrmRow row[] = {''' + card + r'''};
        static int set(int from, int v, int dir) { opts.vr_stereo = from; (void)dir; ''' + hook.group(1) + r''' return v; }
        static int loaded(int saved) { opts.vr_stereo = saved; ''' + load.group(1) + r''' return opts.vr_stereo; }
        int main() {
            assert(Options().vr_stereo == 0 && loaded(0) == 0 && loaded(4) == 4);
            const bool on = Options::vr_from_depth;
            assert(loaded(1) == (on ? 1 : 0));
            const int n = row[0].cards[2].label ? 3 : 2;
            assert(n == (on ? 3 : 2));
            for (int c = 0; c < n; c++) {
                assert(!strcmp(row[0].cards[c].label, tx_vr_stereo(row[0].cards[c].value)));
                // (from every mode, the card's own: v1.0.2's first three cards stepped instead, and Flat to True stereo gave From depth)
                for (int f = 0; f < n; f++) {
                    const int from = row[0].cards[f].value, v = row[0].cards[c].value;
                    if (from != v) assert(set(from, v, v > from ? 1 : -1) == v);
                }
            }
            assert(!strcmp(tx_vr_stereo(0), "Mono") && !strcmp(tx_vr_stereo(4), "Stereo"));
        }
        ''', cpp=True)
    eyes = read('eye_rendering')
    loop = eyes[eyes.index('const bool flat_copy = opts.vr_stereo == 0'):]
    assert loop.index('continue;') < loop.index('vkCmdDispatch('), 'Mono: the picture copied, the warp skipped'
    present = function_body(read('headset'), 'void present(VkCommandBuffer cmd, VkImage src,')
    slot = present[present.index('make_image(sl.color,'):][:300]
    assert 'VK_IMAGE_USAGE_TRANSFER_SRC_BIT' in slot, 'the slot\'s picture copied from in Mono: TRANSFER_SRC'


def check_flat_eyes():
    """flat frames (v1.0.2): each eye its located pose and FOV, the room-fixed screen where that eye sees it; a release switch"""
    switches = (REPO / 'gpu/vk/switches.def').read_text()
    row = switches.index('SW(RX_VR_FLAT_LAYER, kill,')
    assert switches.rindex('#endif', 0, row) > switches.rindex('#ifdef HOOKSHOT_VR_RESEARCH', 0, row), 'a release switch'
    eyes, loop = read('eye_rendering'), read('frame_loop')
    flat = (REPO / 'vr/headset/flat_eyes.inc').read_text(encoding='utf-8')
    assert 'const char* e = getenv("RX_VR_FLAT_LAYER");' in eyes
    mode = eyes[eyes.index('enum FlatMode'):eyes.index('const char* flat_mode_name() {')]
    assert 'vli.space = s.local;\n            XrViewState rs' in loop, 'the eyes located in the room, at the display time'
    views = flat[flat.index('int flat_eyes_views('):]
    assert 'flat_eyes.fov[e] = located[e].fov;' in views and 'v.pose = flat_eyes.pose[e];' in views and 'v.fov = flat_eyes.fov[e];' in views
    assert 'const FlatEyePush push = flat_eye_push(' in flat, 'the push constants kept by value (a pointer into the temporary drew black)'
    push = eyes[eyes.index('struct FlatEyePush'):eyes.index('// ---- a gameplay frame both eyes share')]
    math = (REPO / 'vr/headset/poses.inc').read_text(encoding='utf-8')
    quats = math[math.index('XrQuaternionf q'):math.index('\n', math.index('XrQuaternionf qinv('))]
    compile_run(r"""
    #include <cassert>
    #include <cmath>
    #include <cstdio>
    #include <cstdlib>
    #include <cstring>
    struct XrVector3f { float x, y, z; };
    struct XrQuaternionf { float x, y, z, w; };
    struct XrPosef { XrQuaternionf orientation; XrVector3f position; };
    struct XrFovf { float angleLeft, angleRight, angleUp, angleDown; };
    #define logf(...) (void)0
    """ + mode + quats + push + r"""
    static float rad(float deg) { return deg / 57.2958f; }
    int main(int, char**) {
        setenv("RX_VR_FLAT_LAYER", "projection", 1);
        assert(flat_mode() == kFlatProjection);
        const float yaw = rad(30), hx = 2.45f, hy = 1.38f;
        const XrPosef screen{{0, std::sin(yaw / 2), 0, std::cos(yaw / 2)}, {0.4f, 1.5f, -3.2f}};
        for (int e = 0; e < 2; e++) {   // (asymmetric FOVs, 63 mm apart, each turned 5 degrees outward, the head rolled and moved)
            const float cant = rad(e ? -5 : 5), roll = rad(12);
            const XrQuaternionf eq = qmul(XrQuaternionf{0, 0, std::sin(roll / 2), std::cos(roll / 2)}, XrQuaternionf{0, std::sin(cant / 2), 0, std::cos(cant / 2)});
            const XrPosef eye{eq, {e ? 0.13f : 0.067f, 1.7f, 0.2f}};
            const XrFovf fov = e ? XrFovf{rad(-42), rad(52), rad(46), rad(-50)} : XrFovf{rad(-52), rad(42), rad(46), rad(-50)};
            const FlatEyePush p = flat_eye_push(screen, 2 * hx, 2 * hy, eye, fov, 2000, 2200, 2);
            const float* v = p.v;
            assert(v[11] == 2000 && v[15] == 2200 && v[20] == hx && v[21] == hy);
            for (int c = 0; c < 5; c++) {   // (the corners and the middle: where the eye sees each in the room, the shader finds it)
                const float at[3] = {c == 4 ? 0.0f : c & 1 ? hx : -hx, c == 4 ? 0.0f : c & 2 ? hy : -hy, 0};
                float w[3], dir[3];
                qrot(screen.orientation, at, w);
                const float d[3] = {w[0] + screen.position.x - eye.position.x, w[1] + screen.position.y - eye.position.y, w[2] + screen.position.z - eye.position.z};
                qrot(qinv(eye.orientation), d, dir);   // (in the eye's view)
                const float tx = dir[0] / -dir[2], ty = dir[1] / -dir[2];
                // (vr_flat_eyes.comp hit: the view's tangents at that pixel, the ray in the screen's axes)
                const float px = (tx - v[4]) / (v[5] - v[4]) * v[11], py = (ty - v[7]) / (v[6] - v[7]) * v[15];
                const float mx = v[4] + (v[5] - v[4]) * px / v[11], my = v[7] + (v[6] - v[7]) * py / v[15];
                float r[3];
                for (int i = 0; i < 3; i++) r[i] = v[8 + i] * mx + v[12 + i] * my - v[16 + i];
                const float k = v[2] / -r[2];
                assert(std::fabs(v[0] + r[0] * k - at[0]) < 2e-4f && std::fabs(v[1] + r[1] * k - at[1]) < 2e-4f);
            }
        }
    }
    """, cpp=True)
    shaders = REPO / 'vr/headset/shaders'
    comp = (shaders / 'vr_flat_eyes.comp').read_text()
    assert 'vec3 d = pc.c0.xyz * mix(pc.tangent.x, pc.tangent.y, t.x) + pc.c1.xyz * mix(pc.tangent.w, pc.tangent.z, t.y) - pc.c2.xyz;' in comp
    if not shutil.which('glslangValidator'):
        skip('glslangValidator not found (the SPIR-V header not compared)')
    with tempfile.TemporaryDirectory(prefix='hookshot-vr-flat-') as directory:
        out = Path(directory) / 'flat.h'
        subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.3', '--vn', 'VR_FLAT_EYES_COMP_SPV', str(shaders / 'vr_flat_eyes.comp'),
                        '-o', str(out)], check=True, capture_output=True)
        words = lambda text: re.findall(r'0x[0-9a-f]{8}', text)
        assert words(out.read_text()) == words((shaders / 'vr_flat_eyes_comp_spv.h').read_text()), 'vr_flat_eyes_comp_spv.h is stale'


def check_mono_pose():
    """a gameplay frame both eyes share one picture of (Mono, no depth): each view at its own eye, the picture at infinity"""
    switches = (REPO / 'gpu/vk/switches.def').read_text()
    row = switches.index('SW(RX_VR_MONO_POSE, kill,')
    assert switches.rindex('#endif', 0, row) > switches.rindex('#ifdef HOOKSHOT_VR_RESEARCH', 0, row), 'a release switch'
    eyes, loop = read('eye_rendering'), read('frame_loop')
    assert 'f.own = !sl.head_locked && !stereo && eye_pose_ok && mono_pose_eyes();' in eyes, "Stereo's views stay as they are"
    assert 'if (f.own) pose = shared_eye_pose(f.drawn, eye_pose[e], f.turned);' in eyes, 'from the pose the picture was drawn at'
    assert '? "Mono" : opts.vr_stereo == 4 ?' in loop and '"no (Mono)"' in eyes and '"Flat" : opts.vr_stereo' not in loop
    # (parallel eyes: never the resampling pass, so the picture goes as before)
    assert 'f.turned = f.own && f.projection_ok && eye_turned(eye_pose[e].orientation);' in eyes
    math = (REPO / 'vr/headset/poses.inc').read_text(encoding='utf-8')
    quats = math[math.index('XrQuaternionf q'):math.index('\n', math.index('XrQuaternionf qinv('))]
    push = eyes[eyes.index('struct FlatEyePush'):eyes.index('// ---- a gameplay frame both eyes share')]
    code = eyes[eyes.index('/// Whether an eye is turned from the head'):eyes.index('/// Each eye from its slot')]
    compile_run(r"""
    #include <algorithm>
    #include <cassert>
    #include <cmath>
    #include <cstdint>
    struct XrVector3f { float x, y, z; };
    struct XrQuaternionf { float x, y, z, w; };
    struct XrPosef { XrQuaternionf orientation; XrVector3f position; };
    struct XrFovf { float angleLeft, angleRight, angleUp, angleDown; };
    struct XrOffset2Di { int32_t x, y; };
    struct XrExtent2Di { int32_t width, height; };
    struct XrRect2Di { XrOffset2Di offset; XrExtent2Di extent; };
    """ + quats + push + code + r"""
    static float rad(float deg) { return deg / 57.2958f; }
    int main() {
        assert(!eye_turned({0, 0, 0, 1}) && !eye_turned({0, 0.0004f, 0, 1}) && eye_turned({0, std::sin(rad(0.5f)), 0, std::cos(rad(0.5f))}));
        const XrPosef drawn{{0, std::sin(rad(20)), 0, std::cos(rad(20))}, {1, 2, 3}};
        const XrPosef eye{{0, std::sin(rad(2.5f)), 0, std::cos(rad(2.5f))}, {-0.032f, 0.01f, -0.005f}};
        const XrPosef p = shared_eye_pose(drawn, eye, false), q = shared_eye_pose(drawn, eye, true);
        assert(p.orientation.y == drawn.orientation.y && p.orientation.w == drawn.orientation.w);   // (the turn the picture has)
        float off[3] = {p.position.x - 1, p.position.y - 2, p.position.z - 3}, back[3];
        qrot(qinv(drawn.orientation), off, back);
        assert(std::fabs(back[0] + 0.032f) < 1e-6f && std::fabs(back[1] - 0.01f) < 1e-6f && std::fabs(back[2] + 0.005f) < 1e-6f);
        assert(std::fabs(q.orientation.y - std::sin(rad(22.5f))) < 1e-5f && q.position.x == p.position.x);
        // a direction in the head's frame: its picture pixel, and where the turned eye's view of the picture shows it
        const XrFovf own{rad(-52), rad(42), rad(46), rad(-50)};
        const float p00 = 0.76f, p11 = 0.8f;
        XrFovf fov;
        XrRect2Di r;
        const FlatEyePush e = turned_eye_view(eye, own, false, 1.03f, p00, p11, 2880, 2304, &fov, &r);
        assert(std::fabs(std::tan(fov.angleLeft) - 1.03f * std::tan(rad(-52))) < 1e-4f && r.extent.width <= 2880 && r.extent.height <= 2304);
        for (float x = -0.8f; x < 0.9f; x += 0.4f)
            for (float y = -0.8f; y < 0.9f; y += 0.4f) {
                const float dir[3] = {x, y, -1};
                float d[3];
                qrot(qinv(eye.orientation), dir, d);
                const float tx = d[0] / -d[2], ty = d[1] / -d[2];   // (in the eye: its pixel by the submitted fov and rect)
                const float px = (tx - std::tan(fov.angleLeft)) / (std::tan(fov.angleRight) - std::tan(fov.angleLeft)) * r.extent.width;
                const float py = (std::tan(fov.angleUp) - ty) / (std::tan(fov.angleUp) - std::tan(fov.angleDown)) * r.extent.height;
                const float* v = e.v;
                const float mx = v[4] + (v[5] - v[4]) * px / v[11], my = v[7] + (v[6] - v[7]) * py / v[15];   // (vr_flat_eyes.comp)
                float ray[3];
                for (int i = 0; i < 3; i++) ray[i] = v[8 + i] * mx + v[12 + i] * my - v[16 + i];
                const float k = v[2] / -ray[2];
                const float u = 0.5f + 0.5f * (v[0] + ray[0] * k) / v[20], w = 0.5f - 0.5f * (v[1] + ray[1] * k) / v[21];
                assert(std::fabs(u - (0.5f + 0.5f * x * p00)) < 1e-4f && std::fabs(w - (0.5f - 0.5f * y * p11)) < 1e-4f);
            }
        assert(e.v[22] == 1);
    }
    """, cpp=True)


def check_tester_log():
    """the log a player's picture is settled from (v1.0.2): the options at the start and each change, in every build, bounded"""
    log = (REPO / 'vr/headset/tester_log.inc').read_text(encoding='utf-8')
    assert 'vr_research_getenv' not in log and 'HOOKSHOT_VR_RESEARCH' not in log, 'in players\' builds'
    loop, session = read('frame_loop'), (REPO / 'vr/headset/runtime_session.inc').read_text(encoding='utf-8')
    assert loop.index('const XrResult ended = xrEndFrame(') < loop.index('tester_frame(layers, n_layers, named,')
    assert loop.index('&blank_quad}}') > loop.index('{"the VR menu", &panel_quad}'), 'the blank layer is the last named'
    assert session.index('xrCreateSwapchain = tester_swapchain;') < session.index('make_controls();')
    assert 'tester_extensions(ci);' in session and 'tester_session_report(formats);' in session
    assert 'logf("vr: [%.1f s] VR menu %s", run_s(), open ? "opened" : "closed");' in (REPO / 'vr/menu/vr_menu_api.inc').read_text()
    code = log[log.index('struct TesterOption {'):log.index("// ---- the runtime's offer")]
    fields = sorted(set(re.findall(r'opts\.(\w+)', code)))
    rows = (REPO / 'gpu/vk/upscaler_rows.h').read_text(encoding='utf-8')
    compile_run(r"""
    #include <cassert>
    #include <cstdarg>
    #include <cstdint>
    #include <cstdio>
    #include <cstdlib>
    #include <cstring>
    #include <string>
    #include <vector>
    static struct { int """ + ', '.join(f + ' = 0' for f in fields) + r"""; } opts;
    constexpr int kAutoPreset = 6;
    namespace upscaler_rows { constexpr int kUpscalers = 9; constexpr int kPercent[] = {0, 100, 75, 67}; inline int percent(int v) { return v > 0 && v < 4 ? kPercent[v] : 0; } }
    namespace switches { struct Row { const char* name; }; static const Row rows[] = {{"RX_VK_OTHER"}, {"RX_VR_FLAT_LAYER"}, {"RX_VR_MONO_POSE"}}; }
    static std::vector<std::string> said;
    static void logf(const char* f, ...) { char b[1024]; va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a); said.push_back(b); }
    static double run_s() { return 12.34; }
    static const char* flat_mode_name() { return "eyes"; }
    static bool mono_pose_eyes() { return true; }
    static bool layer_fov_located() { return true; }
    static bool has(const char* t) { for (auto& l : said) if (l.find(t) != std::string::npos) return true; return false; }
    """ + code + r"""
    int main() {
        setenv("RX_VR_FLAT_LAYER", "quad", 1), setenv("RX_VK_OTHER", "1", 1);
        opts.vr_mode = 1, opts.resolution = 3, opts.preset = 6;
        assert(!tester_options_frame() && has("View First person (1)") && has("3D Mono (0)") && has("Render resolution 67% (3)") && has("Auto (this PC) (6)"));
        assert(has("switch set: RX_VR_FLAT_LAYER=quad") && !has("RX_VK_OTHER") && !has("RX_VR_MONO_POSE"));
        for (auto& l : said) assert(l.size() < 900);
        said.clear();
        assert(!tester_options_frame() && said.empty());
        opts.vr_mode = 0, opts.vr_window = 1, opts.vr_stereo = 4;
        assert(tester_options_frame() && said.size() == 2 && has("[12.3 s] option View: First person (1) -> Window (2)") && has("3D: Mono (0) -> Stereo (4)"));
        for (int i = 0; i < 1000; i++) opts.vr_stereo ^= 4, tester_options_frame();
        assert(said.size() <= 300);
    }
    """, cpp=True)
    assert 'kUpscalers = 9' in rows


def check_layer_eye_fov():
    """each eye's layer view is its own located FOV (+3%), cut from the symmetric picture: the rect matches the angles"""
    eyes = read('eye_rendering')
    head = 'bool located_eye_cut(const XrFovf& eye, float guard, float p00, float p11, uint32_t w, uint32_t h, XrFovf* fov, XrRect2Di* rect)'
    assert head in eyes, 'located_eye_cut: its parameters changed'
    cut = head + ' ' + function_body(eyes, head)
    assert 'located_eye_cut(own, kCameraGuardBand, f.proj_x, f.proj_y, f.w, f.h, &cut, &rect)' in eyes
    assert 'RX_VR_LAYER_FOV' in (REPO / 'gpu/vk/switches.def').read_text()
    compile_run(r'''
    #include <algorithm>
    #include <cassert>
    #include <cmath>
    #include <cstdint>
    struct XrFovf { float angleLeft, angleRight, angleUp, angleDown; };
    struct XrOffset2Di { int32_t x, y; };
    struct XrExtent2Di { int32_t width, height; };
    struct XrRect2Di { XrOffset2Di offset; XrExtent2Di extent; };
    ''' + cut + r'''
    static float rad(float deg) { return deg / 57.2958f; }
    int main() {
        // (a Rift-like pair: each eye wider toward its own side; the picture encloses both, widened 3%, as the game's camera)
        const XrFovf left{rad(-52), rad(42), rad(46), rad(-50)}, right{rad(-42), rad(52), rad(46), rad(-50)};
        const float tx = std::tan(rad(52)) * 1.03f, ty = std::tan(rad(50)) * 1.03f, p00 = 1 / tx, p11 = 1 / ty;
        const uint32_t w = 1920, h = 1080;
        XrFovf f[2];
        XrRect2Di r[2];
        assert(located_eye_cut(left, 1.03f, p00, p11, w, h, &f[0], &r[0]) && located_eye_cut(right, 1.03f, p00, p11, w, h, &f[1], &r[1]));
        assert(r[0].offset.x == 0 && r[0].offset.x + r[0].extent.width < (int32_t)w);   // (the left eye: from the left edge)
        assert(r[1].offset.x > 0 && r[1].offset.x + r[1].extent.width == (int32_t)w);   // (the right: to the right edge)
        assert(r[0].extent.width == r[1].extent.width && r[0].offset.y == r[1].offset.y);   // (mirror images)
        assert(r[0].offset.y > 0 && r[0].offset.y + r[0].extent.height == (int32_t)h);   // (up 46 deg of the picture's 50)
        for (int e = 0; e < 2; e++) {
            const XrFovf& own = e ? right : left;
            // (each angle the eye's own widened by about 3%, to the whole pixel)
            assert(std::fabs(std::tan(-f[e].angleLeft) - std::min(std::tan(-own.angleLeft) * 1.03f, tx)) < 2.5f * tx / w);
            assert(std::fabs(std::tan(f[e].angleRight) - std::min(std::tan(own.angleRight) * 1.03f, tx)) < 2.5f * tx / w);
            assert(std::fabs(std::tan(f[e].angleUp) - std::tan(own.angleUp) * 1.03f) < 2.5f * ty / h);
            // (and the rect's pixels are those angles: x = (t p00 + 1) / 2 w)
            assert(std::fabs((std::tan(f[e].angleLeft) * p00 + 1) * 0.5f * w - r[e].offset.x) < 0.01f);
            assert(std::fabs((std::tan(f[e].angleRight) * p00 + 1) * 0.5f * w - (r[e].offset.x + r[e].extent.width)) < 0.01f);
            assert(std::fabs((1 - std::tan(f[e].angleUp) * p11) * 0.5f * h - r[e].offset.y) < 0.01f);
        }
        // (a symmetric eye the size of the picture: the whole picture)
        const XrFovf whole{-std::atan(tx), std::atan(tx), std::atan(ty), -std::atan(ty)};
        assert(located_eye_cut(whole, 1.0f, p00, p11, w, h, &f[0], &r[0]));
        assert(r[0].offset.x == 0 && r[0].offset.y == 0 && r[0].extent.width == (int32_t)w && r[0].extent.height == (int32_t)h);
        // (nothing usable: the picture's whole FOV stays)
        assert(!located_eye_cut(left, 1.03f, 0, p11, w, h, &f[0], &r[0]));
        assert(!located_eye_cut(XrFovf{0, 0, 0, 0}, 1.03f, p00, p11, w, h, &f[0], &r[0]));
    }
    ''', cpp=True)


# ==========================================================================================================================
# The build: the game hooks really are in the translated game (needs --work)
# ==========================================================================================================================

def check_game_hooks_emitted():
    """every game hook in vr/vr_game_hooks.py made it into the translated game (needs --work)"""
    if not ARGS.work:
        return skip('needs the build\'s work folder: --work <folder>')
    gens = [Path(ARGS.work) / d / 'gen' for d in ('sa', '')]
    code = ''.join(p.read_text(errors='replace') for g in gens for p in g.glob('code_*.c'))
    if not code:
        return skip(f'no translated game in {ARGS.work}')
    sys.path.insert(0, str(path('game_hooks').parent))
    import vr_game_hooks
    wanted = {}
    for off, word, text in vr_game_hooks.sites():   # (each hook's function, and how many sites call it)
        name = text.split('(')[0].split()[-1]
        wanted[name] = wanted.get(name, 0) + 1
    short = {name: (code.count(name + '(c'), n) for name, n in wanted.items() if code.count(name + '(c') < n}
    assert not short, f'hooks missing from the translated game (found, wanted): {short}'


def check_generate_inputs():
    """missing or damaged game files stop the game-data generators with a plain message, writing no header"""
    generate, _ = game_tools()
    with tempfile.TemporaryDirectory(prefix='hookshot-vr-generate-') as folder:
        romfs, out = Path(folder) / 'romfs', Path(folder) / 'out'
        romfs.mkdir()
        try:
            generate.generate(str(romfs), str(out))
        except SystemExit as e:
            message = str(e)
        else:
            raise AssertionError('an empty game folder was accepted')
        assert 'Player_Adult' in message and 'ZsDic.pack.zs' in message, message
        assert not out.exists(), 'headers written without the game files'
        # A model file that is not a BFRES (here: a .zs file that is not compressed at all, just text)
        for rel in {'Pack/ZsDic.pack.zs'} | {r for g in generate.GENERATORS for r in g.INPUTS}:
            (romfs / rel).parent.mkdir(parents=True, exist_ok=True)
            (romfs / rel).write_bytes(b'not a model')
        try:
            generate.generate(str(romfs), str(out))
        except (ValueError, AssertionError, SystemExit):
            pass
        else:
            raise AssertionError('a damaged model was accepted')
        assert not any(out.glob('*.h')), 'a header written from a damaged model'


def check_devmap_formats():
    """the dev map's BYML and SARC code writes what it reads: every value's type kept (64-bit, floats, bools, signed),
    keys sorted, archives found by name; no game files needed"""
    import importlib
    import struct
    sys.path.insert(0, str(REPO / 'vr' / 'devmap'))
    byml = importlib.import_module('byml')
    sarc = importlib.import_module('sarc')
    V = byml.Value
    tree = {'Actors': [{'Gyaml': 'Obj_Test', 'Hash': V.u64(0xfedcba9876543210), 'Translate': [V.float(1.5), V.float(-2.25), V.float(0.0)],
                        'Dynamic': {'Flag': V.bool(True), 'Count': V.int(-3), 'Mask': V.uint(0xffffffff)}, 'Name': 'b'}],
            'Zeta': [], 'Alpha': {}, 'Wide': V(0xd4, (-5) & 0xffffffffffffffff), 'Double': V(0xd6, struct.unpack('<Q', struct.pack('<d', 0.1))[0])}
    data = byml.write(tree)
    assert data[:2] == b'YB' and struct.unpack_from('<H', data, 2)[0] == 7
    back = byml.read(data)
    def same(a, b):
        if type(a) is not type(b): return False
        if isinstance(a, dict): return list(sorted(a)) == list(sorted(b)) and all(same(a[k], b[k]) for k in a)
        if isinstance(a, list): return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
        if isinstance(a, str): return a == b
        return a.t == b.t and a.raw == b.raw
    assert same(tree, back), (tree, back)
    assert back['Actors'][0]['Translate'][1].get() == -2.25 and back['Wide'].get() == -5 and back['Actors'][0]['Dynamic']['Count'].get() == -3
    assert byml.write(back) == data   # (written again: the same bytes)
    files = {'Actor/A.bgyml': data, 'Component/B.bgyml': b'x' * 13, 'C': b''}
    packed = sarc.write(files)
    assert sarc.read(packed) == files
    hashes = [struct.unpack_from('<I', packed, 0x14 + 12 + 16 * i)[0] for i in range(3)]
    assert hashes == sorted(hashes) and hashes[0] == min(sarc.name_hash(n) for n in files)



# ======================================================================================================================
# Bottles (vr/items/bottle)
# ======================================================================================================================

def check_bottle_controls():
    """Bottles in the hands: with a bottle selected a grip takes it (not the sword's grip behind the shoulder), letting go
    lets it go, the game's ZR (its bottle use) reaches the game from no trigger but the holding hand's, and the renderer is
    told the holding hand (-2 while the game still puts it away). The simple bottle: the holding hand's trigger, decided
    as it goes down, is the game's item button (a ZR tap once the game has it back) for a content the game's own use takes, else a use
    asked of the game side and nothing to the game; the full bottle (RX_VR_BOTTLE_FULL): never ZR"""
    host = read('controllers_host')
    hand = host[host.index('typedef struct {\n    int valid;'):host.index('int rx_vr_hands(HookshotVrHands *out);')]
    start = host.index('/* (OoT VR, first person, VR tab\'s Sword from the back)')
    end = host.index('\n}\n', host.index('static int vr_sword(const HookshotVrHand')) + 3
    first = host.index("/* Hookshot VR: a bottle's controls in the VR hands")   # (bottle_controls.inc, put in place by read)
    controls = host[first:host.index('\n}\n', host.index('static int vr_bottle(const HookshotVrHand')) + 3]
    assert 'vr_bottle(v.hand, bottle_on, free, raw, now)' in host and 'rx_option(970, 0)' in host, \
        'the pad read runs the bottle controls with the VR tab row (option 970)'
    compile_run(r"""
    #include <stdint.h>
    #include <string.h>
    #include <math.h>
    #include <assert.h>
    enum { HOS_PAD_SOUTH = 1, HOS_PAD_WEST = 3, HOS_PAD_LTRIGGER = 8, HOS_PAD_RTRIGGER = 9 };
    static void hos_log(const char *f, ...) { (void)f; }
    static int told = -100, away = 0, claims = 0;
    static int rx_option(int id, int otherwise) { (void)id; return otherwise; }
    """ + hand + host[start:end] + controls + r"""
    int rx_vr_head(VrSwordHead *h) { memset(h, 0, sizeof *h); return 0; }
    void rx_vr_sword_hand(int hand) { (void)hand; }
    int hos_vr_sword_drawn(void) { return 0; }
    void rx_vr_bottle_hand(int hand) { told = hand; }
    int hos_vr_bottle_selected(void) { return 1; }
    int hos_vr_bottle_away(void) { return away; }
    static int cork = -1;
    int hos_vr_bottle_at_cork(void) { return cork; }
    static int full = 0, to_game = 0, asked = 0;
    int hos_vr_bottle_full(void) { return full; }
    int hos_vr_bottle_to_game(void) { return to_game; }
    void hos_vr_bottle_ask_use(void) { asked++; }
    static int given = 0, ready = 0;
    void hos_vr_bottle_ask_game_use(void) { given++; }
    int hos_vr_bottle_game_ready(void) { return ready; }
    int hos_vr_bottle_probe_holder(void) { return -1; }
    int hos_vr_bottle_probe_trigger(void) { return 0; }
    int hos_vr_bomb_in_reach(int hand) { (void)hand; return 0; }
    int hos_vr_put_away_held(void) { return 1; }   /* (the sword's and stick's put-away: not this check's) */
    void hos_vr_put_away_cancel(void) {}
    int hos_vr_sword_draw(void) { return 1; }      /* (Sword hits: Blade, the sword's draw: not this check's) */
    void hos_vr_sword_trigger(int held) { (void)held; }   /* (blade_hits.inc: the spin's charge) */
    void rx_vr_rumble(int hand, float a, float t) { (void)hand, (void)a, (void)t; }
    static int64_t t = 1000000000LL;
    static HookshotVrHand hands[2];
    static int on = 1;
    static uint64_t step(uint64_t raw) {
        t += 11000000LL;
        vr_sword_shoulder[0] = claims;
        vr_bottle(hands, on, 1, &raw, t);
        return raw;
    }
    #define BIT(raw, b) ((int)((raw) >> (b) & 1))
    int main(void) {
        const uint64_t real_zr = 1ULL << HOS_PAD_RTRIGGER;
        hands[0].valid = hands[1].valid = 1;
        assert(!BIT(step(real_zr), HOS_PAD_RTRIGGER) && told == -1);   /* selected: ZR (the game's bottle use) kept out */
        on = 0; assert(BIT(step(real_zr), HOS_PAD_RTRIGGER));          /* not selected: the trigger is the game's */
        hands[1].squeeze = 1; step(0); assert(hos_vr_bottle_holder() == -1);   /* ... and a grip takes nothing */
        hands[1].squeeze = 0; step(0); on = 1;
        claims = 1; hands[0].squeeze = 1; step(0); assert(hos_vr_bottle_holder() == -1);   /* the sword's grip behind the shoulder */
        hands[0].squeeze = 0; step(0); claims = 0;
        hands[0].squeeze = 1; step(0); assert(hos_vr_bottle_holder() == 0 && told == 0);   /* a grip takes it */
        hands[0].squeeze = .4f; step(0); assert(hos_vr_bottle_holder() == 0);           /* (held through a looser grip) */
        hands[1].squeeze = 1; step(0); assert(hos_vr_bottle_holder() == 0);             /* one hand at a time */
        hands[1].squeeze = 0;
        const uint64_t zl = 1ULL << HOS_PAD_LTRIGGER;
        assert(!BIT(step(zl), HOS_PAD_LTRIGGER));               /* the holding (left) hand's trigger pulls the cork, not ZL */
        cork = 1; assert(!BIT(step(real_zr), HOS_PAD_RTRIGGER)); cork = -1;   /* the other hand at the cork: the cork's */
        hands[0].squeeze = 0; step(0); hands[1].squeeze = 1; step(0); assert(hos_vr_bottle_holder() == 1);
        assert(BIT(step(zl), HOS_PAD_LTRIGGER));                 /* held in the right: the left trigger is ZL again */
        hands[1].squeeze = 0; step(0); hands[0].squeeze = 1; step(0); assert(hos_vr_bottle_holder() == 0);
        /* the simple bottle, held in the left hand: its trigger is the game's item button (ZR) for the game's own use */
        to_game = 1; hands[0].trigger = 1;
        uint64_t r = step(zl); assert(!BIT(r, HOS_PAD_RTRIGGER) && !BIT(r, HOS_PAD_LTRIGGER) && asked == 0 && given == 1);
        assert(!BIT(step(0), HOS_PAD_RTRIGGER));                        /* (not before the game has the bottle back) */
        ready = 1; r = step(zl); assert(BIT(r, HOS_PAD_RTRIGGER) && !BIT(r, HOS_PAD_LTRIGGER));   /* then its ZR */
        to_game = 0; assert(BIT(step(0), HOS_PAD_RTRIGGER) && given == 1);   /* (a tap of kVrBottleZrNs...) */
        for (int k = 0; k < 12; k++) step(0);
        assert(!BIT(step(0), HOS_PAD_RTRIGGER) && given == 1);          /* (...not held on with the trigger: one use) */
        hands[0].trigger = 0; step(0); ready = 0;
        to_game = 1; hands[0].trigger = 1; step(0); hands[0].trigger = 0; step(0);   /* a quick press, handed back late */
        ready = 1; assert(BIT(step(0), HOS_PAD_RTRIGGER) && given == 2);   /* (still reaches the game) */
        for (int k = 0; k < 12; k++) step(0);
        assert(!BIT(step(0), HOS_PAD_RTRIGGER)); ready = 0; to_game = 0;
        hands[0].trigger = 1; r = step(0); assert(!BIT(r, HOS_PAD_RTRIGGER) && asked == 1);   /* a drink: by calls */
        to_game = 1; assert(!BIT(step(0), HOS_PAD_RTRIGGER) && asked == 1);   /* (decided as it went down: still nothing) */
        hands[0].trigger = 0; step(0); assert(asked == 1);
        hands[1].trigger = 1; assert(!BIT(step(real_zr), HOS_PAD_RTRIGGER)); hands[1].trigger = 0;   /* the other hand's: not it */
        full = 1; hands[0].trigger = 1; assert(!BIT(step(0), HOS_PAD_RTRIGGER) && asked == 1); hands[0].trigger = 0; step(0); full = 0;
        to_game = 0;
        away = 1; hands[0].squeeze = 0; step(0); assert(hos_vr_bottle_holder() == -1 && told == -2);   /* let go, being put away */
        away = 0; step(0); assert(told == -1);
        return 0;
    }
    """)


def check_bottle_keep_drawn():
    """the game hook that keeps a held bottle out: in the stow itself, after the handle's generation is loaded, it turns
    away only a stow of Link's handle 1 while a VR hand holds a bottle there and no event plays (w8 = -1, the stow's own
    early return); the Deku Stick, Link's other handles, other actors' stows and the put-away after a let-go all go
    through, because the holding mark is cleared the update a hand lets go, before the put-away's stow"""
    hooks = (REPO / 'vr/vr_game_hooks.py').read_text(encoding='utf-8')
    assert '(0xe34cb0, 0xb9401328, "{ extern void hos_vr_bottle_keep_drawn(Ctx *c); hos_vr_bottle_keep_drawn(c); }")' in hooks, \
        'the hook sits after ldr w8, [x25, #0x10] in f_80e54c70 (main + 0xe34cb0)'
    game = (REPO / 'vr/items/bottle/bottle_host.c').read_text(encoding='utf-8')
    rule = game[game.index('static int vr_bottle_keep_drawn('):game.index('\n}\n', game.index('static int vr_bottle_keep_drawn(')) + 3]
    handler = game[game.index('void hos_vr_bottle_keep_drawn(Ctx *c) {'):game.index('\n}\n', game.index('void hos_vr_bottle_keep_drawn(Ctx *c) {')) + 3]
    update = game[game.index('static void vr_bottle_link_update('):game.index('\n}\n', game.index('static void vr_bottle_link_update(')) + 3]
    mark = update.index('__atomic_store_n(&vr_bottle_user_now, want && !given ? user : 0')
    assert mark < update.index('vr_bottle_put_away(c, user)'), 'the holding mark is cleared before the put-away stows'
    give = game[game.index('static int vr_bottle_game_use_step('):game.index('\n}\n', game.index('static int vr_bottle_game_use_step(')) + 3]
    assert give.index('__atomic_store_n(&vr_bottle_user_now, 0') < give.index('vr_bottle_put_away(c, user)'), \
        'handed to the game\'s own use: the mark is cleared before its stow'
    assert "strncmp(name, \"Weapon_Bottle\", 13)" in game, 'only a bottle in handle 1 counts (never the Deku Stick)'
    compile_run(r"""
    #include <stdint.h>
    #include <assert.h>
    typedef struct { uint64_t x[32]; } Ctx;
    #define kVrBottleSlot 1
    static void hos_log(const char *f, ...) { (void)f; }
    static uint64_t vr_bottle_user_now;
    static int event;
    int hos_vr_event_playing(void) { return event; }
    #define __atomic_load_n(p, o) (*(p))
    """ + rule + handler + r"""
    static int stow(uint64_t user, uint32_t slot) {   /* (1: the stow goes on, 0: turned away) */
        Ctx c = {{0}};
        c.x[0] = user, c.x[1] = slot, c.x[8] = 7;   /* (a live handle's generation) */
        hos_vr_bottle_keep_drawn(&c);
        return (uint32_t)c.x[8] != 0xffffffffu;
    }
    int main(void) {
        const uint64_t link = 0x1000, other = 0x2000;
        assert(stow(link, 1) && stow(link, 0));             /* no bottle held: every stow goes on */
        vr_bottle_user_now = link;                           /* a VR hand holds a bottle in Link's handle 1 */
        assert(!stow(link, 1));                              /* ... its stow is turned away */
        assert(stow(link, 0) && stow(link, 3) && stow(link, 4));   /* Link's sword, bow, shield: as the game has them */
        assert(stow(other, 1));                              /* another actor's handle 1 */
        event = 1; assert(stow(link, 1)); event = 0;         /* an event's own stow goes through */
        vr_bottle_user_now = 0;                              /* let go: the mark cleared */
        assert(stow(link, 1));                               /* ... so the put-away's stow goes through */
        return 0;
    }
    """)


def murmur3(text):
    """MurmurHash3 x86 32, seed 0: the hash the game names its save flags by (host/oot_state_tap.c state_murmur)"""
    d, h, c1, c2 = text.encode(), 0, 0xcc9e2d51, 0x1b873593
    rotl = lambda v, r: ((v << r) | (v >> (32 - r))) & 0xffffffff
    n = len(d) // 4 * 4
    for i in range(0, n, 4):
        k = rotl(int.from_bytes(d[i:i + 4], 'little') * c1 & 0xffffffff, 15) * c2 & 0xffffffff
        h = (rotl(h ^ k, 13) * 5 + 0xe6546b64) & 0xffffffff
    if len(d) > n:
        h ^= rotl(int.from_bytes(d[n:], 'little') * c1 & 0xffffffff, 15) * c2 & 0xffffffff
    h ^= len(d)
    for shift, mul in ((16, 0x85ebca6b), (13, 0xc2b2ae35)):
        h = (h ^ (h >> shift)) * mul & 0xffffffff
    return h ^ (h >> 16)


def check_wrist():
    """the wrist check HUD's game side: each dungeon's DungeonInfo flags are the hashes of their names; the values are sent
    whole, Life falling is a hurt (3 s in a fight), and any of the sword, shield or bow out, a VR hand holding the sword or a
    stick, a lock-on, an event or the radial menu keeps the panel away; a boss room's banc counts as its dungeon"""
    host = read('wrist_host')
    rows = re.findall(r'\{"(\w+)", 0x(\w+)u, 0x(\w+)u, 0x(\w+)u, 0x(\w+)u, [\w |]+\},', host)
    assert len(rows) == 13, len(rows)
    for banc, *hashes in rows:
        for field, h in zip(('SmallKeyNum', 'IsGetBossKey', 'IsGetMap', 'IsGetCompass'), hashes):
            assert murmur3(f'DungeonInfo.{banc}.{field}') == int(h, 16), (banc, field)
    for name, macro in (('PlayerStatus.Life', 'kVrWristLife'), ('PlayerStatus.MaxLife', 'kVrWristMaxLife'),
                        ('PlayerStatus.MP', 'kVrWristMp'), ('PlayerStatus.MPMax', 'kVrWristMpMax'),
                        ('PlayerStatus.CurrentRupee', 'kVrWristRupees')):
        assert re.search(r'#define %s 0x%08xu\b' % (macro, murmur3(name)), host), name
    assert murmur3('Sequence_CurrentBanc') == 0x1d6189da and 'oot_gamedata_string(c, 0x1d6189dau, banc' in host
    sys.path.insert(0, str(GENERATE))
    try:
        import wrist_maps   # (the renderer's map table names a banc as the game side does)
    finally:
        sys.path.pop(0)
    assert wrist_maps.fnv1a('InsideTheDekuTree') == 0xf4d922e7
    # (called with the radial's state, every game frame the headset is on; its bridge's name is the renderer's export)
    view = read('view_host')
    assert view.index('oot_vr_radial_frame(c);') < view.index('oot_vr_wrist_frame(c);') < view.index('oot_vr_player_camera(c, view)')
    assert 'GetProcAddress(vk_module, "hookshot_vr_wrist")' in (REPO / 'vr/headset/bridges_host.c').read_text()
    api = (REPO / 'vr/headset/host_api.inc').read_text()
    assert 'extern "C" void hookshot_vr_wrist(const int32_t* data, int n) {\n    vr::wrist_set(data, n);' in api
    assert 'GetProcAddress(vk_module, "hookshot_vr_wrist_flags")' in (REPO / 'vr/headset/bridges_host.c').read_text()
    assert 'extern "C" int hookshot_vr_wrist_flags(uint32_t* hashes, int max) { return vr::wrist_flags(hashes, max); }' in api
    main = (REPO / 'host/hos_main.c').read_text()
    assert main.index('"../vr/menu/radial_host.c"') < main.index('"../vr/menu/wrist_host.c"') < main.index('"../vr/camera/view_host.c"')
    start = host.index('#define kVrWristLife')
    end = host.index('/* Every game frame with the headset on')
    compile_run(r'''
    #include <stdint.h>
    #include <string.h>
    #include <assert.h>
    ''' + host[start:end] + r'''
    int main(void) {
        // (the dungeon by the map's banc: a boss room's own name counts; elsewhere none)
        assert(vr_wrist_dungeon_of("InsideTheDekuTree") == 0 && vr_wrist_dungeon_of("InsideTheDekuTree_Boss") == 0);
        assert(vr_wrist_dungeon_of("ForestTemple") == 3 && vr_wrist_dungeon_of("KokiriForest") == -1 && vr_wrist_dungeon_of("") == -1);
        assert(vr_wrist_dungeons[0].has == kVrWristHasMaps && vr_wrist_dungeons[3].has == kVrWristHasAll);   // (no keys in the Deku Tree)
        // (the block bits)
        assert(vr_wrist_block(0, -1, -1, 0, 0, 0, 10, 0) == 0);
        assert(vr_wrist_block(1, -1, -1, 0, 0, 0, 10, 0) == kVrWristSword && vr_wrist_block(2, -1, -1, 0, 0, 0, 10, 0) == kVrWristShield);
        assert(vr_wrist_block(4, -1, -1, 0, 0, 0, 10, 0) == kVrWristBow && vr_wrist_block(0, 1, -1, 0, 0, 0, 10, 0) == kVrWristHeld);
        assert(vr_wrist_block(0, -1, 0, 0, 0, 0, 10, 0) == kVrWristHeld && vr_wrist_block(0, -1, -1, 1, 0, 0, 10, 0) == kVrWristLocked);
        assert(vr_wrist_block(0, -1, -1, -1, 0, 0, 10, 0) == 0);   // (a lock-on not known: not a fight)
        assert(vr_wrist_block(0, -1, -1, 0, 1, 0, 10, 0) == kVrWristEvent && vr_wrist_block(0, -1, -1, 0, 0, 2, 10, 0) == kVrWristRadial);
        assert(vr_wrist_block(0, -1, -1, 0, 0, 0, 2000000000LL, 1) == kVrWristHurt);           // (hurt 2 s ago)
        assert(vr_wrist_block(0, -1, -1, 0, 0, 0, 4000000000LL, 1) == 0);                      // (4 s ago: over)
        // (the values: no save, then a save, then a hurt)
        VrWrist w;
        memset(&w, 0, sizeof w);
        const int64_t none[5] = {0, 0, 0, 0, 0}, ok[5] = {12, 12, 0, 0, 45}, hurt[5] = {10, 12, 0, 0, 45};
        vr_wrist_values(&w, 1, none, 0, 200, -1, 0, 0);
        assert(w.v[kVrWristShown] == 0);
        vr_wrist_values(&w, 2, ok, 1, 200, -1, 0, 0);
        assert(w.v[kVrWristShown] == 1 && w.v[kVrWristLifeAt] == 12 && w.v[kVrWristMaxLifeAt] == 12 && w.v[kVrWristRupeesAt] == 45);
        assert(w.v[kVrWristWalletAt] == 200 && w.v[kVrWristKeysAt] == -1 && w.v[kVrWristDungeonAt] == 0 && !w.hurt_at);
        vr_wrist_values(&w, 3, hurt, 1, 200, 3, 2, kVrWristMap);   // (the Forest Temple, 2 keys, its map)
        assert(w.v[kVrWristLifeAt] == 10 && w.hurt_at == 3 && w.v[kVrWristKeysAt] == 2);
        assert(w.v[kVrWristDungeonAt] == (kVrWristMap | kVrWristHasAll << 4));
        const int64_t healed[5] = {12, 12, 24, 48, 45};
        vr_wrist_values(&w, 4, healed, 1, 500, -1, 0, 0);
        assert(w.hurt_at == 3 && w.v[kVrWristMpAt] == 24 && w.v[kVrWristMpMaxAt] == 48 && w.v[kVrWristWalletAt] == 500);
        assert(kVrWristN == 17 && kVrWristBancAt == 10 && kVrWristFlagsAt == 16);   // (vr/menu/wrist_draw.inc wrist_set's order)
        assert(vr_wrist_banc_hash("InsideTheDekuTree") == 0xf4d922e7u && vr_wrist_banc_hash("") == 0x811c9dc5u);   // (FNV-1a, as wrist_maps.py)
        return 0;
    }
    ''')


def check_wrist_draw():
    """the wrist check HUD in the headset: shown after the back of a wrist has faced the eyes for half a second (near, in
    view, the hands still), kept with looser limits, hidden 0.2 s after the pose is lost and at once when blocked (a fight);
    drawn from the game's values: hearts by quarters, a magic bar only once owned, the rupees, a dungeon's keys; Full's
    minimap: the area's picture (its later one once its flag is set), a dungeon floor's by Link's height once its map is
    found, else none (the Simple layout); its frame sized to the map, Link's arrow at his place, turned his way"""
    draw = read('wrist_draw')
    start = draw.index('// ---- what it shows, and the gesture')
    end = draw.index("// ---- the headset's part ----")
    font = REPO / 'third_party/fonts/libertinus/LibertinusSerif-Semibold.ttf'
    assert font.exists()
    loop = read('frame_loop')
    assert loop.index('vignette_layer(vignette_quad)') < loop.index('wrist_layer(wrist_quad, head)') < loop.index('radial_layer(radial_quad')
    assert 'opts.vr_wrist_hud == 0' in draw and 'panel.open.load()' in draw and 'flat_screen.shown' in draw
    # (both views: no first-person gate, the wrist calibration in both (third person without it: the left wrist missed,
    # headset 22 Nov); three sizes, Large the default)
    look = function_body(draw, 'WristLook wrist_look(const XrPosef& head, int h) {')
    assert 'opts.vr_mode' not in look and 'opts.vr_wrist_l_roll' in look and 'opts.vr_mode != 1' not in draw
    assert 'int vr_wrist_size = 2;' in (REPO / 'vr/menu/vr_options.inc').read_text()
    assert 'kWristWidths[std::clamp(opts.vr_wrist_size, 0, 2)]' in draw
    assert '"Obj_Rupee_Green", "Obj_Rupee_Blue", "Obj_Rupee_Red", "Item_Dungeon_SmallKey"' in \
        (REPO / 'vr/tools/generate/icons.py').read_text()
    compile_run(r'''
    #include <algorithm>
    #include <cassert>
    #include <chrono>
    #include <cmath>
    #include <cstdint>
    #include <cstdio>
    #include <cstring>
    #include <mutex>
    #include <unordered_map>
    #include <vector>
    #define STBTT_STATIC
    #define STB_TRUETYPE_IMPLEMENTATION
    #include "''' + str(REPO / 'third_party/imgui/imstb_truetype.h') + r'''"
    namespace art {   // (no game icons: the drawn ones; four map pictures: an area's two, a dungeon's two floors)
    constexpr int icon_size = 2;
    struct Icon { const char* name; const unsigned char* rgba; };
    inline constexpr Icon icons[1] = {{"none", nullptr}};
    struct WristMap { unsigned banc, flag; float y0, y1, x0, z0, x1, z1; int w, h; const char* zst; unsigned size; };
    inline constexpr int wrist_map_count = 4;
    inline constexpr WristMap wrist_maps[4] = {{0xa, 0, -1e4f, 1e4f, -10, -10, 10, 10, 40, 40, "", 0},
                                               {0xa, 0x5eed, -1e4f, 1e4f, -10, -10, 10, 10, 40, 40, "", 0},
                                               {0xd, 0, -1e4f, 10, 0, 0, 20, 10, 40, 20, "", 0},
                                               {0xd, 0, 10, 1e4f, 0, 0, 20, 10, 40, 20, "", 0}};
    }
    static std::vector<unsigned char> ttf;
    ''' + draw[start:end] + r'''
    const unsigned char* wrist_font_data() { return ttf.data(); }
    static WristLook facing(float dist) {   // (the eyes at the origin looking down -z; the wrist below and ahead, its inside to the eyes)
        WristLook w;
        w.tracked = true, w.forward[2] = -1;
        w.at[1] = -dist * .342f, w.at[2] = -dist * .94f;   // (20 degrees below the eyes' forward)
        for (int k = 0; k < 3; k++) w.face[k] = -w.at[k] / dist;
        return w;
    }
    static float run(WristGesture& g, const WristLook& w, bool blocked, float secs) {
        float a = 0;
        for (float t = 0; t < secs - 1e-4f; t += 1 / 90.0f) a = wrist_gesture(g, w, blocked, 1 / 90.0f);
        return a;
    }
    static std::vector<uint8_t> px(kWristW * kWristH * 4);
    static const uint8_t* at(float x, float y) { return &px[((size_t)(y * kWristS) * kWristW + (size_t)(x * kWristS)) * 4]; }
    static bool reddish(const uint8_t* p) { return p[3] > 200 && p[0] > 150 && p[1] < 110; }
    int main() {
        FILE* f = fopen("''' + str(font) + r'''", "rb");
        assert(f);
        for (int ch; (ch = fgetc(f)) != EOF;) ttf.push_back((unsigned char)ch);
        fclose(f);
        // ---- the gesture
        WristGesture g;
        assert(run(g, facing(.3f), false, .4f) == 0 && !g.on);       // (not yet: half a second)
        assert(run(g, facing(.3f), false, .15f) > 0 && g.on);        // (shown, fading in)
        assert(run(g, facing(.3f), false, .2f) == 1);
        assert(run(g, facing(.43f), false, .5f) == 1);              // (a little further: kept, 45 cm to stay)
        WristLook away = facing(.3f);
        away.face[0] = 1, away.face[1] = away.face[2] = 0;          // (the wrist turned 90 degrees)
        run(g, away, false, .1f);
        assert(g.on);                                               // (0.1 s lost: still on)
        run(g, away, false, .15f);
        assert(!g.on && g.alpha > 0 && g.alpha < 1);                 // (0.25 s: off, fading out)
        assert(run(g, away, false, .3f) == 0);
        WristGesture g2;
        assert(run(g2, facing(.43f), false, 1) == 0);               // (43 cm: too far to begin)
        assert(run(g2, facing(.5f), false, 1) == 0);
        WristLook fast = facing(.3f);
        fast.speed[1] = 1.2f;                                       // (the right hand swinging)
        assert(run(g2, fast, false, 1) == 0);
        WristLook spun = facing(.3f);
        spun.spin = 5;
        assert(run(g2, spun, false, 1) == 0);
        WristLook aside = facing(.3f);
        aside.forward[0] = -1, aside.forward[2] = 0;                // (looking away to the left)
        assert(run(g2, aside, false, 1) == 0);
        WristLook untracked = facing(.3f);
        untracked.tracked = false;
        assert(run(g2, untracked, false, 1) == 0);
        assert(run(g2, facing(.3f), true, 1) == 0);                 // (blocked: a fight)
        assert(run(g2, facing(.3f), false, .7f) == 1 && g2.on);
        wrist_gesture(g2, facing(.3f), true, 1 / 90.0f);
        assert(!g2.on);                                             // (blocked while shown: off at once)
        // ---- the values from the game side
        const int32_t sent[10] = {1, 11, 12, 0, 0, 87, 200, -1, 0, 0};
        wrist_set(sent, 10);
        WristState st = wrist_in;
        assert(st.shown && st.life == 11 && st.max_life == 12 && st.mp_max == 0 && st.rupees == 87 && st.keys == -1 && !st.block);
        const int32_t none[10] = {0, 0, 0, 0, 0, 0, 0, -1, 0, 3};
        wrist_set(none, 10);
        assert(!wrist_in.shown && wrist_in.block == 3);
        // ---- the panel: Life 11 of 12 (three hearts, the last three quarters), no magic
        wrist_px = px.data();
        wrist_compose(st);
        const float x0 = 7 + 26, y0 = 7 + 22;
        assert(reddish(at(x0 + 15, y0 + 12)) && reddish(at(x0 + 34 + 15, y0 + 12)));   // (two full hearts)
        assert(reddish(at(x0 + 68 + 15, y0 + 12)) && !reddish(at(x0 + 68 + 26, y0 + 10)));   // (the third: 3 of 4)
        assert(!reddish(at(x0 + 102 + 15, y0 + 12)) && at(x0 + 102 + 15, y0 + 12)[3] == 255);   // (no fourth: the leather)
        assert(at(1, 1)[3] == 0 && at(200, 2)[3] == 255);           // (the corner outside, the rim)
        const float simple = wrist_height(st);
        assert(simple == 2 * (7 + 22) + 27 + 16 + 30);
        bool text = false;   // (the rupees' digits: bright, beside the rupee)
        for (float x = x0 + 30; x < x0 + 70; x += .5f)
            for (float y = y0 + 27 + 16; y < y0 + 27 + 16 + 30; y += .5f) text |= at(x, y)[0] > 220 && at(x, y)[1] > 200;
        assert(text);
        // (magic owned: the bar under the hearts, 24 of 48 half full)
        st.mp = 24, st.mp_max = 48;
        wrist_compose(st);
        assert(wrist_height(st) == simple + 32);
        const float by = y0 + 27 + 16 + 8;
        const uint8_t* green = at(x0 + 40, by);
        const uint8_t* empty = at(x0 + 150, by);
        assert(green[1] > 120 && green[1] > green[0] + 30 && empty[1] < 60);
        st.mp = 390, st.mp_max = 480;   // (the game's own numbers, single magic: a bar 200 wide)
        wrist_compose(st);
        assert(at(x0 + 150, by)[1] > 120 && at(x0 + 260, by)[1] < 60);
        st.mp = st.mp_max = 960;        // (double: 340 wide)
        wrist_compose(st);
        assert(at(x0 + 300, by)[1] > 120);
        // (twenty hearts: two rows)
        st.max_life = 80, st.life = 80;
        assert(wrist_heart_rows(st) == 2 && wrist_height(st) == simple + 32 + 31);
        wrist_compose(st);
        assert(reddish(at(x0 + 15, y0 + 31 + 12)) && reddish(at(x0 + 9 * 34 + 15, y0 + 31 + 12)));
        // (a dungeon: its keys and count at the right; the Deku Tree's map and compass only, dimmed until found)
        st.max_life = st.life = 12, st.mp_max = st.mp = 0;
        wrist_compose(st);
        std::vector<uint8_t> plain = px;
        st.keys = 2, st.dungeon = kWristMap | 15 << 4;
        wrist_compose(st);
        int changed = 0;
        for (size_t i = 0; i < px.size(); i += 4) changed += px[i] != plain[i] && (i / 4 % kWristW) > kWristW / 2;
        assert(changed > 200);
        st.keys = 0, st.dungeon = 6 << 4;   // (the Deku Tree: no keys, its map and compass not found)
        wrist_compose(st);
        std::vector<uint8_t> deku = px;
        st.dungeon = 6 << 4 | kWristMap | kWristCompass;   // (both found: brighter)
        wrist_compose(st);
        int lit = 0;
        for (size_t i = 0; i < px.size(); i += 4) lit += px[i] > deku[i] + 40;
        assert(lit > 100);
        // ---- Full's minimap: the picture by the map's name, its later picture once its flag is set, a dungeon's floor by
        // Link's height once its map is found; none (the Simple panel) otherwise
        uint32_t asked[8];
        assert(wrist_flags(asked, 8) == 1 && asked[0] == 0x5eed);
        WristState full = st;
        full.keys = -1, full.dungeon = 0, full.mode = 2, full.banc = 0xa, full.flags = 0;
        WristLink at_centre;
        at_centre.ok = true;
        assert(wrist_map_pick(full, at_centre) == 0);
        full.flags = 1;
        assert(wrist_map_pick(full, at_centre) == 1);
        full.mode = 1;
        assert(wrist_map_pick(full, at_centre) == -1);   // (Simple)
        full.mode = 2, full.banc = 0xb;
        assert(wrist_map_pick(full, at_centre) == -1);   // (a map with no picture: a house, a grotto)
        WristLink unknown;
        full.banc = 0xa;
        assert(wrist_map_pick(full, unknown) == -1);
        WristState dun = full;
        dun.banc = 0xd, dun.keys = 0, dun.dungeon = 6 << 4, dun.flags = 0;
        WristLink low = at_centre, high = at_centre;
        low.y = 5, high.y = 12;
        assert(wrist_map_pick(dun, low) == -1);             // (its map not found yet)
        dun.dungeon |= kWristMap;
        assert(wrist_map_pick(dun, low) == 2 && wrist_map_pick(dun, high) == 3);
        // (the frame sized to the map: 40 x 40 fits 220 x 220, under the rows; the picture inside it; the arrow on it)
        full.flags = 0, full.map = 0;
        assert(wrist_minimap(full) == 220 + 2 * (10 + 3) && wrist_height(full) == simple + 16 + 246);
        std::vector<uint8_t> grass(40 * 40 * 4);
        for (size_t i = 0; i < grass.size(); i += 4) grass[i + 1] = 200, grass[i + 3] = 255;
        wrist_map_rgba = grass.data();
        wrist_compose(full);
        const float mx = wrist_map_at.x, my = wrist_map_at.y;
        assert(std::abs(wrist_map_at.w - 220) < .01f && std::abs(wrist_map_at.h - 220) < .01f && std::abs(mx - 90) < .01f);
        assert(at(mx + 40, my + 40)[1] == 200 && at(mx + 40, my + 40)[0] == 0);   // (the picture)
        assert(at(mx - 8, my + 40)[3] == 255 && at(mx - 8, my + 40)[1] < 150);     // (its margin: the dark well)
        auto gold = [](const uint8_t* p) { return p[0] > 180 && p[1] > 150 && p[2] < 120; };
        WristLink south = at_centre;   // (at the picture's middle, facing +z: down the picture)
        assert(wrist_arrow(0, south, 1));
        assert(gold(at(mx + 110, my + 110 + 3)) && !gold(at(mx + 110, my + 110 - 9)) && !gold(at(mx + 110, my + 110 + 11)));
        wrist_compose(full);
        WristLink east = at_centre;    // (facing +x: to the right)
        east.facing = 3.14159265f / 2, east.x = 5;   // (a quarter of the way in from the right edge)
        assert(wrist_arrow(0, east, 1));
        assert(gold(at(mx + 165 + 3, my + 110)) && !gold(at(mx + 165 - 9, my + 110)) && !gold(at(mx + 165 + 11, my + 110)));
        WristLink away_off = at_centre;
        away_off.x = 50;   // (past the picture's right edge: kept on it)
        wrist_compose(full);
        assert(wrist_arrow(0, away_off, 1) && gold(at(mx + 220, my + 110)));
        full.map = -1;
        assert(wrist_minimap(full) == 0 && wrist_height(full) == simple);   // (no map: the Simple layout)
        wrist_map_rgba = nullptr;
        st.shown = 0;   // (no save: nothing)
        wrist_compose(st);
        for (size_t i = 3; i < px.size(); i += 4) assert(px[i] == 0);
        return 0;
    }
    ''', cpp=True)


def check_bottle_radial():
    """the five bottles are in the VR radial's item menu (items 14-17 and 50, both ages, owned from the save), each shown
    with its content's own icon: the entry is sent as its index + 256 x (content + 2) and read back the same way; every
    bottle icon is made from your game"""
    items = (REPO / 'vr/menu/radial_items.h').read_text(encoding='utf-8')
    icons = (REPO / 'vr/tools/generate/icons.py').read_text(encoding='utf-8')
    draw = (REPO / 'vr/menu/radial_draw.inc').read_text(encoding='utf-8')
    host = (REPO / 'vr/menu/radial_host.c').read_text(encoding='utf-8')
    for n, c in zip((14, 15, 16, 17, 50), 'ABCDE'):
        assert '{%d, "bottle:%s", "Item_Bottle", 0},' % (n, c) in items, ('bottle', c)
    names = re.findall(r'"(Item_Bottle[A-Za-z_]*)"', items[items.index('vr_radial_bottle_icons[13]'):])
    assert len(names) == 13 and all('"%s"' % n in icons for n in names), 'every content\'s icon is generated'
    assert 'i + 256 * (oot_vr_radial_bottle[bottle] + 2)' in host and 'd[4 + k] & 0xff' in draw and '(d[4 + k] >> 8) - 2' in draw
    assert 'vr_bottle_owned(c, bottle)' in host, 'a bottle is owned from the save, not the pouch count'
    compile_run(r"""
    #include <assert.h>
    #include <string.h>
    """ + items + r"""
    int main(void) {
        assert(VR_RADIAL_ITEMS < 256);   /* (the index fits the encoding's low byte) */
        for (int i = 0; i < VR_RADIAL_ITEMS; i++) {
            if (strncmp(vr_radial_items[i].own, "bottle:", 7)) continue;
            for (int content = -1; content <= 11; content++) {
                const int v = i + 256 * (content + 2);
                assert((v & 0xff) == i && (v >> 8) - 2 == content);
                assert(!strncmp(vr_radial_bottle_icons[content + 1], "Item_Bottle", 11));
            }
        }
        /* (code index 8 is a bug, as the save probe read: headless 18 Nov) */
        assert(!strcmp(vr_radial_bottle_icons[8 + 1], "Item_Bottle_Bug") && !strcmp(vr_radial_bottle_icons[0], "Item_Bottle"));
        return 0;
    }
    """)
    if ARGS.romfs:
        for n in names:
            assert (Path(ARGS.romfs) / 'UI/Tex/Icon' / (n + '.bntx.zs')).exists(), n


def check_radial_art():
    """the radial wheel looks as the canvas board: the user's leather (the frame art) in a copper trim, every icon it can show has
    its name under it, and the picture is composed on a thread of its own, never in the headset's frame"""
    draw = (REPO / 'vr/menu/radial_draw.inc').read_text(encoding='utf-8')
    items = (REPO / 'vr/menu/radial_items.h').read_text(encoding='utf-8')
    labels = draw[draw.index('const char* radial_label('):draw.index('void radial_compose_base(')]
    shown = set(re.findall(r'\{\d+, "[^"]+", "(\w+)", \d\}', items)) | set(re.findall(r'\{"(\w+)", \d, \d\}', items))
    shown |= set(re.findall(r'"(Item_Bottle\w*)"', items[items.index('vr_radial_bottle_icons[13]'):]))
    assert len(shown) > 40, len(shown)
    for icon in sorted(shown):
        assert '{"%s", "' % icon in labels, f'the radial shows {icon} with no name'
    assert 'panel_frame_pixels(&radial_art.w' in draw, 'the leather is the frame art'
    layer = draw[draw.index('bool radial_layer('):]
    assert 'radial_ask(st)' in layer and 'radial_compose(' not in layer, 'composed off the headset thread'
    assert 'radial_compose(st);' in draw[draw.index('void radial_ask('):draw.index('bool radial_take(')]


def check_bottle_use():
    """stage 2, the open bottle: a filled bottle comes out corked and the renderer is told when it is opened (the holding hand's
    trigger, or the other hand at the cork); only an open bottle is drunk (at the headset, tipped) or tipped out; a fairy
    heals with the game's own fairy amount when uncorked; a bug, a fish or Blue Fire is never emptied by tipping until its
    animal or flame can be let out; put away unused, it is corked again; the glass's mouth is where the model has it"""
    game = (REPO / 'vr/items/bottle/bottle_host.c').read_text(encoding='utf-8')
    use = game[game.index('static void vr_bottle_use_update('):game.index('\n}\n', game.index('static void vr_bottle_use_update(')) + 3]
    assert 'u->corked = u->content >= 0' in use, 'a filled bottle comes out corked'
    assert use.index('if (u->corked) return;') < use.index('const int drink ='), 'nothing is drunk or tipped out while corked'
    assert 'e2 < kVrDrinkReach * kVrDrinkReach && up < kVrDrinkTip' in use and 'now - u->at_mouth_since >= kVrDrinkNs' in use
    assert 'vr_bottle_call(c, 0x82931ef0ULL, heal' in use, 'a fairy heals by the game\'s own fairy amount'
    held = use[use.index('const int held_in ='):]
    assert 'vr_bottle_set_content' not in held[held.index('else if (now - u->tipped_since >= kVrTipNs && held_in'):], \
        'a bug, fish or Blue Fire is kept until it can be let out'
    assert 'kVrMaxMilk ? kVrHalfMilk : kVrEmpty' in use, 'milk is drunk in two halves'
    update = game[game.index('static void vr_bottle_link_update('):]
    assert 'vr_bottle_use.corked = 1;   /* (put away unused: corked again) */' in update, 'put away unused, it is corked again'
    assert 'rx_vr_bottle_cork(u->corked)' in use, 'the renderer is told whether the cork is in (it draws the cork popping)'
    mouth = float(re.search(r'#define kVrBottleMouth ([0-9.]+)f', game).group(1))
    header = generated('vk_vr_bottle.h').read_text()
    assert abs(mouth - float(re.search(r'bottle_mouth = ([-0-9.]+)f', header).group(1))) < 2e-3, 'the mouth is the model\'s'


def check_bottle_simple():
    """the simple bottle (the default): the empty bottle's catch, a bug, a fish, Blue Fire, Ruto's Letter and a Big Poe are
    the game's own bottle use; the potions, milk, half milk, a fairy and a Poe are used by the game's calls at once (milk
    leaves half milk, the rest empty; green and blue fill the magic as the game's OneShotPlayerHealMPMax); the full
    bottle's way (the cork, the face, tipping) only with RX_VR_BOTTLE_FULL; no draw is asked while an event plays"""
    game = (REPO / 'vr/items/bottle/bottle_host.c').read_text(encoding='utf-8')
    enum = game[game.index('enum { kVrEmpty = -1'):game.index('static const char *const kVrBottleContentNames')]
    rule = game[game.index('static int vr_bottle_used_by_game('):game.index('\n}\n', game.index('static int vr_bottle_used_by_game(')) + 3]
    calls = game[game.index('static void vr_bottle_use_by_calls('):game.index('\n}\n', game.index('static void vr_bottle_use_by_calls(')) + 3]
    for case in ('case kVrRedPotion: field = kVrHealRedPotion;', 'case kVrGreenPotion: magic = 1;',
                 'case kVrBluePotion: field = kVrHealBluePotion, magic = 1;', 'case kVrMaxMilk: field = kVrHealMilk, left = kVrHalfMilk;',
                 'case kVrHalfMilk: field = kVrHealMilk;', 'case kVrFairy: field = kVrHealRevivalFairy;', 'case kVrPoe: field = kVrHealPoeHeal;',
                 'default: return;'):
        assert case in calls, case
    assert "vr_bottle_call(c, 0x82ea0f80ULL, component" in game and 'parts + 0x358' in game, 'the magic: the game\'s own fill'
    update = game[game.index('static void vr_bottle_link_update('):game.index('\n}\n', game.index('static void vr_bottle_link_update(')) + 3]
    assert 'if (full) vr_bottle_use_update(' in update and 'else vr_bottle_simple_update(' in update, 'the full way behind its switch'
    assert 'env_int("RX_VR_BOTTLE_FULL", 0)' in game and 'SW(RX_VR_BOTTLE_FULL,' in (REPO / 'gpu/vk/switches.def').read_text()
    assert 'kVrBottleRetryNs && !hos_vr_event_playing()' in update, 'no draw asked while an event plays (it takes its own)'
    compile_run(r"""
    #include <assert.h>
    """ + enum + rule + r"""
    int main(void) {
        const int game[] = {kVrEmpty, kVrBlueFire, kVrFish, kVrLetter, kVrBigPoe, kVrBug};
        const int calls[] = {kVrRedPotion, kVrGreenPotion, kVrBluePotion, kVrMaxMilk, kVrHalfMilk, kVrFairy, kVrPoe};
        for (unsigned k = 0; k < sizeof game / sizeof game[0]; k++) assert(vr_bottle_used_by_game(game[k]));
        for (unsigned k = 0; k < sizeof calls / sizeof calls[0]; k++) assert(!vr_bottle_used_by_game(calls[k]));
        return 0;
    }
    """)


def check_bottle_held():
    """the held bottle's draws: a bottle model in Link's game hand is moved to the VR hand that holds it (all its shapes by
    one change), hidden when no VR hand does; a bottle in the world, or a palette that is not the bottle's own (the
    liquid's colour pass, tried in uniform buffer 4 first), is passed over; once the bottle is open its cork pops out of
    the neck, falls to Link's feet and is gone"""
    src = read('body')
    math = src[src.index('constexpr float kPi'):src.index('// ---- body state kept between frames')]
    held = src[src.index('// Hookshot VR: a bottle in the VR hand that holds it'):src.index('// Hookshot VR: swords, sheaths and shields')]
    compile_run(r"""
    #include <algorithm>
    #include <atomic>
    #include <cassert>
    #include <chrono>
    #include <cmath>
    #include <cstdint>
    #include <cstring>
    #include <memory>
    #include <mutex>
    #include <thread>
    #include <vector>
    #include "@BOTTLE@"
    #include "@POSE@"
    namespace rig = mkwvk::vr::rig;
    """.replace('@BOTTLE@', str(generated('vk_vr_bottle.h'))).replace('@POSE@', str(REPO / 'vr/items/bottle/bottle_pose.h')) +
    math + r"""
    static void logf(const char*, ...) {}
    struct LinkHand { bool valid = true; float position[3]{}; float basis[9]{1, 0, 0, 0, 1, 0, 0, 0, 1}; };
    struct LinkBody { float root[3]{}; bool child = true; float wrist_deg[2][3]{}; LinkHand hand[2]; };
    struct NativeHands { uint64_t frame = ~0ull; bool adult = false; V3 root{}, wrist[2]{}; };
    static NativeHands native_hands[32];
    static std::mutex cache_mu;
    static int held_hand = 0, corked = 1;
    static int bottle_hand() { return held_hand; }
    static int bottle_cork() { return corked; }
    """ + held + r"""
    static std::vector<float> palette_at(V3 at, unsigned n) {   /* (every matrix the model at this place, turned none) */
        std::vector<float> m(n * 12, 0.0f);
        for (unsigned i = 0; i < n; i++) {
            float* r = m.data() + i * 12;
            r[0] = r[5] = r[10] = 1, r[3] = at.x, r[7] = at.y, r[11] = at.z + rig::bottle_root[11];
        }
        return m;
    }
    int main() {
        LinkBody body;
        body.child = true, body.root[1] = 100;
        native_hands[0].frame = 10, native_hands[0].adult = false, native_hands[0].root = {0, 100, 0};
        native_hands[0].wrist[0] = {.3f, 100.8f, .2f}, native_hands[0].wrist[1] = {-.3f, 100.8f, .2f};
        body.hand[0].position[0] = 1, body.hand[0].position[1] = 101.2f, body.hand[0].position[2] = 0;
        const rig::BottleShape* glass = nullptr, *cap = nullptr;
        for (const rig::BottleShape& sh : rig::bottle_shapes) {
            if (sh.model == 0 && !sh.cork && !glass) glass = &sh;
            if (sh.model == 0 && sh.cork) cap = &sh;
        }
        assert(glass && cap);
        const V3 o{0, 0, 0};
        const auto in_hand = palette_at({.3f, 100.8f, .25f}, glass->matrices);
        auto out = held_bottle(glass->vb0, in_hand, o, 10, body);
        assert(out && std::fabs((*out)[3] - 1) < .2f && std::fabs((*out)[7] - 101.2f) < .3f);   /* moved to the VR left hand */
        assert(!held_bottle(glass->vb0, palette_at({5, 100, 5}, glass->matrices), o, 10, body));   /* a world bottle: passed over */
        assert(!held_bottle(glass->vb0, std::vector<float>(glass->matrices * 12, 0.0f), o, 10, body));   /* a wrong buffer */
        held_hand = -1;
        out = held_bottle(glass->vb0, in_hand, o, 10, body);
        assert(out && (*out)[3] == 0 && (*out)[7] == 0);   /* no VR hand: hidden */
        held_hand = 0;
        const auto cap_in = palette_at({.3f, 100.8f, .25f}, cap->matrices);
        const float corked_y = (*held_bottle(cap->vb0, cap_in, o, 10, body))[7];
        corked = 0;   /* opened: the cork pops up, falls to the floor, then is gone */
        const float popped_y = (*held_bottle(cap->vb0, cap_in, o, 10, body))[7];
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const float rising_y = (*held_bottle(cap->vb0, cap_in, o, 10, body))[7];
        assert(rising_y > popped_y && std::fabs(popped_y - corked_y) < .05f);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        const float lying_y = (*held_bottle(cap->vb0, cap_in, o, 10, body))[7];
        assert(std::fabs(lying_y - (body.root[1] - kCorkRest + rig::bottle_root[11])) < .02f);   /* on the floor */
        const float glass_y = (*held_bottle(glass->vb0, in_hand, o, 10, body))[7];
        assert(std::fabs(glass_y - corked_y) < .05f);   /* the bottle itself stays in the hand */
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        out = held_bottle(cap->vb0, cap_in, o, 10, body);
        assert(out && (*out)[7] == 0);   /* gone */
        return 0;
    }
    """, cpp=True)


def check_bottle_pose():
    """a bottle sits in the VR hand by its neck where the fingers close (the renderer's ball grip, as on a bomb), its
    mouth toward the thumb, in the same place for the game side (its actor) and the renderer (its model); the held
    model's draws are the game's bottle models (generated from your game), all moved together"""
    pose = (REPO / 'vr/items/bottle/bottle_pose.h').read_text(encoding='utf-8')
    arms = (REPO / 'vr/body/arms_and_pose.inc').read_text(encoding='utf-8')
    held = (REPO / 'vr/items/bottle/held_bottle.inc').read_text(encoding='utf-8')
    game = (REPO / 'vr/items/bottle/bottle_host.c').read_text(encoding='utf-8')
    assert 'mul(palm, kVrBottlePalm + kVrBottleNeckRadius)' in arms and 'h.ball_radius = kVrBottleNeckRadius' in arms, \
        'the fingers close on the neck where bottle_pose.h puts it'
    assert 'vr_bottle_pose(side, body.hand[side].position' in held and 'vr_bottle_pose(hand, o + 1, o + 9, o + 15' in game, \
        'the renderer and the game side place the bottle with the same pose'
    compile_run(r"""
    #include <assert.h>
    #include <math.h>
    """ + pose + r"""
    static float dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    int main(void) {
        const float grip[3] = {1, 2, 3}, gx[3] = {0, 0, 1}, gz[3] = {0, -1, 0};   /* (a fist held upright) */
        for (int side = 0; side < 2; side++) {
            float o[3], r[9];
            vr_bottle_pose(side, grip, gx, gz, o, r);
            float c[3][3];
            for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) c[i][k] = r[3 * k + i];
            for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) assert(fabsf(dot(c[i], c[j]) - (i == j)) < 1e-5f);
            const float det = c[0][0] * (c[1][1] * c[2][2] - c[1][2] * c[2][1]) - c[0][1] * (c[1][0] * c[2][2] - c[1][2] * c[2][0]) +
                              c[0][2] * (c[1][0] * c[2][1] - c[1][1] * c[2][0]);
            assert(det > .99f);                                     /* a rotation, both hands */
            assert(c[2][1] > .99f);                                 /* the mouth up the fist (-gz) */
            float neck[3], ball[3];
            for (int k = 0; k < 3; k++) neck[k] = o[k] + c[2][k] * kVrBottleNeck,
                                        ball[k] = grip[k] + gx[k] * (side ? -1 : 1) * (kVrBottlePalm + kVrBottleNeckRadius);
            for (int k = 0; k < 3; k++) assert(fabsf(neck[k] - ball[k]) < 1e-5f);   /* the neck where the fingers close */
        }
        return 0;
    }
    """)
    header = generated('vk_vr_bottle.h').read_text()
    rows = re.findall(r'\{(\d+), (\d+), (\d+), (\d+), (true|false)\}', header)
    assert len(rows) > 20, 'every bottle model\'s shapes'
    for vb0, matrices, root, model, cork in rows:
        assert int(root) < int(matrices), (vb0, root, matrices)
    assert len({(v, m) for v, m, *_ in rows}) == len(rows), 'each shape once'
    assert {m for *_, m, c in rows if c == 'true'} >= {str(k) for k in range(6)}, 'each glass has its cork shape'

CHECKS = [
    ('devmap', check_devmap_formats),
    ('headset', check_headset_layout),
    ('headset', check_headset_rotation),
    ('headset', check_replay_recorder),
    ('headset', check_headset_queue),
    ('headset', check_hud_decided_once),
    ('headset', check_headset_frame_ownership),
    ('headset', check_window_frustum),
    ('headset', check_headset_clean_exit),
    ('menu', check_menu_setting_keys),
    ('menu', check_menu_rows),
    ('input', check_menu_hold),
    ('input', check_menu_y_hold),
    ('input', check_menu_f2),
    ('input', check_menu_hint),
    ('input', check_controller_tables),
    ('camera', check_cull_margin),
    ('camera', check_world_cull),
    ('input', check_pad_combo_binding),
    ('tools', check_window_test_verdict),
    ('input', check_stick_turn),
    ('input', check_stick_jump),
    ('input', check_quick_select),
    ('input', check_radial),
    ('input', check_selected_item),
    ('body', check_frame_body),
    ('body', check_body_timing),
    ('body', check_link_shadow),
    ('body', check_link_blink),
    ('camera', check_camera_fade),
    ('body', check_canonical_rig),
    ('body', check_arm_reach),
    ('body', check_gait),
    ('bow', check_arrow_effect_camera),
    ('body', check_armor_attachment),
    ('body', check_tunic_attachment),
    ('body', check_bracelet_attachment),
    ('body', check_ocarina_belt),
    ('body', check_item_get),
    ('bow', check_bow_patch),
    ('bow', check_bow_input),
    ('bow', check_bow_launch),
    ('bow', check_tp_aim),
    ('bow', check_bow_string),
    ('bow', check_sling_pouch),
    ('bow', check_arrow_carry),
    ('bow', check_reticle_view),
    ('sword', check_sword_socket),
    ('sword', check_sword_controls),
    ('blade', check_blade_hits),
    ('input', check_stick_controls),
    ('input', check_hook_controls),
    ('input', check_boomerang_controls),
    ('input', check_ocarina_controls),
    ('bottle', check_bottle_controls),
    ('bottle', check_bottle_pose),
    ('bottle', check_bottle_keep_drawn),
    ('bottle', check_bottle_radial),
    ('menu', check_radial_art),
    ('menu', check_wrist),
    ('menu', check_wrist_draw),
    ('bottle', check_bottle_use),
    ('bottle', check_bottle_simple),
    ('bottle', check_bottle_held),
    ('sword', check_stick_socket),
    ('gear', check_equipment_state),
    ('gear', check_equipment_visible),
    ('gear', check_put_away),
    ('gear', check_gear_near),
    ('gear', check_bracelet_found),
    ('bombs', check_bomb_math),
    ('bombs', check_bomb_hands),
    ('bombs', check_bomb_input_snapshot),
    ('bombs', check_bomb_marks),
    ('bombs', check_bomb_options),
    ('bombs', check_bomb_flower_model),
    ('shield', check_guard_heading),
    ('shield', check_shield_grip),
    ('shield', check_shield_beside_items),
    ('camera', check_heading_realign),
    ('camera', check_crawl),
    ('camera', check_event_views),
    ('camera', check_comfort),
    ('stereo', check_stereo_projection),
    ('stereo', check_stereo_samples),
    ('stereo', check_stereo_camera_patch),
    ('stereo', check_stereo_shift_gate),
    ('stereo', check_stereo_small_draws),
    ('stereo', check_stereo_particle_camera),
    ('stereo', check_stereo_world_over_scene),
    ('stereo', check_stereo_forget_module),
    ('stereo', check_stereo_camera_per_eye),
    ('stereo', check_stereo_capture),
    ('stereo', check_scene_depth_any_share),
    ('stereo', check_warp_near_plane),
    ('stereo', check_warp_splat),
    ('stereo', check_3d_modes),
    ('headset', check_layer_eye_fov),
    ('headset', check_flat_eyes),
    ('headset', check_mono_pose),
    ('headset', check_tester_log),
    ('headset', check_rebar_hint),
    ('build', check_generate_inputs),
    ('build', check_game_hooks_emitted),
]


def main():
    global ARGS
    parser = argparse.ArgumentParser(description='Hookshot VR master tester')
    parser.add_argument('systems', nargs='*', help='only these systems')
    parser.add_argument('--list', action='store_true', help='list the checks')
    parser.add_argument('--work', help='the build\'s work folder, for the game hooks check')
    parser.add_argument('--romfs', help='your own game files (the romfs folder), for the checks that read them')
    parser.add_argument('--headless-game', help='also run a built game in true stereo, without a headset')
    parser.add_argument('--save-root', help='save folder to COPY for the headless run; never played in place')
    parser.add_argument('--seconds', type=int, default=150, help='headless run duration, including loading')
    parser.add_argument('-v', '--verbose', action='store_true', help='show a failed check\'s full output')
    ARGS = parser.parse_args()
    chosen = [(s, f) for s, f in CHECKS if not ARGS.systems or s in ARGS.systems]
    if ARGS.list:
        for s, f in chosen:
            print(f'{s:8} {f.__name__[6:]:22} {f.__doc__}')
        return 0
    results = {'PASS': 0, 'FAIL': 0, 'SKIP': 0}
    for s, f in chosen:
        try:
            f()
            status, note = 'PASS', ''
        except Skip as e:
            status, note = 'SKIP', str(e)
        except subprocess.CalledProcessError as e:
            status = 'FAIL'
            text = lambda b: b.decode(errors='replace') if isinstance(b, bytes) else (b or '')   # (a compiled check's output is bytes)
            out = text(e.stderr) + text(e.stdout)
            note = out.strip().splitlines()[-1][:150] if out.strip() else f'exit {e.returncode}'
            if ARGS.verbose:
                print(out)
        except Exception as e:
            status, note = 'FAIL', f'{type(e).__name__}: {e}'[:150]
            if ARGS.verbose:
                traceback.print_exc()
        results[status] += 1
        print(f'{status}  {s:8} {f.__name__[6:]:22} {note or f.__doc__}', flush=True)
    print(f"\n{results['PASS']} passed, {results['FAIL']} failed, {results['SKIP']} skipped")
    if results['FAIL']:
        return 1
    if ARGS.headless_game:
        return headless_smoke(ARGS.headless_game, ARGS.save_root, ARGS.seconds)
    return 0


def headless_smoke(game_folder, save_root, seconds):
    """One opt-in runtime check, using a copied save. Pictures/logs stay local in a temporary folder."""
    game = Path(game_folder).resolve()
    binary = next((p for p in (game / 'bin/hookshot', game / 'hookshot.exe') if p.is_file()), None)
    if not binary or not save_root or not Path(save_root).is_dir() or seconds < 1:
        print('FAIL  headless needs a built game, --save-root <save folder>, and a positive --seconds')
        return 1
    # Never stop another person's game or run a second large game process alongside it.
    if os.name == 'posix':
        active = subprocess.run(['pgrep', '-x', 'hookshot'], capture_output=True).returncode == 0
    else:
        processes = subprocess.run(['tasklist', '/FO', 'CSV', '/NH'], capture_output=True, text=True, check=True).stdout.lower()
        active = '"hookshot.exe"' in processes
    if active:
        print('FAIL  headless: close the existing game first; it was left untouched')
        return 1
    root = Path(tempfile.mkdtemp(prefix='hookshot-vr-smoke-'))
    shutil.copytree(Path(save_root).resolve(), root / 'user/save')
    pictures = root / 'pictures'
    pictures.mkdir()
    # Do not inherit one-off research switches from the current shell or the player's environment file.
    env = {k: v for k, v in os.environ.items() if not k.startswith(('RX_VR_', 'RX_VK_', 'HOS_', 'HOOKSHOT_VR'))}
    empty_env = root / 'environment.txt'
    empty_env.write_text('', encoding='utf-8')
    presses = ','.join(f'{t}:A/120' for t in range(5, seconds, 5))
    env.update(HOOKSHOT_VR='0', HOS_HEADLESS='1', HOS_AUDIO='0', SDL_AUDIODRIVER='dummy',
               HOOKSHOT_NO_UPDATE_CHECK='1', RX_VK_PRECOMPILE='0', HOS_ENV_FILE=str(empty_env),
               HOS_INPUT_SCRIPT=presses, HOS_INPUT_SCRIPT_BACKGROUND='1', HOS_INPUT_BACKGROUND='1',
               RX_VR_EYES_TEST=str(pictures))
    print(f'Headless true-stereo test (copied save): {root}', flush=True)
    with (root / 'run.log').open('w', encoding='utf-8') as log:
        proc = subprocess.Popen([str(binary), '--user', str(root / 'user')], cwd=game, env=env,
                                stdout=log, stderr=subprocess.STDOUT)
        ended_early = False
        try:
            proc.wait(timeout=seconds)
            ended_early = True
        except subprocess.TimeoutExpired:
            pass
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
    pairs = [p for p in pictures.glob('eyes_*_left.png')
             if p.with_name(p.name.replace('_left.png', '_right.png')).exists()]
    text = (root / 'run.log').read_text(encoding='utf-8', errors='replace')
    gameplay = any(float(n) > 100 for n in re.findall(r'passes and ([0-9.]+) draws in both eyes', text))
    ok = bool(pairs) and gameplay and not ended_early
    print(f'{"PASS" if ok else "FAIL"}  headless: {len(pairs)} eye pairs; gameplay draw counts {"seen" if gameplay else "not reached"}')
    if ended_early:
        print(f'Game exited before the test deadline (exit {proc.returncode}); inspect run.log.')
    print(f'Inspect pictures and run.log in {root}; your original save was not modified.')
    print('This does not verify real headset submission, tracking or first-person equipment.')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
