#include <bg/time_series/ref/ref_provider.hpp>

#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/graph/node.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/util/hash_dictionary.hpp>
#include <bg/util/scope_exit.hpp>

#include <cassert>
#include <limits>
#include <memory>
#include <ranges>

namespace bg::details {

ReferenceRepresentation::~ReferenceRepresentation() = default;

namespace {

struct ObservedDictionaryItem {
    DynamicSubscribers *subscribers{};
    ReferenceRoute route;
    bool item_valid{};
    bool retained{};
    std::uint32_t retired{};
};

using ObservedItemMap = util::hash_dictionary<TimeSeriesItemId, ObservedDictionaryItem>;

struct ObservedDictionary {
    ObservedDictionary(DictionaryObservers &owner, ReferenceAddress reference,
                       RetainedDictionaryKey retained) noexcept
        : owner{&owner}, reference{reference}, runtime_type{retained.runtime_type}, value{retained.value} {}

    DictionaryObservers *owner;
    ReferenceAddress reference;
    const TimeSeriesDictionaryRuntimeType *runtime_type;
    void *value;
    ObservedItemMap items;
    bool registered{};
};

} // namespace

class DictionaryObservers {
    struct RetiredKey {
        ObservedDictionary *dictionary;
        TimeSeriesItemId id;
        EvaluationTime time;
    };

  public:
    DictionaryObservers(ReferenceProvider &provider, std::pmr::memory_resource &memory)
        : provider_{&provider}, observed_dictionaries_{&memory}, retired_keys_{&memory}, memory_{&memory} {}

    ~DictionaryObservers() {
        detach();
        for (auto *dictionary : observed_dictionaries_) {
            release_dictionary(*dictionary);
        }
    }

    void detach() noexcept {
        for (auto *dictionary : observed_dictionaries_) {
            if (dictionary->registered) {
                remove_reference_observer(dictionary->reference, provider_->owner(), dictionary);
                dictionary->registered = false;
            }
        }
    }

    [[nodiscard]] DictionaryKeySubscription add(ReferenceAddress reference, const ScalarData &key, RuntimeGraph &graph,
                                                std::uint32_t node_index) {
        release_retired(graph.evaluation_time());

        const auto target = reference_target(reference);
        const auto *runtime_type = dynamic_cast<const TimeSeriesDictionaryRuntimeType *>(target.runtime_type);
        if (runtime_type == nullptr || target.value == nullptr) {
            return {};
        }

        auto retained = runtime_type->retain_key(const_cast<void *>(target.value), key, *memory_);
        if (retained.runtime_type == nullptr) {
            return {};
        }
        auto release_retention = util::scope_exit{[&]() noexcept {
            retained.runtime_type->release_key(retained.value, retained.id, *memory_);
        }};

        auto &dictionary = require_dictionary(reference, retained);
        if (retired_keys_.capacity() <= entry_count_) {
            retired_keys_.reserve(std::max<std::size_t>(retired_keys_.capacity() * 2, 8));
        }
        auto remove_empty_dictionary = util::scope_exit{[&]() noexcept {
            if (dictionary.items.empty()) {
                erase_dictionary(dictionary);
            }
        }};
        auto [entry, inserted] = dictionary.items.try_emplace(retained.id, *memory_);
        auto remove_new_key = util::scope_exit{[&]() noexcept {
            if (inserted) {
                erase_key(dictionary, retained.id);
            }
        }};

        if (inserted) {
            entry->route.bind_item(reference, retained.id);
            if (dictionary.runtime_type->key_live(dictionary.value, retained.id)) {
                entry->item_valid =
                    dictionary.runtime_type->item(dictionary.value, retained.id).last_updated > MIN_DT;
            }
            ++entry_count_;
            entry->retained = true;
            release_retention.release();
        } else if (!entry->retained) {
            entry->retained = true;
            entry->item_valid = dictionary.runtime_type->key_live(dictionary.value, retained.id) &&
                                dictionary.runtime_type->item(dictionary.value, retained.id).last_updated > MIN_DT;
            release_retention.release();
        }

        auto &subscribers = DynamicSubscribers::ensure(entry->subscribers, *memory_);
        subscribers.add(graph, node_index, 0, node_index, true, false);

        remove_new_key.release();
        remove_empty_dictionary.release();
        return DictionaryKeySubscription{&dictionary, retained.id};
    }

