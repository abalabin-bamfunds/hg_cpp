#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/time_series/details/output_path.hpp>
#include <bg/util/polymorphic_value.hpp>

#include <utility>

namespace bg::details {

template <typename View> class output_view_proxy {
  public:
    [[nodiscard]] static output_view_proxy copy(const View &prototype, EvaluationContext &context) {
        auto *storage = context.scratch().allocate(sizeof(View), alignof(View));
        return output_view_proxy{static_cast<View &>(util::polymorphic_value<View>::copy_to(storage, prototype))};
    }

    [[nodiscard]] static output_view_proxy copy(const View &prototype, void *output_path) {
        auto &context = output_path_context(output_path);
        return copy(prototype, context);
    }

    explicit output_view_proxy(View &view) noexcept : view_{&view} {}

    output_view_proxy(const output_view_proxy &) = delete;
    output_view_proxy &operator=(const output_view_proxy &) = delete;

    output_view_proxy(output_view_proxy &&other) noexcept : view_{std::exchange(other.view_, nullptr)} {}
    output_view_proxy &operator=(output_view_proxy &&) = delete;

    ~output_view_proxy() {
        if (view_ != nullptr) {
            view_->release();
        }
    }

    [[nodiscard]] View &get() noexcept { return *view_; }
    [[nodiscard]] const View &get() const noexcept { return *view_; }
    [[nodiscard]] View *operator->() noexcept { return view_; }
    [[nodiscard]] const View *operator->() const noexcept { return view_; }

  private:
    View *view_;
};

} // namespace bg::details
