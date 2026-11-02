#include <hg/frame/frame.hpp>
#include <hg/frame/tsc.hpp>
#include <hg/library/operators.hpp>
#include <hg/node.hpp>
#include <hg/overload.hpp>

namespace hg::frame {
namespace {

struct collection_overloads {
    overload<operators::len_> len_tsf{[](In<TSF<SCHEMA>> value, Out<TS<i64>> output) {
        output.set(static_cast<i64>(value.size()));
    }};
    overload<operators::len_> len_tsc{[](In<TSC<SCALAR>> value, Out<TS<i64>> output) {
        output.set(static_cast<i64>(value.size()));
    }};
    overload<operators::is_empty> is_empty_tsf{[](In<TSF<SCHEMA>> value, Out<TS<bool>> output) {
        output.set(value.size() == 0);
    }};
    overload<operators::is_empty> is_empty_tsc{[](In<TSC<SCALAR>> value, Out<TS<bool>> output) {
        output.set(value.size() == 0);
    }};
};

const auto registered = [] {
    register_overloads<collection_overloads>();
    return true;
}();

} // namespace
} // namespace hg::frame
