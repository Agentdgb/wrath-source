#include "NativeTextures.h"
#include "modules/render/arraylist/al_gl.h"
#include "core/assets/EmbeddedResource.h"
#include "vgui/framework/render/stb/stb_image.hh"
#include <memory>
#include <stdexcept>
namespace dt::modules::bloom {
unsigned loadNativeTexture(int resource, bool mipmaps, bool repeat) {
    const auto &g = al::gl::api();
    if (!g.ok)
        throw std::runtime_error("OpenGL texture API unavailable");
    auto data = assets::embeddedResource(resource);
    int width = 0, height = 0, channels = 0;
    std::unique_ptr<unsigned char, decltype(&stbi_image_free)> pixels(
        stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &width, &height, &channels, 4),
        stbi_image_free);
    if (!pixels || width < 1 || height < 1 || width > 8192 || height > 8192)
        throw std::runtime_error("Invalid native texture resource");
    GLint active = 0, old = 0, unpack = 0, alignment = 0, rowLength = 0, skipRows = 0, skipPixels = 0;
    glGetIntegerv(0x84E0, &active);
    g.ActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old);
    glGetIntegerv(0x88EF, &unpack);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    g.BindBuffer(0x88EC, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.get());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : 0x812F);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : 0x812F);
    auto proc = wglGetProcAddress("glGenerateMipmap");
    auto address = reinterpret_cast<std::intptr_t>(proc);
    auto generate = address > 3 ? reinterpret_cast<void(APIENTRY *)(GLenum)>(proc) : nullptr;
    bool haveMip = mipmaps && generate;
    if (haveMip) {
        glTexParameteri(GL_TEXTURE_2D, 0x813D, 4);
        generate(GL_TEXTURE_2D);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, haveMip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, old);
    g.BindBuffer(0x88EC, unpack);
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, skipRows);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels);
    g.ActiveTexture(active);
    if (!texture)
        throw std::runtime_error("Native texture allocation failed");
    return texture;
}
} // namespace dt::modules::bloom
