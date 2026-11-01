#pragma once

#include <hg/scalar/declaration.hpp>
#include <hg/time_series/time_series_type.hpp>
#include <hg/type/scalar_variable.hpp>

namespace hg::operators {

using KEY_VALUE_KEY = ScalarVariable<"KEY_VALUE_KEY", Hashable>;
using KEY_VALUE_VALUE = ScalarVariable<"KEY_VALUE_VALUE">;

template <template <typename, auto...> typename Field> struct key_value {
    Field<TS<KEY_VALUE_KEY>> key;
    Field<TS<KEY_VALUE_VALUE>> value;
};

} // namespace hg::operators
