#pragma once
#include <cstdint>
#include <vector>

namespace dt::asm_ {

// Fast-path stack-map rebuilder for Recipe A. All Recipe A splices place their
// join at the very first original instruction (offset +7 after the injected
// prelude), where the frame is exactly the same as the method-entry frame.
// A single StackMapTable entry with tag F_SAME at delta=6 is therefore always
// valid, and we emit that fixed sequence unconditionally.
//
// For methods whose original StackMapTable already had frames, we prepend our
// F_SAME_EXTENDED(delta=6) entry and adjust the *first* subsequent frame's
// delta accordingly (subtracting 7). Downstream frames remain untouched.
std::vector<std::uint8_t> emitRecipeAStackMap(
    const std::uint8_t* originalStackMap, std::size_t originalLen);

} // namespace dt::asm_
