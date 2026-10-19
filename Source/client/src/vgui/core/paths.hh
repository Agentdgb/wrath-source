#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

/* @core::paths:
	asset resolution (in-memory embedded VFS with optional disk fallback).
*/
namespace core
{
	struct embedded_asset_view_t
	{
		const std::uint8_t* data{ nullptr };
		std::size_t size{ 0 };
		constexpr bool valid() const noexcept { return data != nullptr && size > 0; }
	};

	/* @find_embedded_asset:
	resolves a relative asset path (e.g. "fonts/proxima.ttf", "icons/render.svg") directly
	to its compiled-in byte array in .rdata without touching the filesystem.
	*/
	embedded_asset_view_t find_embedded_asset(std::string_view relative) noexcept;

	/* @locate_asset_root:
	no-op success when assets are embedded in memory.
	*/
	bool locate_asset_root();

	/* @asset_root:
		the resolved root, without a trailing separator. empty when using embedded VFS.
	*/
	const std::string& asset_root();

	/* @asset_path:
	returns the relative asset key when using in-memory VFS, or disk path if overridden.
	*/
	std::string asset_path(std::string_view relative);

	/* @scrub_paths:
	clears and zeroes cached asset directory strings on client shutdown.
	*/
	void scrub_paths();
}
