// SPDX-License-Identifier: MIT
//
// Minimal self-registering test framework (no external dependency, so the
// test suite builds offline and in any CI image with just a compiler).
//
//   TEST(order_book, fifo_priority) { CHECK_EQ(a, b); }
//
// Run all:            mds_tests
// Run a subset:       mds_tests order_book      (substring match on "suite.name")
#pragma once

#include <cstdio>
#include <exception>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace mds::test {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* suite, const char* name, std::function<void()> fn) {
        registry().push_back({std::string(suite) + "." + name, std::move(fn)});
    }
};

struct Failure : std::exception {
    std::string msg;
    explicit Failure(std::string m) : msg(std::move(m)) {}
    const char* what() const noexcept override { return msg.c_str(); }
};

template <class A, class B>
void check_eq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    if (!(a == b)) {
        std::ostringstream os;
        os << file << ":" << line << ": CHECK_EQ(" << ea << ", " << eb << ") failed: " << a
           << " != " << b;
        throw Failure(os.str());
    }
}

inline int run_all(int argc, char** argv) {
    const std::string filter = argc > 1 ? argv[1] : "";
    int passed = 0, failed = 0;
    for (const auto& t : registry()) {
        if (!filter.empty() && t.name.find(filter) == std::string::npos) continue;
        try {
            t.fn();
            ++passed;
            std::printf("[ PASS ] %s\n", t.name.c_str());
        } catch (const std::exception& e) {
            ++failed;
            std::printf("[ FAIL ] %s\n         %s\n", t.name.c_str(), e.what());
        }
    }
    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}

}  // namespace mds::test

#define MDS_CONCAT_(a, b) a##b
#define MDS_CONCAT(a, b) MDS_CONCAT_(a, b)

#define TEST(suite, name)                                                               \
    static void MDS_CONCAT(test_##suite##_, name)();                                    \
    static ::mds::test::Registrar MDS_CONCAT(reg_##suite##_, name)(#suite, #name,       \
                                                                   &MDS_CONCAT(test_##suite##_, name)); \
    static void MDS_CONCAT(test_##suite##_, name)()

#define CHECK(cond)                                                                           \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            throw ::mds::test::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                                       ": CHECK(" #cond ") failed");                          \
        }                                                                                     \
    } while (0)

#define CHECK_EQ(a, b) ::mds::test::check_eq((a), (b), #a, #b, __FILE__, __LINE__)

#define CHECK_NEAR(a, b, eps)                                                                 \
    do {                                                                                      \
        const double _va = (a), _vb = (b);                                                    \
        if (!(_va - _vb <= (eps) && _vb - _va <= (eps))) {                                    \
            throw ::mds::test::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                                       ": CHECK_NEAR(" #a ", " #b ") failed: " +              \
                                       std::to_string(_va) + " vs " + std::to_string(_vb));   \
        }                                                                                     \
    } while (0)
