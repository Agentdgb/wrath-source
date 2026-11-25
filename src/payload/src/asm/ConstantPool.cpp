#include "asm/ConstantPool.h"

#include "asm/Opcodes.h"

namespace dt::asm_ {

ConstantPoolExtender::ConstantPoolExtender(std::uint16_t existingCount)
    : nextIndex_(existingCount) {}

void ConstantPoolExtender::u1(std::uint8_t v)  { buf_.push_back(v); }
void ConstantPoolExtender::u2(std::uint16_t v) { buf_.push_back(static_cast<std::uint8_t>(v >> 8)); buf_.push_back(static_cast<std::uint8_t>(v & 0xFF)); }

void ConstantPoolExtender::utf8Body(std::string_view s) {
    u2(static_cast<std::uint16_t>(s.size()));
    for (char c : s) buf_.push_back(static_cast<std::uint8_t>(c));
}

std::uint16_t ConstantPoolExtender::addUtf8(std::string_view s) {
    u1(CP_UTF8);
    utf8Body(s);
    return nextIndex_++;
}

std::uint16_t ConstantPoolExtender::addClass(std::string_view internalName) {
    std::uint16_t nameIdx = addUtf8(internalName);
    u1(CP_CLASS);
    u2(nameIdx);
    return nextIndex_++;
}

std::uint16_t ConstantPoolExtender::addNameAndType(std::string_view name, std::string_view desc) {
    std::uint16_t n = addUtf8(name);
    std::uint16_t d = addUtf8(desc);
    u1(CP_NAMEANDTYPE);
    u2(n);
    u2(d);
    return nextIndex_++;
}

std::uint16_t ConstantPoolExtender::addMethodRef(std::string_view owner, std::string_view name, std::string_view desc) {
    std::uint16_t cls = addClass(owner);
    std::uint16_t nat = addNameAndType(name, desc);
    u1(CP_METHODREF);
    u2(cls);
    u2(nat);
    return nextIndex_++;
}

} // namespace dt::asm_
