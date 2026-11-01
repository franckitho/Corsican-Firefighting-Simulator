#include <cstdio>

#include "check.hpp"

int main() {
    int failed_cases = 0;
    for (const auto& c : firesim::test::registry()) {
        const int before = firesim::test::failures();
        c.fn();
        const bool ok = firesim::test::failures() == before;
        if (!ok) ++failed_cases;
        std::printf("%s %s\n", ok ? "[ok]   " : "[ÉCHEC]", c.name);
    }
    std::printf("%zu tests, %d en échec\n", firesim::test::registry().size(), failed_cases);
    return failed_cases == 0 ? 0 : 1;
}
