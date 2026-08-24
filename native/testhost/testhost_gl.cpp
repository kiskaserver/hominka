// Крихітна гра-макет на OpenGL — для перевірки кроку 2 у GL (як Minecraft,
// емулятори, багато інді). Вікно, WGL-контекст, у циклі чистить кадр і
// SwapBuffers. Наш overlay.dll вклинюється в wglSwapBuffers. У випуск не входить.
#include <windows.h>
#include <GL/gl.h>

static HDC g_hdc;
static HGLRC g_rc;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"HominkaTestHostGL";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Hominka test host (OpenGL)",
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
    g_rc = wglCreateContext(g_hdc);
    wglMakeCurrent(g_hdc, g_rc);

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
