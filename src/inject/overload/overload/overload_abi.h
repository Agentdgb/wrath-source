#pragma once
#include <cstdint>

#define OVERLOAD_MAGIC 0x4F564C44u   // 'OVLD'
#define OVERLOAD_PARAMS_END 0x80

#pragma pack(push, 1)
struct OverloadParams {
    uint32_t magic;
    uint32_t pad;
    uint64_t imageBase;          // IN  base devuelta por NtMapViewOfSection
    uint64_t imageSize;          // IN  SizeOfImage
    uint64_t epRva;              // IN  AddressOfEntryPoint (0 = sin EP)
    uint64_t excRva;             // IN  RVA de .pdata
    uint64_t excCount;           // IN  nº de RUNTIME_FUNCTION
    uint64_t tlsCbPtr;           // IN  VA ya relocalizada del array de callbacks
    uint64_t bootstrapNameRva;   // IN  RVA dentro del blob stompeado
    uint64_t shutdownNameRva;    // IN  RVA dentro del blob stompeado
    uint64_t funcTable;          // OUT tabla registrada (0 si no se registró)
    uint64_t shutdownPtr;        // OUT export resuelto
    uint64_t teardownPtr;        // OUT dirección absoluta de StubTeardown
    uint64_t ldrGetDll;          // OUT LdrGetDllHandle
    uint64_t ldrGetProc;         // OUT LdrGetProcedureAddress
};
#pragma pack(pop)

static_assert(sizeof(OverloadParams) == 0x70, "layout OverloadParams != 0x70");
static_assert(sizeof(OverloadParams) <= OVERLOAD_PARAMS_END, "params solapan el stub");
