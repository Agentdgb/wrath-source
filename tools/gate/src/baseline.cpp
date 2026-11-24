#include "baseline.h"
#include "trace/snapshot.h"

#include <iostream>
#include <iomanip>
#include <sstream>

namespace wrath::gate {

namespace {

std::wstring to_hex(const uint8_t* data, size_t len) {
    if (!data || len == 0) return L"(vacio)";
    std::wstringstream ss;
    for (size_t i = 0; i < len; ++i) {
        ss << std::hex << std::setw(2) << std::setfill(L'0') << static_cast<int>(data[i]);
    }
    return ss.str();
}

std::wstring filetime_to_str(const FILETIME& ft) {
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return std::to_wstring(uli.QuadPart);
}

} // namespace

bool capture_baseline(const wchar_t* loader_path, const wchar_t* baseline_path) {
    trace::snapshot_t snap{};
    if (!trace::snapshot_artifacts(loader_path, &snap)) {
        return false;
    }

    bool ok = trace::save_snapshot_to_file(snap, baseline_path);
    trace::free_snapshot(&snap);
    return ok;
}

BaselineDiffResult compare_with_baseline(const wchar_t* baseline_path) {
    BaselineDiffResult res{};
    res.mismatch_count = 0;
    res.identical = true;

    trace::snapshot_t base_snap{};
    if (!trace::load_snapshot_from_file(&base_snap, baseline_path)) {
        BaselineDiffEntry err{};
        err.item = L"baseline_file";
        err.baseline_val = baseline_path;
        err.current_val = L"ERROR_LOAD";
        err.match = false;
        res.diffs.push_back(err);
        res.mismatch_count = 1;
        res.identical = false;
        return res;
    }

    trace::snapshot_t cur_snap{};
    trace::snapshot_artifacts(base_snap.loader_path, &cur_snap);

    // 1. Comparar hash de Prefetch
    {
        BaselineDiffEntry d{};
        d.item = L"Prefetch SHA-256";
        d.baseline_val = to_hex(base_snap.pf_sha256, 32);
        d.current_val  = to_hex(cur_snap.pf_sha256, 32);
        d.match = (memcmp(base_snap.pf_sha256, cur_snap.pf_sha256, 32) == 0);
        if (!d.match) {
            res.mismatch_count++;
            res.identical = false;
        }
        res.diffs.push_back(d);
    }

    // 2. Comparar existencia de BAM
    {
        BaselineDiffEntry d{};
        d.item = L"BAM Entry";
        d.baseline_val = base_snap.bam_exist ? to_hex(base_snap.bam, 8) : L"ABSENT";
        d.current_val  = cur_snap.bam_exist ? to_hex(cur_snap.bam, 8) : L"ABSENT";
        d.match = (base_snap.bam_exist == cur_snap.bam_exist &&
                   (!base_snap.bam_exist || memcmp(base_snap.bam, cur_snap.bam, 8) == 0));
        if (!d.match) {
            res.mismatch_count++;
            res.identical = false;
        }
        res.diffs.push_back(d);
    }

    // 3. Comparar Timestamps $STANDARD_INFORMATION (CreationTime, LastWriteTime)
    {
        BaselineDiffEntry d{};
        d.item = L"Timestamp $SI Creation";
        d.baseline_val = filetime_to_str(base_snap.si[0]);
        d.current_val  = filetime_to_str(cur_snap.si[0]);
        d.match = (base_snap.si[0].dwLowDateTime == cur_snap.si[0].dwLowDateTime &&
                   base_snap.si[0].dwHighDateTime == cur_snap.si[0].dwHighDateTime);
        if (!d.match) {
            res.mismatch_count++;
            res.identical = false;
        }
        res.diffs.push_back(d);
    }

    {
        BaselineDiffEntry d{};
        d.item = L"Timestamp $SI Write";
        d.baseline_val = filetime_to_str(base_snap.si[2]);
        d.current_val  = filetime_to_str(cur_snap.si[2]);
        d.match = (base_snap.si[2].dwLowDateTime == cur_snap.si[2].dwLowDateTime &&
                   base_snap.si[2].dwHighDateTime == cur_snap.si[2].dwHighDateTime);
        if (!d.match) {
            res.mismatch_count++;
            res.identical = false;
        }
        res.diffs.push_back(d);
    }

    // 4. Comparar Timestamps $FILE_NAME
    {
        BaselineDiffEntry d{};
        d.item = L"Timestamp $FN Creation";
        d.baseline_val = filetime_to_str(base_snap.fn[0]);
        d.current_val  = filetime_to_str(cur_snap.fn[0]);
        d.match = (base_snap.fn[0].dwLowDateTime == cur_snap.fn[0].dwLowDateTime &&
                   base_snap.fn[0].dwHighDateTime == cur_snap.fn[0].dwHighDateTime);
        if (!d.match) {
            res.mismatch_count++;
            res.identical = false;
        }
        res.diffs.push_back(d);
    }

    trace::free_snapshot(&base_snap);
    trace::free_snapshot(&cur_snap);
    return res;
}

} // namespace wrath::gate