    void remove(DictionaryKeySubscription subscription, RuntimeGraph &graph, std::uint32_t node_index) noexcept {
        auto &dictionary = *static_cast<ObservedDictionary *>(subscription.dictionary_);
        auto *entry = dictionary.items.find(subscription.item_);
        if (entry == nullptr || entry->subscribers == nullptr) {
            return;
        }
        entry->subscribers->remove(graph, node_index);
        if (!entry->subscribers->empty()) {
            return;
        }
        if (entry->retired == 0) {
            retired_keys_.push_back({&dictionary, subscription.item_, graph.evaluation_time()});
            entry->retired = static_cast<std::uint32_t>(retired_keys_.size());
        } else {
            retired_keys_[entry->retired - 1].time = graph.evaluation_time();
        }
        if (entry->retained) {
            dictionary.runtime_type->release_key(dictionary.value, subscription.item_, *memory_);
            entry->retained = false;
        }
    }

  private:
    using ChangeFunction = void (TimeSeriesDictionaryRuntimeType::*)(const EvaluationTime &, const void *, void *,
                                                                     TimeSeriesRuntimeType::ItemIdFunction) const;

    ObservedDictionary *find_dictionary(ReferenceAddress reference, RetainedDictionaryKey retained) const noexcept {
        const auto found = std::ranges::find_if(
            observed_dictionaries_, [&](const auto *candidate) {
                return candidate->reference == reference && candidate->runtime_type == retained.runtime_type &&
                       candidate->value == retained.value;
            });
        return found == observed_dictionaries_.end() ? nullptr : *found;
    }

    ObservedDictionary &require_dictionary(ReferenceAddress reference, RetainedDictionaryKey retained) {
        if (auto *existing = find_dictionary(reference, retained)) {
            return *existing;
        }

        auto *storage = memory_->allocate(sizeof(ObservedDictionary), alignof(ObservedDictionary));
        auto *result = std::construct_at(static_cast<ObservedDictionary *>(storage), *this, reference, retained);
        auto release = util::scope_exit{[&]() noexcept { release_dictionary(*result); }};
        observed_dictionaries_.push_back(result);
        auto remove_entry = util::scope_exit{[&]() noexcept { observed_dictionaries_.pop_back(); }};
        add_reference_observer(reference, provider_->owner(), result, &notify_dictionary);
        result->registered = true;
        remove_entry.release();
        release.release();
        return *result;
    }

    void erase_key(ObservedDictionary &dictionary, TimeSeriesItemId id) noexcept {
        auto *entry = dictionary.items.find(id);
        if (entry->retained) {
            dictionary.runtime_type->release_key(dictionary.value, id, *memory_);
        }
        dictionary.items.erase_with(id, *memory_, [](TimeSeriesItemId *) noexcept {},
                                    [](ObservedDictionaryItem *value) noexcept {
                                       DynamicSubscribers::release(value->subscribers);
                                       std::destroy_at(value);
                                   });
        --entry_count_;
    }

    void erase_dictionary(ObservedDictionary &dictionary) noexcept {
        const auto position = std::ranges::find(observed_dictionaries_, &dictionary);
        *position = observed_dictionaries_.back();
        observed_dictionaries_.pop_back();
        release_dictionary(dictionary);
    }

    void release_dictionary(ObservedDictionary &dictionary) noexcept {
        if (dictionary.registered) {
            remove_reference_observer(dictionary.reference, provider_->owner(), &dictionary);
        }
        for (const auto entry : dictionary.items) {
            if (entry.value.retained) {
                dictionary.runtime_type->release_key(dictionary.value, entry.key, *memory_);
            }
        }
        dictionary.items.release_with(
            *memory_, [](TimeSeriesItemId *) noexcept {},
            [](ObservedDictionaryItem *entry) noexcept {
                DynamicSubscribers::release(entry->subscribers);
                std::destroy_at(entry);
            });
        std::destroy_at(&dictionary);
        memory_->deallocate(&dictionary, sizeof(ObservedDictionary), alignof(ObservedDictionary));
    }

