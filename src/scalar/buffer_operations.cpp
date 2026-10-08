#include <bg/scalar/buffer_operations.hpp>

namespace bg {

template <> ScalarDispatch<ScalarBufferOperations> &ScalarDispatch<ScalarBufferOperations>::instance() noexcept {
    static auto dispatch = ScalarDispatch<ScalarBufferOperations>{};
    return dispatch;
}

} // namespace bg
