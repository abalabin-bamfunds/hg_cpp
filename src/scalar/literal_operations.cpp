#include <bg/scalar/literal_operations.hpp>
#include <bg/util/static_storage.hpp>

namespace bg {

template <> ScalarDispatch<ScalarLiteralOperations> &ScalarDispatch<ScalarLiteralOperations>::instance() noexcept {
    static auto dispatch = util::static_storage<ScalarDispatch<ScalarLiteralOperations>>{};
    return dispatch.get();
}

} // namespace bg
