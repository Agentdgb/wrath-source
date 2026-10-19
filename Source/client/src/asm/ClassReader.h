#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

namespace vanta::asm_ {

// Non-owning view over a Java class file. Parses just enough to locate methods
// and their Code attributes; leaves the rest as opaque byte spans that
// ClassWriter re-serializes verbatim.
struct ConstantPoolEntryView {
    std::uint8_t                tag       = 0;
    std::uint32_t               byteOffset= 0; // start of the entry in the class file
    std::uint32_t               byteSize  = 0; // total encoded size incl. tag
};

struct MethodView {
    std::uint16_t     accessFlags = 0;
    std::uint16_t     nameIndex   = 0;
    std::uint16_t     descIndex   = 0;
    std::uint32_t     structOffset= 0;  // where this method_info entry begins
    std::uint32_t     structSize  = 0;  // total bytes for this method_info
    std::uint32_t     codeAttrOffset = 0; // offset of Code attribute inside method
    std::uint32_t     codeAttrLen    = 0; // bytes of Code attribute including 6-byte header
    std::uint32_t     codeOffset     = 0; // offset of code[] bytes
    std::uint32_t     codeLen        = 0;
    std::uint16_t     maxStack       = 0;
    std::uint16_t     maxLocals      = 0;
};

struct ClassView {
    const std::uint8_t*                data     = nullptr;
    std::size_t                        size     = 0;
    std::uint16_t                      minor    = 0;
    std::uint16_t                      major    = 0;
    std::uint16_t                      cpCount  = 0;
    std::vector<ConstantPoolEntryView> pool;    // pool[0] unused (JVMS index 1..cpCount-1 used)
    std::uint16_t                      accessFlags = 0;
    std::uint16_t                      thisClassIdx = 0;
    std::uint16_t                      superClassIdx = 0;
    std::uint32_t                      methodsOffset = 0;
    std::uint16_t                      methodsCount  = 0;
    std::vector<MethodView>            methods;

    std::string_view                   thisClassName() const noexcept;
    std::string_view                   utf8At(std::uint16_t idx) const noexcept;
};

class ClassReader {
public:
    // Parses `bytes` into `out`. Returns false on any structural error; the
    // caller must not attempt to use `out` in that case.
    static bool parse(const std::uint8_t* bytes, std::size_t len, ClassView& out);
};

} // namespace vanta::asm_
