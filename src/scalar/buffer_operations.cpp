#include <hg/scalar/buffer_operations.hpp>
#include <hg/util/static_storage.hpp>

namespace hg {

template <> ScalarDispatch<ScalarBufferOperations> &ScalarDispatch<ScalarBufferOperations>::instance() noexcept {
    static auto dispatch = util::static_storage<ScalarDispatch<ScalarBufferOperations>>{};
    return dispatch.get();
}

} // namespace hg
