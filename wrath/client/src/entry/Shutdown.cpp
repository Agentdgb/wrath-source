// The full ordered shutdown lives in entry/Bootstrap.cpp::shutdown(), and the
// self-unload lives next to it in unloadOnThisThread() — it has to, because the
// unload has to wait for the render thread to release the GL objects and for
// every other spawned thread to leave the image first.
//
// This TU used to hold a "spawn a thread that FreeLibraryAndExitThread's after
// 50 ms" trampoline. It was unreferenced, and it was also wrong twice over: the
// delay was a guess rather than a wait for anything in particular, and the
// thread was a std::thread, which MSVC's STL pins the module for — so its
// FreeLibraryAndExitThread could never have brought the count to zero. See
// platform/ThreadUtil.h for the measurement behind that.

namespace vanta::entry {
} // namespace vanta::entry
