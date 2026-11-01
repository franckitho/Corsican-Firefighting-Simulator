#pragma once

// Mini-cadre de tests, pour rester sans dépendance externe.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace firesim::test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int count = 0;
    return count;
}

struct Register {
    Register(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void fail(const char* file, int line, const std::string& message) {
    ++failures();
    std::fprintf(stderr, "  %s:%d: %s\n", file, line, message.c_str());
}

inline bool near(double actual, double expected, double rel, double abs = 1e-9) {
    return std::fabs(actual - expected) <= std::max(abs, rel * std::fabs(expected));
}

}  // namespace firesim::test

#define TEST_CASE(name)                                                   \
    static void name();                                                   \
    static const firesim::test::Register name##_registered(#name, name); \
    static void name()

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) firesim::test::fail(__FILE__, __LINE__, "échec : " #cond);      \
    } while (0)

#define CHECK_NEAR(actual, expected, rel)                                                                       \
    do {                                                                                                        \
        const double a_ = (actual), e_ = (expected);                                                           \
        if (!firesim::test::near(a_, e_, (rel))) {                                                             \
            firesim::test::fail(__FILE__, __LINE__,                                                            \
                                std::string(#actual " = ") + std::to_string(a_) + ", attendu " + std::to_string(e_)); \
        }                                                                                                       \
    } while (0)
