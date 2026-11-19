// TestPayload.cpp -- Verification DLL for Manual-Map + /MT correctness.
//
// Injected via BypassLoaderDbg.exe into any x64 process (use a benign process
// like notepad.exe for testing -- no Minecraft required for phase 1/2).
//
// Exercises exactly the failure modes that Manual Map + /MT exposes:
//
//  Test 1 -- thread_local read/write
//    Requires TLS index allocation and per-thread storage. /MT CRT allocates
//    this in _DllMainCRTStartup. Failure = AV on first thread_local access.
//
//  Test 2 -- throw caught by __try/__except (SEH + CXX exception interop)
//    Requires _pDestructExceptionObject and the MSVC EH runtime from /MT CRT.
//    Failure = unhandled exception / crash in ntdll exception dispatcher.
//    NOTE: without RtlAddFunctionTable the C++ throw can still work because
//    the /MT CRT registers its own frame; __try/__except is SEH-based and
//    works without .pdata registration. This test verifies both coexist.
//
//  Test 3 -- std::thread start + join
//    Requires /MT CRT thread-startup (_beginthreadex path), thread-local
//    CRT state per new thread, and join (WaitForSingleObject on thread handle).
//    Failure = hang, crash in _beginthread, or CRT heap corruption.
//
//  Test 4 -- std::string heap allocation
//    std::string with length > SSO threshold (> 15 chars) forces heap alloc
//    via /MT CRT malloc -> HeapAlloc. Verifies CRT heap is initialized.
//    Failure = AV in RtlAllocateHeap (the crash we fixed in the main payload).
//
//  Test 5 -- PayloadInit export reachable
//    The loader resolves PayloadInit by name after injection. If the EAT is
//    corrupt or the export RVA is wrong, this call never happens. The test
//    sets a flag that PayloadGetResults reads back.
//
// Results are written to a named shared-memory section:
//   "Local\BypassWrathTest_<pid>"
// Read by the loader (BypassLoaderDbg.exe --test) or by any process with
// OpenFileMapping rights on the section.

#include <windows.h>
#include <thread>
#include <string>
#include <stdexcept>
#include <atomic>
#include <cstdint>
#include <cstring>

// ── Result block (matches what the loader reads) ───────────────────────────
#pragma pack(push, 1)
struct TestResults {
    uint32_t magic;             // 0x54455354 "TEST"
    uint8_t  test1_tls;        // 1 = pass
    uint8_t  test2_seh;        // 1 = pass
    uint8_t  test3_thread;     // 1 = pass
    uint8_t  test4_heap;       // 1 = pass
    uint8_t  test5_export;     // 1 = pass (set by PayloadInit)
    uint8_t  all_done;         // 1 = all tests completed
    uint8_t  _pad[2];
    char     detail[256];      // null-terminated error detail if any test fails
};
#pragma pack(pop)

static constexpr uint32_t kMagic = 0x54455354u;

// ── Shared memory ──────────────────────────────────────────────────────────
static HANDLE        g_hSection  = nullptr;
static TestResults*  g_results   = nullptr;

static void OpenResultsSection() {
    wchar_t name[64];
    _snwprintf_s(name, 64, _TRUNCATE,
        L"Local\\BypassWrathTest_%u", GetCurrentProcessId());

    g_hSection = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, sizeof(TestResults), name);
    if (!g_hSection) return;

    g_results = reinterpret_cast<TestResults*>(
        MapViewOfFile(g_hSection, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(TestResults)));
    if (g_results) {
        memset(g_results, 0, sizeof(TestResults));
        g_results->magic = kMagic;
    }
}

// ── Test 1: thread_local ───────────────────────────────────────────────────
thread_local int32_t tls_value = 0;

static bool RunTest1_ThreadLocal() {
    // Write on current thread
    tls_value = 0xDEAD;
    if (tls_value != 0xDEAD) return false;

    // Write on a new thread, verify isolation
    std::atomic<bool> ok{false};
    std::thread t([&] {
        tls_value = 0xBEEF;
        ok = (tls_value == 0xBEEF);
    });
    t.join();

    // Original thread value must be unchanged
    return ok.load() && (tls_value == 0xDEAD);
}

// ── Test 2: throw caught by __try/__except ─────────────────────────────────
// MSVC constraint: __try and C++ objects with destructors cannot coexist in
// the same function scope. Solution: inner function handles C++ throw/catch;
// outer function wraps it in __try to verify SEH frame is not corrupted.

