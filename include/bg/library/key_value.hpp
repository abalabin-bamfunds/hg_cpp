#pragma once

#include <bg/scalar/declaration.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/type/scalar_variable.hpp>

namespace bg::operators {

using KEY_VALUE_KEY = ScalarVariable<"KEY_VALUE_KEY", Hashable>;
using KEY_VALUE_VALUE = ScalarVariable<"KEY_VALUE_VALUE">;

template <template <typename, auto...> typename Field> struct key_value {
    Field<TS<KEY_VALUE_KEY>> key;
    Field<TS<KEY_VALUE_VALUE>> value;
};

} // namespace bg::operators
