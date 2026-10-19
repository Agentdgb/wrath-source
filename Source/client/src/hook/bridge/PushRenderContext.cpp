// pushRenderContext(float[] view, float[] projection, double camX, camY, camZ,
//                   float partialTicks)
// is called from a bytecode splice inside LevelRenderer's tail so the
// SwapBuffers-side WorldRenderer sees the current camera matrices.
//
// Not shipped in v0.1 — WorldRenderer draws in screen space (foreground draw
// list) only. When enabled this file registers a native and publishes the
// captured frame into render::RenderContext.

