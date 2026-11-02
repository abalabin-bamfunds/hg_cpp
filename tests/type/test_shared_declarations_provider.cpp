#include "test_shared_declarations_provider.hpp"

namespace test_shared_declarations {
namespace {

auto factory_calls = std::array<int, 2>{};

template <int Index> const LazyType &lazy_type() {
    static const auto &type = hg::TypeRegistry::instance().get_or_create_static<LazyDeclaration<Index>, LazyType>(
        []() -> const LazyType & {
            ++factory_calls[Index];
            static const auto storage = hg::util::static_storage<LazyType>{};
            return storage.get();
        });
    return type;
}

} // namespace

std::array<const hg::Type *, 17> provider_types() { return declaration_types(); }

std::array<const hg::Variable *, 4> provider_variables() { return declaration_variables(); }

std::array<const hg::Variable *, 2> provider_reverse_variables() { return reverse_variables(); }

const LazyType &provider_lazy_type(int index) {
    return index == 0 ? lazy_type<0>() : lazy_type<1>();
}

int provider_factory_calls(int index) { return factory_calls[index]; }

} // namespace test_shared_declarations
