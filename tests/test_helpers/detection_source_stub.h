// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "detection_source.h"

#include <algorithm>

namespace helix::test {
class DetectionSourceStub : public detection::DetectionSource {
  public:
    static constexpr const char* kSourceId = "test_detector";
    std::string id() const override {
        return kSourceId;
    }
    bool available() const override {
        return capable;
    }
    bool can_tune() const override {
        return true;
    }
    bool self_pauses() const override {
        return true;
    }
    void tune() override {
        ++tune_calls;
    }
    void on_objects_listed(const std::vector<std::string>& objects) override {
        capable = std::find(objects.begin(), objects.end(), kSourceId) != objects.end();
    }
    void set_callback(Callback cb) override {
        callback = std::move(cb);
    }
    bool explains_current_pause() const override {
        return explains;
    }
    bool has_bed_check() const override {
        return true;
    }
    void read_bed_check(BedCheckReadCallback done) override {
        done(BedCheckState{bed_enabled, true});
    }
    void set_bed_check(bool enabled, BedCheckSetCallback done) override {
        bed_enabled = enabled;
        ++bed_writes;
        done(true, {});
    }
    void report(detection::DetectionKind kind) {
        explains = true;
        if (callback)
            callback({kSourceId, kind, true, {}, true, "test detection"});
    }
    bool capable = false;
    bool explains = false;
    bool bed_enabled = true;
    int tune_calls = 0;
    int bed_writes = 0;
    Callback callback;
};
} // namespace helix::test
