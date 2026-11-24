// Recipe B — wraps the final return of a value-returning method so a nonzero
// bridge return replaces the value using an AltSlot ThreadLocal<long[]>.
//
// Not shipped in v0.1. When wired, this recipe is used for GetFov, EyeHeight,
// and Damage-arg replacement. See docs/hook_layer.md §Recipe B for the
// pre-assembled bytecode template and StackMap analysis.

namespace vanta::hook::recipes {

// int applyRecipeB_ReturnRewrite(...) — TODO.

} // namespace vanta::hook::recipes
