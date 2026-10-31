#include <bg/scalar/buffer_operations.hpp>
#include <bg/util/static_storage.hpp>

namespace bg {

template <> ScalarDispatch<ScalarBufferOperations> &ScalarDispatch<ScalarBufferOperations>::instance() noexcept {
    static auto dispatch = util::static_storage<ScalarDispatch<ScalarBufferOperations>>{};
    return dispatch.get();
}

} // namespace bg
