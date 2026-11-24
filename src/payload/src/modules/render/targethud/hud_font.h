// hud_font.h — the Target HUD's named font pool.
//
// Every other pool in this client is tied to one typeface for its whole life:
// the arraylist borrows the gui's Proxima Bold, hitfx has one instance per
// display face. Nothing takes a font NAME, because until now nothing let the
// user pick one.
//
// This pool holds one face at a time and rebakes when the selection changes.
// The alternative — baking all of them up front so a switch is free — was
// rejected on atlas size: four faces across a size ladder at the
// oversampling this needs is the largest texture in the client by a wide
// margin, and it would be paid for on every injection to make an action the
// user takes once a session instant. A switch costs one rebuild, which is one
// hitch, at the moment the user asked for a change and is looking at it.

#pragma once

#include "imgui.h"

#include <cstddef>

namespace vanta::modules::targethud {

/* @the faces on offer.

   Four, and every one of them is compiled into the DLL.

   The list was fourteen: three files under assets/, ten read straight out of
   C:/Windows/Fonts, and ProggyClean. The Windows ten had to go. What is in that
   directory depends on the Windows edition, and an entry that resolves to
   nothing is worse than a shorter list — you pick a name, the HUD keeps the old
   face, and the setting reads as broken. Nothing in the list can do that now,
   because nothing in it is a file.

   Every entry also has to survive being a HUD: drawn at 15px over moving grass
   with nothing behind the glyphs. That is what rules out the display faces this
   repo also ships — Badaboom and Sniglet are poster type, and at that size they
   stop being legible and start being texture. What is left is two weights of a
   text face, one rounded, and one pixel. */
enum class face_id : int {
    proxima_bold = 0,
    proxima,
    comfortaa,
    minecraft,
    count
};

/* Display names, in enum order. This is the exact list the module's EnumSetting
   is built from, so the two can never disagree about what index 6 means. */
const char* const* face_names();
constexpr std::size_t face_count = static_cast<std::size_t>(face_id::count);

/* @select:
    asks for a face. Cheap and idempotent when it is already the current one;
    the rebake happens inside ensure(), on the render thread, because that is
    where the GL texture can be created. */
void select(face_id which);

/* @ensure:
    builds the atlas and uploads its texture if the selection changed. Safe to
    call every frame. False when the face could not be loaded at all — the
    caller should fall back to the arraylist pool rather than draw nothing. */
bool ensure();

/* @get:
    the bake whose size is the smallest still >= desired, or the largest baked
    if the caller wants more than the ladder covers. AddText renders at the
    requested size either way; the pick only decides which glyphs get sampled. */
ImFont* get(float desired_pixel_size);

/* The pool's own GL texture. Must be pushed on the draw list before any AddText
   from this pool — ImDrawList does not rebind atlases on its own, and the
   release build does not assert when it is wrong; the glyphs just come out of
   whatever texture was bound and look like garbage or nothing at all. */
ImTextureID texture_id();

void destroy();

} // namespace vanta::modules::targethud
