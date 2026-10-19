#pragma once

/* @vanta::emoji:
	colour emoji support.

	the font atlas is rasterized by stb_truetype, which only understands monochrome outlines, so a
	colour emoji can never be a glyph in it. instead every emoji is rendered on its own through
	direct2d + directwrite (the only path on windows that understands COLR/CPAL layered fonts),
	straight into a d3d texture, and cached per size.

	the shipped font is the system one, Segoe UI Emoji. apple's set is not redistributable, so it
	is not bundled; drop an emoji font at assets/fonts/emoji.ttf and it takes priority.
*/
namespace vanta::emoji
{
	/* @initialize:
		builds the direct2d device on top of the existing d3d11 one. returns false when the
		platform cannot do colour text, in which case the gui simply draws no emoji.
	*/
	bool initialize();

	/* @shutdown:
		releases every cached texture and the direct2d objects
	*/
	void shutdown();

	/* @available:
		false when initialize() failed
	*/
	bool available();

	/* @texture:
		the colour bitmap for one emoji sequence at an exact pixel size, baked on first use
	*/
	ImTextureID texture(const std::string& sequence, int pixel_size);

	/* @is_emoji:
		true for codepoints that belong to a pictographic block
	*/
	bool is_emoji(char32_t codepoint);

	/* @run_t:
		a stretch of a string that is either plain text or one whole emoji sequence.
		a sequence can be several codepoints: a base plus a variation selector, a skin tone
		modifier, or joiners, and it has to stay together to render as one picture.
	*/
	struct run_t
	{
		std::string m_text{};
		bool m_emoji{ false };
	};

	/* @segment:
		splits utf-8 text into alternating text and emoji runs
	*/
	std::vector<run_t> segment(const std::string& text);

	/* @palette:
		the set offered by the picker
	*/
	const std::vector<std::string>& palette();
}
