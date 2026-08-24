// Малювання кадру чату поверх кадру гри для OpenGL.
//
// Точка перехоплення — wglSwapBuffers (плоский експорт opengl32.dll), тож
// чіпляємось інлайн-хуком (common/inline_hook.h), а не через vtable. Малюємо
// фіксованим конвеєром 1.1: текстура + квадрат в ортопроєкції екрана. Це
// працює в переважній більшості OpenGL-ігор (сумісний контекст); стан GL
// повністю зберігаємо (glPushAttrib) і повертаємо — гра нічого не помічає.
#pragma once

#include <windows.h>
#include <GL/gl.h>

#include "../common/log.h"
#include "shared_frame_reader.h"

#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_CURRENT_PROGRAM
#define GL_CURRENT_PROGRAM 0x8B8D
#endif
#ifndef GL_CONTEXT_PROFILE_MASK
#define GL_CONTEXT_PROFILE_MASK 0x9126
#endif
#ifndef GL_CONTEXT_CORE_PROFILE_BIT
#define GL_CONTEXT_CORE_PROFILE_BIT 0x00000001
#endif
#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#endif
#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE 0x84E0
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif

namespace hominka {

class OverlayGL {
public:
    void draw() {
        // Малюємо лише коли на цьому потоці справді активний GL-контекст. Деякі
        // ігри викликають swap і без нього (або з іншого потоку) — тоді будь-який
        // виклик GL — краш. wglGetCurrentContext бере з opengl32 напряму.
        static PFN_curctx cur = load_cur_ctx();
        if (cur && !cur()) return;

        if (!reader_.ensure_open()) return;

        FrameView f;
        bool got = reader_.read(&f);
        if (got) {
            if (f.target_pid && f.target_pid != GetCurrentProcessId()) return;
            if (!logged_) { logged_ = true;
                log("overlay(gl): кадр — enabled=%u target=%u ми=%u розмір=%ux%u",
                    (unsigned)f.enabled, f.target_pid, (unsigned)GetCurrentProcessId(),
                    f.width, f.height); }
            if (!f.enabled) { enabled_ = false; return; }
            enabled_ = true;
            if (f.seq != tex_seq_ || f.width != tex_w_ || f.height != tex_h_ || !tex_)
                upload(f);
            last_ = f;
        } else if (!enabled_ || tex_seq_ == 0) {
            return;
        }
        if (!tex_ || tex_w_ == 0) return;
        blit();
    }

