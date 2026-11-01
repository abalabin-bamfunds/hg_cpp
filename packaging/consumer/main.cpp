#include <hg/hg.hpp>

int main() {
    const hg::util::symbol value{"installed-sdk"};
    return value.has_value() && !hg::version().empty() ? 0 : 1;
}
