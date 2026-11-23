// SDL2 and OpenGL loaded at run time, and ImGui's platform and renderer parts on them (window.h).
//
// SDL2's own declarations are not included: the few calls and event layouts used here are declared below, from SDL2's ABI
// (stable since 2.0.0). That needs no SDL development package to build, and on Windows the SDL2.dll Hookshot ships beside
// the game does (sdl2-compat over SDL3). The renderer is ImGui 1.92's texture protocol on GL 3.3 core: the font atlas
// grows as new sizes are drawn (ImGuiBackendFlags_RendererHasTextures).
#include "window.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#undef CreateWindow   // (windows.h's macro, which would rename SDL's CreateWindow below)
#define GLAPIENTRY __stdcall
static void *lib_open(const char *name) { return (void *)LoadLibraryA(name); }
static void *lib_sym(void *lib, const char *name) { return (void *)GetProcAddress((HMODULE)lib, name); }
static const char *kSdlNames[] = {"SDL2.dll"};
#else
#include <dlfcn.h>
#define GLAPIENTRY
static void *lib_open(const char *name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
static void *lib_sym(void *lib, const char *name) { return dlsym(lib, name); }
static const char *kSdlNames[] = {"libSDL2-2.0.so.0", "libSDL2.so"};
#endif

namespace {

// ---- SDL2, the parts used ----
enum : uint32_t {
    SDL_INIT_VIDEO = 0x20,
    SDL_WINDOW_OPENGL = 0x2, SDL_WINDOW_HIDDEN = 0x8, SDL_WINDOW_RESIZABLE = 0x20, SDL_WINDOW_ALLOW_HIGHDPI = 0x2000,
    SDL_WINDOWPOS_CENTERED = 0x2FFF0000u,
    SDL_QUIT = 0x100, SDL_WINDOWEVENT = 0x200, SDL_KEYDOWN = 0x300, SDL_KEYUP = 0x301, SDL_TEXTINPUT = 0x303,
    SDL_MOUSEMOTION = 0x400, SDL_MOUSEBUTTONDOWN = 0x401, SDL_MOUSEBUTTONUP = 0x402, SDL_MOUSEWHEEL = 0x403,
};
enum { WE_LEAVE = 11, WE_FOCUS_GAINED = 12, WE_FOCUS_LOST = 13, WE_CLOSE = 14 };
enum { GL_RED_SIZE_ = 0, GL_DOUBLEBUFFER_ = 5, GL_DEPTH_SIZE_ = 6, GL_MAJOR_ = 17, GL_MINOR_ = 18, GL_FLAGS_ = 20,
       GL_PROFILE_ = 21 };
struct SDL_Rect { int x, y, w, h; };
union SDL_Event {
    uint32_t type;
    struct { uint32_t type, timestamp, windowID; uint8_t event, p1, p2, p3; int32_t data1, data2; } window;
    struct { uint32_t type, timestamp, windowID; uint8_t state, repeat, p2, p3; int32_t scancode, sym; uint16_t mod;
             uint32_t unused; } key;
    struct { uint32_t type, timestamp, windowID; char text[32]; } text;
    struct { uint32_t type, timestamp, windowID, which, state; int32_t x, y, xrel, yrel; } motion;
    struct { uint32_t type, timestamp, windowID, which; uint8_t button, state, clicks, padding; int32_t x, y; } button;
    struct { uint32_t type, timestamp, windowID, which; int32_t x, y; uint32_t direction; float px, py; } wheel;
    uint8_t padding[56];
};
static_assert(sizeof(SDL_Event) == 56, "SDL2's event is 56 bytes");

struct Sdl {
    int (*Init)(uint32_t);
    void (*Quit)();
    const char *(*GetError)();
    int (*SetHint)(const char *, const char *);
    void *(*CreateWindow)(const char *, int, int, int, int, uint32_t);
    void (*DestroyWindow)(void *);
    int (*GL_SetAttribute)(int, int);
    void *(*GL_CreateContext)(void *);
    void (*GL_DeleteContext)(void *);
    void (*GL_SwapWindow)(void *);
    int (*GL_SetSwapInterval)(int);
    void *(*GL_GetProcAddress)(const char *);
    void (*GL_GetDrawableSize)(void *, int *, int *);
    void (*GetWindowSize)(void *, int *, int *);
    void (*SetWindowMinimumSize)(void *, int, int);
    int (*GetDisplayUsableBounds)(int, SDL_Rect *);
    int (*PollEvent)(SDL_Event *);
    int (*WaitEventTimeout)(SDL_Event *, int);
    uint64_t (*GetPerformanceCounter)();
    uint64_t (*GetPerformanceFrequency)();
    void *(*CreateSystemCursor)(int);
    void (*SetCursor)(void *);
    void (*RaiseWindow)(void *);
    void (*HideWindow)(void *);
    void (*ShowWindow)(void *);
} sdl;

bool load_sdl() {
    void *lib = nullptr;
    for (const char *n : kSdlNames)
        if ((lib = lib_open(n))) break;
    if (!lib) return false;
    struct { void **slot; const char *name; } calls[] = {
#define S(f) {(void **)&sdl.f, "SDL_" #f}
        S(Init), S(Quit), S(GetError), S(SetHint), S(CreateWindow), S(DestroyWindow), S(GL_SetAttribute), S(GL_CreateContext),
        S(GL_DeleteContext), S(GL_SwapWindow), S(GL_SetSwapInterval), S(GL_GetProcAddress), S(GL_GetDrawableSize),
        S(GetWindowSize), S(SetWindowMinimumSize), S(GetDisplayUsableBounds), S(PollEvent), S(WaitEventTimeout),
        S(GetPerformanceCounter), S(GetPerformanceFrequency), S(CreateSystemCursor), S(SetCursor), S(RaiseWindow),
        S(HideWindow), S(ShowWindow),
#undef S
    };
    for (auto &c : calls)
        if (!(*c.slot = lib_sym(lib, c.name))) {
            fprintf(stderr, "launcher: %s missing from SDL2\n", c.name);
            return false;
        }
    return true;
}

// ---- OpenGL 3.3, the parts used ----
typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef unsigned char GLboolean;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr, GLintptr;
enum : GLenum {
    GL_COLOR_BUFFER_BIT = 0x4000, GL_BLEND = 0x0BE2, GL_SCISSOR_TEST = 0x0C11, GL_CULL_FACE = 0x0B44,
    GL_DEPTH_TEST = 0x0B71, GL_FUNC_ADD = 0x8006, GL_SRC_ALPHA = 0x0302, GL_ONE_MINUS_SRC_ALPHA = 0x0303, GL_ONE = 1,
    GL_TEXTURE_2D = 0x0DE1, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800, GL_LINEAR = 0x2601,
    GL_LINEAR_MIPMAP_LINEAR = 0x2703, GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803, GL_CLAMP_TO_EDGE = 0x812F,
    GL_REPEAT = 0x2901, GL_UNPACK_ROW_LENGTH = 0x0CF2, GL_UNPACK_ALIGNMENT = 0x0CF5, GL_PACK_ALIGNMENT = 0x0D05,
    GL_RGBA = 0x1908, GL_RGBA8 = 0x8058, GL_UNSIGNED_BYTE = 0x1401, GL_UNSIGNED_SHORT = 0x1403, GL_UNSIGNED_INT = 0x1405,
    GL_FLOAT = 0x1406, GL_TRIANGLES = 4, GL_VERTEX_SHADER = 0x8B31, GL_FRAGMENT_SHADER = 0x8B30,
    GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82, GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER = 0x8893,
    GL_STREAM_DRAW = 0x88E0, GL_TEXTURE0 = 0x84C0, GL_FRAMEBUFFER = 0x8D40, GL_COLOR_ATTACHMENT0 = 0x8CE0,
    GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
};
struct Gl {
    void(GLAPIENTRY *Viewport)(GLint, GLint, GLsizei, GLsizei);
    void(GLAPIENTRY *ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    void(GLAPIENTRY *Clear)(GLbitfield);
    void(GLAPIENTRY *Enable)(GLenum);
    void(GLAPIENTRY *Disable)(GLenum);
    void(GLAPIENTRY *BlendEquation)(GLenum);
    void(GLAPIENTRY *BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
    void(GLAPIENTRY *Scissor)(GLint, GLint, GLsizei, GLsizei);
    void(GLAPIENTRY *GenTextures)(GLsizei, GLuint *);
    void(GLAPIENTRY *BindTexture)(GLenum, GLuint);
    void(GLAPIENTRY *TexParameteri)(GLenum, GLenum, GLint);
    void(GLAPIENTRY *PixelStorei)(GLenum, GLint);
    void(GLAPIENTRY *TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void(GLAPIENTRY *TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
    void(GLAPIENTRY *DeleteTextures)(GLsizei, const GLuint *);
    void(GLAPIENTRY *GenerateMipmap)(GLenum);
    GLuint(GLAPIENTRY *CreateShader)(GLenum);
    void(GLAPIENTRY *ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
    void(GLAPIENTRY *CompileShader)(GLuint);
    void(GLAPIENTRY *GetShaderiv)(GLuint, GLenum, GLint *);
    void(GLAPIENTRY *GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
    GLuint(GLAPIENTRY *CreateProgram)();
    void(GLAPIENTRY *AttachShader)(GLuint, GLuint);
    void(GLAPIENTRY *LinkProgram)(GLuint);
    void(GLAPIENTRY *GetProgramiv)(GLuint, GLenum, GLint *);
    void(GLAPIENTRY *UseProgram)(GLuint);
    GLint(GLAPIENTRY *GetUniformLocation)(GLuint, const GLchar *);
    void(GLAPIENTRY *Uniform1i)(GLint, GLint);
    void(GLAPIENTRY *UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat *);
    void(GLAPIENTRY *GenBuffers)(GLsizei, GLuint *);
    void(GLAPIENTRY *BindBuffer)(GLenum, GLuint);
    void(GLAPIENTRY *BufferData)(GLenum, GLsizeiptr, const void *, GLenum);
    void(GLAPIENTRY *GenVertexArrays)(GLsizei, GLuint *);
    void(GLAPIENTRY *BindVertexArray)(GLuint);
    void(GLAPIENTRY *EnableVertexAttribArray)(GLuint);
    void(GLAPIENTRY *VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
    void(GLAPIENTRY *DrawElementsBaseVertex)(GLenum, GLsizei, GLenum, const void *, GLint);
    void(GLAPIENTRY *ActiveTexture)(GLenum);
    void(GLAPIENTRY *GenFramebuffers)(GLsizei, GLuint *);
    void(GLAPIENTRY *BindFramebuffer)(GLenum, GLuint);
    void(GLAPIENTRY *FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    GLenum(GLAPIENTRY *CheckFramebufferStatus)(GLenum);
    void(GLAPIENTRY *ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    void(GLAPIENTRY *Finish)();
} gl;

bool load_gl() {
    struct { void **slot; const char *name; } calls[] = {
#define G(f) {(void **)&gl.f, "gl" #f}
        G(Viewport), G(ClearColor), G(Clear), G(Enable), G(Disable), G(BlendEquation), G(BlendFuncSeparate), G(Scissor),
        G(GenTextures), G(BindTexture), G(TexParameteri), G(PixelStorei), G(TexImage2D), G(TexSubImage2D), G(DeleteTextures),
        G(GenerateMipmap), G(CreateShader), G(ShaderSource), G(CompileShader), G(GetShaderiv), G(GetShaderInfoLog),
        G(CreateProgram), G(AttachShader), G(LinkProgram), G(GetProgramiv), G(UseProgram), G(GetUniformLocation),
        G(Uniform1i), G(UniformMatrix4fv), G(GenBuffers), G(BindBuffer), G(BufferData), G(GenVertexArrays),
        G(BindVertexArray), G(EnableVertexAttribArray), G(VertexAttribPointer), G(DrawElementsBaseVertex), G(ActiveTexture),
        G(GenFramebuffers), G(BindFramebuffer), G(FramebufferTexture2D), G(CheckFramebufferStatus), G(ReadPixels), G(Finish),
#undef G
    };
    for (auto &c : calls)
        if (!(*c.slot = sdl.GL_GetProcAddress(c.name))) {
            fprintf(stderr, "launcher: %s missing from OpenGL\n", c.name);
            return false;
        }
    return true;
}

void *window, *context, *cursor_arrow, *cursor_hand;
bool hand_now, is_focused = true;
uint64_t last_counter;
GLuint program, vao, vbo, ibo;
GLint u_proj, u_tex;
// (the offscreen image, for --screenshot)
int off_w, off_h;
float off_scale = 1.0f;
GLuint off_fbo, off_tex;

GLuint compile(GLenum kind, const char *src) {
    GLuint s = gl.CreateShader(kind);
    gl.ShaderSource(s, 1, &src, nullptr);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        gl.GetShaderInfoLog(s, sizeof log, nullptr, log);
        fprintf(stderr, "launcher: shader: %s\n", log);
    }
    return s;
}

bool renderer_init() {
    const char *vs =
        "#version 330 core\n"
        "layout(location=0) in vec2 pos; layout(location=1) in vec2 uv; layout(location=2) in vec4 col;\n"
        "uniform mat4 proj; out vec2 f_uv; out vec4 f_col;\n"
        "void main(){ f_uv=uv; f_col=col; gl_Position=proj*vec4(pos,0,1); }\n";
    const char *fs =
        "#version 330 core\n"
        "in vec2 f_uv; in vec4 f_col; uniform sampler2D tex; out vec4 o;\n"
        "void main(){ o=f_col*texture(tex,f_uv); }\n";
    program = gl.CreateProgram();
    gl.AttachShader(program, compile(GL_VERTEX_SHADER, vs));
    gl.AttachShader(program, compile(GL_FRAGMENT_SHADER, fs));
    gl.LinkProgram(program);
    GLint ok = 0;
    gl.GetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) return false;
    u_proj = gl.GetUniformLocation(program, "proj");
    u_tex = gl.GetUniformLocation(program, "tex");
    gl.GenVertexArrays(1, &vao);
    gl.GenBuffers(1, &vbo);
    gl.GenBuffers(1, &ibo);
    gl.BindVertexArray(vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
    gl.EnableVertexAttribArray(0);
    gl.EnableVertexAttribArray(1);
    gl.EnableVertexAttribArray(2);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, 0, sizeof(ImDrawVert), (void *)offsetof(ImDrawVert, pos));
    gl.VertexAttribPointer(1, 2, GL_FLOAT, 0, sizeof(ImDrawVert), (void *)offsetof(ImDrawVert, uv));
    gl.VertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, 1, sizeof(ImDrawVert), (void *)offsetof(ImDrawVert, col));
    gl.BindVertexArray(0);
    ImGuiIO &io = ImGui::GetIO();
    io.BackendRendererName = "hookshotvr_gl3";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    return true;
}

// ImGui 1.92's texture requests: the font atlas, made and grown as sizes are drawn
void update_texture(ImTextureData *tex) {
    if (tex->Status == ImTextureStatus_WantCreate) {
        GLuint id = 0;
        gl.GenTextures(1, &id);
        gl.BindTexture(GL_TEXTURE_2D, id);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tex->Width, tex->Height, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex->GetPixels());
        tex->SetTexID((ImTextureID)(intptr_t)id);
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantUpdates) {
        gl.BindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)tex->TexID);
        gl.PixelStorei(GL_UNPACK_ROW_LENGTH, tex->Width);
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        for (ImTextureRect &r : tex->Updates)
            gl.TexSubImage2D(GL_TEXTURE_2D, 0, r.x, r.y, r.w, r.h, GL_RGBA, GL_UNSIGNED_BYTE, tex->GetPixelsAt(r.x, r.y));
        gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) {
        GLuint id = (GLuint)(intptr_t)tex->TexID;
        gl.DeleteTextures(1, &id);
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
}

void render(ImDrawData *dd, int fb_w, int fb_h) {
    if (dd->Textures)
        for (ImTextureData *tex : *dd->Textures)
            if (tex->Status != ImTextureStatus_OK) update_texture(tex);
    gl.Viewport(0, 0, fb_w, fb_h);
    gl.ClearColor(0x14 / 255.f, 0x10 / 255.f, 0x10 / 255.f, 1);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    gl.Enable(GL_BLEND);
    gl.BlendEquation(GL_FUNC_ADD);
    gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl.Disable(GL_CULL_FACE);
    gl.Disable(GL_DEPTH_TEST);
    gl.Enable(GL_SCISSOR_TEST);
    const float L = dd->DisplayPos.x, R = L + dd->DisplaySize.x, T = dd->DisplayPos.y, B = T + dd->DisplaySize.y;
    const float proj[16] = {2 / (R - L), 0, 0, 0, 0, 2 / (T - B), 0, 0, 0, 0, -1, 0, (R + L) / (L - R), (T + B) / (B - T), 0, 1};
    gl.UseProgram(program);
    gl.Uniform1i(u_tex, 0);
    gl.UniformMatrix4fv(u_proj, 1, 0, proj);
    gl.BindVertexArray(vao);
    gl.ActiveTexture(GL_TEXTURE0);
    const ImVec2 clip_off = dd->DisplayPos, clip_scale = dd->FramebufferScale;
    for (ImDrawList *list : dd->CmdLists) {
        gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
        gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)list->VtxBuffer.Size * sizeof(ImDrawVert), list->VtxBuffer.Data, GL_STREAM_DRAW);
        gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        gl.BufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)list->IdxBuffer.Size * sizeof(ImDrawIdx), list->IdxBuffer.Data,
                      GL_STREAM_DRAW);
        for (const ImDrawCmd &cmd : list->CmdBuffer) {
            if (cmd.UserCallback) continue;
            float x0 = (cmd.ClipRect.x - clip_off.x) * clip_scale.x, y0 = (cmd.ClipRect.y - clip_off.y) * clip_scale.y;
            float x1 = (cmd.ClipRect.z - clip_off.x) * clip_scale.x, y1 = (cmd.ClipRect.w - clip_off.y) * clip_scale.y;
            if (x1 <= x0 || y1 <= y0) continue;
            gl.Scissor((GLint)x0, (GLint)(fb_h - y1), (GLsizei)(x1 - x0), (GLsizei)(y1 - y0));
            gl.BindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)cmd.GetTexID());
            gl.DrawElementsBaseVertex(GL_TRIANGLES, (GLsizei)cmd.ElemCount,
                                      sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                                      (void *)(intptr_t)(cmd.IdxOffset * sizeof(ImDrawIdx)), (GLint)cmd.VtxOffset);
        }
    }
    gl.Disable(GL_SCISSOR_TEST);
    gl.BindVertexArray(0);
}

