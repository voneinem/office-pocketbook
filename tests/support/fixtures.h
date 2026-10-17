#pragma once
// Reads a file below tests/fixtures (PBO_FIXTURE_DIR is set by tests/CMakeLists.txt).

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace pbtest {

inline std::string fixture(const std::string& relative) {
    std::ifstream in(std::string(PBO_FIXTURE_DIR) + "/" + relative, std::ios::binary);
    if (!in) throw std::runtime_error("missing fixture: " + relative);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace pbtest
