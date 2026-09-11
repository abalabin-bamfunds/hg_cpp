#include <bg/time_series/ref/ref_provider.hpp>

#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/graph/node.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/util/scope_exit.hpp>

#include <cassert>
#include <limits>
#include <memory>
#include <ranges>

namespace bg::details {

ReferenceRepresentation::~ReferenceRepresentation() = default;
ReferenceProviderComponent::~ReferenceProviderComponent() = default;

class ReferenceItemNotificationLinks {
    struct Link {
        std::uint32_t target;
        TimeSeriesItemId item;

        bool operator==(const Link &) const = default;
    };

    struct NotificationTarget {
        RuntimeGraph *graph;
        void *value;
        ReferenceItemNotifyFunction notify_function;
        ReferenceItemPublishFunction publish_function;
        std::uint32_t links{};
        bool touched{};
    };

    struct Group {
        explicit Group(std::pmr::memory_resource &memory) : links{&memory} {}

        std::pmr::vector<Link> links;
    };

  public:
    explicit ReferenceItemNotificationLinks(std::pmr::memory_resource &memory)
        : targets_{&memory}, groups_{&memory}, touched_{&memory}, memory_{&memory} {}

    ~ReferenceItemNotificationLinks() {
        for (auto *group : groups_) {
            if (group == nullptr) {
                continue;
            }
            std::destroy_at(group);
            memory_->deallocate(group, sizeof(Group), alignof(Group));
        }
    }

    void add(ReferenceRoute *route, RuntimeGraph &graph, void *target, TimeSeriesItemId item,
             ReferenceItemNotifyFunction notify_function, ReferenceItemPublishFunction publish_function) {
        auto &subscriptions = route == nullptr ? root_ : *route;
        const auto target_count = targets_.size();
        const auto target_index = require_target(graph, target, notify_function, publish_function);
        auto remove_unused_target = util::scope_exit{[&]() noexcept {
            if (targets_.size() != target_count && targets_.back().links == 0) {
                targets_.pop_back();
            }
        }};
        const auto value = Link{target_index, item};

        if (subscriptions.item_notification_link == 0) {
            set_inline(subscriptions, value);
            ++targets_[target_index].links;
            remove_unused_target.release();
            return;
        }
        if (!is_group(subscriptions)) {
            if (inline_link(subscriptions) == value) {
                return;
            }

            auto *group = allocate_group();
            auto release_group = util::scope_exit{[&]() noexcept {
                std::destroy_at(group);
                memory_->deallocate(group, sizeof(Group), alignof(Group));
            }};
            group->links.push_back(inline_link(subscriptions));
            group->links.push_back(value);

            const auto group_index = store_group(group);
            release_group.release();
            subscriptions.item_notification_link = group_flag | (group_index + 1);
            subscriptions.notification_item = 0;
            ++targets_[target_index].links;
            remove_unused_target.release();
            return;
        }

        auto &group = group_for(subscriptions);
        if (std::ranges::find(group.links, value) != group.links.end()) {
            return;
        }
        group.links.push_back(value);
        ++targets_[target_index].links;
        remove_unused_target.release();
    }

    void remove(ReferenceRoute *route, RuntimeGraph &graph, const void *target, TimeSeriesItemId item,
                ReferenceItemNotifyFunction notify_function) noexcept {
        auto &subscriptions = route == nullptr ? root_ : *route;
        if (subscriptions.item_notification_link == 0) {
            return;
        }

        const auto matches = [&](Link value) {
            const auto &notification_target = targets_[value.target];
            return value.item == item && notification_target.graph == &graph &&
                   notification_target.value == target && notification_target.notify_function == notify_function;
        };
        if (!is_group(subscriptions)) {
            const auto value = inline_link(subscriptions);
            if (!matches(value)) {
                return;
            }
            --targets_[value.target].links;
            subscriptions.item_notification_link = 0;
            subscriptions.notification_item = 0;
            return;
        }

        auto &group = group_for(subscriptions);
        const auto found = std::ranges::find_if(group.links, matches);
        if (found == group.links.end()) {
            return;
        }
        --targets_[found->target].links;
        *found = group.links.back();
        group.links.pop_back();
        if (group.links.size() == 1) {
            const auto remaining = group.links.front();
            release_group(subscriptions);
            set_inline(subscriptions, remaining);
        }
    }

