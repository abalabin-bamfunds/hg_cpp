#include <hg/scalar/literal_operations.hpp>
#include <hg/util/static_storage.hpp>

namespace hg {

template <> ScalarDispatch<ScalarLiteralOperations> &ScalarDispatch<ScalarLiteralOperations>::instance() noexcept {
    static auto dispatch = util::static_storage<ScalarDispatch<ScalarLiteralOperations>>{};
    return dispatch.get();
}

} // namespace hg