static bool InnerCxxThrow() {
    // C++ throw + catch in a function with no __try
    try {
        throw std::runtime_error("test_seh");
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

static bool RunTest2_SEH() {
    // __try wrapper: verifies the SEH frame is intact after Manual Map.
    // If CRT exception tables are broken, the SEH dispatcher faults here.
    bool caught = false;
    __try {
        caught = InnerCxxThrow();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Landing here means SEH intercepted what should have been a C++ catch
        caught = false;
    }
    return caught;
}

// ── Test 3: std::thread start + join ──────────────────────────────────────
static bool RunTest3_StdThread() {
    std::atomic<bool> ran{false};
    std::thread t([&] {
        Sleep(10);  // Minimal work -- verify the thread actually ran
        ran = true;
    });
    t.join();
    return ran.load();
}

// ── Test 4: std::string heap allocation ───────────────────────────────────
static bool RunTest4_Heap() {
    // SSO threshold in MSVC is 15 chars; force heap path with 32 chars
    std::string s(32, 'A');
    s += std::string(32, 'B');
    if (s.size() != 64) return false;
    // Verify content (proves the heap round-trip is correct)
    for (int i = 0; i < 32; ++i) if (s[i] != 'A') return false;
    for (int i = 32; i < 64; ++i) if (s[i] != 'B') return false;
    return true;
}

// ── Test runner thread ─────────────────────────────────────────────────────
static DWORD WINAPI RunTests(LPVOID) {
    OpenResultsSection();
    if (!g_results) return 1;

    // Test 1
    g_results->test1_tls = RunTest1_ThreadLocal() ? 1 : 0;

    // Test 2
    g_results->test2_seh = RunTest2_SEH() ? 1 : 0;

    // Test 3
    g_results->test3_thread = RunTest3_StdThread() ? 1 : 0;

    // Test 4
    g_results->test4_heap = RunTest4_Heap() ? 1 : 0;

    // Test 5 is set by PayloadInit (called by loader after injection)
    // Wait up to 3s for it
    for (int i = 0; i < 60 && !g_results->test5_export; ++i) Sleep(50);

    g_results->all_done = 1;

    // Write summary to detail field
    _snprintf_s(g_results->detail, sizeof(g_results->detail), _TRUNCATE,
        "TLS=%d SEH=%d Thread=%d Heap=%d Export=%d",
        g_results->test1_tls,
        g_results->test2_seh,
        g_results->test3_thread,
        g_results->test4_heap,
        g_results->test5_export);

    return 0;
}

// ── Exports ────────────────────────────────────────────────────────────────

// Called by the loader (or manually) after injection.
// Sets test5 flag in shared memory.
extern "C" __declspec(dllexport) void PayloadInit() {
    // May be called before RunTests writes g_results.
    // Spin briefly to get the pointer.
    for (int i = 0; i < 100 && !g_results; ++i) Sleep(10);
    if (g_results) g_results->test5_export = 1;

    // Wait up to 1s for the test runner to finish (all_done flag).
    for (int i = 0; i < 20 && g_results && !g_results->all_done; ++i) Sleep(50);

    // ── Log file: proof of execution ─────────────────────────────────────────
    // Win32 only -- CreateFileW is safe under Manual Map; std::ofstream is not.
    HANDLE hLog = CreateFileW(
        L"C:\\Users\\Admin\\Desktop\\BypassWrath\\test_log.txt",
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,          // create if absent, open if present
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (hLog != INVALID_HANDLE_VALUE) {
        // Seek to end for append semantics.
        SetFilePointer(hLog, 0, nullptr, FILE_END);

        // Build the log line.
        char line[256];
        DWORD pid = GetCurrentProcessId();
        uint8_t tls    = g_results ? g_results->test1_tls    : 0;
        uint8_t seh    = g_results ? g_results->test2_seh    : 0;
        uint8_t thread = g_results ? g_results->test3_thread : 0;
        uint8_t heap   = g_results ? g_results->test4_heap   : 0;

        int len = _snprintf_s(line, sizeof(line), _TRUNCATE,
            "[BYPASSWITH TEST] PID=%u TLS=%d SEH=%d Thread=%d Heap=%d Export=1\n",
            pid, tls, seh, thread, heap);

        if (len > 0) {
            DWORD written = 0;
            WriteFile(hLog, line, static_cast<DWORD>(len), &written, nullptr);
        }
        CloseHandle(hLog);
    }
}

// Called by the loader to read results after PayloadInit.
// Returns pointer to the shared mapping (valid in the loader's address space
// only if the section is opened by name -- the loader must call
// OpenFileMappingW("Local\BypassWrathTest_<pid>") itself to read the struct).
// In-process use: returns local mapped view address (for testing from same process).
extern "C" __declspec(dllexport) const TestResults* PayloadGetResults() {
    return g_results;
}

// ── DllMain ─────────────────────────────────────────────────────────────────
BOOL APIENTRY DllMain(HMODULE /*hMod*/, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE t = CreateThread(nullptr, 0, RunTests, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
