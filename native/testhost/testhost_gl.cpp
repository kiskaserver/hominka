// Крихітна гра-макет на OpenGL — для перевірки кроку 2 у GL (як Minecraft,
// емулятори, багато інді). Вікно, WGL-контекст, у циклі чистить кадр і
// SwapBuffers. Наш overlay.dll вклинюється в wglSwapBuffers. У випуск не входить.
//
// Аргумент керує типом контексту — саме він відрізняє «падає / не падає»:
//   (без аргументу)  сумісний 3.3 + прив'язаний VAO (як багато сучасних ігор);
//   core             core-профіль 3.3 (фіксований конвеєр заборонено —
//                    overlay має чесно НЕ малювати, а не покласти гру);
//   legacy           старий 1.1-контекст (як зовсім давні ігри).
#include <windows.h>
#include <GL/gl.h>
#include <string.h>

static HDC g_hdc;
static HGLRC g_rc;

// WGL / GL 3.0 константи й типи, яких немає в mingw <GL/gl.h>.
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002
typedef HGLRC (WINAPI *PFNCreateCtxAttribs)(HDC, HGLRC, const int*);
typedef void (APIENTRY *PFNGenVao)(GLsizei, GLuint*);
typedef void (APIENTRY *PFNBindVao)(GLuint);

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmd, int show) {
    bool wantCore = wcsstr(cmd, L"core") != nullptr;
    bool wantLegacy = wcsstr(cmd, L"legacy") != nullptr;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"HominkaTestHostGL";
    RegisterClassExW(&wc);
    const wchar_t* title = wantCore ? L"Hominka test host (OpenGL core)"
                         : wantLegacy ? L"Hominka test host (OpenGL legacy)"
                         : L"Hominka test host (OpenGL compat+VAO)";
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title,
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                               800, 500, NULL, NULL, inst, NULL);
    ShowWindow(hwnd, show);

    g_hdc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    int pf = ChoosePixelFormat(g_hdc, &pfd);
    SetPixelFormat(g_hdc, pf, &pfd);

    // Спершу старий контекст — без нього не дістати wglCreateContextAttribsARB.
    g_rc = wglCreateContext(g_hdc);
    wglMakeCurrent(g_hdc, g_rc);

    if (!wantLegacy) {
        PFNCreateCtxAttribs createAttribs =
            (PFNCreateCtxAttribs)wglGetProcAddress("wglCreateContextAttribsARB");
        if (createAttribs) {
            int bit = wantCore ? WGL_CONTEXT_CORE_PROFILE_BIT_ARB
                               : WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB;
            int attribs[] = {
                WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
                WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                WGL_CONTEXT_PROFILE_MASK_ARB, bit,
                0 };
            HGLRC rc2 = createAttribs(g_hdc, nullptr, attribs);
            if (rc2) {
                wglMakeCurrent(g_hdc, rc2);
                wglDeleteContext(g_rc);
                g_rc = rc2;
            }
        }
        // Сучасні контексти вимагають прив'язаного VAO для будь-якого малювання
        // — прив'язуємо, щоб відтворити саме той стан, на якому падав overlay.
        PFNGenVao genVao = (PFNGenVao)wglGetProcAddress("glGenVertexArrays");
        PFNBindVao bindVao = (PFNBindVao)wglGetProcAddress("glBindVertexArray");
        if (genVao && bindVao) {
            GLuint vao = 0; genVao(1, &vao); bindVao(vao);
        }
    }

    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }
        RECT r; GetClientRect(hwnd, &r);
        glViewport(0, 0, r.right, r.bottom);
        glClearColor(0.05f, 0.20f, 0.10f, 1.0f);   // темно-зелений
        glClear(GL_COLOR_BUFFER_BIT);
        SwapBuffers(g_hdc);   // сюди вклиниться overlay.dll (wglSwapBuffers)
        Sleep(8);
    }

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(g_rc);
    ReleaseDC(hwnd, g_hdc);
    return 0;
}