    static void notify_dictionary(OutputChangeContext &context, void *value, OutputChange change) {
        auto &dictionary = *static_cast<ObservedDictionary *>(value);
        dictionary.owner->notify(context, dictionary, change);
    }

    void notify(OutputChangeContext &context, ObservedDictionary &dictionary, OutputChange change) {
        const auto target = reference_target(dictionary.reference);
        const auto *runtime_type = dynamic_cast<const TimeSeriesDictionaryRuntimeType *>(target.runtime_type);
        if (runtime_type == nullptr || target.value == nullptr) {
            if (change == OutputChange::invalidation) {
                notify_all(context, dictionary, change);
            }
            return;
        }
        if (runtime_type != dictionary.runtime_type || target.value != dictionary.value) {
            notify_all(context, dictionary, change);
            return;
        }

        if (change != OutputChange::update) {
            notify_present(context, dictionary, target, *runtime_type, change);
            return;
        }

        notify_changes(context, dictionary, target, *runtime_type,
                       &TimeSeriesDictionaryRuntimeType::for_each_added, OutputChange::revalidation);
        notify_changes(context, dictionary, target, *runtime_type,
                       &TimeSeriesDictionaryRuntimeType::for_each_removed, OutputChange::invalidation);
        notify_changes(context, dictionary, target, *runtime_type,
                       &TimeSeriesDictionaryRuntimeType::for_each_modified, OutputChange::update);
    }

    static void notify_all(OutputChangeContext &context, ObservedDictionary &dictionary, OutputChange change) {
        for (const auto entry : dictionary.items) {
            context.notify(entry.value.subscribers, change);
        }
    }

    static void notify_present(OutputChangeContext &context, ObservedDictionary &dictionary, ReferenceTarget target,
                               const TimeSeriesDictionaryRuntimeType &runtime_type, OutputChange change) {
        for (const auto entry : dictionary.items) {
            if (runtime_type.key_live(target.value, entry.key)) {
                entry.value.item_valid = runtime_type.item(target.value, entry.key).last_updated > MIN_DT;
                context.notify(entry.value.subscribers, change);
            }
        }
    }

    void notify_changes(OutputChangeContext &context, ObservedDictionary &dictionary, ReferenceTarget target,
                        const TimeSeriesDictionaryRuntimeType &runtime_type, ChangeFunction changes,
                        OutputChange change) {
        struct Callback {
            DictionaryObservers *owner;
            ObservedDictionary *dictionary;
            const TimeSeriesDictionaryRuntimeType *runtime_type;
            ReferenceTarget target;
            OutputChangeContext *context;
            OutputChange change;
        } callback{this, &dictionary, &runtime_type, target, &context, change};

        (runtime_type.*changes)(*target.last_updated, target.value, &callback, [](void *value, TimeSeriesItemId id) {
            auto &callback = *static_cast<Callback *>(value);
            callback.owner->notify_item(*callback.context, *callback.dictionary, *callback.runtime_type,
                                        callback.target, id, callback.change);
        });
    }

    static void notify_item(OutputChangeContext &context, ObservedDictionary &dictionary,
                            const TimeSeriesDictionaryRuntimeType &runtime_type, ReferenceTarget target,
                            TimeSeriesItemId id, OutputChange change) {
        auto *entry = dictionary.items.find(id);
        if (entry == nullptr) {
            return;
        }

        const auto item_is_reference =
            dynamic_cast<const TimeSeriesReferenceRuntimeType *>(&runtime_type.item_runtime_type()) != nullptr;
        if (change != OutputChange::update || item_is_reference) {
            entry->item_valid = change != OutputChange::invalidation;
            context.notify(entry->subscribers, change);
            return;
        }

        const auto item = runtime_type.item(target.value, id);
        const auto item_valid = item.last_updated > MIN_DT;
        if (item_valid != entry->item_valid) {
            entry->item_valid = item_valid;
            context.notify(entry->subscribers,
                           item_valid ? OutputChange::revalidation : OutputChange::invalidation);
            return;
        }
        if (item_valid) {
            notify_reference(ReferenceAddress{&entry->route}, context, OutputChange::update);
        }
    }

