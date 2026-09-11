#pragma once

#include <bg/scalar/scalar.hpp>
#include <bg/time_series/item_id.hpp>
#include <bg/time_series/ref/ref_provider.hpp>

#include <cstdint>

namespace bg::details {

class TsdKeySubscriptions;

class TsdKeySubscription {
  public:
    TsdKeySubscription() = default;

    explicit operator bool() const noexcept { return subscribed_tsd_ != nullptr; }

  private:
    friend class TsdKeySubscriptions;
    friend Reference tsd_key_subscription_value(TsdKeySubscription);
    friend bool tsd_key_subscription_is_final_and_valid(TsdKeySubscription);
    friend void remove_tsd_key_subscriber(TsdKeySubscription, RuntimeGraph &, std::uint32_t) noexcept;

    TsdKeySubscription(void *subscribed_tsd, item_id_t item_id) noexcept
        : subscribed_tsd_{subscribed_tsd}, item_id_{item_id} {}

    void *subscribed_tsd_{};
    item_id_t item_id_{item_id_t(-1)};
};

static_assert(sizeof(TsdKeySubscription) == 2 * sizeof(void *));

[[nodiscard]] TsdKeySubscription add_tsd_key_subscriber(Reference reference, Scalar<> key, RuntimeGraph &graph,
                                                        std::uint32_t node_index);
Reference tsd_key_subscription_value(TsdKeySubscription subscription);
bool tsd_key_subscription_is_final_and_valid(TsdKeySubscription subscription);
void remove_tsd_key_subscriber(TsdKeySubscription subscription, RuntimeGraph &graph, std::uint32_t node_index) noexcept;

} // namespace bg::details
