#include "../../includes.hh"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

/* @vanta::emoji

   Colour emoji, rasterised on the CPU and uploaded as GL textures.

   Direct2D + DirectWrite is still the only thing on Windows that understands
   COLR/CPAL layered colour fonts, so it is still what draws the glyph. What
   changed with the move into Minecraft's OpenGL frame is the surface: the
   original rendered straight into a D3D texture and handed ImGui the shader
   resource view, and there is no D3D device here to do that with — the two APIs
   cannot share a surface.

   So the glyph is drawn into a WIC bitmap, which is plain system memory and
   needs no graphics device at all, and those pixels are uploaded as an ordinary
   GL texture. Slower on first use, identical afterwards, and cached per
   (sequence, size) exactly as before.

   The shipped font is the system one, Segoe UI Emoji. */

namespace vanta::emoji
{
	namespace
	{
		struct state_t
		{
			ID2D1Factory*       m_factory{};
			IWICImagingFactory* m_wic{};
			IDWriteFactory*     m_writer{};
			IDWriteTextFormat*  m_format{};

			float m_format_size{ 0.f };
			bool  m_ready{ false };
			bool  m_com_owned{ false };

			/* GLuint, not a COM view: 0 is the "tried and failed" marker, which
			   is cached too so a broken sequence is not re-rasterised every
			   frame. */
			std::unordered_map<std::string, GLuint> m_cache{};
		};

		state_t g_state{};

		/* @decode_utf8:
			walks one codepoint, returning how many bytes it consumed
		*/
		char32_t decode_utf8(const std::string& text, std::size_t& cursor)
		{
			const unsigned char lead = static_cast<unsigned char>(text[cursor]);

			int extra = 0;
			char32_t codepoint = 0;

			if (lead < 0x80) { codepoint = lead; extra = 0; }
			else if ((lead >> 5) == 0x06) { codepoint = lead & 0x1F; extra = 1; }
			else if ((lead >> 4) == 0x0E) { codepoint = lead & 0x0F; extra = 2; }
			else if ((lead >> 3) == 0x1E) { codepoint = lead & 0x07; extra = 3; }
			else { ++cursor; return 0xFFFD; }

			if (cursor + static_cast<std::size_t>(extra) >= text.size())
			{
				cursor = text.size();
				return 0xFFFD;
			}

			for (int index = 1; index <= extra; ++index)
			{
				const unsigned char continuation = static_cast<unsigned char>(text[cursor + index]);
				if ((continuation >> 6) != 0x02)
				{
					cursor += 1;
					return 0xFFFD;
				}

				codepoint = (codepoint << 6) | (continuation & 0x3F);
			}

			cursor += static_cast<std::size_t>(extra) + 1;
			return codepoint;
		}

		bool is_modifier(char32_t codepoint)
		{
			return codepoint == 0xFE0F                              // variation selector 16
				|| codepoint == 0xFE0E
				|| codepoint == 0x200D                              // zero width joiner
				|| (codepoint >= 0x1F3FB && codepoint <= 0x1F3FF)   // skin tones
				|| codepoint == 0x20E3;                             // combining keycap
		}

		/* @to_utf16:
			directwrite speaks utf-16
		*/
		std::wstring to_utf16(const std::string& text)
		{
			if (text.empty())
				return {};

			const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
				static_cast<int>(text.size()), nullptr, 0);
			if (length <= 0)
				return {};

			std::wstring result(static_cast<std::size_t>(length), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
				result.data(), length);

			return result;
		}

		bool ensure_format(float size)
		{
			if (g_state.m_format != nullptr && std::fabs(g_state.m_format_size - size) < 0.01f)
				return true;

			if (g_state.m_format != nullptr)
			{
				g_state.m_format->Release();
				g_state.m_format = nullptr;
			}

			const HRESULT result = g_state.m_writer->CreateTextFormat(
				L"Segoe UI Emoji", nullptr,
				DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
				size, L"en-us", &g_state.m_format);

			if (FAILED(result) || g_state.m_format == nullptr)
				return false;

			g_state.m_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
			g_state.m_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
			g_state.m_format_size = size;

			return true;
		}