    void release_retired(EvaluationTime evaluation_time) noexcept {
        auto retained = retired_keys_.begin();
        for (const auto retired : retired_keys_) {
            auto *entry = retired.dictionary->items.find(retired.id);
            if (entry == nullptr) {
                continue;
            }
            if (entry->subscribers != nullptr && !entry->subscribers->empty()) {
                entry->retired = 0;
                continue;
            }
            if (retired.time >= evaluation_time || !provider_->release_route(entry->route)) {
                *retained++ = retired;
                entry->retired = static_cast<std::uint32_t>(retained - retired_keys_.begin());
                continue;
            }
            erase_key(*retired.dictionary, retired.id);
        }
        retired_keys_.erase(retained, retired_keys_.end());

        auto dictionary = std::size_t{};
        while (dictionary < observed_dictionaries_.size()) {
            if (observed_dictionaries_[dictionary]->items.empty()) {
                erase_dictionary(*observed_dictionaries_[dictionary]);
            } else {
                ++dictionary;
            }
        }
    }

    ReferenceProvider *provider_;
    std::pmr::vector<ObservedDictionary *> observed_dictionaries_;
    std::pmr::vector<RetiredKey> retired_keys_;
    std::size_t entry_count_{};
    std::pmr::memory_resource *memory_;
};

class ReferenceItemSubscribers {
    struct Subscription {
        std::uint32_t subscriber;
        TimeSeriesItemId item;

        bool operator==(const Subscription &) const = default;
    };

    struct Subscriber {
        RuntimeGraph *graph;
        void *target;
        ReferenceItemChangeFunction change_function;
        ReferenceItemPublishFunction publish_function;
        std::uint32_t references{};
        bool touched{};
    };

    struct Group {
        explicit Group(std::pmr::memory_resource &memory) : subscriptions{&memory} {}

        std::pmr::vector<Subscription> subscriptions;
    };

  public:
    explicit ReferenceItemSubscribers(std::pmr::memory_resource &memory)
        : subscribers_{&memory}, groups_{&memory}, touched_{&memory}, memory_{&memory} {}

    ~ReferenceItemSubscribers() {
        for (auto *group : groups_) {
            if (group == nullptr) {
                continue;
            }
            std::destroy_at(group);
            memory_->deallocate(group, sizeof(Group), alignof(Group));
        }
    }

    void add(ReferenceRoute *route, RuntimeGraph &graph, void *target, TimeSeriesItemId item,
             ReferenceItemChangeFunction change_function, ReferenceItemPublishFunction publish_function) {
        auto &subscriptions = route == nullptr ? root_ : *route;
        const auto subscriber_count = subscribers_.size();
        const auto subscriber = require_subscriber(graph, target, change_function, publish_function);
        auto remove_unused_subscriber = util::scope_exit{[&]() noexcept {
            if (subscribers_.size() != subscriber_count && subscribers_.back().references == 0) {
                subscribers_.pop_back();
            }
        }};
        const auto value = Subscription{subscriber, item};

        if (subscriptions.item_subscriber == 0) {
            set_inline(subscriptions, value);
            ++subscribers_[subscriber].references;
            remove_unused_subscriber.release();
            return;
        }
        if (!is_group(subscriptions)) {
            if (inline_subscription(subscriptions) == value) {
                return;
            }

            auto *group = allocate_group();
            auto release_group = util::scope_exit{[&]() noexcept {
                std::destroy_at(group);
                memory_->deallocate(group, sizeof(Group), alignof(Group));
            }};
            group->subscriptions.push_back(inline_subscription(subscriptions));
            group->subscriptions.push_back(value);

            const auto group_index = store_group(group);
            release_group.release();
            subscriptions.item_subscriber = group_flag | (group_index + 1);
            subscriptions.subscriber_item = 0;
            ++subscribers_[subscriber].references;
            remove_unused_subscriber.release();
            return;
        }

        auto &group = group_for(subscriptions);
        if (std::ranges::find(group.subscriptions, value) != group.subscriptions.end()) {
            return;
        }
        group.subscriptions.push_back(value);
        ++subscribers_[subscriber].references;
        remove_unused_subscriber.release();
    }

