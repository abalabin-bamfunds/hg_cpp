#pragma once

namespace bg {

template <template <template <typename, auto...> typename> typename Schema>
class Bundle {
  public:
    Bundle() = delete;

    template <template <typename, auto...> typename Field>
    using schema_type = Schema<Field>;
};

} // namespace bg
