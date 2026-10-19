# Native visual ports

Runtime: C++ in WrathPayload.dll, using the existing JNI/JVMTI bridge and
Minecraft's OpenGL context. No external Java mod, JAR, resource pack or resource
reload is installed. Bindings were verified against the locally installed
Fabric/Yarn **Minecraft 1.21.11** classes. Other 1.21 versions remain unverified.

## Modules

- **WorldParticles:** DriftPassCache simulation, five particle families, density,
  theme/manual color, visibility fade and ground height.
- **DeathEffects:** EchoPassCache simulation: Soul, Shockwave, Shatter, Lightning,
  Supernova, Void and Random, with scale, amount, eight-effect cap, sounds,
  optional flash, body hiding and living-entity filters. Status-3 packets retain
  their originating world; collision, sound and entity sampling run on the game
  thread.
- **TargetESP:** the existing module exposes HaloPassCache's seven styles:
  Orbits, Star, Wings, Sparkles, Ring, Sweep and Bolts, with target/self/both,
  color, size, speed, opacity and glow. The native attack observer supplies the
  target for three seconds.
- **FallingPetals:** SceneLayerCache simulation, Normal/Heavy density and GUI
  matrix transforms. The complete original 1024x2048 atlas and 49 branch anchor
  records replace the earlier adapted flower atlas and empty emitter data.
- **Atmosphere:** NodeZoneRenderer's Tint/Solid, original palettes/theme, separate
  top color, rainbow cycle, cloud tint, stars, fog density and world tint.
  Explicit fog distances remain available. The original LerpZoneBuffer
  environment profiles and ShellArrayCache sky geometry are native C++:
  Blizzard, Inferno, Sakura, Galaxy, Aurora, Autumn, Fireflies, Sandstorm,
  Blood Moon, Synthwave, Dusk, Frost and Neon, plus None. These control sky/fog,
  stars, gradients, celestial angles and the applicable galaxy, aurora, cloud,
  moon, meteor and celestial texture layers. Profile strength, density and
  detail are adjustable. Visual time changes environment angles without
  changing world/server time. Fluid fog and world transitions are handled.

## Complete-source integration

The supplied Bloomclient-buildable.zip provided the missing pure numeric
classes: PaletteArrayCache, FacetArrayCache, BatchArrayCache, SpanArrayCache,
StrataArrayCache, RibbonArrayCache, SectorArrayCache, SliceArrayCache and
ShellArrayCache, plus the environment configuration. Their C++ ports are in
native/. Source copies and translation scripts live in client/tools/bloom-port.
The source copies are provenance inputs and do not run in Minecraft.

wrath_bloom_asset_baker generates the original particle/glyph atlas, branch/
petal atlas, seven sky textures, branch anchors and star geometry using the C++
numeric kernels and Windows' PNG encoder. These outputs are embedded as DLL
resources. Span's offline image workers run sequentially. Runtime texture
uploads restore texture units, pixel-unpack buffer, alignment, row length and
skip state; GPU objects are released on their owning context.

## Remaining differences

This ports the requested visual modules and their sky dependencies. The separate
foreground weather system (rain/snow/ambient weather particles and weather
sounds), GUI weather overlays and decorative static branches are not implemented.
Some presets therefore change fog/environment without an additional textured sky
layer. The original separate bloom postprocessing is not reproduced; effects use
their original sprite artwork with native additive/alpha passes. The death flash
retains simplified camera compensation. Unrelated HeadESP code is not exposed.

## Review and verification

Two review passes covered JNI signatures and callback lifetimes, world ownership,
GPU state and cleanup, then source math, preset interpolation, shared arrays,
UVs, sprite extents, settings, submersion and disable transitions. Corrections
include restoring alpha blending before MotionBlur, restoring all pixel-unpack
layout state, independent preset buffers and clearing celestial masks on exit.

Automated regression targets cover storage, existing core behavior, SwordReplace,
character/visual math, world particles, all target/death styles, both petal
densities with the original anchors, 13 sky profiles, Atmosphere transitions,
fog and visual time. The hidden OpenGL test executes the actual native sky
renderer across all presets, reads rendered pixels, tests world tint and checks
texture-state restoration and cleanup. Shader compilation and embedded resources
are checked separately.

Standalone JVM probes verify the actual 1.21.11 methods/fields and JNIHook-style
class copies under -Xverify:all, including the sky renderer class. These checks
do **not** constitute an in-game visual test or a guarantee for other versions.