    void remove(ReferenceRoute *route, RuntimeGraph &graph, const void *target, TimeSeriesItemId item,
                ReferenceItemChangeFunction change_function) noexcept {
        auto &subscriptions = route == nullptr ? root_ : *route;
        if (subscriptions.item_subscriber == 0) {
            return;
        }

        const auto matches = [&](Subscription value) {
            const auto &subscriber = subscribers_[value.subscriber];
            return value.item == item && subscriber.graph == &graph && subscriber.target == target &&
                   subscriber.change_function == change_function;
        };
        if (!is_group(subscriptions)) {
            const auto value = inline_subscription(subscriptions);
            if (!matches(value)) {
                return;
            }
            --subscribers_[value.subscriber].references;
            subscriptions.item_subscriber = 0;
            subscriptions.subscriber_item = 0;
            return;
        }

        auto &group = group_for(subscriptions);
        const auto found = std::ranges::find_if(group.subscriptions, matches);
        if (found == group.subscriptions.end()) {
            return;
        }
        --subscribers_[found->subscriber].references;
        *found = group.subscriptions.back();
        group.subscriptions.pop_back();
        if (group.subscriptions.size() == 1) {
            const auto remaining = group.subscriptions.front();
            release_group(subscriptions);
            set_inline(subscriptions, remaining);
        }
    }

    void notify(ReferenceRoute *route, OutputChangeContext &context, OutputChange change) {
        const auto &subscriptions = route == nullptr ? root_ : *route;
        if (subscriptions.item_subscriber == 0) {
            return;
        }
        if (!is_group(subscriptions)) {
            notify(inline_subscription(subscriptions), context, change);
            return;
        }

        for (const auto subscription : group_for(subscriptions).subscriptions) {
            notify(subscription, context, change);
        }
    }

    bool empty(const ReferenceRoute &route) const noexcept { return route.item_subscriber == 0; }

    void begin() noexcept { ++batch_depth_; }

    void cancel() noexcept {
        assert(batch_depth_ != 0);
        if (--batch_depth_ != 0) {
            return;
        }

        for (auto position = std::size_t{}; position < touched_.size(); ++position) {
            const auto index = touched_[position];
            subscribers_[index].touched = false;
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
            subscribers_[index].touched = false;
            const auto graph = subscribers_[index].graph;
            const auto target = subscribers_[index].target;
            const auto function = subscribers_[index].publish_function;
            auto target_context = context.for_graph(*graph);
            function(target_context, target);
        }
        touched_.clear();
        batch_depth_ = 0;
    }

  private:
    static constexpr auto group_flag = std::uint32_t{1} << 31;
    static constexpr auto index_mask = group_flag - 1;

    std::uint32_t require_subscriber(RuntimeGraph &graph, void *target,
                                     ReferenceItemChangeFunction change_function,
                                     ReferenceItemPublishFunction publish_function) {
        const auto existing = std::ranges::find_if(subscribers_, [&](const auto &subscriber) {
            return subscriber.graph == &graph && subscriber.target == target &&
                   subscriber.change_function == change_function && subscriber.publish_function == publish_function;
        });
        if (existing != subscribers_.end()) {
            return static_cast<std::uint32_t>(existing - subscribers_.begin());
        }

        const auto available = std::ranges::find_if(
            subscribers_, [](const auto &subscriber) { return subscriber.references == 0 && !subscriber.touched; });
        if (available != subscribers_.end()) {
            *available = {&graph, target, change_function, publish_function};
            return static_cast<std::uint32_t>(available - subscribers_.begin());
        }
        if (subscribers_.size() == index_mask) {
            throw std::length_error{"reference item subscriber table is full"};
        }

        subscribers_.push_back({&graph, target, change_function, publish_function});
        return static_cast<std::uint32_t>(subscribers_.size() - 1);
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
            throw std::length_error{"reference item subscription group table is full"};
        }

        groups_.push_back(group);
        return static_cast<std::uint32_t>(groups_.size() - 1);
    }

    static bool is_group(const ReferenceRoute &route) noexcept {
        return (route.item_subscriber & group_flag) != 0;
    }

    static Subscription inline_subscription(const ReferenceRoute &route) noexcept {
        return {route.item_subscriber - 1, route.subscriber_item};
    }