    void notify(ReferenceRoute *route, OutputChangeContext &context, OutputChange change) {
        const auto &subscriptions = route == nullptr ? root_ : *route;
        if (subscriptions.item_notification_link == 0) {
            return;
        }
        if (!is_group(subscriptions)) {
            notify(inline_link(subscriptions), context, change);
            return;
        }

        for (const auto link : group_for(subscriptions).links) {
            notify(link, context, change);
        }
    }

    bool empty(const ReferenceRoute &route) const noexcept { return route.item_notification_link == 0; }

    void begin() noexcept { ++batch_depth_; }

    void cancel() noexcept {
        assert(batch_depth_ != 0);
        if (--batch_depth_ != 0) {
            return;
        }

        for (auto position = std::size_t{}; position < touched_.size(); ++position) {
            const auto index = touched_[position];
            targets_[index].touched = false;
        }
        touched_.clear();
    }

    void publish(OutputChangeContext &context) {
        assert(batch_depth_ != 0);
        if (batch_depth_ != 1) {
            --batch_depth_;
            return;
        }

        for (const auto index : touched_) {
            targets_[index].touched = false;
            const auto graph = targets_[index].graph;
            const auto value = targets_[index].value;
            const auto function = targets_[index].publish_function;
            auto target_context = context.for_graph(*graph);
            function(target_context, value);
        }
        touched_.clear();
        batch_depth_ = 0;
    }

  private:
    static constexpr auto group_flag = std::uint32_t{1} << 31;
    static constexpr auto index_mask = group_flag - 1;

    std::uint32_t require_target(RuntimeGraph &graph, void *value, ReferenceItemNotifyFunction notify_function,
                                 ReferenceItemPublishFunction publish_function) {
        const auto existing = std::ranges::find_if(targets_, [&](const auto &target) {
            return target.graph == &graph && target.value == value && target.notify_function == notify_function &&
                   target.publish_function == publish_function;
        });
        if (existing != targets_.end()) {
            return static_cast<std::uint32_t>(existing - targets_.begin());
        }

        const auto available = std::ranges::find_if(targets_, [](const auto &target) {
            return target.links == 0 && !target.touched;
        });
        if (available != targets_.end()) {
            *available = {&graph, value, notify_function, publish_function};
            return static_cast<std::uint32_t>(available - targets_.begin());
        }
        if (targets_.size() == index_mask) {
            throw std::length_error{"reference item notification target table is full"};
        }

        targets_.push_back({&graph, value, notify_function, publish_function});
        return static_cast<std::uint32_t>(targets_.size() - 1);
    }

    Group *allocate_group() {
        auto *storage = memory_->allocate(sizeof(Group), alignof(Group));
        return std::construct_at(static_cast<Group *>(storage), *memory_);
    }

    std::uint32_t store_group(Group *group) {
        const auto available = std::ranges::find(groups_, nullptr);
        if (available != groups_.end()) {
            *available = group;
            return static_cast<std::uint32_t>(available - groups_.begin());
        }
        if (groups_.size() == index_mask) {
            throw std::length_error{"reference item notification link group table is full"};
        }

        groups_.push_back(group);
        return static_cast<std::uint32_t>(groups_.size() - 1);
    }

    static bool is_group(const ReferenceRoute &route) noexcept {
        return (route.item_notification_link & group_flag) != 0;
    }

    static Link inline_link(const ReferenceRoute &route) noexcept {
        return {route.item_notification_link - 1, route.notification_item};
    }

    static void set_inline(ReferenceRoute &route, Link link) noexcept {
        route.item_notification_link = link.target + 1;
        route.notification_item = link.item;
    }

    Group &group_for(const ReferenceRoute &route) const noexcept {
        const auto index = (route.item_notification_link & index_mask) - 1;
        return *groups_[index];
    }

    void release_group(ReferenceRoute &route) noexcept {
        const auto index = (route.item_notification_link & index_mask) - 1;
        auto *group = groups_[index];
        std::destroy_at(group);
        memory_->deallocate(group, sizeof(Group), alignof(Group));
        groups_[index] = nullptr;
    }

