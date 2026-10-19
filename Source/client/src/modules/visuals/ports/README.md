# Native visual ports

The runtime implementation is C++ inside `WrathPayload.dll`. `VisualModules.cpp`
registers MotionBlur, TotemSpirit, SwingAnimation, SlowSwing (included in the swing
source bundle), and 3D Character. Installation is deferred until a requested module
is enabled and the JVM is available.

The runtime bindings in this port target Fabric intermediary names from 1.21.11.
Other Minecraft releases have not been validated. Missing signatures are reported
in module descriptions rather than presented as successful activation.

## Rendering

* MotionBlur uses the original frame retention formula and dithering shader. Its
  native pass runs after world rendering, before the game's depth clear and HUD.
* TotemSpirit consumes status 35 and bundled packets. Queued packets retain their
  originating world, are filtered before allocation, and are sampled on the game
  thread. The original six-part pose, ascent, dissolve, shell, seal and crest are
  implemented in native code; the source bundle did not include GlyphCanvas or
  GlyphShapes, so the ankh drawing uses native geometric strokes.
* SwingAnimation replaces the first-person swing transform and adjusts only the
  local player's duration. The six curves are in `VisualMath.cpp`.
* 3D Character submits native geometry through Minecraft's command queue. Native
  callbacks replace the selected player model, selected feature layers and first
  person arms. Textures are registered in memory without a resource pack reload.

## Character assets

`tools/character-baker` retains the provided procedural design generator strictly
as an offline asset tool. No generator or Java module runs inside Minecraft. The
DLL embeds 21 baked meshes (seven characters, three detail levels) and seven PNG
atlases. The tiny JVM bridge classes in `tools/visual-bridges` contain native
method declarations and data fields; C++ owns the rendering and animation logic.

The input references `cj.anc` and `neighbor.anc` but does not contain either file.
Those characters are not advertised as available. The original custom GLTF/ANC
folder importer and folder-opening action have not been ported.

## Validation and remaining differences

The C++ regression target checks mirrored swing curves, endpoints, frame-rate
independent retention, spirit geometry, every baked mesh, bounds, indices and
truncated input rejection. A standalone JVM with the real 1.21.11 intermediary
game jar accepted the three bridge classes and verified copies of all six hooked
Minecraft classes. This verifies class compatibility, not in-game rendering.

Two code review passes covered state/matrix restoration, packet/world ownership,
lazy installation, JNI references, and teardown. The resulting fixes preserve
pixel-unpack state, unwind matrix pushes on failure, prevent stale-world totem
pops, and restore feature lists before unregistering native methods.

Full in-game visual validation is still required. Character lighting and some
joint/held-item pose handling use reduced native equivalents of the source;
custom skeleton import and the original lighting-context mixin are not included.