ImGuiKey key_of(int scancode) {
    switch (scancode) {
        case 40: return ImGuiKey_Enter;
        case 88: return ImGuiKey_KeypadEnter;
        case 41: return ImGuiKey_Escape;
        case 42: return ImGuiKey_Backspace;
        case 43: return ImGuiKey_Tab;
        case 44: return ImGuiKey_Space;
        case 79: return ImGuiKey_RightArrow;
        case 80: return ImGuiKey_LeftArrow;
        case 81: return ImGuiKey_DownArrow;
        case 82: return ImGuiKey_UpArrow;
        default: return ImGuiKey_None;
    }
}

}  // namespace

namespace win {

bool open(const char *title, int width, int height, bool hidden) {
    if (!load_sdl()) {
        fprintf(stderr, "launcher: SDL2 not found (libSDL2-2.0.so.0 / SDL2.dll)\n");
        return false;
    }
    sdl.SetHint("SDL_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR", "0");   // (a plain window: the desktop's compositor stays on)
    sdl.SetHint("SDL_VIDEO_X11_WMCLASS", "HookshotVR");
    sdl.SetHint("SDL_VIDEO_WAYLAND_WMCLASS", "HookshotVR");
    if (sdl.Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "launcher: SDL_Init: %s\n", sdl.GetError());
        return false;
    }
    sdl.GL_SetAttribute(GL_MAJOR_, 3);
    sdl.GL_SetAttribute(GL_MINOR_, 3);
    sdl.GL_SetAttribute(GL_PROFILE_, 1);   // (core)
    sdl.GL_SetAttribute(GL_DOUBLEBUFFER_, 1);
    sdl.GL_SetAttribute(GL_DEPTH_SIZE_, 0);
    // (the window fits the desktop: the design is 1280 x 820, smaller screens get it scaled down)
    SDL_Rect usable{};
    if (!hidden && sdl.GetDisplayUsableBounds(0, &usable) == 0 && usable.w > 0 && usable.h > 0) {
        float fit = 0.92f * (usable.w / (float)width < usable.h / (float)height ? usable.w / (float)width : usable.h / (float)height);
        if (fit < 1.0f) width = (int)(width * fit), height = (int)(height * fit);
    }
    uint32_t flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (hidden ? SDL_WINDOW_HIDDEN : 0);
    window = sdl.CreateWindow(title, (int)SDL_WINDOWPOS_CENTERED, (int)SDL_WINDOWPOS_CENTERED, width, height, flags);
    if (!window) {
        fprintf(stderr, "launcher: window: %s\n", sdl.GetError());
        return false;
    }
    sdl.SetWindowMinimumSize(window, 640, 410);
    context = sdl.GL_CreateContext(window);
    if (!context || !load_gl()) {
        fprintf(stderr, "launcher: OpenGL 3.3: %s\n", sdl.GetError());
        return false;
    }
    sdl.GL_SetSwapInterval(1);
    cursor_arrow = sdl.CreateSystemCursor(0);
    cursor_hand = sdl.CreateSystemCursor(11);
    last_counter = sdl.GetPerformanceCounter();
    ImGuiIO &io = ImGui::GetIO();
    io.BackendPlatformName = "hookshotvr_sdl2";
    return renderer_init();
}

