#pragma once

// The other half of the same namespace. Pulled in here so a view that asks for
// the head also has name() to hand — they are always drawn together.
#include "core/profile/PlayerProfile.h"

/* @dt::profile (gui half):
	the account head as a GL texture.

	core/profile/PlayerProfile owns the other half — the name from the JVM and
	the PNG from the network, both on a worker thread. Only this half may touch
	GL, so the handover is decoded pixels, picked up on the first frame after
	they land.
*/
namespace dt::profile
{
	/* @head_texture:
		the account head, or 0 while it is still unknown. Uploads on the first
		call after the fetch completes, so it must be called from the render
		thread — which is the only place anything draws anyway.
	*/
	ImTextureID head_texture();

	/* @release_head_texture:
		drops the GL texture. Must run on the render thread, with the context
		current — c_app::shutdown is such a place.
	*/
	void release_head_texture();
}
