#include "services.h"
#include "baseline.h"

#include <iostream>
#include <iomanip>
#include <string>

using namespace wrath::gate;

namespace {

void print_row(const std::wstring& name, const std::wstring& expected, const std::wstring& actual, bool passed, const std::wstring& detail = L"") {
    std::wcout << std::left << std::setw(16) << name << L"| "
               << std::setw(15) << expected << L"| "
               << std::setw(15) << actual << L"| "
               << std::setw(10) << (passed ? L"PASS" : L"FAIL") << L"| "
               << detail << std::endl;
}

int do_check(bool silent) {
    auto audit = audit_services();
    auto configs = audit_system_config();

    size_t total = audit.results.size() + configs.size();
    size_t passed = 0;

    if (!silent) {
        std::wcout << std::left << std::setw(16) << L"NOMBRE" << L"| "
                   << std::setw(15) << L"ESPERADO" << L"| "
                   << std::setw(15) << L"REAL" << L"| "
                   << std::setw(10) << L"VEREDICTO" << L"| DETALLE" << std::endl;
        std::wcout << L"----------------|----------------|----------------|-----------|--------" << std::endl;
    }

    for (const auto& r : audit.results) {
        if (r.passed) passed++;
        if (!silent) {
            print_row(r.name, r.expected, r.actual, r.passed, r.detail);
        }
    }

    for (const auto& c : configs) {
        if (c.passed) passed++;
        if (!silent) {
            print_row(c.name, c.expected, c.actual, c.passed, c.detail);
        }
    }

    int exit_code = audit.all_passed ? 0 : 1;

    if (!silent) {
        std::wcout << std::endl;
        std::wcout << L"Resumen: " << passed << L"/" << total << L" PASS | "
                   << L"Mascara Servicios: 0x" << std::hex << std::setw(5) << std::setfill(L'0') << audit.fail_mask
                   << L" | " << (exit_code == 0 ? L"PASS GLOBAL" : L"FAIL") << std::endl;
    }

    return exit_code;
}

int do_baseline(int argc, wchar_t* argv[]) {
    wchar_t loader_path[MAX_PATH]{};
    const wchar_t* baseline_file = L"baseline.dat";

    if (argc >= 3) {
        wcscpy_s(loader_path, argv[2]);
    } else {
        GetModuleFileNameW(nullptr, loader_path, MAX_PATH);
    }

    if (argc >= 4) {
        baseline_file = argv[3];
    }

    std::wcout << L"Capturando baseline de: " << loader_path << std::endl;
    std::wcout << L"Archivo de destino: " << baseline_file << std::endl;

    if (capture_baseline(loader_path, baseline_file)) {
        std::wcout << L"[PASS] Baseline capturado exitosamente en " << baseline_file << std::endl;
        return 0;
    } else {
        std::wcout << L"[FAIL] Error capturando baseline de artefactos." << std::endl;
        return 4;
    }
}

int do_diff(int argc, wchar_t* argv[]) {
    const wchar_t* baseline_file = L"baseline.dat";
    if (argc >= 3) {
        baseline_file = argv[2];
    }

    std::wcout << L"Comparando estado actual con: " << baseline_file << std::endl;
    auto diff = compare_with_baseline(baseline_file);

    std::wcout << std::left << std::setw(25) << L"ITEM" << L"| "
               << std::setw(20) << L"BASELINE" << L"| "
               << std::setw(20) << L"ACTUAL" << L"| "
               << L"ESTADO" << std::endl;
    std::wcout << L"-------------------------|---------------------|---------------------|--------" << std::endl;

    for (const auto& d : diff.diffs) {
        std::wcout << std::left << std::setw(25) << d.item << L"| "
                   << std::setw(20) << d.baseline_val.substr(0, 18) << L"| "
                   << std::setw(20) << d.current_val.substr(0, 18) << L"| "
                   << (d.match ? L"PASS (0)" : L"DIFF (+1)") << std::endl;
    }

    std::wcout << std::endl;
    if (diff.identical) {
        std::wcout << L"Resumen: [DIFF 0] Cero discrepancias respecto al baseline. PASS." << std::endl;
        return 0;
    } else {
        std::wcout << L"Resumen: [DIFF " << diff.mismatch_count << L"] discrepancias encontradas. FAIL." << std::endl;
        return 2;
    }
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) {
        std::wcout << L"Uso: gate.exe [/check | /baseline [ruta] | /diff [archivo] | /silent]" << std::endl;
        return 4;
    }

    std::wstring mode = argv[1];
    if (mode == L"/check") {
        return do_check(false);
    } else if (mode == L"/silent") {
        return do_check(true);
    } else if (mode == L"/baseline") {
        return do_baseline(argc, argv);
    } else if (mode == L"/diff") {
        return do_diff(argc, argv);
    } else {
        std::wcout << L"Modo no reconocido: " << mode << std::endl;
        std::wcout << L"Uso: gate.exe [/check | /baseline | /diff | /silent]" << std::endl;
        return 4;
    }
}