void close() {
    if (context) sdl.GL_DeleteContext(context);
    if (window) sdl.DestroyWindow(window);
    if (sdl.Quit) sdl.Quit();
    context = window = nullptr;
}

void set_offscreen(int width, int height, float scale) {
    off_w = width, off_h = height, off_scale = scale;
    gl.GenTextures(1, &off_tex);
    gl.BindTexture(GL_TEXTURE_2D, off_tex);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl.GenFramebuffers(1, &off_fbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER, off_fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, off_tex, 0);
    if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fprintf(stderr, "launcher: offscreen image failed\n");
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool read_offscreen(uint8_t *rgba) {
    if (!off_fbo) return false;
    std::vector<uint8_t> rows((size_t)off_w * off_h * 4);
    gl.BindFramebuffer(GL_FRAMEBUFFER, off_fbo);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl.ReadPixels(0, 0, off_w, off_h, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    for (int y = 0; y < off_h; y++)   // (GL's rows go up from the bottom)
        memcpy(rgba + (size_t)y * off_w * 4, rows.data() + (size_t)(off_h - 1 - y) * off_w * 4, (size_t)off_w * 4);
    for (size_t i = 3; i < rows.size(); i += 4) rgba[i] = 255;
    return true;
}

bool begin_frame(int wait_ms) {
    ImGuiIO &io = ImGui::GetIO();
    bool open_still = true;
    SDL_Event e;
    bool have = off_fbo ? sdl.PollEvent(&e) : sdl.WaitEventTimeout(&e, wait_ms);
    while (have) {
        switch (e.type) {
            case SDL_QUIT: open_still = false; break;
            case SDL_WINDOWEVENT:
                if (e.window.event == WE_CLOSE) open_still = false;
                else if (e.window.event == WE_LEAVE) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
                else if (e.window.event == WE_FOCUS_GAINED) io.AddFocusEvent(true), is_focused = true;
                else if (e.window.event == WE_FOCUS_LOST) io.AddFocusEvent(false), is_focused = false;
                break;
            case SDL_MOUSEMOTION: io.AddMousePosEvent((float)e.motion.x, (float)e.motion.y); break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: {
                int b = e.button.button == 1 ? 0 : e.button.button == 3 ? 1 : e.button.button == 2 ? 2 : -1;
                if (b >= 0) io.AddMouseButtonEvent(b, e.type == SDL_MOUSEBUTTONDOWN);
                break;
            }
            case SDL_MOUSEWHEEL: io.AddMouseWheelEvent(-e.wheel.px, e.wheel.py); break;
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                io.AddKeyEvent(ImGuiMod_Ctrl, (e.key.mod & 0xC0) != 0);
                io.AddKeyEvent(ImGuiMod_Shift, (e.key.mod & 0x3) != 0);
                io.AddKeyEvent(ImGuiMod_Alt, (e.key.mod & 0x300) != 0);
                ImGuiKey k = key_of(e.key.scancode);
                if (k != ImGuiKey_None) io.AddKeyEvent(k, e.type == SDL_KEYDOWN);
                break;
            }
            case SDL_TEXTINPUT: io.AddInputCharactersUTF8(e.text.text); break;
        }
        have = sdl.PollEvent(&e);
    }
    if (off_fbo) {
        io.DisplaySize = ImVec2(off_w / off_scale, off_h / off_scale);
        io.DisplayFramebufferScale = ImVec2(off_scale, off_scale);
    } else {
        int w, h, fw, fh;
        sdl.GetWindowSize(window, &w, &h);
        sdl.GL_GetDrawableSize(window, &fw, &fh);
        io.DisplaySize = ImVec2((float)w, (float)h);
        if (w > 0 && h > 0) io.DisplayFramebufferScale = ImVec2((float)fw / w, (float)fh / h);
    }
    uint64_t now = sdl.GetPerformanceCounter();
    io.DeltaTime = now > last_counter ? (float)((double)(now - last_counter) / sdl.GetPerformanceFrequency()) : 1.0f / 60;
    if (io.DeltaTime <= 0) io.DeltaTime = 1.0f / 60;
    last_counter = now;
    return open_still;
}

void end_frame() {
    ImGui::Render();
    ImDrawData *dd = ImGui::GetDrawData();
    if (off_fbo) {
        gl.BindFramebuffer(GL_FRAMEBUFFER, off_fbo);
        render(dd, off_w, off_h);
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        gl.Finish();
        return;
    }
    int fw, fh;
    sdl.GL_GetDrawableSize(window, &fw, &fh);
    render(dd, fw, fh);
    sdl.GL_SwapWindow(window);
}

ImTextureID texture(const uint8_t *rgba, int width, int height, bool repeat) {
    GLuint id = 0;
    gl.GenTextures(1, &id);
    gl.BindTexture(GL_TEXTURE_2D, id);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    gl.GenerateMipmap(GL_TEXTURE_2D);
    return (ImTextureID)(intptr_t)id;
}

void set_hand_cursor(bool hand) {
    if (hand == hand_now || !cursor_hand) return;
    hand_now = hand;
    sdl.SetCursor(hand ? cursor_hand : cursor_arrow);
}
void raise() { if (window) sdl.RaiseWindow(window); }
void hide() { if (window) sdl.HideWindow(window); }
void show() { if (window) sdl.ShowWindow(window); }
bool focused() { return is_focused; }

}  // namespace win
