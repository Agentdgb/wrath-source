#include "asm/ClassReader.h"

#include "asm/Opcodes.h"
#include "util/Log.h"

#include <cstring>

namespace dt::asm_ {

namespace {

struct Cursor {
    const std::uint8_t* p;
    const std::uint8_t* end;
    bool ok = true;

    std::uint8_t  u1() {
        if (p + 1 > end) { ok = false; return 0; }
        return *p++;
    }
    std::uint16_t u2() {
        if (p + 2 > end) { ok = false; return 0; }
        std::uint16_t v = static_cast<std::uint16_t>((p[0] << 8) | p[1]);
        p += 2;
        return v;
    }
    std::uint32_t u4() {
        if (p + 4 > end) { ok = false; return 0; }
        std::uint32_t v = static_cast<std::uint32_t>(
            (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
            (std::uint32_t{p[2]} << 8)  | (std::uint32_t{p[3]}));
        p += 4;
        return v;
    }
    void skip(std::size_t n) {
        if (p + n > end) { ok = false; return; }
        p += n;
    }
};

bool skipAttributes(Cursor& c, std::size_t base) {
    std::uint16_t count = c.u2();
    for (std::uint16_t i = 0; i < count && c.ok; ++i) {
        c.u2();                   // attribute_name_index
        std::uint32_t len = c.u4();
        c.skip(len);
        (void)base;
    }
    return c.ok;
}

} // namespace

std::string_view ClassView::utf8At(std::uint16_t idx) const noexcept {
    if (idx == 0 || idx >= pool.size()) return {};
    const auto& e = pool[idx];
    if (e.tag != CP_UTF8 || e.byteSize < 3) return {};
    const std::uint8_t* p = data + e.byteOffset + 1;    // skip tag
    std::uint16_t len     = static_cast<std::uint16_t>((p[0] << 8) | p[1]);
    return {reinterpret_cast<const char*>(p + 2), len};
}

std::string_view ClassView::thisClassName() const noexcept {
    if (thisClassIdx == 0 || thisClassIdx >= pool.size()) return {};
    const auto& cls = pool[thisClassIdx];
    if (cls.tag != CP_CLASS || cls.byteSize < 3) return {};
    std::uint16_t nameIdx = static_cast<std::uint16_t>(
        (data[cls.byteOffset + 1] << 8) | data[cls.byteOffset + 2]);
    return utf8At(nameIdx);
}

bool ClassReader::parse(const std::uint8_t* bytes, std::size_t len, ClassView& out) {
    if (!bytes || len < 10) return false;

    Cursor c{bytes, bytes + len};
    std::uint32_t magic = c.u4();
    if (magic != 0xCAFEBABE) { VLOG_TRACE("asm", "bad magic {:x}", magic); return false; }

    out.data  = bytes;
    out.size  = len;
    out.minor = c.u2();
    out.major = c.u2();
    out.cpCount = c.u2();

    out.pool.assign(out.cpCount, {});
    for (std::uint16_t i = 1; i < out.cpCount && c.ok; ++i) {
        std::uint32_t start = static_cast<std::uint32_t>(c.p - bytes);
        std::uint8_t  tag   = c.u1();
        std::uint32_t size  = 1;
        switch (tag) {
            case CP_UTF8: { auto ln = c.u2(); c.skip(ln); size = 3u + ln; break; }
            case CP_INTEGER: case CP_FLOAT:                        c.skip(4); size = 5; break;
            case CP_LONG: case CP_DOUBLE:                          c.skip(8); size = 9; break;
            case CP_CLASS: case CP_STRING: case CP_MODULE: case CP_PACKAGE:
                                                                   c.skip(2); size = 3; break;
            case CP_METHODTYPE:                                    c.skip(2); size = 3; break;
            case CP_FIELDREF: case CP_METHODREF: case CP_INTERFACEMETHODREF:
            case CP_NAMEANDTYPE: case CP_DYNAMIC: case CP_INVOKEDYNAMIC:
                                                                   c.skip(4); size = 5; break;
            case CP_METHODHANDLE:                                  c.skip(3); size = 4; break;
            default: VLOG_TRACE("asm", "unknown cp tag {}", tag); return false;
        }
        out.pool[i] = {tag, start, size};
        if (tag == CP_LONG || tag == CP_DOUBLE) ++i; // double slot
    }
    if (!c.ok) return false;

    out.accessFlags   = c.u2();
    out.thisClassIdx  = c.u2();
    out.superClassIdx = c.u2();

    // Skip interfaces
    std::uint16_t ifCount = c.u2();
    c.skip(ifCount * 2);

    // Skip fields
    std::uint16_t fieldCount = c.u2();
    for (std::uint16_t i = 0; i < fieldCount && c.ok; ++i) {
        c.u2(); c.u2(); c.u2(); // access, name, desc
        if (!skipAttributes(c, static_cast<std::size_t>(c.p - bytes))) return false;
    }

    // Methods
    out.methodsOffset = static_cast<std::uint32_t>(c.p - bytes);
    out.methodsCount  = c.u2();
    out.methods.reserve(out.methodsCount);
    for (std::uint16_t i = 0; i < out.methodsCount && c.ok; ++i) {
        MethodView mv;
        mv.structOffset = static_cast<std::uint32_t>(c.p - bytes);
        mv.accessFlags  = c.u2();
        mv.nameIndex    = c.u2();
        mv.descIndex    = c.u2();
        std::uint16_t attrCount = c.u2();
        for (std::uint16_t a = 0; a < attrCount && c.ok; ++a) {
            std::uint32_t attrStart = static_cast<std::uint32_t>(c.p - bytes);
            std::uint16_t nameIdx   = c.u2();
            std::uint32_t attrLen   = c.u4();
            auto attrName = out.utf8At(nameIdx);
            if (attrName == "Code") {
                mv.codeAttrOffset = attrStart;
                mv.codeAttrLen    = 6 + attrLen;
                mv.maxStack       = c.u2();
                mv.maxLocals      = c.u2();
                std::uint32_t codeLen = c.u4();
                mv.codeOffset     = static_cast<std::uint32_t>(c.p - bytes);
                mv.codeLen        = codeLen;
                c.skip(codeLen);
                std::uint16_t exceptions = c.u2();
                c.skip(exceptions * 8);
                if (!skipAttributes(c, static_cast<std::size_t>(c.p - bytes))) return false;
            } else {
                c.skip(attrLen);
            }
        }
        mv.structSize = static_cast<std::uint32_t>(c.p - bytes) - mv.structOffset;
        out.methods.push_back(mv);
    }
    return c.ok;
}

} // namespace dt::asm_
