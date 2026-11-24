#pragma once
#ifndef WRATH_GATE_BASELINE_H
#define WRATH_GATE_BASELINE_H

#include <windows.h>
#include <string>
#include <vector>

namespace wrath::gate {

struct BaselineDiffEntry {
    std::wstring item;
    std::wstring baseline_val;
    std::wstring current_val;
    bool match;
};

struct BaselineDiffResult {
    std::vector<BaselineDiffEntry> diffs;
    size_t mismatch_count;
    bool identical;
};

// Captura el estado baseline limpio y lo guarda en disco (.dat)
bool capture_baseline(const wchar_t* loader_path, const wchar_t* baseline_path);

// Compara el estado actual del sistema con el archivo de baseline capturado
BaselineDiffResult compare_with_baseline(const wchar_t* baseline_path);

} // namespace wrath::gate

#endif // WRATH_GATE_BASELINE_H