    void release() {
        // GL-текстуру НЕ видаляємо: release() кличеться при вивантаженні DLL, де
        // поточного GL-контексту вже немає, а glDeleteTextures без нього — UB.
        // На виході з процесу драйвер звільнить її сам.
        tex_ = 0;
        tex_w_ = tex_h_ = tex_seq_ = 0;
        reader_.close();
    }

private:
    void upload(const FrameView& f) {
        if (!tex_) glGenTextures(1, &tex_);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        if (f.width != tex_w_ || f.height != tex_h_) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, f.width, f.height, 0,
                         GL_BGRA_EXT, GL_UNSIGNED_BYTE, f.pixels);
            tex_w_ = f.width; tex_h_ = f.height;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f.width, f.height,
                            GL_BGRA_EXT, GL_UNSIGNED_BYTE, f.pixels);
        }
        tex_seq_ = f.seq;
    }

    void blit() {
        // Ключове для стабільності в СУЧАСНИХ GL-іграх (саме тут був
        // випадковий краш у драйвері вже після нашого малювання):
        //
        // 1. Core-профіль. Якщо гра створила контекст core-профілю, режим
        //    негайного малювання (glBegin/glOrtho/фіксований конвеєр) там не
        //    просто заборонений — деякі драйвери від нього падають, а не тихо
        //    повертають помилку. Тоді чесно не малюємо взагалі (краще без чату,
        //    ніж покласти гру). Малюнок лишається для сумісних контекстів —
        //    Minecraft, емулятори, багато інді.
        if (is_core_profile()) {
            if (!core_logged_) { core_logged_ = true;
                log("overlay(gl): контекст core-профілю — не малюємо, щоб не "
                    "покласти гру (потрібен сумісний контекст)"); }
            while (glGetError() != GL_NO_ERROR) {}   // прибрати нашу ж помилку від запиту
            return;
        }

        GLint vp[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, vp);
        float sw = (float)vp[2], sh = (float)vp[3];
        if (sw < 1 || sh < 1) return;
        float ow = (float)tex_w_, oh = (float)tex_h_;

        float x, y;
        switch (last_.anchor) {
            case ANCHOR_TOP_RIGHT:    x = sw - ow - last_.margin_x; y = (float)last_.margin_y; break;
            case ANCHOR_BOTTOM_LEFT:  x = (float)last_.margin_x; y = sh - oh - last_.margin_y; break;
            case ANCHOR_BOTTOM_RIGHT: x = sw - ow - last_.margin_x; y = sh - oh - last_.margin_y; break;
            default:                  x = (float)last_.margin_x; y = (float)last_.margin_y; break;
        }

        // Сучасні GL-ігри тримають прив'язану шейдерну програму — вимикаємо її на
        // час нашого фіксованого малювання, потім повертаємо.
        // Зберігаємо РІВНО те, що чіпаємо (як у DX9). glPushAttrib(GL_ALL) на
        // деяких драйверах поводиться норовливо, тож без нього.
        GLint prog = 0;
        static PFN_useprog useProgram = load_use_program();
        if (useProgram) { glGetIntegerv(GL_CURRENT_PROGRAM, &prog); if (prog) useProgram(0); }

        // Друга причина того самого краху: гра лишила прив'язаним неткочовий
        // VAO. Режим негайного малювання на багатьох драйверах (AMD/Intel)
        // вимагає VAO 0 — інакше падіння. Тимчасово ставимо 0, потім повертаємо.
        // Робимо це лише коли glBindVertexArray справді є (сумісний 3.0+); на
        // legacy-контексті функції немає — і VAO там не існує, тож не чіпаємо.
        static PFN_bindvao bindVao = load_bind_vao();
        GLint oldVao = 0;
        if (bindVao) { glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVao); if (oldVao) bindVao(0); }

        // І третє: гра могла лишити активним не нульовий текстурний блок. Наше
        // фіксоване малювання семплить із блоку 0 — повертаємо його на час
        // малюнка, інакше текстура «зникає» або береться чужа.
        static PFN_activetex activeTex = load_active_tex();
        GLint oldActive = GL_TEXTURE0;
        if (activeTex) { glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActive); if (oldActive != GL_TEXTURE0) activeTex(GL_TEXTURE0); }

        GLboolean wasDepth = glIsEnabled(GL_DEPTH_TEST);
        GLboolean wasLight = glIsEnabled(GL_LIGHTING);
        GLboolean wasCull = glIsEnabled(GL_CULL_FACE);
        GLboolean wasScissor = glIsEnabled(GL_SCISSOR_TEST);
        GLboolean wasBlend = glIsEnabled(GL_BLEND);
        GLboolean wasTex = glIsEnabled(GL_TEXTURE_2D);
        GLint oldTex = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTex);
        GLint oldBlendS = 0, oldBlendD = 0;
        glGetIntegerv(GL_BLEND_SRC, &oldBlendS);
        glGetIntegerv(GL_BLEND_DST, &oldBlendD);

        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
        glOrtho(0, sw, sh, 0, -1, 1);   // (0,0) — лівий верхній кут
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_LIGHTING);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glColor4f(1.f, 1.f, 1.f, last_.opacity / 255.f);

        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(x, y);
        glTexCoord2f(1, 0); glVertex2f(x + ow, y);
        glTexCoord2f(1, 1); glVertex2f(x + ow, y + oh);
        glTexCoord2f(0, 1); glVertex2f(x, y + oh);
        glEnd();

        glMatrixMode(GL_PROJECTION); glPopMatrix();
        glMatrixMode(GL_MODELVIEW); glPopMatrix();

        // Повертаємо все як було.
        glBindTexture(GL_TEXTURE_2D, (GLuint)oldTex);
        glBlendFunc((GLenum)oldBlendS, (GLenum)oldBlendD);
        set_enabled_gl(GL_DEPTH_TEST, wasDepth);
        set_enabled_gl(GL_LIGHTING, wasLight);
        set_enabled_gl(GL_CULL_FACE, wasCull);
        set_enabled_gl(GL_SCISSOR_TEST, wasScissor);
        set_enabled_gl(GL_BLEND, wasBlend);
        set_enabled_gl(GL_TEXTURE_2D, wasTex);
        if (activeTex && oldActive != GL_TEXTURE0) activeTex((GLenum)oldActive);
        if (bindVao && oldVao) bindVao((GLuint)oldVao);
        if (useProgram && prog) useProgram((GLuint)prog);

        // Прибираємо будь-яку помилку, яку могли згенерувати самі, щоб перевірки
        // glGetError у грі не спіткнулися об чужу помилку. Обмежуємо цикл.
        for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; ++i) {}
    }

    static void set_enabled_gl(GLenum cap, GLboolean on) {
        if (on) glEnable(cap); else glDisable(cap);
    }

    typedef HGLRC (WINAPI *PFN_curctx)();
    static PFN_curctx load_cur_ctx() {
        HMODULE gl = GetModuleHandleW(L"opengl32.dll");
        if (!gl) return nullptr;
        return (PFN_curctx)GetProcAddress(gl, "wglGetCurrentContext");
    }

    // Будь-яка функція GL новіша за 1.1 живе не в opengl32.dll, а в драйвері й
    // береться через wglGetProcAddress. ВАЖЛИВО перевіряти результат: на
    // контексті, де функції немає, wglGetProcAddress повертає не NULL, а сміття
    // (0,1,2,3,-1), і виклик такого «вказівника» — краш. Тому — один спільний
    // завантажувач із перевіркою.
    static void* load_gl_proc(const char* name) {
        typedef PROC (WINAPI *WGLGetProc)(LPCSTR);
        HMODULE gl = GetModuleHandleW(L"opengl32.dll");
        if (!gl) return nullptr;
        static WGLGetProc wglGet = (WGLGetProc)GetProcAddress(gl, "wglGetProcAddress");
        if (!wglGet) return nullptr;
        PROC p = wglGet(name);
        intptr_t v = (intptr_t)p;
        if (v == 0 || v == 1 || v == 2 || v == 3 || v == -1) return nullptr;
        return (void*)p;
    }

    typedef void (APIENTRY *PFN_useprog)(GLuint);
    static PFN_useprog load_use_program() {   // glUseProgram — GL 2.0
        return (PFN_useprog)load_gl_proc("glUseProgram");
    }

    typedef void (APIENTRY *PFN_bindvao)(GLuint);
    static PFN_bindvao load_bind_vao() {      // glBindVertexArray — GL 3.0
        return (PFN_bindvao)load_gl_proc("glBindVertexArray");
    }

    typedef void (APIENTRY *PFN_activetex)(GLenum);
    static PFN_activetex load_active_tex() {  // glActiveTexture — GL 1.3
        return (PFN_activetex)load_gl_proc("glActiveTexture");
    }

    // Чи це контекст core-профілю (без фіксованого конвеєра). На старих
    // контекстах (1.x/2.x) сам запит непідтримуваний — тоді mask лишається 0,
    // помилку прибираємо, і вважаємо контекст сумісним (малюємо).
    static bool is_core_profile() {
        GLint mask = 0;
        glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &mask);
        return (mask & GL_CONTEXT_CORE_PROFILE_BIT) != 0;
    }

    SharedFrameReader reader_;
    GLuint tex_ = 0;
    uint32_t tex_w_ = 0, tex_h_ = 0, tex_seq_ = 0;
    bool enabled_ = false;
    bool logged_ = false;
    bool core_logged_ = false;
    FrameView last_;
};

}  // namespace hominka
