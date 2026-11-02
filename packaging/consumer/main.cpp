#include <hg/hg.hpp>
#include <hg/library.hpp>

int main() {
    const hg::util::symbol value{"installed-sdk"};
    const auto graph = [](hg::wp<hg::TS<double>> lhs, hg::wp<hg::TS<double>> rhs) {
        return hg::operators::add(lhs, rhs);
    };
    const auto result = hg::run_graph(graph, {1.0, 2.0}, {10.0, 20.0});
    return value.has_value() && !hg::version().empty() && result.equals<double>({11.0, 22.0}) ? 0 : 1;
}
