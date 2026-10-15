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

namespace details {
struct ServiceInstance;
}

class ServiceCatalogue {
    struct State;

  public:
    class MultiserviceRegistration {
      public:
        template <typename Target, typename... Parameters, typename... Filters>
            requires std::derived_from<Target, ServiceDefinition> && (details::graph_code_argument<Filters> && ...)
        void register_impl(const details::MappedWiring<Target, Parameters...> &service, Filters &&...filters) {
            auto values = std::array<WiringArgument, sizeof...(Filters)>{
                WiringArgument{std::forward<Filters>(filters)}...};
            auto state = state_.lock();
            if (!state) {
                throw std::logic_error{"the multiservice registration's catalogue no longer exists"};
            }
            ServiceCatalogue::add_multiservice_impl(state, index_, service.target(), service.type_map().copy(),
                                                    values);
        }

        template <typename... Filters>
            requires(details::graph_code_argument<Filters> && ...)
        void register_impl(const ServiceDefinition &service, Filters &&...filters) {
            auto values = std::array<WiringArgument, sizeof...(Filters)>{
                WiringArgument{std::forward<Filters>(filters)}...};
            auto state = state_.lock();
            if (!state) {
                throw std::logic_error{"the multiservice registration's catalogue no longer exists"};
            }
            ServiceCatalogue::add_multiservice_impl(state, index_, service, TypeMap{}, values);
        }

      private:
        friend class ServiceCatalogue;

        MultiserviceRegistration(const std::shared_ptr<State> &state, std::size_t index) noexcept
            : state_{state}, index_{index} {}

        std::weak_ptr<State> state_;
        std::size_t index_;
    };

    ServiceCatalogue();
    ~ServiceCatalogue();

    ServiceCatalogue(const ServiceCatalogue &) = delete;
    ServiceCatalogue &operator=(const ServiceCatalogue &) = delete;
    ServiceCatalogue(ServiceCatalogue &&) noexcept;
    ServiceCatalogue &operator=(ServiceCatalogue &&) noexcept;

    template <typename Implementation, typename... Arguments>
        requires std::derived_from<std::remove_cvref_t<Implementation>, WiringTemplate> &&
                 (details::graph_code_argument<Arguments> && ...)
    MultiserviceRegistration register_multiservice(Implementation &&implementation, Arguments &&...arguments) {
        auto values = std::array<WiringArgument, sizeof...(Arguments)>{
            WiringArgument{std::forward<Arguments>(arguments)}...};
        auto graph = std::make_shared<std::remove_cvref_t<Implementation>>(
            std::forward<Implementation>(implementation));
        return MultiserviceRegistration{state_, add_multiservice(std::move(graph), values)};
    }

    template <typename Implementation, typename... Arguments>
        requires std::derived_from<std::remove_cvref_t<Implementation>, WiringTemplate> &&
                 (details::graph_code_argument<Arguments> && ...)
    void register_impl(const ServiceDefinition &service, Implementation &&implementation, Arguments &&...arguments) {
        auto values = std::array<WiringArgument, sizeof...(Arguments)>{
            WiringArgument{std::forward<Arguments>(arguments)}...};
        add_impl(service, TypeMap{},
                 std::make_shared<std::remove_cvref_t<Implementation>>(std::forward<Implementation>(implementation)),
                 values);
    }

    void register_impl(const ServiceDefinition &service, std::shared_ptr<const WiringTemplate> implementation,
                       std::span<const WiringArgument> arguments);

    template <typename Target, typename... Parameters, typename Implementation, typename... Arguments>
        requires std::derived_from<Target, ServiceDefinition> &&
                 std::derived_from<std::remove_cvref_t<Implementation>, WiringTemplate> &&
                 (details::graph_code_argument<Arguments> && ...)
    void register_impl(const details::MappedWiring<Target, Parameters...> &service, Implementation &&implementation,
                       Arguments &&...arguments) {
        auto values = std::array<WiringArgument, sizeof...(Arguments)>{
            WiringArgument{std::forward<Arguments>(arguments)}...};
        add_impl(service.target(), service.type_map().copy(),
                 std::make_shared<std::remove_cvref_t<Implementation>>(std::forward<Implementation>(implementation)),
                 values);
    }

    void build(GraphWiring &wiring) const;

  private:
    struct BuildOperation;

    void add_impl(const ServiceDefinition &service, TypeMap type_map,
                  std::shared_ptr<const WiringTemplate> implementation, std::span<const WiringArgument> arguments);
    std::size_t add_multiservice(std::shared_ptr<const WiringTemplate> implementation,
                                 std::span<const WiringArgument> arguments);
    static void add_multiservice_impl(const std::shared_ptr<State> &state, std::size_t index,
                                      const ServiceDefinition &service, TypeMap type_map,
                                      std::span<const WiringArgument> filters);

    std::shared_ptr<State> state_;
};

} // namespace bg