    void notify(Link link, OutputChangeContext &context, OutputChange change) {
        auto &target = targets_[link.target];
        auto target_context = context.for_graph(*target.graph);
        target.notify_function(target_context, target.value, link.item, change);
        if (batch_depth_ == 0) {
            target.publish_function(target_context, target.value);
            return;
        }
        if (!target.touched) {
            touched_.push_back(link.target);
            target.touched = true;
        }
    }

    ReferenceRoute root_;
    std::pmr::vector<NotificationTarget> targets_;
    std::pmr::vector<Group *> groups_;
    std::pmr::vector<std::uint32_t> touched_;
    std::pmr::memory_resource *memory_;
    std::size_t batch_depth_{};
};

ReferenceProvider::ReferenceProvider(std::pmr::memory_resource &memory, ReferenceRoot &root,
                                     DynamicSubscribers *root_subscribers)
    : memory_{&memory}, root_{&root}, representations_{&memory}, subscriber_groups_{&memory} {
    subscriber_groups_.push_back(root_subscribers);
}

ReferenceProvider::~ReferenceProvider() {
    for (auto *component = components_; component != nullptr; component = component->next_) {
        component->detach();
    }
    for (auto *representation : representations_ | std::views::reverse) {
        if (representation == nullptr) {
            continue;
        }
        remove_reference_observer(representation->input(), owner(), representation);
    }
    for (auto *representation : representations_ | std::views::reverse) {
        if (representation == nullptr) {
            continue;
        }
        representation->destroy(*memory_);
    }
    while (components_ != nullptr) {
        auto *component = components_;
        components_ = component->next_;
        component->destroy(*memory_);
    }
    for (auto *subscribers : subscriber_groups_) {
        DynamicSubscribers::release(subscribers);
    }
    if (item_notification_links_ != nullptr) {
        std::destroy_at(item_notification_links_);
        memory_->deallocate(item_notification_links_, sizeof(ReferenceItemNotificationLinks),
                            alignof(ReferenceItemNotificationLinks));
    }
}

ReferenceTarget ReferenceProvider::representation_target(std::uint32_t index) const {
    assert(representations_[index] != nullptr);
    return representations_[index]->target();
}

void ReferenceProvider::add_root_subscriber(RuntimeGraph &graph, std::uint32_t input_offset, std::size_t data,
                                            std::uint32_t node_index, bool active, bool required,
                                            InputChangeFunction callback) {
    auto &subscribers = DynamicSubscribers::ensure(subscriber_groups_.front(), *memory_);
    subscribers.add(graph, input_offset, data, node_index, active, required, callback);
}

void ReferenceProvider::add_root_observer(RuntimeGraph &graph, void *target, DynamicObserverFunction function) {
    auto &subscribers = DynamicSubscribers::ensure(subscriber_groups_.front(), *memory_);
    subscribers.add_observer(graph, target, function);
}

void ReferenceProvider::remove_root_subscriber(RuntimeGraph &graph, std::size_t input_offset) noexcept {
    if (subscriber_groups_.front() != nullptr) {
        subscriber_groups_.front()->remove(graph, input_offset);
    }
}

void ReferenceProvider::remove_root_observer(RuntimeGraph &graph, const void *target) noexcept {
    if (subscriber_groups_.front() != nullptr) {
        subscriber_groups_.front()->remove_observer(graph, target);
    }
}

void ReferenceProvider::notify_root(OutputChangeContext &context, OutputChange change) {
    context.notify(subscriber_groups_.front(), change);
    if (item_notification_links_ != nullptr) {
        item_notification_links_->notify(nullptr, context, change);
    }
}

void ReferenceProvider::add_subscriber(ReferenceRoute &route, RuntimeGraph &graph, std::uint32_t input_offset,
                                       std::size_t data, std::uint32_t node_index, bool active, bool required,
                                       InputChangeFunction callback) {
    if (route.subscriber_group == 0) {
        subscriber_groups_.push_back(nullptr);
        route.subscriber_group = static_cast<std::uint32_t>(subscriber_groups_.size());
    }

    auto &subscribers = DynamicSubscribers::ensure(subscriber_groups_[route.subscriber_group - 1], *memory_);
    subscribers.add(graph, input_offset, data, node_index, active, required, callback);
}

void ReferenceProvider::add_observer(ReferenceRoute &route, RuntimeGraph &graph, void *target,
                                     DynamicObserverFunction function) {
    if (route.subscriber_group == 0) {
        subscriber_groups_.push_back(nullptr);
        route.subscriber_group = static_cast<std::uint32_t>(subscriber_groups_.size());
    }

    auto &subscribers = DynamicSubscribers::ensure(subscriber_groups_[route.subscriber_group - 1], *memory_);
    subscribers.add_observer(graph, target, function);
}

void ReferenceProvider::remove_subscriber(ReferenceRoute &route, RuntimeGraph &graph,
                                          std::size_t input_offset) noexcept {
    if (route.subscriber_group != 0) {
        subscriber_groups_[route.subscriber_group - 1]->remove(graph, input_offset);
    }
}

void ReferenceProvider::remove_observer(ReferenceRoute &route, RuntimeGraph &graph, const void *target) noexcept {
    if (route.subscriber_group != 0) {
        subscriber_groups_[route.subscriber_group - 1]->remove_observer(graph, target);
    }
}

void ReferenceProvider::notify(ReferenceRoute &route, OutputChangeContext &context, OutputChange change) {
    if (route.subscriber_group != 0) {
        context.notify(subscriber_groups_[route.subscriber_group - 1], change);
    }
    if (item_notification_links_ != nullptr) {
        item_notification_links_->notify(&route, context, change);
    }
}

void ReferenceProvider::begin_item_notification_batch() noexcept {
    if (item_notification_links_ != nullptr) {
        item_notification_links_->begin();
    }
}

void ReferenceProvider::cancel_item_notification_batch() noexcept {
    if (item_notification_links_ != nullptr) {
        item_notification_links_->cancel();
    }
}

void ReferenceProvider::publish_item_notification_batch(OutputChangeContext &context) {
    if (item_notification_links_ != nullptr) {
        item_notification_links_->publish(context);
    }
}

void ReferenceProvider::add_item_notification_link(ReferenceRoute *route, RuntimeGraph &graph, void *target,
                                                   TimeSeriesItemId item,
                                                   ReferenceItemNotifyFunction notify_function,
                                                   ReferenceItemPublishFunction publish_function) {
    if (item_notification_links_ == nullptr) {
        auto *storage =
            memory_->allocate(sizeof(ReferenceItemNotificationLinks), alignof(ReferenceItemNotificationLinks));
        auto release = util::scope_exit{[&]() noexcept {
            memory_->deallocate(storage, sizeof(ReferenceItemNotificationLinks),
                                alignof(ReferenceItemNotificationLinks));
        }};
        item_notification_links_ =
            std::construct_at(static_cast<ReferenceItemNotificationLinks *>(storage), *memory_);
        release.release();
    }
    item_notification_links_->add(route, graph, target, item, notify_function, publish_function);
}

void ReferenceProvider::remove_item_notification_link(ReferenceRoute *route, RuntimeGraph &graph,
                                                      const void *target, TimeSeriesItemId item,
                                                      ReferenceItemNotifyFunction notify_function) noexcept {
    if (item_notification_links_ != nullptr) {
        item_notification_links_->remove(route, graph, target, item, notify_function);
    }
}

void ReferenceProvider::add_cycle_callback(void *target, void (*function)(void *) noexcept) {
    owner().add_cycle_callback(target, function);
}

void ReferenceProvider::remove_cycle_callback(const void *target) noexcept {
    owner().remove_cycle_callback(target);
}

bool ReferenceProvider::release_route(ReferenceRoute &route) noexcept {
    const auto release_subscribers = [&](ReferenceRoute &released) {
        if (released.subscriber_group == 0) {
            return true;
        }

        auto &subscribers = subscriber_groups_[released.subscriber_group - 1];
        if (subscribers != nullptr && !subscribers->empty()) {
            return false;
        }

        DynamicSubscribers::release(subscribers);
        subscribers = nullptr;
        released.subscriber_group = 0;
        return true;
    };
    const auto retired = ReferenceAddress{&route};
    const auto depends_on_retired_route = [&](ReferenceAddress address) {
        while (address) {
            if (address == retired) {
                return true;
            }
            const auto *parent = address.get<ReferenceRoute>();
            if (parent == nullptr) {
                return false;
            }
            address = parent->parent;
        }
        return false;
    };

    for (auto index = representations_.size(); index != 0; --index) {
        auto *&representation = representations_[index - 1];
        if (representation == nullptr || !depends_on_retired_route(representation->input())) {
            continue;
        }

        auto &representation_route = *representation->address().get<ReferenceRoute>();
        if (!release_subscribers(representation_route)) {
            return false;
        }
        remove_reference_observer(representation->input(), owner(), representation);
        representation->destroy(*memory_);
        representation = nullptr;
    }

    return (item_notification_links_ == nullptr || item_notification_links_->empty(route)) &&
           release_subscribers(route);
}

void ReferenceProvider::add_component(ReferenceProviderComponent &component) noexcept {
    component.next_ = components_;
    components_ = &component;
}

ReferenceAddress ReferenceProvider::representation(ReferenceAddress input, const TimeSeriesType &requested_type) {
    const auto input_target = reference_target(input);
    if (input_target.runtime_type->satisfies_reference_request(requested_type)) {
        return input;
    }
    const auto root_target = reference_target(ReferenceAddress{root_});
    if (root_target.runtime_type->satisfies_reference_request(requested_type)) {
        return ReferenceAddress{root_};
    }

    for (auto *representation : representations_) {
        if (representation == nullptr) {
            continue;
        }
        if (representation->input() == input &&
            representation->target().runtime_type->satisfies_reference_request(requested_type)) {
            return representation->address();
        }
    }

    auto *result =
        input_target.runtime_type->create_reference_representation(*this, input, requested_type, *memory_);
    auto release = util::scope_exit{[&]() noexcept { result->destroy(*memory_); }};
    result->bind(static_cast<std::uint32_t>(representations_.size()));
    representations_.push_back(result);
    auto remove_entry = util::scope_exit{[&]() noexcept { representations_.pop_back(); }};
    add_reference_observer(input, owner(), result, [](OutputChangeContext &context, void *value, OutputChange change) {
        static_cast<ReferenceRepresentation *>(value)->notify(context, change);
    });
    remove_entry.release();
    release.release();
    return result->address();
}

namespace {

ReferenceRoot &reference_root(ReferenceAddress address) noexcept {
    while (auto *route = address.get<ReferenceRoute>()) {
        address = route->parent;
    }
    return *address.get<ReferenceRoot>();
}

ReferenceProvider &require_provider(ReferenceRoot &root) {
    if (auto *provider = root.data.get<ReferenceProvider>()) {
        return *provider;
    }

    auto *subscribers = root.data.get<DynamicSubscribers>();
    auto &memory = root.graph->memory_resource();
    auto *storage = memory.allocate(sizeof(ReferenceProvider), alignof(ReferenceProvider));
    auto release = util::scope_exit{
        [&]() noexcept { memory.deallocate(storage, sizeof(ReferenceProvider), alignof(ReferenceProvider)); }};
    auto *provider = std::construct_at(static_cast<ReferenceProvider *>(storage), memory, root, subscribers);
    root.data = provider;
    release.release();
    return *provider;
}

ReferenceProvider &route_provider(ReferenceAddress address) noexcept {
    return *reference_root(address).data.get<ReferenceProvider>();
}

} // namespace

ReferenceTarget reference_target(ReferenceAddress address) {
    if (auto *root = address.get<ReferenceRoot>()) {
        const auto node = root->graph->node(*root->node);
        return {&node.input_last_updated(0), node.input_value(0), root->runtime_type};
    }
    const auto &route = *address.get<ReferenceRoute>();
    if (route.is_representation()) {
        return route_provider(address).representation_target(route.representation());
    }

    const auto parent = reference_target(route.parent);
    const auto item = parent.runtime_type->item(parent.value, route.item());
    return {&item.last_updated, item.value, &item.runtime_type};
}

ReferenceAddress reference_representation(ReferenceAddress input, const TimeSeriesType &requested_type) {
    const auto target = reference_target(input);
    if (&target.runtime_type->type() == &requested_type) {
        return input;
    }
    return require_reference_provider(input).representation(input, requested_type);
}

ReferenceProvider &require_reference_provider(ReferenceAddress address) {
    return require_provider(reference_root(address));
}

void add_reference_subscriber(ReferenceAddress address, RuntimeGraph &graph, std::uint32_t input_offset,
                              std::size_t data, std::uint32_t node_index, bool active, bool required,
                              InputChangeFunction callback) {
    if (auto *root = address.get<ReferenceRoot>()) {
        if (auto *provider = root->data.get<ReferenceProvider>()) {
            provider->add_root_subscriber(graph, input_offset, data, node_index, active, required, callback);
            return;
        }
        auto *subscribers = root->data.get<DynamicSubscribers>();
        auto &result = DynamicSubscribers::ensure(subscribers, root->graph->memory_resource());
        root->data = subscribers;
        result.add(graph, input_offset, data, node_index, active, required, callback);
        return;
    }
    auto &route = *address.get<ReferenceRoute>();
    route_provider(address).add_subscriber(route, graph, input_offset, data, node_index, active, required, callback);
}

void add_reference_observer(ReferenceAddress address, RuntimeGraph &graph, void *target,
                            DynamicObserverFunction function) {
    if (auto *root = address.get<ReferenceRoot>()) {
        if (auto *provider = root->data.get<ReferenceProvider>()) {
            provider->add_root_observer(graph, target, function);
            return;
        }
        auto *subscribers = root->data.get<DynamicSubscribers>();
        auto &result = DynamicSubscribers::ensure(subscribers, root->graph->memory_resource());
        root->data = subscribers;
        result.add_observer(graph, target, function);
        return;
    }
    auto &route = *address.get<ReferenceRoute>();
    route_provider(address).add_observer(route, graph, target, function);
}

void remove_reference_subscriber(ReferenceAddress address, RuntimeGraph &graph, std::size_t input_offset) noexcept {
    if (auto *root = address.get<ReferenceRoot>()) {
        if (auto *provider = root->data.get<ReferenceProvider>()) {
            provider->remove_root_subscriber(graph, input_offset);
        } else if (auto *subscribers = root->data.get<DynamicSubscribers>()) {
            subscribers->remove(graph, input_offset);
        }
        return;
    }
    auto &route = *address.get<ReferenceRoute>();
    route_provider(address).remove_subscriber(route, graph, input_offset);
}

void remove_reference_observer(ReferenceAddress address, RuntimeGraph &graph, const void *target) noexcept {
    if (auto *root = address.get<ReferenceRoot>()) {
        if (auto *provider = root->data.get<ReferenceProvider>()) {
            provider->remove_root_observer(graph, target);
        } else if (auto *subscribers = root->data.get<DynamicSubscribers>()) {
            subscribers->remove_observer(graph, target);
        }
        return;
    }
    auto &route = *address.get<ReferenceRoute>();
    route_provider(address).remove_observer(route, graph, target);
}

void notify_reference(ReferenceAddress address, OutputChangeContext &context, OutputChange change) {
    if (auto *root = address.get<ReferenceRoot>()) {
        if (auto *provider = root->data.get<ReferenceProvider>()) {
            provider->notify_root(context, change);
        } else {
            context.notify(root->data.get<DynamicSubscribers>(), change);
        }
        return;
    }
    auto &route = *address.get<ReferenceRoute>();
    route_provider(address).notify(route, context, change);
}

void add_reference_item_notification_link(ReferenceAddress address, RuntimeGraph &graph, void *target,
                                          TimeSeriesItemId item, ReferenceItemNotifyFunction notify_function,
                                          ReferenceItemPublishFunction publish_function) {
    auto &provider = require_reference_provider(address);
    provider.add_item_notification_link(address.get<ReferenceRoute>(), graph, target, item, notify_function,
                                        publish_function);
}

void remove_reference_item_notification_link(ReferenceAddress address, RuntimeGraph &graph, const void *target,
                                             TimeSeriesItemId item,
                                             ReferenceItemNotifyFunction notify_function) noexcept {
    if (!address) {
        return;
    }
    auto *root = reference_root(address).data.get<ReferenceProvider>();
    if (root == nullptr) {
        return;
    }
    root->remove_item_notification_link(address.get<ReferenceRoute>(), graph, target, item, notify_function);
}

void destroy_reference_root(ReferenceRoot &root, std::pmr::memory_resource &memory) noexcept {
    if (auto *provider = root.data.get<ReferenceProvider>()) {
        std::destroy_at(provider);
        memory.deallocate(provider, sizeof(ReferenceProvider), alignof(ReferenceProvider));
    } else {
        DynamicSubscribers::release(root.data.get<DynamicSubscribers>());
    }
    std::destroy_at(&root);
}

} // namespace bg::details
