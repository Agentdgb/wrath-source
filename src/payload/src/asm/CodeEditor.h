#pragma once
#include "asm/ClassReader.h"
#include "asm/ClassWriter.h"

#include <cstdint>
#include <string_view>

namespace vanta::asm_ {

// Emit method-entry INVOKESTATIC to HookBridge.<name><desc> and branch out of
// the method with the appropriate RETURN opcode when the bridge returns nonzero.
// Recipe A — used for tick/key/packet/attack/chat/... hooks (Cancellable path).
//
// On success, populates r.newCode + r.newMaxStack + r.newMaxLocals so the
// caller can ClassWriter::emit() the resulting class.
bool emitRecipeA_PreCancel(
    const ClassView& v,
    const MethodView& target,
    std::string_view bridgeInternalName,   // e.g. "jdk/internal/math/DoublePrecision"
    std::string_view bridgeMethodName,
    std::string_view bridgeDesc,           // e.g. "()I"
    std::uint8_t     returnOpcode,         // OP_RETURN / OP_IRETURN / OP_ARETURN / ...
    RewriteOneMethod& r);

} // namespace vanta::asm_
