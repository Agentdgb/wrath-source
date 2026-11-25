#include "asm/ClassWriter.h"

#include "util/Log.h"

#include <cstring>

namespace dt::asm_ {

namespace {

void appendU2(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}
void appendU4(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}
void appendBytes(std::vector<std::uint8_t>& out, const std::uint8_t* p, std::size_t n) {
    out.insert(out.end(), p, p + n);
}

} // namespace

std::vector<std::uint8_t> ClassWriter::emit(const ClassView& v, const RewriteOneMethod& r) {
    if (r.targetMethod >= v.methods.size()) {
        VLOG_ERR("asm", "ClassWriter: target method OOB");
        return {};
    }
    const auto& tgt = v.methods[r.targetMethod];
    if (tgt.codeAttrOffset == 0) {
        VLOG_ERR("asm", "ClassWriter: target method has no Code attribute");
        return {};
    }

    std::vector<std::uint8_t> out;
    out.reserve(v.size + r.newCode.size() + r.cpAdditions.appendBytes().size() + 64);

    // Header up to and including cp_count.
    appendBytes(out, v.data, 8);
    std::uint16_t oldCpCount = v.cpCount;
    std::uint16_t newCpCount = r.cpAdditions.newCount();
    appendU2(out, newCpCount);

    // Existing constant pool (indices 1..oldCpCount-1). We know its end from
    // the pool[oldCpCount-1] entry (or from the first non-CP byte if the pool
    // ends earlier). Simplest: pool ends at pool[N].byteOffset + pool[N].byteSize
    // where N is the last valid entry.
    std::uint32_t cpEndOff = 10; // after magic+ver+cpCount
    for (std::uint16_t i = 1; i < oldCpCount; ++i) {
        if (v.pool[i].byteSize) cpEndOff = v.pool[i].byteOffset + v.pool[i].byteSize;
    }
    appendBytes(out, v.data + 10, cpEndOff - 10);

    // Appended CP entries.
    const auto& add = r.cpAdditions.appendBytes();
    appendBytes(out, add.data(), add.size());

    // Everything from cpEndOff up to the target method's structOffset — access,
    // this/super/interfaces/fields/attributes, plus preceding methods.
    appendBytes(out, v.data + cpEndOff, tgt.structOffset - cpEndOff);

    // Rebuild the target method's method_info with a rewritten Code attribute.
    appendU2(out, tgt.accessFlags);
    appendU2(out, tgt.nameIndex);
    appendU2(out, tgt.descIndex);

    // Collect attributes into a temp buffer so we can emit their count.
    std::vector<std::uint8_t> attrBuf;

    // 1) Rewritten Code attribute.
    {
        std::vector<std::uint8_t> codeBody;
        appendU2(codeBody, r.newMaxStack);
        appendU2(codeBody, r.newMaxLocals);
        appendU4(codeBody, static_cast<std::uint32_t>(r.newCode.size()));
        appendBytes(codeBody, r.newCode.data(), r.newCode.size());
        appendU2(codeBody, 0);  // exception_table_length — we do not preserve exceptions in v0.1

        // Inner attributes of Code — optionally the StackMapTable.
        std::vector<std::uint8_t> innerAttrBuf;
        std::uint16_t             innerAttrCount = 0;
        if (!r.newStackMap.empty()) {
            std::uint16_t nameIdx = 0;
            for (std::uint16_t i = 1; i < v.cpCount; ++i) {
                if (v.pool[i].tag != 1 /*CP_UTF8*/) continue;
                if (v.utf8At(i) == "StackMapTable") { nameIdx = i; break; }
            }
            if (nameIdx == 0) {
                // The CP has no StackMapTable UTF8 yet — synthesize one via cpAdditions.
                // (In v0.1 that would require re-serializing the appended CP; document
                // it and fall through without a stack map. JVM verification will fail
                // if the target method needs one — Recipe A avoids this by placing the
                // splice at offset 0 where F_SAME is trivially valid.)
                VLOG_WARN("asm", "Class has no 'StackMapTable' UTF8 in CP; stack map dropped");
            } else {
                appendU2(innerAttrBuf, nameIdx);
                appendU4(innerAttrBuf, static_cast<std::uint32_t>(r.newStackMap.size()));
                appendBytes(innerAttrBuf, r.newStackMap.data(), r.newStackMap.size());
                ++innerAttrCount;
            }
        }
        appendU2(codeBody, innerAttrCount);
        appendBytes(codeBody, innerAttrBuf.data(), innerAttrBuf.size());

        std::uint16_t codeNameIdx = 0;
        for (std::uint16_t i = 1; i < v.cpCount; ++i) {
            if (v.pool[i].tag == 1 /*CP_UTF8*/ && v.utf8At(i) == "Code") { codeNameIdx = i; break; }
        }
        if (codeNameIdx == 0) { VLOG_ERR("asm", "No 'Code' UTF8 in CP"); return {}; }
        appendU2(attrBuf, codeNameIdx);
        appendU4(attrBuf, static_cast<std::uint32_t>(codeBody.size()));
        appendBytes(attrBuf, codeBody.data(), codeBody.size());
    }

    // 2) Preserve any non-Code attributes on the original method.
    {
        const std::uint8_t* mp    = v.data + tgt.structOffset;
        std::uint16_t       attrs = (mp[6] << 8) | mp[7];
        const std::uint8_t* aptr  = mp + 8;
        std::uint16_t       kept  = 1;   // Code (just emitted above)
        for (std::uint16_t i = 0; i < attrs; ++i) {
            std::uint16_t nameIdx = (aptr[0] << 8) | aptr[1];
            std::uint32_t len     = (static_cast<std::uint32_t>(aptr[2]) << 24) |
                                    (static_cast<std::uint32_t>(aptr[3]) << 16) |
                                    (static_cast<std::uint32_t>(aptr[4]) << 8)  |
                                    (static_cast<std::uint32_t>(aptr[5]));
            auto name = v.utf8At(nameIdx);
            if (name != "Code") {
                appendBytes(attrBuf, aptr, 6 + len);
                ++kept;
            }
            aptr += 6 + len;
        }
        appendU2(out, kept);
        appendBytes(out, attrBuf.data(), attrBuf.size());
    }

    // Trailing bytes after the rewritten method: subsequent methods + class attrs.
    std::uint32_t rest = tgt.structOffset + tgt.structSize;
    appendBytes(out, v.data + rest, v.size - rest);

    return out;
}

} // namespace dt::asm_
