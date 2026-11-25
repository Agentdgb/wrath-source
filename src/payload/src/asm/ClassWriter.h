#pragma once
#include "asm/ClassReader.h"
#include "asm/ConstantPool.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace dt::asm_ {

// Rewrites a class file, replacing one method's Code attribute with a new
// bytecode+stackmap sequence and appending new constant pool entries.
// Non-modified sections are copied verbatim from the original bytes.
struct RewriteOneMethod {
    std::uint16_t                targetMethod;    // index into ClassView::methods
    std::vector<std::uint8_t>    newCode;         // full code[] bytes (no header)
    std::uint16_t                newMaxStack;
    std::uint16_t                newMaxLocals;
    std::vector<std::uint8_t>    newStackMap;     // optional; empty = no StackMapTable attribute
    ConstantPoolExtender         cpAdditions;

    RewriteOneMethod(const ClassView& v)
        : targetMethod(0), newMaxStack(0), newMaxLocals(0), cpAdditions(v.cpCount) {}
};

class ClassWriter {
public:
    // Emits the modified class file bytes. On failure returns an empty vector.
    static std::vector<std::uint8_t> emit(const ClassView& v, const RewriteOneMethod& r);
};

} // namespace dt::asm_
