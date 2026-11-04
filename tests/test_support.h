#pragma once

#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace test {
inline void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

inline void expect_near(float actual, double expected, std::string_view message) {
    if (!std::isfinite(actual) || std::abs(static_cast<double>(actual) - expected) > 2.0e-7) {
        std::ostringstream detail;
        detail << message << ": expected " << expected << ", got " << actual;
        throw std::runtime_error(detail.str());
    }
}

class Suite {
public:
    template <typename Function>
    void run(std::string_view name, Function&& function) {
        try {
            function();
            ++passed_;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failed_;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }

    int finish() const {
        std::cout << passed_ << " passed, " << failed_ << " failed\n";
        return failed_ == 0 ? 0 : 1;
    }

private:
    int passed_ = 0;
    int failed_ = 0;
};
} // namespace test
