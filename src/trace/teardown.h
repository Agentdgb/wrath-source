#pragma once
#ifndef WRATH_TRACE_TEARDOWN_H
#define WRATH_TRACE_TEARDOWN_H

#include "trace/snapshot.h"

namespace wrath::trace {

struct teardown_result_t {
    bool prefetch_restored;
    bool bam_restored;
    bool pca_restored;
    bool stamps_restored;

    bool all_passed() const noexcept {
        return prefetch_restored && bam_restored && pca_restored && stamps_restored;
    }
};

bool restore_prefetch(const snapshot_t& snap);
bool restore_bam(const snapshot_t& snap);
bool restore_pca(const snapshot_t& snap);
bool restore_stamps(const snapshot_t& snap);

// Teardown forense completo en orden fijo obligatorio
teardown_result_t teardown(const snapshot_t& snap);

} // namespace wrath::trace

#endif // WRATH_TRACE_TEARDOWN_H
