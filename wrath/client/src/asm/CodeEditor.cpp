#include "asm/CodeEditor.h"

#include "asm/Opcodes.h"

namespace vanta::asm_ {

bool emitRecipeA_PreCancel(
    const ClassView& v,
    const MethodView& target,
    std::string_view bridgeOwner,
    std::string_view bridgeName,
    std::string_view bridgeDesc,
    std::uint8_t     returnOpcode,
    RewriteOneMethod& r)
{
    // Bytecode we prepend at offset 0:
    //   INVOKESTATIC HookBridge.<name><desc>   (returns int)
    //   IFEQ +N                                (N = offset past our RETURN)
    //   <returnOpcode>                         (RETURN / IRETURN / FRETURN / ARETURN)
    //
    // The original code[] then follows unchanged.
    std::uint16_t methodRef = r.cpAdditions.addMethodRef(bridgeOwner, bridgeName, bridgeDesc);

    std::vector<std::uint8_t>& out = r.newCode;
    out.reserve(target.codeLen + 8);

    // INVOKESTATIC methodRef
    out.push_back(OP_INVOKESTATIC);
    out.push_back(static_cast<std::uint8_t>(methodRef >> 8));
    out.push_back(static_cast<std::uint8_t>(methodRef & 0xFF));
    // IFEQ +N — N is the branch offset from the position of the IFEQ itself.
    //   IFEQ is 3 bytes; the return we skip is 1 byte -> target relative to IFEQ = +4
    // so operand = 4.
    out.push_back(OP_IFEQ);
    out.push_back(0x00);
    out.push_back(0x04);
    out.push_back(returnOpcode);

    // Original code[] appended verbatim. This does not fix branch offsets in the
    // original body (branches are self-relative and target offsets are unchanged
    // relative to the start of the original code, but their instruction offsets
    // shift by 7 bytes — every RELATIVE branch stays valid because both source
    // and target shift by the same amount). Absolute-address consumers
    // (StackMapTable, LineNumberTable) DO shift; StackMapTable is regenerated
    // as F_SAME at offset 7 in a follow-up step (Templates.cpp).
    out.insert(out.end(), v.data + target.codeOffset, v.data + target.codeOffset + target.codeLen);

    // Stack effect: our INVOKESTATIC returns 1 int; IFEQ consumes it. So we
    // need at least 1 stack slot beyond the method's original maxStack.
    r.newMaxStack  = static_cast<std::uint16_t>(target.maxStack + 1);
    r.newMaxLocals = target.maxLocals;
    return true;
}

} // namespace vanta::asm_
