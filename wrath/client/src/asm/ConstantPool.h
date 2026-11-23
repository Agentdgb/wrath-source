#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

namespace vanta::asm_ {

// Append-only extender that produces new bytes appended after an existing
// constant pool. Only *new* entries added by the transformer live here — the
// original CP bytes are re-serialized verbatim, so old indices remain valid.
//
// Only the entries needed to emit INVOKESTATIC HookBridge.onXxx( ... )I are
// supported: UTF8, Class, NameAndType, MethodRef.
class ConstantPoolExtender {
public:
    explicit ConstantPoolExtender(std::uint16_t existingCount);

    std::uint16_t addUtf8(std::string_view s);
    std::uint16_t addClass(std::string_view internalName);
    std::uint16_t addNameAndType(std::string_view name, std::string_view desc);
    std::uint16_t addMethodRef(std::string_view owner, std::string_view name, std::string_view desc);

    std::uint16_t newCount() const noexcept { return nextIndex_; }
    const std::vector<std::uint8_t>& appendBytes() const noexcept { return buf_; }
private:
    std::vector<std::uint8_t> buf_;
    std::uint16_t             nextIndex_;

    void   u1(std::uint8_t  v);
    void   u2(std::uint16_t v);
    void   utf8Body(std::string_view s);
};

} // namespace vanta::asm_
