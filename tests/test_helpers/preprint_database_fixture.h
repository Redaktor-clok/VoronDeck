// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "printer_database_fixture.h"

#include <iterator>
#include <stdexcept>

namespace helix::test {

class PreprintDatabaseFixture : public PrinterDatabaseFixture {
  public:
    PreprintDatabaseFixture() : PrinterDatabaseFixture(read_fixture()) {}

  private:
    static std::string read_fixture() {
        std::ifstream input("tests/fixtures/printers/preprint_options.json");
        if (!input)
            throw std::runtime_error("Missing pre-print test database");
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
};

} // namespace helix::test
