#pragma once

#include <hg/scalar/canonical.hpp>
#include <hg/scalar/data.hpp>
#include <hg/scalar/details/value_storage.hpp>
#include <hg/scalar/scalar.hpp>
#include <hg/scalar/value.hpp>
#include <hg/util/lifetimebound.hpp>

#include <concepts>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace literal {

class ScalarLiteralItem {
  private:
    using Visit = void (*)(const void *, hg::ScalarVisitTypes &);

    template <typename Source> static constexpr bool supported_source = [] {
        if constexpr (hg::scalar_source<Source> || std::same_as<Source, hg::ScalarData> ||
                      requires(const Source &source) { hg::details::scalar_data(source); }) {
            return true;
        } else if constexpr (hg::scalar_type_mapped<Source>) {
            return requires(const Source &source) {
                typename hg::scalar_storage_traits<Source>::value_type;
                requires std::constructible_from<typename hg::scalar_storage_traits<Source>::value_type,
                                                 const Source &>;
            };
        } else {
            return false;
        }
    }();

  public:
    template <typename Source>
        requires (!std::same_as<std::remove_cvref_t<Source>, ScalarLiteralItem>) && supported_source<Source>
    ScalarLiteralItem(const Source &source HG_LIFETIMEBOUND) noexcept
        : source_{std::addressof(source)}, visit_{&visit_source<std::remove_cv_t<Source> >} {}

    void accept(hg::ScalarVisitTypes &visitor) const { visit_(source_, visitor); }

    template <typename Visitor, typename... Arguments>
        requires ddv::SerialVisitorType<std::remove_cvref_t<Visitor>>
    auto visit(Visitor &&visitor, Arguments &&...arguments) const {
        auto dispatch = ddv::make_visitable_visitor<hg::ScalarVisitTypes>(
            std::forward_as_tuple(std::forward<Arguments>(arguments)...), std::forward<Visitor>(visitor));
        auto &types = static_cast<hg::ScalarVisitTypes &>(dispatch);
        accept(types);
        if constexpr (!decltype(dispatch)::is_result_void) {
            return *dispatch;
        }
    }

    template <typename Function, typename... Functions>
    decltype(auto) visit(Function &&function, Functions &&...functions) const {
        return visit(ddv::serial{std::forward<Function>(function), std::forward<Functions>(functions)...});
    }

    template <typename Visitor, typename... Arguments>
        requires ddv::SerialVisitorType<std::remove_cvref_t<Visitor>>
    auto visit_scalar(Visitor &&visitor, Arguments &&...arguments) const {
        return visit(std::forward<Visitor>(visitor), std::forward<Arguments>(arguments)...);
    }

    template <typename Function, typename... Functions>
    decltype(auto) visit_scalar(Function &&function, Functions &&...functions) const {
        return visit(std::forward<Function>(function), std::forward<Functions>(functions)...);
    }

  private:
    template <typename ScalarType, typename Source>
    static void visit_value(const Source &source, hg::ScalarVisitTypes &visitor) {
        using Value = typename hg::scalar_storage_traits<ScalarType>::value_type;
        if constexpr (std::same_as<std::remove_cvref_t<Source>, std::remove_cvref_t<Value>>) {
            const auto descriptor = hg::details::make_scalar_source<ScalarType>(source);
            auto scalar = hg::Scalar<>::from(descriptor);
            scalar.accept(visitor);
        } else {
            Value adapted(source);
            const auto descriptor = hg::details::make_scalar_source<ScalarType>(adapted);
            auto scalar = hg::Scalar<>::from(descriptor);
            scalar.accept(visitor);
        }
    }

    template <typename Source> static void visit_source(const void *source, hg::ScalarVisitTypes &visitor) {
        const auto &value = *static_cast<const Source *>(source);
        if constexpr (std::same_as<Source, hg::ScalarData>) {
            auto scalar = hg::Scalar<>::from(value);
            scalar.accept(visitor);
        } else if constexpr (requires { hg::details::scalar_data(value); }) {
            auto scalar = hg::Scalar<>::from(hg::details::scalar_data(value));
            scalar.accept(visitor);
        } else if constexpr (hg::scalar_source<Source>) {
            using ScalarType = hg::canonical_scalar_t<Source>;
            decltype(auto) canonical = hg::canonical_scalar(value);
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
