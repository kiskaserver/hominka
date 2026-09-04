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

// --- сучасний GL (шейдери, VAO, FBO) — для core-профілю та приховування ------
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ARRAY_BUFFER_BINDING
#define GL_ARRAY_BUFFER_BINDING 0x8894
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW 0x88E0
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#endif
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_TEXTURE_BINDING_2D
#define GL_TEXTURE_BINDING_2D 0x8069
#endif
#ifndef GL_FUNC_ADD
#define GL_FUNC_ADD 0x8006
#endif
#ifndef GL_BLEND_EQUATION_RGB
#define GL_BLEND_EQUATION_RGB 0x8009
#endif
#ifndef GL_BLEND_EQUATION_ALPHA
#define GL_BLEND_EQUATION_ALPHA 0x883D
#endif
#ifndef GL_COLOR_WRITEMASK
#define GL_COLOR_WRITEMASK 0x0C23
#endif
#ifndef GL_TEXTURE_ALPHA_SIZE
#define GL_TEXTURE_ALPHA_SIZE 0x805F
#endif
#ifndef GL_SAMPLER_BINDING
#define GL_SAMPLER_BINDING 0x8919
#endif
#ifndef GL_UNPACK_SWAP_BYTES
#define GL_UNPACK_SWAP_BYTES 0x0CF0
#endif
#ifndef GL_UNPACK_LSB_FIRST
#define GL_UNPACK_LSB_FIRST 0x0CF1
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
#ifndef GL_UNPACK_SKIP_ROWS
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#endif
#ifndef GL_UNPACK_SKIP_PIXELS
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#endif
#ifndef GL_UNPACK_IMAGE_HEIGHT
#define GL_UNPACK_IMAGE_HEIGHT 0x806E
#endif
#ifndef GL_UNPACK_SKIP_IMAGES
#define GL_UNPACK_SKIP_IMAGES 0x806D
#endif
#ifndef GL_PACK_ROW_LENGTH
#define GL_PACK_ROW_LENGTH 0x0D02
#endif
#ifndef GL_PACK_SKIP_ROWS
#define GL_PACK_SKIP_ROWS 0x0D03
#endif
#ifndef GL_PACK_SKIP_PIXELS
#define GL_PACK_SKIP_PIXELS 0x0D04
#endif

#ifndef GLchar
typedef char GLchar;
#endif
#ifndef HM_GL_PTR_TYPES
#define HM_GL_PTR_TYPES
typedef ptrdiff_t hm_GLsizeiptr;
#endif

