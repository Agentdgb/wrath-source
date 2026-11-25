#pragma once
#include <cstdint>

namespace dt::asm_ {

// Full opcode enum for JVMS §6.5. Only the ones we actually emit or scan for
// are named; the numeric holes reflect the unused ones (breakpoint, wide, ...).
enum : std::uint8_t {
    OP_NOP           = 0x00,
    OP_ACONST_NULL   = 0x01,
    OP_ICONST_0      = 0x03,
    OP_ICONST_1      = 0x04,
    OP_ICONST_2      = 0x05,
    OP_LCONST_0      = 0x09,
    OP_FCONST_0      = 0x0B,
    OP_BIPUSH        = 0x10,
    OP_SIPUSH        = 0x11,
    OP_LDC           = 0x12,
    OP_LDC_W         = 0x13,
    OP_LDC2_W        = 0x14,
    OP_ILOAD         = 0x15,
    OP_ALOAD_0       = 0x2A,
    OP_ALOAD_1       = 0x2B,
    OP_ALOAD         = 0x19,
    OP_ISTORE        = 0x36,
    OP_ASTORE        = 0x3A,
    OP_POP           = 0x57,
    OP_DUP           = 0x59,
    OP_IFEQ          = 0x99,
    OP_IFNE          = 0x9A,
    OP_GOTO          = 0xA7,
    OP_IRETURN       = 0xAC,
    OP_FRETURN       = 0xAE,
    OP_ARETURN       = 0xB0,
    OP_RETURN        = 0xB1,
    OP_GETSTATIC     = 0xB2,
    OP_INVOKEVIRTUAL = 0xB6,
    OP_INVOKESPECIAL = 0xB7,
    OP_INVOKESTATIC  = 0xB8,
    OP_INVOKEINTERFACE = 0xB9,
    OP_NEW           = 0xBB,
    OP_CHECKCAST     = 0xC0,
    OP_WIDE          = 0xC4,
};

// Class file constant pool tags.
enum : std::uint8_t {
    CP_UTF8           = 1,
    CP_INTEGER        = 3,
    CP_FLOAT          = 4,
    CP_LONG           = 5,
    CP_DOUBLE         = 6,
    CP_CLASS          = 7,
    CP_STRING         = 8,
    CP_FIELDREF       = 9,
    CP_METHODREF      = 10,
    CP_INTERFACEMETHODREF = 11,
    CP_NAMEANDTYPE    = 12,
    CP_METHODHANDLE   = 15,
    CP_METHODTYPE     = 16,
    CP_DYNAMIC        = 17,
    CP_INVOKEDYNAMIC  = 18,
    CP_MODULE         = 19,
    CP_PACKAGE        = 20,
};

} // namespace dt::asm_
