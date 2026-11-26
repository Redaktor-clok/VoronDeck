// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config_dir_guard.h"
#include "printer_detector.h"

#include <fstream>
#include <memory>

namespace helix::test {

class PrinterDatabaseFixture {
  public:
    explicit PrinterDatabaseFixture(const std::string& json)
        : config_(std::make_unique<ConfigDirGuard>("printer_database_fixture")) {
        const auto directory = config_->dir / "printer_database.d";
        std::filesystem::create_directories(directory);
        std::ofstream(directory / "test.json") << json;
        PrinterDetector::reload();
    }

    ~PrinterDatabaseFixture() {
        config_.reset();
        PrinterDetector::reload();
    }

  private:
    std::unique_ptr<ConfigDirGuard> config_;
};

} // namespace helix::test
