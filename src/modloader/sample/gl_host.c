// Minimal OpenGL host for scripts/test-modloader.sh.
//
// The main test host (host.c) never touches OpenGL, so the overlay's
// wglSwapBuffers hook would stay dormant.  This host creates a real GL context
// and swaps a few frames, which drives the loader's ImGui overlay through
// initialisation and at least one rendered frame.  If wine has no usable GL, the
// test treats it as "not exercised" rather than a failure.

#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>

// Not declared by MinGW's GL headers.
BOOL WINAPI wglSwapBuffers(HDC);

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);  // unbuffered: keep output on a crash
    if (!LoadLibraryA("cdeloader.dll")) {
        printf("GLHOST: failed to load cdeloader.dll (%lu)\n",
               (unsigned long)GetLastError());
        return 1;
    }
    printf("GLHOST: cdeloader.dll loaded\n");

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "CdeGlTest";
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowA("CdeGlTest", "cde-gl-test", WS_OVERLAPPEDWINDOW,
                              0, 0, 320, 240, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        printf("GLHOST: CreateWindow failed (%lu)\n", (unsigned long)GetLastError());
        return 2;
    }
    HDC dc = GetDC(hwnd);

    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    int pf = ChoosePixelFormat(dc, &pfd);
    if (!pf) {
        printf("GLHOST: ChoosePixelFormat failed\n");
        return 3;
    }
    SetPixelFormat(dc, pf, &pfd);

    HGLRC rc = wglCreateContext(dc);
    if (!rc) {
        printf("GLHOST: wglCreateContext failed\n");
        return 4;
    }
    wglMakeCurrent(dc, rc);

    const char* ver = (const char*)glGetString(GL_VERSION);
    printf("GLHOST: GL_VERSION=%s\n", ver ? ver : "(null)");

    Sleep(300);  // let the loader's init thread finish and install the hook
    for (int i = 0; i < 60; i++) {
        glClearColor(0.1f, 0.1f, 0.2f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        wglSwapBuffers(dc);
        Sleep(20);
    }
    printf("GLHOST: swapped 60 frames\n");

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    ReleaseDC(hwnd, dc);
    DestroyWindow(hwnd);
    return 0;
}
