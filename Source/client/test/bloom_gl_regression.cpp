#include "modules/render/arraylist/al_gl.h"
#include "modules/visuals/bloom/AtmosphereMath.h"
#include "modules/visuals/bloom/NativeTextures.h"
#include "modules/visuals/bloom/SkyRenderer.h"
#define STB_IMAGE_IMPLEMENTATION
#include "vgui/framework/render/stb/stb_image.hh"
#include <cstdlib>
#include <iostream>
namespace vanta::modules::bloom {
AtmosphereSettings atmosphere;
AtmosphereModel model;
AtmosphereModel &atmosphereModel() { return model; }
float atmosphereFrameDelta() { return 1.f / 60; }
float atmosphereNaturalLight() { return 1; }
} // namespace vanta::modules::bloom
using namespace vanta::modules::bloom;
void check(bool okay, const char *message) {
    if (!okay) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
int main() {
    WNDCLASSW wc{};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BloomNativeRenderTest";
    check(RegisterClassW(&wc), "RegisterClass");
    auto window =
        CreateWindowW(wc.lpszClassName, L"", WS_POPUP, 0, 0, 128, 128, nullptr, nullptr, wc.hInstance, nullptr);
    check(window != nullptr, "Hidden window");
    auto dc = GetDC(window);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    auto format = ChoosePixelFormat(dc, &pfd);
    check(format && SetPixelFormat(dc, format, &pfd), "Pixel format");
    auto rc = wglCreateContext(dc);
    check(rc && wglMakeCurrent(dc, rc), "Hidden GL context");
    const auto &g = vanta::al::gl::api();
    check(g.ok, "OpenGL loader");
    GLuint unpack = 0;
    g.GenBuffers(1, &unpack);
    g.BindBuffer(0x88EC, unpack);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 71);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 3);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 2);
    g.ActiveTexture(GL_TEXTURE0 + 3);
    auto atlas = loadNativeTexture(2302);
    GLint n = 0;
    glGetIntegerv(0x84E0, &n);
    check(n == GL_TEXTURE0 + 3, "Texture unit leaked");
    glGetIntegerv(0x88EF, &n);
    check(n == GLint(unpack), "Unpack buffer leaked");
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &n);
    check(n == 71, "Unpack row length leaked");
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &n);
    check(n == 3, "Unpack row skip leaked");
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &n);
    check(n == 2, "Unpack pixel skip leaked");
    check(glGetError() == GL_NO_ERROR, "Atlas upload error");
    g.BindBuffer(0x88EC, 0);
    g.DeleteBuffers(1, &unpack);
    glDeleteTextures(1, &atlas);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    g.ActiveTexture(GL_TEXTURE0);
    vanta::modules::hitfx::render_frame frame{};
    frame.viewport_w = frame.viewport_h = 128;
    frame.modelview[0] = frame.modelview[5] = frame.modelview[10] = frame.modelview[15] = 1;
    frame.projection[0] = frame.projection[5] = 1;
    frame.projection[10] = -1.0004f;
    frame.projection[11] = -1;
    frame.projection[14] = -.10002f;
    glViewport(0, 0, 128, 128);
    glReadBuffer(GL_BACK);
    int coloredProfiles = 0;
    atmosphere.enabled = true;
    for (int preset = 0; preset < 13; ++preset) {
        atmosphere.preset = preset;
        for (int i = 0; i < 300; ++i)
            model.update(atmosphere, 1.f / 60, 10, {1, 1, 1});
        for (int i = 0; i < 9; ++i) {
            glDepthMask(GL_TRUE);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            drawAtmosphereSky(frame);
            check(!atmosphereSkyError(), atmosphereSkyError() ? atmosphereSkyError() : "Sky error");
            check(glGetError() == GL_NO_ERROR, "Sky GL error");
        }
        std::vector<unsigned char> pixels(128 * 128 * 4);
        glReadPixels(0, 0, 128, 128, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        int colored = 0;
        for (int i = 0; i < 128 * 128; ++i)
            if (pixels[i * 4] + pixels[i * 4 + 1] + pixels[i * 4 + 2] > 3)
                ++colored;
        if (colored > 8)
            ++coloredProfiles;
    }
    check(coloredProfiles >= 2, "No sky pixels drawn");
    model.fade = model.tint = 1;
    model.weather.cv = 0;
    model.base = {1, .1f, .1f};
    glClearColor(.5f, .5f, .5f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    drawAtmosphereTint();
    unsigned char pixel[4]{};
    glReadPixels(64, 64, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    check(pixel[0] > pixel[1] * 2, "World tint did not draw");
    check(glGetError() == GL_NO_ERROR, "Tint GL error");
    shutdownAtmosphereGl();
    check(glGetError() == GL_NO_ERROR, "Sky cleanup GL error");
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(rc);
    ReleaseDC(window, dc);
    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    std::cout << "Native GL passed: texture state, 13 sky presets (" << coloredProfiles
              << " with visible pixels), world tint and cleanup.\n";
}
