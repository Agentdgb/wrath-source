#include "gl_ext.h"

#include "util/Log.h"

namespace dt::al::gl {

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

    #define DT_AL_LOAD(name)                                              \
        table.name = resolve<PFN_gl##name>(opengl32, "gl" #name)

    DT_AL_LOAD(CreateShader);
    DT_AL_LOAD(DeleteShader);
    DT_AL_LOAD(ShaderSource);
    DT_AL_LOAD(CompileShader);
    DT_AL_LOAD(GetShaderiv);
    DT_AL_LOAD(GetShaderInfoLog);
    DT_AL_LOAD(CreateProgram);
    DT_AL_LOAD(DeleteProgram);
    DT_AL_LOAD(AttachShader);
    DT_AL_LOAD(LinkProgram);
    DT_AL_LOAD(GetProgramiv);
    DT_AL_LOAD(GetProgramInfoLog);
    DT_AL_LOAD(UseProgram);
    DT_AL_LOAD(GetUniformLocation);
    DT_AL_LOAD(Uniform1i);
    DT_AL_LOAD(Uniform1f);
    DT_AL_LOAD(Uniform2f);
    DT_AL_LOAD(Uniform3f);
    DT_AL_LOAD(Uniform4f);

    DT_AL_LOAD(GenVertexArrays);
    DT_AL_LOAD(DeleteVertexArrays);
    DT_AL_LOAD(BindVertexArray);
    DT_AL_LOAD(GenBuffers);
    DT_AL_LOAD(DeleteBuffers);
    DT_AL_LOAD(BindBuffer);
    DT_AL_LOAD(BufferData);
    DT_AL_LOAD(EnableVertexAttribArray);
    DT_AL_LOAD(DisableVertexAttribArray);
    DT_AL_LOAD(VertexAttribPointer);

    DT_AL_LOAD(GenFramebuffers);
    DT_AL_LOAD(DeleteFramebuffers);
    DT_AL_LOAD(BindFramebuffer);
    DT_AL_LOAD(FramebufferTexture2D);
    DT_AL_LOAD(CheckFramebufferStatus);

    DT_AL_LOAD(ActiveTexture);
    // BindSampler is 3.3-only — leave null on older contexts and treat that as
    // "don't try to clear the unit's sampler object".
    DT_AL_LOAD(BindSampler);

    #undef DT_AL_LOAD

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

} // namespace dt::al::gl