    static void set_inline(ReferenceRoute &route, Subscription subscription) noexcept {
        route.item_subscriber = subscription.subscriber + 1;
        route.subscriber_item = subscription.item;
    }

    Group &group_for(const ReferenceRoute &route) const noexcept {
        const auto index = (route.item_subscriber & index_mask) - 1;
        return *groups_[index];
    }

    void release_group(ReferenceRoute &route) noexcept {
        const auto index = (route.item_subscriber & index_mask) - 1;
        auto *group = groups_[index];
        std::destroy_at(group);
        memory_->deallocate(group, sizeof(Group), alignof(Group));
        groups_[index] = nullptr;
    }

    void notify(Subscription subscription, OutputChangeContext &context, OutputChange change) {
        auto &subscriber = subscribers_[subscription.subscriber];
        auto target_context = context.for_graph(*subscriber.graph);
        subscriber.change_function(target_context, subscriber.target, subscription.item, change);
        if (batch_depth_ == 0) {
            subscriber.publish_function(target_context, subscriber.target);
            return;
        }
        if (!subscriber.touched) {
            touched_.push_back(subscription.subscriber);
            subscriber.touched = true;
        }
    }

    ReferenceRoute root_;
    std::pmr::vector<Subscriber> subscribers_;
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
    if (dictionary_observers_ != nullptr) {
        dictionary_observers_->detach();
    }
    for (auto *representation : representations_ | std::views::reverse) {
        if (representation == nullptr) {
            continue;
        }
        remove_reference_observer(representation->input(), owner(), representation);
    }
    if (dictionary_observers_ != nullptr) {
        std::destroy_at(dictionary_observers_);
        memory_->deallocate(dictionary_observers_, sizeof(DictionaryObservers), alignof(DictionaryObservers));
    }
    for (auto *representation : representations_ | std::views::reverse) {
        if (representation == nullptr) {
            continue;
        }
        representation->destroy(*memory_);
    }
    for (auto *subscribers : subscriber_groups_) {
        DynamicSubscribers::release(subscribers);
    }
    if (item_subscribers_ != nullptr) {
        std::destroy_at(item_subscribers_);
        memory_->deallocate(item_subscribers_, sizeof(ReferenceItemSubscribers), alignof(ReferenceItemSubscribers));
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
    if (item_subscribers_ != nullptr) {
        item_subscribers_->notify(nullptr, context, change);
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
    if (item_subscribers_ != nullptr) {
        item_subscribers_->notify(&route, context, change);
    }
}

void ReferenceProvider::begin_item_notifications() noexcept {
    if (item_subscribers_ != nullptr) {
        item_subscribers_->begin();
    }
}

void ReferenceProvider::cancel_item_notifications() noexcept {
    if (item_subscribers_ != nullptr) {
        item_subscribers_->cancel();
    }
}

void ReferenceProvider::publish_item_notifications(OutputChangeContext &context) {
    if (item_subscribers_ != nullptr) {
        item_subscribers_->publish(context);
    }
}

void ReferenceProvider::add_item_subscriber(ReferenceRoute *route, RuntimeGraph &graph, void *target,
                                            TimeSeriesItemId item,
                                            ReferenceItemChangeFunction change_function,
                                            ReferenceItemPublishFunction publish_function) {
    if (item_subscribers_ == nullptr) {
        auto *storage = memory_->allocate(sizeof(ReferenceItemSubscribers), alignof(ReferenceItemSubscribers));
        auto release = util::scope_exit{[&]() noexcept {
            memory_->deallocate(storage, sizeof(ReferenceItemSubscribers), alignof(ReferenceItemSubscribers));
        }};
        item_subscribers_ = std::construct_at(static_cast<ReferenceItemSubscribers *>(storage), *memory_);
        release.release();
    }
    item_subscribers_->add(route, graph, target, item, change_function, publish_function);
}

void ReferenceProvider::remove_item_subscriber(ReferenceRoute *route, RuntimeGraph &graph, const void *target,
                                               TimeSeriesItemId item,
                                               ReferenceItemChangeFunction change_function) noexcept {
    if (item_subscribers_ != nullptr) {
        item_subscribers_->remove(route, graph, target, item, change_function);
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

    return (item_subscribers_ == nullptr || item_subscribers_->empty(route)) && release_subscribers(route);
}

DictionaryKeySubscription ReferenceProvider::add_dictionary_key_subscriber(ReferenceAddress dictionary,
                                                                           const ScalarData &key, RuntimeGraph &graph,
                                                                           std::uint32_t node_index) {
    if (dictionary_observers_ == nullptr) {
        auto *storage = memory_->allocate(sizeof(DictionaryObservers), alignof(DictionaryObservers));
        auto release = util::scope_exit{[&]() noexcept {
            memory_->deallocate(storage, sizeof(DictionaryObservers), alignof(DictionaryObservers));
        }};
        dictionary_observers_ =
            std::construct_at(static_cast<DictionaryObservers *>(storage), *this, *memory_);
        release.release();
    }
    return dictionary_observers_->add(dictionary, key, graph, node_index);
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

ReferenceProvider &require_provider(ReferenceAddress address) {
    return require_provider(reference_root(address));
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
    return require_provider(input).representation(input, requested_type);
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

void add_reference_item_subscriber(ReferenceAddress address, RuntimeGraph &graph, void *target,
                                   TimeSeriesItemId item, ReferenceItemChangeFunction change_function,
                                   ReferenceItemPublishFunction publish_function) {
    auto &provider = require_provider(address);
    provider.add_item_subscriber(address.get<ReferenceRoute>(), graph, target, item, change_function,
                                 publish_function);
}

void remove_reference_item_subscriber(ReferenceAddress address, RuntimeGraph &graph, const void *target,
                                      TimeSeriesItemId item,
                                      ReferenceItemChangeFunction change_function) noexcept {
    if (!address) {
        return;
    }
    auto *root = reference_root(address).data.get<ReferenceProvider>();
    if (root == nullptr) {
        return;
    }
    root->remove_item_subscriber(address.get<ReferenceRoute>(), graph, target, item, change_function);
}

DictionaryKeySubscription add_reference_dictionary_key_subscriber(ReferenceAddress address, const ScalarData &key,
                                                                  RuntimeGraph &graph, std::uint32_t node_index) {
    return require_provider(address).add_dictionary_key_subscriber(address, key, graph, node_index);
}

bool reference_dictionary_key_subscription_matches(DictionaryKeySubscription subscription, ReferenceAddress reference,
                                                   const ScalarData &key) {
    if (!subscription) {
        return false;
    }

    const auto &dictionary = *static_cast<const ObservedDictionary *>(subscription.dictionary_);
    if (dictionary.reference != reference) {
        return false;
    }

    const auto target = reference_target(reference);
    const auto *runtime_type = dynamic_cast<const TimeSeriesDictionaryRuntimeType *>(target.runtime_type);
    if (runtime_type == nullptr || target.value == nullptr) {
        return false;
    }
    const auto storage = runtime_type->retention_storage(const_cast<void *>(target.value));
    return storage.runtime_type == dictionary.runtime_type && storage.value == dictionary.value &&
           dictionary.runtime_type->key_runtime_type().equals(
               dictionary.runtime_type->key(dictionary.value, subscription.item_), key);
}

ReferenceAddress reference_dictionary_key_subscription_target(DictionaryKeySubscription subscription) {
    if (!subscription) {
        return {};
    }

    auto &dictionary = *static_cast<ObservedDictionary *>(subscription.dictionary_);
    if (!dictionary.runtime_type->key_live(dictionary.value, subscription.item_)) {
        return {};
    }

    auto *entry = dictionary.items.find(subscription.item_);
    assert(entry != nullptr);
    const auto item = dictionary.runtime_type->item(dictionary.value, subscription.item_);
    if (item.last_updated <= MIN_DT) {
        return {};
    }
    if (dynamic_cast<const TimeSeriesReferenceRuntimeType *>(&item.runtime_type) != nullptr) {
        return item.runtime_type.reference_address(item.value);
    }

    return ReferenceAddress{&entry->route};
}

void remove_reference_dictionary_key_subscriber(DictionaryKeySubscription subscription, RuntimeGraph &graph,
                                                std::uint32_t node_index) noexcept {
    if (subscription) {
        static_cast<const ObservedDictionary *>(subscription.dictionary_)
            ->owner->remove(subscription, graph, node_index);
    }
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
