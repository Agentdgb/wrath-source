#include "asm/StackMapRebuilder.h"

namespace vanta::asm_ {

std::vector<std::uint8_t> emitRecipeAStackMap(const std::uint8_t* originalStackMap, std::size_t originalLen) {
    std::vector<std::uint8_t> out;

    // number_of_entries: u2 (existingEntries + 1)
    std::uint16_t existing = 0;
    if (originalLen >= 2 && originalStackMap) {
        existing = static_cast<std::uint16_t>((originalStackMap[0] << 8) | originalStackMap[1]);
    }
    std::uint16_t total = static_cast<std::uint16_t>(existing + 1);
    out.push_back(static_cast<std::uint8_t>(total >> 8));
    out.push_back(static_cast<std::uint8_t>(total & 0xFF));

    // Our prepended frame: F_SAME with delta=6 (single byte 6).
    out.push_back(6);

    if (existing == 0 || originalLen < 2) return out;

    // For the first existing frame we need to subtract 7 from its offset delta
    // and re-emit (its tag may change if the delta drops below the tag's range).
    const std::uint8_t* p = originalStackMap + 2;
    std::uint8_t        tag = *p;
    std::size_t         firstEntryLen = 1;

    auto originalDelta = [&]() -> int {
        if (tag <= 63)            return tag;                          // SAME_FRAME
        if (tag >= 64 && tag <= 127) return tag - 64;                  // SAME_LOCALS_1_STACK_ITEM
        if (tag == 247) {                                              // SAME_LOCALS_1_STACK_ITEM_EXTENDED
            return (p[1] << 8) | p[2];
        }
        if (tag >= 248 && tag <= 250) {                                // CHOP
            return (p[1] << 8) | p[2];
        }
        if (tag == 251) {                                              // SAME_FRAME_EXTENDED
            return (p[1] << 8) | p[2];
        }
        if (tag >= 252 && tag <= 254) {                                // APPEND
            return (p[1] << 8) | p[2];
        }
        if (tag == 255) {                                              // FULL_FRAME
            return (p[1] << 8) | p[2];
        }
        return -1;
    }();

    if (originalDelta < 0) {
        // Unknown/unsupported tag — pass through unchanged, adjust nothing.
        out.insert(out.end(), originalStackMap + 2, originalStackMap + originalLen);
        return out;
    }
    int newDelta = originalDelta - 7;
    if (newDelta < 0) newDelta = 0;

    auto emitExtendedSame = [&](int d) {
        out.push_back(251);
        out.push_back(static_cast<std::uint8_t>((d >> 8) & 0xFF));
        out.push_back(static_cast<std::uint8_t>(d & 0xFF));
    };

    if (tag <= 63) {
        if (newDelta <= 63) { out.push_back(static_cast<std::uint8_t>(newDelta)); }
        else                { emitExtendedSame(newDelta); }
        firstEntryLen = 1;
    } else {
        // For anything else we just rewrite the delta in-place if possible.
        // (In v0.1 we keep the same tag and let the JVM verifier catch a
        // malformed edge case. If StackMapRebuilder fails, HookManager falls
        // back to skipping the transform and the module reports unavailable.)
        std::size_t len = 3;
        switch (tag) {
            case 247:                        len = 4; break;
            case 248: case 249: case 250:    len = 3; break;
            case 251:                        len = 3; break;
            case 252: case 253: case 254:    len = 3 + (tag - 251); break;
            case 255: {
                std::uint16_t locals  = (p[3] << 8) | p[4];
                std::size_t   ofsAfter = 5;
                for (std::uint16_t i = 0; i < locals; ++i) {
                    std::uint8_t t = p[ofsAfter++];
                    if (t == 7 || t == 8) ofsAfter += 2;
                }
                std::uint16_t stackN = (p[ofsAfter] << 8) | p[ofsAfter + 1];
                ofsAfter += 2;
                for (std::uint16_t i = 0; i < stackN; ++i) {
                    std::uint8_t t = p[ofsAfter++];
                    if (t == 7 || t == 8) ofsAfter += 2;
                }
                len = ofsAfter;
                break;
            }
        }
        out.push_back(tag);
        out.push_back(static_cast<std::uint8_t>((newDelta >> 8) & 0xFF));
        out.push_back(static_cast<std::uint8_t>(newDelta & 0xFF));
        out.insert(out.end(), p + 3, p + len);
        firstEntryLen = len;
    }

    // Copy the remaining frames verbatim.
    out.insert(out.end(), p + firstEntryLen, originalStackMap + originalLen);
    return out;
}

} // namespace vanta::asm_
