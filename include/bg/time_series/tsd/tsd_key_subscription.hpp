#pragma once

#include <bg/time_series/item_id.hpp>
#include <bg/time_series/ref/ref_provider.hpp>

#include <cstdint>

namespace bg::details {

class ScalarData;
class TsdKeySubscriptions;

class TsdKeySubscription {
  public:
    TsdKeySubscription() = default;

    explicit operator bool() const noexcept { return subscribed_tsd_ != nullptr; }

  private:
    friend class TsdKeySubscriptions;
    friend bool tsd_key_subscription_matches(TsdKeySubscription, Reference, const ScalarData &);
    friend Reference tsd_key_subscription_reference(TsdKeySubscription);
    friend void remove_tsd_key_subscriber(TsdKeySubscription, RuntimeGraph &, std::uint32_t) noexcept;

    TsdKeySubscription(void *subscribed_tsd, item_id_t item_id) noexcept
        : subscribed_tsd_{subscribed_tsd}, item_id_{item_id} {}

    void *subscribed_tsd_{};
    item_id_t item_id_{item_id_t(-1)};
};

static_assert(sizeof(TsdKeySubscription) == 2 * sizeof(void *));

[[nodiscard]] TsdKeySubscription add_tsd_key_subscriber(Reference reference, const ScalarData &key, RuntimeGraph &graph,
                                                        std::uint32_t node_index);
bool tsd_key_subscription_matches(TsdKeySubscription subscription, Reference tsd, const ScalarData &key);
Reference tsd_key_subscription_reference(TsdKeySubscription subscription);
void remove_tsd_key_subscriber(TsdKeySubscription subscription, RuntimeGraph &graph, std::uint32_t node_index) noexcept;

} // namespace bg::details
