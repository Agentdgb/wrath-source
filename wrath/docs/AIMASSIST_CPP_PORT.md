# AimAssist: independent mode engines

The module has two independent implementation trees and engines:

- aimassist/wind/Engine.h/.cpp owns the original Vanta Runtime, worker, shared state and lifecycle. Original algorithm/cache/debug source and extracted targeting helpers come from C:/Sources/Projects/DirectXColorRenderer/Modulos Source/Vanta Aimssist. Four original files are byte-identical, and the extracted targeting-function text also matches. See wind/SOURCE_IDENTITY.json. The header/control mutex and runtime bridge are host adaptations.
- aimassist/simple/Engine.h owns the existing mod-derived frame state and JNI adapter. Controllers.h, AimModel.h and MouseSensitivity.h moved into this folder without changing a byte. The adapter's namespace and class name changed to simple::Engine; its logic was retained.

AimAssist.cpp is the settings/activation dispatcher: it builds a Simple settings snapshot or a Vanta SettingsView, pauses the other engine, and calls exactly one engine per frame. Each mode retains separate stored values. Original Wind helpers remain in their original namespace to preserve source text; the owning runtime bridge is aimassist::wind::Engine and Simple is aimassist::simple::Engine. They do not share controller state, target references, prediction or output logic.

The previous legacy folder and loose root engine headers are removed from active source. Only wind and simple remain. The canonical reference for Wind is the user-provided Vanta Aimssist folder; Simple remains the existing C++ port of our Readable Aim mod with its previously accepted corrections.

Four host tests pass, including real-JVM OG rendering/worker lifecycle and a Wind-to-Simple-to-Wind sequence through the independent public engines. The portable Simple regression is separate. Tests do not replace live Minecraft/server validation or demonstrate anti-cheat compatibility. Original Wind uses its physical-click, sword-only and distance semantics; the current host still supplies focus/menu/activity guards and synchronization. Full-client timing is not claimed to be bit-identical to the reference executable.

Build: cmake --build build-wrath --config Release --target vantaclient --parallel 4
Checks: ctest --test-dir build-wrath -C Release -R ^wrath_aimassist_ --output-on-failure

Canonical source is wrath/client/src/modules/combat/AimAssist.cpp and aimassist/{simple,wind}. Source ZIP and both payload DLLs are deployed to build_out. Keep the separate Fabric AimAssist disabled while using this client.
