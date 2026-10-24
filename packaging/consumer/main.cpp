#include <bg/bg.hpp>

int main() {
    const bg::util::symbol value{"installed-sdk"};
    return value.has_value() && !bg::version().empty() ? 0 : 1;
}