		/* @upload_bgra:
			hands premultiplied BGRA pixels to GL as a texture.

			Direct2D only renders premultiplied, and ImGui blends with
			(SRC_ALPHA, ONE_MINUS_SRC_ALPHA) — which expects straight alpha. Left
			premultiplied, every emoji would come out with a dark fringe that
			gets worse the more transparent the pixel is, so the colour is
			divided back out here rather than special-casing the blend state for
			one kind of draw call.
		*/
		GLuint upload_bgra(std::vector<std::uint8_t>& pixels, int width, int height)
		{
			for (std::size_t i = 0; i + 3 < pixels.size(); i += 4)
			{
				const std::uint8_t a = pixels[i + 3];
				if (a == 0 || a == 255)
					continue;

				for (int c = 0; c < 3; ++c)
				{
					const int straight = (pixels[i + c] * 255 + a / 2) / a;
					pixels[i + c] = static_cast<std::uint8_t>(straight > 255 ? 255 : straight);
				}
			}

			GLuint texture = 0;
			glGenTextures(1, &texture);
			if (texture == 0)
				return 0;

			glBindTexture(GL_TEXTURE_2D, texture);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F /*GL_CLAMP_TO_EDGE*/);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F /*GL_CLAMP_TO_EDGE*/);
			glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
				0x80E1 /*GL_BGRA*/, GL_UNSIGNED_BYTE, pixels.data());
			glBindTexture(GL_TEXTURE_2D, 0);

			if (glGetError() != GL_NO_ERROR)
			{
				glDeleteTextures(1, &texture);
				return 0;
			}

			return texture;
		}

		/* @rasterise:
			draws one emoji sequence into system memory via WIC, returning BGRA
			pixels. No graphics device is involved anywhere in here.
		*/
		bool rasterise(const std::string& sequence, int pixel_size,
			std::vector<std::uint8_t>& out)
		{
			IWICBitmap*          bitmap{};
			ID2D1RenderTarget*   target{};
			ID2D1SolidColorBrush* brush{};
			IDWriteTextLayout*   layout{};
			IWICBitmapLock*      lock{};

			const auto cleanup = [&]()
				{
					if (lock   != nullptr) lock->Release();
					if (layout != nullptr) layout->Release();
					if (brush  != nullptr) brush->Release();
					if (target != nullptr) target->Release();
					if (bitmap != nullptr) bitmap->Release();
				};

			if (FAILED(g_state.m_wic->CreateBitmap(
					static_cast<UINT>(pixel_size), static_cast<UINT>(pixel_size),
					GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)))
			{
				cleanup();
				return false;
			}

			const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
				D2D1_RENDER_TARGET_TYPE_SOFTWARE,
				D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
				96.f, 96.f);

			// the glyph is inset a touch so round emoji do not touch the tile edges
			const float font_size = static_cast<float>(pixel_size) * 0.82f;

			const std::wstring wide = to_utf16(sequence);

			if (FAILED(g_state.m_factory->CreateWicBitmapRenderTarget(bitmap, properties, &target))
				|| !ensure_format(font_size)
				|| FAILED(target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush))
				|| wide.empty())
			{
				cleanup();
				return false;
			}

			/* A laid out run rather than DrawText: winuser.h owns the name
			   DrawText as a macro, and a layout also lets the format centre the
			   glyph inside the tile for us. */
			if (FAILED(g_state.m_writer->CreateTextLayout(
					wide.c_str(), static_cast<UINT32>(wide.size()), g_state.m_format,
					static_cast<float>(pixel_size), static_cast<float>(pixel_size), &layout))
				|| layout == nullptr)
			{
				cleanup();
				return false;
			}

			target->BeginDraw();
			target->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));
			target->DrawTextLayout(D2D1::Point2F(0.f, 0.f), layout, brush,
				D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);

			if (FAILED(target->EndDraw()))
			{
				cleanup();
				return false;
			}

			const WICRect rect{ 0, 0, pixel_size, pixel_size };
			if (FAILED(bitmap->Lock(&rect, WICBitmapLockRead, &lock)))
			{
				cleanup();
				return false;
			}

			UINT stride = 0;
			UINT buffer_size = 0;
			BYTE* source = nullptr;

			if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&buffer_size, &source))
				|| source == nullptr)
			{
				cleanup();
				return false;
			}

			out.resize(static_cast<std::size_t>(pixel_size) * pixel_size * 4);
			for (int row = 0; row < pixel_size; ++row)
			{
				std::memcpy(out.data() + static_cast<std::size_t>(row) * pixel_size * 4,
					source + static_cast<std::size_t>(row) * stride,
					static_cast<std::size_t>(pixel_size) * 4);
			}

			cleanup();
			return true;
		}
	}

	bool available() { return g_state.m_ready; }

	bool initialize()
	{
		if (g_state.m_ready)
			return true;

		/* WIC is COM, and this runs on the game's render thread, which may or
		   may not already have an apartment. RPC_E_CHANGED_MODE means the thread
		   is already initialised in the other mode — perfectly usable, and we
		   must not balance it with a CoUninitialize we do not owe. */
		const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		g_state.m_com_owned = SUCCEEDED(com);

		if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
			__uuidof(ID2D1Factory), nullptr,
			reinterpret_cast<void**>(&g_state.m_factory))))
		{
			slog::log::warning("emoji: direct2d is unavailable, emoji will not be drawn");
			shutdown();
			return false;
		}

		if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
				IID_PPV_ARGS(&g_state.m_wic)))
			|| FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
				reinterpret_cast<IUnknown**>(&g_state.m_writer))))
		{
			slog::log::warning("emoji: could not create the WIC/DirectWrite pipeline");
			shutdown();
			return false;
		}

		g_state.m_ready = true;
		slog::log::success("emoji: colour pipeline ready (Segoe UI Emoji, software raster)");

		return true;
	}

	void shutdown()
	{
		/* GL objects must die on the thread that owns the context. This is
		   called from c_app::shutdown, which the render thread runs with our
		   context current, so deleting here is correct. */
		for (auto& [key, texture] : g_state.m_cache)
		{
			if (texture != 0)
			{
				GLuint id = texture;
				glDeleteTextures(1, &id);
			}
		}
		g_state.m_cache.clear();

		if (g_state.m_format  != nullptr) { g_state.m_format->Release();  g_state.m_format = nullptr; }
		if (g_state.m_writer  != nullptr) { g_state.m_writer->Release();  g_state.m_writer = nullptr; }
		if (g_state.m_wic     != nullptr) { g_state.m_wic->Release();     g_state.m_wic = nullptr; }
		if (g_state.m_factory != nullptr) { g_state.m_factory->Release(); g_state.m_factory = nullptr; }

		if (g_state.m_com_owned)
		{
			CoUninitialize();
			g_state.m_com_owned = false;
		}

		g_state.m_ready = false;
	}

	ImTextureID texture(const std::string& sequence, int pixel_size)
	{
		if (!g_state.m_ready || sequence.empty() || pixel_size <= 0)
			return static_cast<ImTextureID>(0);

		const std::string key = sequence + "#" + std::to_string(pixel_size);

		if (const auto cached = g_state.m_cache.find(key); cached != g_state.m_cache.end())
			return static_cast<ImTextureID>(static_cast<std::uintptr_t>(cached->second));

		std::vector<std::uint8_t> pixels{};
		GLuint texture = 0;

		if (rasterise(sequence, pixel_size, pixels))
			texture = upload_bgra(pixels, pixel_size, pixel_size);

		// cached even on failure, so a sequence the font has no glyph for is not
		// re-rasterised on every single frame
		g_state.m_cache[key] = texture;

		if (texture == 0)
			slog::log::warning("emoji: could not bake a glyph at {}px", pixel_size);

		return static_cast<ImTextureID>(static_cast<std::uintptr_t>(texture));
	}

	bool is_emoji(char32_t codepoint)
	{
		return (codepoint >= 0x1F300 && codepoint <= 0x1FAFF)   // pictographs, emoticons, transport, supplemental
			|| (codepoint >= 0x1F000 && codepoint <= 0x1F02F)   // mahjong / dominoes
			|| (codepoint >= 0x1F0A0 && codepoint <= 0x1F0FF)   // playing cards
			|| (codepoint >= 0x1F100 && codepoint <= 0x1F1FF)   // enclosed alphanumerics, regional indicators
			|| (codepoint >= 0x2600 && codepoint <= 0x27BF)     // misc symbols and dingbats
			|| (codepoint >= 0x2B00 && codepoint <= 0x2BFF)
			|| (codepoint >= 0x2190 && codepoint <= 0x21FF)
			|| (codepoint >= 0x231A && codepoint <= 0x231B)
			|| (codepoint >= 0x23E9 && codepoint <= 0x23FA)
			|| (codepoint >= 0x25AA && codepoint <= 0x25FE)
			|| codepoint == 0x2934 || codepoint == 0x2935
			|| codepoint == 0x3030 || codepoint == 0x303D
			|| codepoint == 0x3297 || codepoint == 0x3299;
	}

	std::vector<run_t> segment(const std::string& text)
	{
		std::vector<run_t> runs{};

		std::string pending{};
		std::size_t cursor = 0;

		const auto flush_text = [&]()
			{
				if (!pending.empty())
				{
					runs.push_back(run_t{ pending, false });
					pending.clear();
				}
			};

		while (cursor < text.size())
		{
			const std::size_t start = cursor;
			const char32_t codepoint = decode_utf8(text, cursor);

			if (!is_emoji(codepoint))
			{
				pending.append(text, start, cursor - start);
				continue;
			}

			/* an emoji can carry modifiers and joined partners behind it, all of which belong to
			   the same picture and have to be handed to directwrite together */
			std::size_t end = cursor;

			while (end < text.size())
			{
				std::size_t probe = end;
				const char32_t next = decode_utf8(text, probe);

				if (is_modifier(next))
				{
					end = probe;

					// a joiner always drags its partner along
					if (next == 0x200D && probe < text.size())
					{
						std::size_t partner = probe;
						decode_utf8(text, partner);
						end = partner;
					}

					continue;
				}

				break;
			}

			flush_text();
			runs.push_back(run_t{ text.substr(start, end - start), true });

			cursor = end;
		}

		flush_text();
		return runs;
	}

	const std::vector<std::string>& palette()
	{
		static const std::vector<std::string> set{
			"😀", "😂", "🙂", "😊", "😍", "😎", "🤩", "🥳",
			"😭", "😤", "😱", "🤔", "🙃", "😴", "🤯", "🥶",
			"👍", "👎", "👌", "🙏", "👀", "💪", "🤝", "✌️",
			"❤️", "🔥", "⭐", "✨", "💜", "💀", "👻", "🎉",
			"🚀", "⚡", "💎", "🏆", "🎯", "🎮", "🕹️", "🖥️",
			"⌨️", "🖱️", "📎", "📌", "🔒", "🔑", "⚙️", "🧩",
			"🍕", "🍔", "☕", "🍺", "🌙", "🌈", "🌊", "🍀",
			"⚔️", "🛡️", "🏹", "💣", "🧨", "🩸", "👑", "💯"
		};

		return set;
	}
}
