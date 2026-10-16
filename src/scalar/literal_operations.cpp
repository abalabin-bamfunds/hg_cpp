#include <bg/scalar/literal_operations.hpp>

namespace bg {

template <> ScalarDispatch<ScalarLiteralOperations> &ScalarDispatch<ScalarLiteralOperations>::instance() noexcept {
    static auto dispatch = ScalarDispatch<ScalarLiteralOperations>{};
    return dispatch;
}

} // namespace bg
