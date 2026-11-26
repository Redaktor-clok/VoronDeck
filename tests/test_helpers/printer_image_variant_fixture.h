// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config_dir_guard.h"
#include "printer_detector.h"

#include <fstream>
#include <memory>

namespace helix::test {

/// Exercise image selection through a user database extension, independent of shipped models.
class PrinterImageVariantFixture {
  public:
    PrinterImageVariantFixture() : config_(std::make_unique<ConfigDirGuard>("image_variants")) {
        const auto directory = config_->dir / "printer_database.d";
        std::filesystem::create_directories(directory);
        std::ofstream(directory / "image_variants.json") << R"({"printers":[{
            "id":"test_image_variants", "name":"Test Image Variants",
            "manufacturer":"Test", "image":"voron-v2.png",
            "image_variants":[
                {"when":{"filament_system":"afc"},"image":"voron-trident.png"},
                {"when":{"enclosure_top":true},"image":"voron-trident.png"},
                {"when":{"chamber_heater":true},"image":"voron-trident.png"}
            ]
        }]})";
        PrinterDetector::reload();
    }
    ~PrinterImageVariantFixture() {
        config_.reset();
        PrinterDetector::reload();
    }

  private:
    std::unique_ptr<ConfigDirGuard> config_;
};

} // namespace helix::test
