#pragma once

#include <bg/scalar/canonical.hpp>
#include <bg/scalar/data.hpp>
#include <bg/scalar/details/value_storage.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/scalar/value.hpp>

#include <concepts>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace literal {

class ScalarLiteralItem {
  private:
    using Visit = void (*)(const void *, bg::ScalarVisitTypes &);

    template <typename Source> static constexpr bool supported_source = [] {
        if constexpr (bg::scalar_source<Source> || std::same_as<Source, bg::ScalarData> ||
                      requires(const Source &source) { bg::details::scalar_data(source); }) {
            return true;
        } else if constexpr (bg::scalar_type_mapped<Source>) {
            return requires(const Source &source) {
                typename bg::scalar_storage_traits<Source>::value_type;
                requires std::constructible_from<typename bg::scalar_storage_traits<Source>::value_type,
                                                 const Source &>;
            };
        } else {
            return false;
        }
    }();

  public:
    template <typename Source>
        requires (supported_source<Source>) && (!std::same_as<std::remove_cvref_t<Source>, ScalarLiteralItem>)
    ScalarLiteralItem(const Source &source) noexcept
        : source_{std::addressof(source)}, visit_{&visit_source<std::remove_cv_t<Source> >} {}

    void accept(bg::ScalarVisitTypes &visitor) const { visit_(source_, visitor); }

    template <typename Visitor, typename... Arguments>
        requires ddv::SerialVisitorType<std::remove_cvref_t<Visitor>>
    auto visit(Visitor &&visitor, Arguments &&...arguments) const {
        auto dispatch = ddv::make_visitable_visitor<bg::ScalarVisitTypes>(
            std::forward_as_tuple(std::forward<Arguments>(arguments)...), std::forward<Visitor>(visitor));
        auto &types = static_cast<bg::ScalarVisitTypes &>(dispatch);
        accept(types);
        if constexpr (!decltype(dispatch)::is_result_void) {
            return *dispatch;
        }
    }

    template <typename Function, typename... Functions>
    decltype(auto) visit(Function &&function, Functions &&...functions) const {
        return visit(ddv::serial{std::forward<Function>(function), std::forward<Functions>(functions)...});
    }

  private:
    template <typename ScalarType, typename Source>
    static void visit_value(const Source &source, bg::ScalarVisitTypes &visitor) {
        using Value = typename bg::scalar_storage_traits<ScalarType>::value_type;
        if constexpr (std::same_as<std::remove_cvref_t<Source>, std::remove_cvref_t<Value>>) {
            const auto descriptor = bg::details::make_scalar_source<ScalarType>(source);
            auto scalar = bg::Scalar<>::from(descriptor);
            scalar.accept(visitor);
        } else {
            Value adapted(source);
            const auto descriptor = bg::details::make_scalar_source<ScalarType>(adapted);
            auto scalar = bg::Scalar<>::from(descriptor);
            scalar.accept(visitor);
        }
    }

    template <typename Source> static void visit_source(const void *source, bg::ScalarVisitTypes &visitor) {
        const auto &value = *static_cast<const Source *>(source);
        if constexpr (std::same_as<Source, bg::ScalarData>) {
            auto scalar = bg::Scalar<>::from(value);
            scalar.accept(visitor);
        } else if constexpr (requires { bg::details::scalar_data(value); }) {
            auto scalar = bg::Scalar<>::from(bg::details::scalar_data(value));
            scalar.accept(visitor);
        } else if constexpr (bg::scalar_source<Source>) {
            using ScalarType = bg::canonical_scalar_t<Source>;
            decltype(auto) canonical = bg::canonical_scalar(value);
            visit_value<ScalarType>(canonical, visitor);
        } else {
            using ScalarType = std::remove_cv_t<Source>;
            visit_value<ScalarType>(value, visitor);
        }
    }

    const void *source_;
    Visit visit_;
};

static_assert(sizeof(ScalarLiteralItem) == 2 * sizeof(void *));

} // namespace literal
