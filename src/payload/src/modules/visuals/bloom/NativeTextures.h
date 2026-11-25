#pragma once
namespace dt::modules::bloom {
// Call on the owning GL context; the caller owns the returned texture name.
unsigned loadNativeTexture(int resource, bool mipmaps = true, bool repeat = false);
} // namespace dt::modules::bloom
