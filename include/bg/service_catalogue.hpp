#pragma once

#include <bg/graph/service.hpp>
#include <bg/graph/type_mapping.hpp>
#include <bg/graph/wiring_argument.hpp>

#include <array>
#include <concepts>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace bg {

class GraphWiring;

class ServiceCatalogue {
  public:
    ServiceCatalogue();
    ~ServiceCatalogue();

    ServiceCatalogue(const ServiceCatalogue &) = delete;
    ServiceCatalogue &operator=(const ServiceCatalogue &) = delete;
    ServiceCatalogue(ServiceCatalogue &&) noexcept;
    ServiceCatalogue &operator=(ServiceCatalogue &&) noexcept;

    template <typename Implementation, typename... Scalars>
        requires std::derived_from<std::remove_cvref_t<Implementation>, WiringTemplate> &&
                 (details::graph_code_argument<Scalars> && ...)
    void register_impl(const Service &service, Implementation &&implementation, Scalars &&...scalars) {
        auto arguments = std::array<WiringArgument, sizeof...(Scalars)>{
            WiringArgument{std::forward<Scalars>(scalars)}...};
        add_impl(service, TypeMap{},
                 std::make_shared<std::remove_cvref_t<Implementation>>(std::forward<Implementation>(implementation)),
                 arguments);
    }

    template <typename Target, typename... Parameters, typename Implementation, typename... Scalars>
        requires std::derived_from<Target, Service> &&
                 std::derived_from<std::remove_cvref_t<Implementation>, WiringTemplate> &&
                 (details::graph_code_argument<Scalars> && ...)
    void register_impl(const details::MappedWiring<Target, Parameters...> &service, Implementation &&implementation,
                       Scalars &&...scalars) {
        auto arguments = std::array<WiringArgument, sizeof...(Scalars)>{
            WiringArgument{std::forward<Scalars>(scalars)}...};
        add_impl(details::mapped_wiring_access::target(service),
                 details::mapped_wiring_access::type_map(service).copy(),
                 std::make_shared<std::remove_cvref_t<Implementation>>(std::forward<Implementation>(implementation)),
                 arguments);
    }

    void build(GraphWiring &wiring) const;

  private:
    struct State;

    void add_impl(const Service &service, TypeMap type_map, std::shared_ptr<const WiringTemplate> implementation,
                  std::span<const WiringArgument> scalar_filters);

    std::unique_ptr<State> state_;
};

} // namespace bg
