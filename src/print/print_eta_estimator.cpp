// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_eta_estimator.h"

#include <algorithm>
#include <limits>

namespace helix {

void PrintEtaEstimator::reset() {
    // No history or model-dependent state: the selected file owns the estimate.
}

int PrintEtaEstimator::remaining_seconds(const PrintEtaInputs& in) {
    const int progress = std::clamp(in.progress_pct, 0, 100);
    if (progress == 100) {
        return 0;
    }
    if (in.slicer_estimate_s <= 0) {
        return kNoEstimate;
    }
    if (progress == 0) {
        const auto total = static_cast<long long>(in.slicer_estimate_s) +
                           std::max(0, in.preprint_remaining_s);
        return static_cast<int>(std::min(total, static_cast<long long>(std::numeric_limits<int>::max())));
    }
    // Scale the file's estimate by reported progress; do not add model/thermal
    // preparation guesses or replace it with elapsed-time extrapolation.
    return static_cast<int>(static_cast<long long>(in.slicer_estimate_s) *
                            (100 - progress) / 100);
}

} // namespace helix