namespace hominka {

// Вказівники на сучасні GL-функції (їх немає в opengl32.dll — беруться через
// wglGetProcAddress). Один набір на процес; вантажимо ліниво, коли активний
// контекст. Використовується і для малювання в core-профілі, і для FBO-знімка
// чистого кадру (приховування від OBS).
struct GL3 {
    // шейдери / програма
    GLuint (APIENTRY *CreateShader)(GLenum) = nullptr;
    void   (APIENTRY *ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
    void   (APIENTRY *CompileShader)(GLuint) = nullptr;
    void   (APIENTRY *GetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
    void   (APIENTRY *GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    GLuint (APIENTRY *CreateProgram)() = nullptr;
    void   (APIENTRY *AttachShader)(GLuint, GLuint) = nullptr;
    void   (APIENTRY *BindAttribLocation)(GLuint, GLuint, const GLchar*) = nullptr;
    void   (APIENTRY *LinkProgram)(GLuint) = nullptr;
    void   (APIENTRY *GetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
    void   (APIENTRY *DeleteShader)(GLuint) = nullptr;
    void   (APIENTRY *UseProgram)(GLuint) = nullptr;
    GLint  (APIENTRY *GetUniformLocation)(GLuint, const GLchar*) = nullptr;
    void   (APIENTRY *Uniform1i)(GLint, GLint) = nullptr;
    void   (APIENTRY *Uniform1f)(GLint, GLfloat) = nullptr;
    // VAO / VBO
    void   (APIENTRY *GenVertexArrays)(GLsizei, GLuint*) = nullptr;
    void   (APIENTRY *BindVertexArray)(GLuint) = nullptr;
    void   (APIENTRY *GenBuffers)(GLsizei, GLuint*) = nullptr;
    void   (APIENTRY *BindBuffer)(GLenum, GLuint) = nullptr;
    void   (APIENTRY *BufferData)(GLenum, hm_GLsizeiptr, const void*, GLenum) = nullptr;
    void   (APIENTRY *VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) = nullptr;
    void   (APIENTRY *EnableVertexAttribArray)(GLuint) = nullptr;
    void   (APIENTRY *ActiveTexture)(GLenum) = nullptr;
    void   (APIENTRY *BlendEquationSeparate)(GLenum, GLenum) = nullptr;
    void   (APIENTRY *BindSampler)(GLuint, GLuint) = nullptr;
    // FBO
    void   (APIENTRY *GenFramebuffers)(GLsizei, GLuint*) = nullptr;
    void   (APIENTRY *BindFramebuffer)(GLenum, GLuint) = nullptr;
    void   (APIENTRY *FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
    void   (APIENTRY *BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint,
                                       GLint, GLint, GLbitfield, GLenum) = nullptr;
    void   (APIENTRY *DeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;

    bool core_ok = false;   // всі функції для малювання в core-профілі є
    bool fbo_ok = false;    // всі функції для FBO-знімка є
};

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

        ensure_gl3();
        // Ховаємося від OBS: ПЕРЕД малюванням чату знімаємо чистий кадр із
        // дефолтного фреймбуфера (екран гри) у власний cleanFBO. OBS у своєму
        // хуку копіює екран через glBlitFramebuffer(read=0 → своя текстура);
        // наш хук на glBlitFramebuffer підмінить джерело з 0 на cleanFBO, і OBS
        // забере кадр БЕЗ чату. На моніторі чат лишається.
        if (last_.hide_from_obs) snapshot_clean();

        if (is_core_profile()) blit_core(); else blit();
    }

    void release() {
        // GL-текстуру НЕ видаляємо: release() кличеться при вивантаженні DLL, де
        // поточного GL-контексту вже немає, а glDeleteTextures без нього — UB.
        // На виході з процесу драйвер звільнить її сам.
        tex_ = 0;
        tex_w_ = tex_h_ = tex_seq_ = 0;
        reader_.close();
    }

    // --- приховування від OBS (звертається хук glBlitFramebuffer у dllmain) ---
    // Адреса справжньої glBlitFramebuffer у драйвері — щоб dllmain поставив на
    // неї інлайн-хук. OBS копіює нею екран; ми підмінимо джерело на cleanFBO.
    void* blitframebuffer_proc() { return load_gl_proc("glBlitFramebuffer"); }

    bool hiding() const { return enabled_ && last_.hide_from_obs && clean_fbo_ != 0; }
    bool recording_snapshot() const { return recording_snapshot_; }
    GLuint clean_fbo() const { return clean_fbo_; }

    int current_read_fbo() {
        GLint b = 0; glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &b); return b;
    }
    void bind_read_fbo(GLuint fbo) {
        if (g3_.BindFramebuffer) g3_.BindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    }

private:
    void upload(const FrameView& f) {
        if (!tex_) glGenTextures(1, &tex_);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // Скидаємо ВСЕ стан розпакування пікселів: сучасні ігри (Minecraft для
        // своїх атласів) лишають GL_UNPACK_ROW_LENGTH та skip-параметри
        // ненульовими, і тоді glTexImage2D читає наші пікселі з чужим кроком —
        // на екрані каша замість чату. Ставимо стандартні значення й повертаємо.
        GLint uAlign = 4, uRow = 0, uSkipR = 0, uSkipP = 0, uSwap = 0, uLsb = 0;
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &uAlign);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &uRow);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &uSkipR);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &uSkipP);
        glGetIntegerv(GL_UNPACK_SWAP_BYTES, &uSwap);
        glGetIntegerv(GL_UNPACK_LSB_FIRST, &uLsb);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_SWAP_BYTES, 0);
        glPixelStorei(GL_UNPACK_LSB_FIRST, 0);
        if (f.width != tex_w_ || f.height != tex_h_) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, f.width, f.height, 0,
                         GL_BGRA_EXT, GL_UNSIGNED_BYTE, f.pixels);
            tex_w_ = f.width; tex_h_ = f.height;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f.width, f.height,
                            GL_BGRA_EXT, GL_UNSIGNED_BYTE, f.pixels);
        }
        // Повертаємо стан розпакування, як був — щоб не зламати завантаження
        // текстур самою грою після нас.
        glPixelStorei(GL_UNPACK_ALIGNMENT, uAlign);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, uRow);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, uSkipR);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, uSkipP);
        glPixelStorei(GL_UNPACK_SWAP_BYTES, uSwap);
        glPixelStorei(GL_UNPACK_LSB_FIRST, uLsb);
        tex_seq_ = f.seq;
    }

    // Фіксований конвеєр 1.1 — для сумісних (compat) контекстів: старий
    // Minecraft, id Tech 3/4, HPL, багато інді. Core-профіль іде в blit_core().
    void blit() {
        GLint vp[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, vp);
        float sw = (float)vp[2], sh = (float)vp[3];
        if (sw < 1 || sh < 1) return;
        float x, y, ow, oh;   // рамка чату — частки кадру, масштабуємо під гру
        last_.rect(sw, sh, &x, &y, &ow, &oh);

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

    // Малювання в CORE-профілі (GL 3.2+): фіксованого конвеєра там немає, тож
    // шейдерна програма + VAO/VBO. Квадрат рахуємо в NDC на CPU щокадру.
    void blit_core() {
        if (core_failed_) return;
        if (!g3_.core_ok) {
            if (!core_failed_) { core_failed_ = true;
                log("overlay(gl): core-профіль, але не всі сучасні GL-функції "
                    "доступні — чат не малюємо"); }
            return;
        }
        if (!ensure_core_program()) { core_failed_ = true;
            log("overlay(gl): не вдалося зібрати шейдер для core-профілю — "
                "чат не малюємо"); return; }

        GLint vp[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, vp);
        float sw = (float)vp[2], sh = (float)vp[3];
        if (sw < 1 || sh < 1) return;
        float x, y, ow, oh;
        last_.rect(sw, sh, &x, &y, &ow, &oh);

        // Верхньо-лівий початок координат → NDC з переворотом Y. UV як у compat:
        // v=0 зверху (рядок 0 картинки лежить у teximage як v=0).
        auto ndx = [&](float px) { return (px / sw) * 2.f - 1.f; };
        auto ndy = [&](float py) { return 1.f - (py / sh) * 2.f; };
        const float verts[16] = {
            ndx(x),      ndy(y),      0.f, 0.f,   // TL
            ndx(x),      ndy(y + oh), 0.f, 1.f,   // BL
            ndx(x + ow), ndy(y),      1.f, 0.f,   // TR
            ndx(x + ow), ndy(y + oh), 1.f, 1.f,   // BR
        };

        // Малюємо у ДЕФОЛТНИЙ фреймбуфер (екран): деякі ігри (напр. сучасний
        // Minecraft) на момент swap лишають прив'язаним власний FBO — намалюй ми
        // туди, чат не потрапив би на монітор. Тимчасово ставимо draw=0.
        GLint sDrawFbo = 0;
        if (g3_.BindFramebuffer) {
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &sDrawFbo);
            if (sDrawFbo != 0) g3_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        }

        // Зберігаємо рівно те, що чіпаємо (машина станів глобальна).
        GLint sProg = 0, sVao = 0, sAbuf = 0, sActive = GL_TEXTURE0, sTex0 = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &sProg);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &sVao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &sAbuf);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &sActive);
        GLboolean wasBlend = glIsEnabled(GL_BLEND);
        GLboolean wasDepth = glIsEnabled(GL_DEPTH_TEST);
        GLboolean wasCull = glIsEnabled(GL_CULL_FACE);
        GLboolean wasScissor = glIsEnabled(GL_SCISSOR_TEST);
        GLint sBlendS = GL_SRC_ALPHA, sBlendD = GL_ONE_MINUS_SRC_ALPHA;
        glGetIntegerv(GL_BLEND_SRC, &sBlendS);
        glGetIntegerv(GL_BLEND_DST, &sBlendD);
        // Ігри лишають нестандартні стани, через які наше змішування «не діє» і
        // напівпрозорий фон чату виходить чорним прямокутником: рівняння
        // змішування ≠ FUNC_ADD (сучасний Minecraft ставить своє) або вимкнений
        // запис якогось каналу маскою кольору. Зберігаємо й ставимо стандартні.
        GLboolean sMask[4] = {1, 1, 1, 1};
        glGetBooleanv(GL_COLOR_WRITEMASK, sMask);
        GLint sEqRGB = GL_FUNC_ADD, sEqA = GL_FUNC_ADD;
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &sEqRGB);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &sEqA);

        g3_.UseProgram(prog_);
        g3_.ActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &sTex0);
        glBindTexture(GL_TEXTURE_2D, tex_);
        // Сучасні ігри (Minecraft) тримають на блоці 0 ОБ'ЄКТ-СЕМПЛЕР (glBindSampler)
        // із мінфільтром під міпмапи. Він ПЕРЕКРИВАЄ параметри нашої текстури, і та
        // без міпмап стає «неповною» — семпл повертає (0,0,0,1), тобто чорний
        // напівпрозорий прямокутник без вмісту. Знімаємо семплер на час малюнка.
        GLint sSampler = 0;
        if (g3_.BindSampler) {
            glGetIntegerv(GL_SAMPLER_BINDING, &sSampler);
            if (sSampler) g3_.BindSampler(0, 0);
        }
        g3_.Uniform1i(u_tex_, 0);
        g3_.Uniform1f(u_opacity_, last_.opacity / 255.f);

        g3_.BindVertexArray(vao_);
        g3_.BindBuffer(GL_ARRAY_BUFFER, vbo_);
        g3_.BufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        if (g3_.BlendEquationSeparate) g3_.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        // Повертаємо все як було.
        glBindTexture(GL_TEXTURE_2D, (GLuint)sTex0);
        if (g3_.BindSampler && sSampler) g3_.BindSampler(0, (GLuint)sSampler);
        glBlendFunc((GLenum)sBlendS, (GLenum)sBlendD);
        glColorMask(sMask[0], sMask[1], sMask[2], sMask[3]);
        if (g3_.BlendEquationSeparate) g3_.BlendEquationSeparate((GLenum)sEqRGB, (GLenum)sEqA);
        set_enabled_gl(GL_BLEND, wasBlend);
        set_enabled_gl(GL_DEPTH_TEST, wasDepth);
        set_enabled_gl(GL_CULL_FACE, wasCull);
        set_enabled_gl(GL_SCISSOR_TEST, wasScissor);
        g3_.BindBuffer(GL_ARRAY_BUFFER, (GLuint)sAbuf);
        g3_.BindVertexArray((GLuint)sVao);
        g3_.UseProgram((GLuint)sProg);
        if ((GLenum)sActive != GL_TEXTURE0) g3_.ActiveTexture((GLenum)sActive);
        if (g3_.BindFramebuffer && sDrawFbo != 0)
            g3_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)sDrawFbo);
        for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; ++i) {}
    }

    // Збирає шейдерну програму + VAO/VBO один раз. false — якщо не вдалося.
    bool ensure_core_program() {
        if (prog_) return true;
        static const char* VS =
            "#version 150\n"
            "in vec2 aPos; in vec2 aUV; out vec2 vUV;\n"
            "void main(){ vUV=aUV; gl_Position=vec4(aPos,0.0,1.0); }\n";
        static const char* FS =
            "#version 150\n"
            "uniform sampler2D uTex; uniform float uOpacity;\n"
            "in vec2 vUV; out vec4 frag;\n"
            "void main(){ vec4 c=texture(uTex,vUV); frag=vec4(c.rgb, c.a*uOpacity); }\n";
        GLuint vs = compile_shader(GL_VERTEX_SHADER, VS);
        GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FS);
        if (!vs || !fs) return false;
        GLuint p = g3_.CreateProgram();
        if (!p) return false;
        g3_.AttachShader(p, vs);
        g3_.AttachShader(p, fs);
        g3_.BindAttribLocation(p, 0, "aPos");
        g3_.BindAttribLocation(p, 1, "aUV");
        g3_.LinkProgram(p);
        GLint ok = 0; g3_.GetProgramiv(p, GL_LINK_STATUS, &ok);
        g3_.DeleteShader(vs); g3_.DeleteShader(fs);
        if (!ok) return false;
        u_tex_ = g3_.GetUniformLocation(p, "uTex");
        u_opacity_ = g3_.GetUniformLocation(p, "uOpacity");

        // VAO з описом атрибутів (посилається на vbo_). Зберігаємо/повертаємо
        // попередні прив'язки, щоб не зачепити гру.
        GLint sVao = 0, sAbuf = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &sVao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &sAbuf);
        g3_.GenVertexArrays(1, &vao_);
        g3_.BindVertexArray(vao_);
        g3_.GenBuffers(1, &vbo_);
        g3_.BindBuffer(GL_ARRAY_BUFFER, vbo_);
        g3_.BufferData(GL_ARRAY_BUFFER, sizeof(float) * 16, nullptr, GL_STREAM_DRAW);
        g3_.EnableVertexAttribArray(0);
        g3_.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, (void*)0);
        g3_.EnableVertexAttribArray(1);
        g3_.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4,
                                (void*)(sizeof(float) * 2));
        g3_.BindVertexArray((GLuint)sVao);
        g3_.BindBuffer(GL_ARRAY_BUFFER, (GLuint)sAbuf);
        prog_ = p;
        log("overlay(gl): core-профіль — шейдер зібрано, малюємо");
        return true;
    }

    GLuint compile_shader(GLenum type, const char* src) {
        GLuint s = g3_.CreateShader(type);
        if (!s) return 0;
        g3_.ShaderSource(s, 1, &src, nullptr);
        g3_.CompileShader(s);
        GLint ok = 0; g3_.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char buf[512] = {0}; GLsizei n = 0;
            if (g3_.GetShaderInfoLog) g3_.GetShaderInfoLog(s, sizeof(buf) - 1, &n, buf);
            log("overlay(gl): помилка компіляції шейдера: %s", buf);
            g3_.DeleteShader(s);
            return 0;
        }
        return s;
    }

    // Знімок чистого кадру (екран гри БЕЗ чату) у cleanFBO. Кличемо ПЕРЕД
    // малюванням чату. recording_snapshot_ захищає наш власний blit від того,
    // щоб хук на glBlitFramebuffer підмінив ЙОГО джерело.
    void snapshot_clean() {
        if (!g3_.fbo_ok) return;
        GLint vp[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, vp);
        int w = vp[2], h = vp[3];
        if (w < 1 || h < 1) return;
        if (!ensure_clean_fbo(w, h)) return;

        recording_snapshot_ = true;
        GLint sRead = 0, sDraw = 0, sReadBuf = 0x0405 /*GL_BACK*/;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &sRead);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &sDraw);
        g3_.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glGetIntegerv(0x0C02 /*GL_READ_BUFFER*/, &sReadBuf);
        glReadBuffer(0x0405 /*GL_BACK*/);
        g3_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, clean_fbo_);
        g3_.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glReadBuffer((GLenum)sReadBuf);
        g3_.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)sRead);
        g3_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)sDraw);
        recording_snapshot_ = false;
        for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; ++i) {}
    }

    bool ensure_clean_fbo(int w, int h) {
        if (clean_fbo_ && clean_w_ == w && clean_h_ == h) return true;
        GLint sTex = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &sTex);
        if (!clean_tex_) glGenTextures(1, &clean_tex_);
        glBindTexture(GL_TEXTURE_2D, clean_tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, (GLuint)sTex);

        if (!clean_fbo_) g3_.GenFramebuffers(1, &clean_fbo_);
        GLint sRead = 0, sDraw = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &sRead);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &sDraw);
        g3_.BindFramebuffer(GL_FRAMEBUFFER, clean_fbo_);
        g3_.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, clean_tex_, 0);
        g3_.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)sRead);
        g3_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)sDraw);
        clean_w_ = w; clean_h_ = h;
        return true;
    }

    // Ліниво вантажимо сучасні GL-функції (потрібен активний контекст). Один раз.
    void ensure_gl3() {
        if (gl3_tried_) return;
        gl3_tried_ = true;
        g3_.CreateShader = (GLuint (APIENTRY*)(GLenum))load_gl_proc("glCreateShader");
        g3_.ShaderSource = (void (APIENTRY*)(GLuint, GLsizei, const GLchar* const*, const GLint*))load_gl_proc("glShaderSource");
        g3_.CompileShader = (void (APIENTRY*)(GLuint))load_gl_proc("glCompileShader");
        g3_.GetShaderiv = (void (APIENTRY*)(GLuint, GLenum, GLint*))load_gl_proc("glGetShaderiv");
        g3_.GetShaderInfoLog = (void (APIENTRY*)(GLuint, GLsizei, GLsizei*, GLchar*))load_gl_proc("glGetShaderInfoLog");
        g3_.CreateProgram = (GLuint (APIENTRY*)())load_gl_proc("glCreateProgram");
        g3_.AttachShader = (void (APIENTRY*)(GLuint, GLuint))load_gl_proc("glAttachShader");
        g3_.BindAttribLocation = (void (APIENTRY*)(GLuint, GLuint, const GLchar*))load_gl_proc("glBindAttribLocation");
        g3_.LinkProgram = (void (APIENTRY*)(GLuint))load_gl_proc("glLinkProgram");
        g3_.GetProgramiv = (void (APIENTRY*)(GLuint, GLenum, GLint*))load_gl_proc("glGetProgramiv");
        g3_.DeleteShader = (void (APIENTRY*)(GLuint))load_gl_proc("glDeleteShader");
        g3_.UseProgram = (void (APIENTRY*)(GLuint))load_gl_proc("glUseProgram");
        g3_.GetUniformLocation = (GLint (APIENTRY*)(GLuint, const GLchar*))load_gl_proc("glGetUniformLocation");
        g3_.Uniform1i = (void (APIENTRY*)(GLint, GLint))load_gl_proc("glUniform1i");
        g3_.Uniform1f = (void (APIENTRY*)(GLint, GLfloat))load_gl_proc("glUniform1f");
        g3_.GenVertexArrays = (void (APIENTRY*)(GLsizei, GLuint*))load_gl_proc("glGenVertexArrays");
        g3_.BindVertexArray = (void (APIENTRY*)(GLuint))load_gl_proc("glBindVertexArray");
        g3_.GenBuffers = (void (APIENTRY*)(GLsizei, GLuint*))load_gl_proc("glGenBuffers");
        g3_.BindBuffer = (void (APIENTRY*)(GLenum, GLuint))load_gl_proc("glBindBuffer");
        g3_.BufferData = (void (APIENTRY*)(GLenum, hm_GLsizeiptr, const void*, GLenum))load_gl_proc("glBufferData");
        g3_.VertexAttribPointer = (void (APIENTRY*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))load_gl_proc("glVertexAttribPointer");
        g3_.EnableVertexAttribArray = (void (APIENTRY*)(GLuint))load_gl_proc("glEnableVertexAttribArray");
        g3_.ActiveTexture = (void (APIENTRY*)(GLenum))load_gl_proc("glActiveTexture");
        g3_.BlendEquationSeparate = (void (APIENTRY*)(GLenum, GLenum))load_gl_proc("glBlendEquationSeparate");
        g3_.BindSampler = (void (APIENTRY*)(GLuint, GLuint))load_gl_proc("glBindSampler");
        g3_.GenFramebuffers = (void (APIENTRY*)(GLsizei, GLuint*))load_gl_proc("glGenFramebuffers");
        g3_.BindFramebuffer = (void (APIENTRY*)(GLenum, GLuint))load_gl_proc("glBindFramebuffer");
        g3_.FramebufferTexture2D = (void (APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint))load_gl_proc("glFramebufferTexture2D");
        g3_.BlitFramebuffer = (void (APIENTRY*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))load_gl_proc("glBlitFramebuffer");
        g3_.DeleteFramebuffers = (void (APIENTRY*)(GLsizei, const GLuint*))load_gl_proc("glDeleteFramebuffers");

        g3_.core_ok = g3_.CreateShader && g3_.ShaderSource && g3_.CompileShader &&
            g3_.GetShaderiv && g3_.CreateProgram && g3_.AttachShader &&
            g3_.BindAttribLocation && g3_.LinkProgram && g3_.GetProgramiv &&
            g3_.DeleteShader && g3_.UseProgram && g3_.GetUniformLocation &&
            g3_.Uniform1i && g3_.Uniform1f && g3_.GenVertexArrays &&
            g3_.BindVertexArray && g3_.GenBuffers && g3_.BindBuffer &&
            g3_.BufferData && g3_.VertexAttribPointer &&
            g3_.EnableVertexAttribArray && g3_.ActiveTexture;
        g3_.fbo_ok = g3_.GenFramebuffers && g3_.BindFramebuffer &&
            g3_.FramebufferTexture2D && g3_.BlitFramebuffer;
        log("overlay(gl): сучасні GL-функції — core=%s, fbo=%s",
            g3_.core_ok ? "так" : "ні", g3_.fbo_ok ? "так" : "ні");
    }

    static void set_enabled_gl(GLenum cap, GLboolean on) {
        if (on) glEnable(cap); else glDisable(cap);
    }

    typedef HGLRC (WINAPI *PFN_curctx)();
    static PFN_curctx load_cur_ctx() {
        HMODULE gl = GetModuleHandleW(L"opengl32.dll");
        if (!gl) return nullptr;
        return (PFN_curctx)(void*)GetProcAddress(gl, "wglGetCurrentContext");
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
        static WGLGetProc wglGet = (WGLGetProc)(void*)GetProcAddress(gl, "wglGetProcAddress");
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
    FrameView last_;

    // Сучасний GL (шейдери/VAO/FBO) — заповнюється ensure_gl3().
    GL3 g3_;
    bool gl3_tried_ = false;

    // core-профіль: шейдерна програма + буфери.
    GLuint prog_ = 0, vao_ = 0, vbo_ = 0;
    GLint u_tex_ = -1, u_opacity_ = -1;
    bool core_failed_ = false;

    // Приховування від OBS: чистий кадр у власному FBO.
    GLuint clean_tex_ = 0, clean_fbo_ = 0;
    int clean_w_ = 0, clean_h_ = 0;
    bool recording_snapshot_ = false;
};

}  // namespace hominka
