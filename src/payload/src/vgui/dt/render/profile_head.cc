#include "../../includes.hh"

#include "core/profile/PlayerProfile.h"

/* @dt::profile (gui half)

   The head texture, split off from core/profile/PlayerProfile deliberately.

   That half runs on a worker thread: it asks the JVM for the account name and
   pulls the head PNG over HTTPS. It cannot create the texture, because a GL
   object may only be made on the thread that owns the context — the game's
   render thread, which is where this half runs. So the worker leaves decoded
   pixels behind and the first frame that asks for the head picks them up. */

namespace dt::profile
{
	namespace
	{
		GLuint g_head = 0;
	}

	ImTextureID head_texture()
	{
		if (::dt::profile::hasPendingHead())
		{
			std::vector<std::uint8_t> rgba{};
			int width = 0, height = 0;

			if (::dt::profile::takeHead(rgba, width, height) && width > 0 && height > 0)
			{
				// a re-fetch replaces the old one rather than leaking it
				if (g_head != 0)
				{
					glDeleteTextures(1, &g_head);
					g_head = 0;
				}

				GLuint texture = 0;
				glGenTextures(1, &texture);

				if (texture != 0)
				{
					glBindTexture(GL_TEXTURE_2D, texture);

					/* NEAREST, not LINEAR. A Minecraft head is pixel art; the
					   bilinear filter every other texture here uses would turn
					   an 8x8 face into mush. */
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F /*GL_CLAMP_TO_EDGE*/);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F /*GL_CLAMP_TO_EDGE*/);
					glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
					glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
						GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
					glBindTexture(GL_TEXTURE_2D, 0);

					if (glGetError() != GL_NO_ERROR)
					{
						glDeleteTextures(1, &texture);
						texture = 0;
						slog::log::warning("profile: could not upload the head texture");
					}
					else
					{
						slog::log::success("profile: head texture ready ({}x{})", width, height);
					}
				}

				g_head = texture;
			}
		}

		return static_cast<ImTextureID>(static_cast<std::uintptr_t>(g_head));
	}

	void release_head_texture()
	{
		if (g_head != 0)
		{
			glDeleteTextures(1, &g_head);
			g_head = 0;
		}
	}
}
