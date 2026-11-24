#include "trace/teardown.h"

namespace wrath::trace {

teardown_result_t teardown(const snapshot_t& snap) {
    teardown_result_t res{};

    // Orden fijo obligatorio:
    // 1. restore_prefetch()
    res.prefetch_restored = restore_prefetch(snap);

    // 2. restore_bam()
    res.bam_restored = restore_bam(snap);

    // 3. restore_pca()
    res.pca_restored = restore_pca(snap);

    // 4. restore_stamps()
    res.stamps_restored = restore_stamps(snap);

    return res;
}

} // namespace wrath::trace
