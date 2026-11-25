/*
 *  MinHook - The Minimalistic API Hooking Library for x64/x86
 *  Modified for B-1: bump allocator over .tramp MEM_IMAGE section (no VirtualAlloc RWX).
 */

#include <windows.h>
#include "buffer.h"

#define PAGE_EXECUTE_FLAGS \
    (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)

#pragma section(".tramp", execute, read)
__declspec(allocate(".tramp")) __declspec(align(4096))
volatile unsigned char g_tramp_pool[16384] = { 0 };

static size_t g_tramp_offset = 0;

VOID InitializeBuffer(VOID)
{
    g_tramp_offset = 0;
}

VOID MH_LockTrampPool(VOID)
{
    DWORD oldProt = 0;
    VirtualProtect((LPVOID)g_tramp_pool, sizeof(g_tramp_pool), PAGE_EXECUTE_READ, &oldProt);
}

VOID UninitializeBuffer(VOID)
{
    MH_LockTrampPool();
    g_tramp_offset = 0;
}

LPVOID AllocateBuffer(LPVOID pOrigin)
{
    (void)pOrigin;
    size_t aligned = (g_tramp_offset + 15) & ~(size_t)15;
    if (aligned + MEMORY_SLOT_SIZE > sizeof(g_tramp_pool))
        return NULL;

    DWORD oldProt = 0;
    if (!VirtualProtect((LPVOID)g_tramp_pool, sizeof(g_tramp_pool), PAGE_EXECUTE_READWRITE, &oldProt))
        return NULL;

    LPVOID pSlot = (LPVOID)(g_tramp_pool + aligned);
    g_tramp_offset = aligned + MEMORY_SLOT_SIZE;

#ifdef _DEBUG
    memset(pSlot, 0xCC, MEMORY_SLOT_SIZE);
#endif
    return pSlot;
}

VOID FreeBuffer(LPVOID pBuffer)
{
    (void)pBuffer;
    // Pool is backed by MEM_IMAGE (.tramp) and freed on module unload.
}

BOOL IsExecutableAddress(LPVOID pAddress)
{
    MEMORY_BASIC_INFORMATION mi;
    VirtualQuery(pAddress, &mi, sizeof(mi));

    return (mi.State == MEM_COMMIT && (mi.Protect & PAGE_EXECUTE_FLAGS));
}
