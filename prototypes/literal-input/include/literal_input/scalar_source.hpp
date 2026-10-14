#pragma once

#include <bg/scalar/canonical.hpp>
#include <bg/scalar/data.hpp>
#include <bg/scalar/details/value_storage.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/scalar/value.hpp>

#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace literal_input {

class ScalarSource {
  private:
    using ErasedVisitor = void (*)(void *, const bg::ScalarData &);
    using Visit = void (*)(const void *, void *, ErasedVisitor);

    template <typename Source>
    static constexpr bool supported_source =
        bg::scalar_source<Source> || std::same_as<Source, bg::ScalarData> ||
        requires(const Source &source) { bg::details::scalar_data(source); };

  public:
    template <typename Source>
        requires (supported_source<Source>) && (!std::same_as<std::remove_cvref_t<Source>, ScalarSource>)
    ScalarSource(const Source &source) noexcept
        : source_{std::addressof(source)}, visit_{&visit_source<std::remove_cv_t<Source> >} {}

    template <typename Visitor> void visit(Visitor &&visitor) const {
        using VisitorType = std::remove_reference_t<Visitor>;
        struct VisitorContext {
            VisitorType *visitor;
        } visitor_context{std::addressof(visitor)};
        visit_(source_, std::addressof(visitor_context), [](void *context, const bg::ScalarData &source) {
            auto &visitor = *static_cast<VisitorContext *>(context)->visitor;
            std::invoke(visitor, source);
        });
    }

  private:
    template <typename Source> static void visit_source(const void *source, void *context, ErasedVisitor visitor) {
        const auto &value = *static_cast<const Source *>(source);
        if constexpr (std::same_as<Source, bg::ScalarData>) {
            visitor(context, value);
        } else if constexpr (requires { bg::details::scalar_data(value); }) {
            visitor(context, bg::details::scalar_data(value));
        } else {
            using ScalarType = bg::canonical_scalar_t<Source>;
            decltype(auto) canonical = bg::canonical_scalar(value);
            const auto descriptor = bg::details::make_scalar_source<ScalarType>(canonical);
            visitor(context, descriptor);
        }
    }

    const void *source_;
    Visit visit_;
};

static_assert(sizeof(ScalarSource) == 2 * sizeof(void *));

} // namespace literal_input
