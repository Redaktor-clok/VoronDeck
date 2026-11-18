// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Remaining time is derived only from file metadata and reported progress.
namespace helix {

struct PrintEtaInputs {
    int progress_pct = 0;
    // Retained for callers; these do not alter the file-based estimate.
    int print_duration_s = 0;
    int slicer_estimate_s = 0;
    int preprint_remaining_s = 0;
};

class PrintEtaEstimator {
  public:
    static constexpr int kNoEstimate = -1;
    [[nodiscard]] int remaining_seconds(const PrintEtaInputs& in);
    void reset();
};

} // namespace helix
