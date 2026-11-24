#include "gl_ext.h"

#include "util/Log.h"

namespace vanta::al::gl {

namespace {

// wglGetProcAddress returns null OR one of the sentinel values (1, 2, 3, -1)
// on some drivers for missing symbols. The base OpenGL32.dll exports are also
// distinct — for those, wglGetProcAddress returns NULL and you have to fall
// back to GetProcAddress on the DLL itself.
template <typename T>
T resolve(HMODULE opengl32, const char* name)
{
    PROC p = wglGetProcAddress(name);
    if (p == nullptr ||
        p == reinterpret_cast<PROC>(1) ||
        p == reinterpret_cast<PROC>(2) ||
        p == reinterpret_cast<PROC>(3) ||
        p == reinterpret_cast<PROC>(-1))
    {
        if (opengl32 != nullptr)
            p = GetProcAddress(opengl32, name);
    }
    return reinterpret_cast<T>(p);
}

} // namespace

const funcs& load()
{
    static funcs table;
    static bool tried = false;

    if (tried)
        return table;

    tried = true;

    HMODULE opengl32 = GetModuleHandleA("opengl32.dll");

    #define VANTA_AL_LOAD(name)                                              \
        table.name = resolve<PFN_gl##name>(opengl32, "gl" #name)

    VANTA_AL_LOAD(CreateShader);
    VANTA_AL_LOAD(DeleteShader);
    VANTA_AL_LOAD(ShaderSource);
    VANTA_AL_LOAD(CompileShader);
    VANTA_AL_LOAD(GetShaderiv);
    VANTA_AL_LOAD(GetShaderInfoLog);
    VANTA_AL_LOAD(CreateProgram);
    VANTA_AL_LOAD(DeleteProgram);
    VANTA_AL_LOAD(AttachShader);
    VANTA_AL_LOAD(LinkProgram);
    VANTA_AL_LOAD(GetProgramiv);
    VANTA_AL_LOAD(GetProgramInfoLog);
    VANTA_AL_LOAD(UseProgram);
    VANTA_AL_LOAD(GetUniformLocation);
    VANTA_AL_LOAD(Uniform1i);
    VANTA_AL_LOAD(Uniform1f);
    VANTA_AL_LOAD(Uniform2f);
    VANTA_AL_LOAD(Uniform3f);
    VANTA_AL_LOAD(Uniform4f);

    VANTA_AL_LOAD(GenVertexArrays);
    VANTA_AL_LOAD(DeleteVertexArrays);
    VANTA_AL_LOAD(BindVertexArray);
    VANTA_AL_LOAD(GenBuffers);
    VANTA_AL_LOAD(DeleteBuffers);
    VANTA_AL_LOAD(BindBuffer);
    VANTA_AL_LOAD(BufferData);
    VANTA_AL_LOAD(EnableVertexAttribArray);
    VANTA_AL_LOAD(DisableVertexAttribArray);
    VANTA_AL_LOAD(VertexAttribPointer);

    VANTA_AL_LOAD(GenFramebuffers);
    VANTA_AL_LOAD(DeleteFramebuffers);
    VANTA_AL_LOAD(BindFramebuffer);
    VANTA_AL_LOAD(FramebufferTexture2D);
    VANTA_AL_LOAD(CheckFramebufferStatus);

    VANTA_AL_LOAD(ActiveTexture);
    // BindSampler is 3.3-only — leave null on older contexts and treat that as
    // "don't try to clear the unit's sampler object".
    VANTA_AL_LOAD(BindSampler);

    #undef VANTA_AL_LOAD

    /* Presence of the mandatory pieces of the pipeline is what "ok" means.
       BindSampler is optional (only used to clear another client's sampler on
       unit 0) so we don't gate on it. */
    table.ok =
        table.CreateShader && table.CreateProgram && table.CompileShader &&
        table.LinkProgram  && table.UseProgram    && table.GetUniformLocation &&
        table.GenVertexArrays && table.BindVertexArray && table.GenBuffers &&
        table.BindBuffer && table.BufferData && table.EnableVertexAttribArray &&
        table.VertexAttribPointer && table.GenFramebuffers &&
        table.BindFramebuffer && table.FramebufferTexture2D &&
        table.CheckFramebufferStatus && table.ActiveTexture;

    if (!table.ok)
        VLOG_WARN("arraylist", "GL 3.0+ loader could not resolve every symbol; pipeline disabled");

    return table;
}

} // namespace vanta::al::gl
