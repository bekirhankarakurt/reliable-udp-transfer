#include <sstream>

#include "check.hpp"
#include "rudp/progress.hpp"

int main() {
    using namespace rudp;

    CHECK_EQ(format_bytes(0), std::string("0 B"));
    CHECK_EQ(format_bytes(1536), std::string("1.50 KiB"));
    CHECK_EQ(format_bytes(5.0 * 1024 * 1024), std::string("5.00 MiB"));

    CHECK_EQ(format_bar(50, 100, 1024.0 * 1024.0, 10),
             std::string("[#####-----]  50.0%  1.00 MiB/s  ETA 00:00"));
    CHECK_EQ(format_bar(100, 100, 0.0, 4), std::string("[####] 100.0%  0 B/s"));
    CHECK_EQ(format_bar(0, 0, 0.0, 4), std::string("[####] 100.0%  0 B/s"));
    CHECK_EQ(format_bar(0, 600, 1.0, 4), std::string("[----]   0.0%  1 B/s  ETA 10:00"));

    std::ostringstream disabled;
    ProgressReporter quiet(disabled, false);
    quiet.update(1, 2);
    quiet.finish();
    CHECK(disabled.str().empty());

    std::ostringstream enabled;
    ProgressReporter bar(enabled, true);
    bar.update(1, 2);
    bar.update(2, 2);  // completion always draws
    bar.finish();
    CHECK(enabled.str().find("100.0%") != std::string::npos);
    CHECK(enabled.str().back() == '\n');

    return check::finish("progress");
}
