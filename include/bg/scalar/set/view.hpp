#pragma once

#include <bg/scalar/view.hpp>

namespace bg {

class ScalarSetInputView : public ScalarValueInputView {
  public:
    VISITOR_SUPPORT(final)

    struct Item {
        const ScalarValueInputView &prototype;
        const void *data;
    };

    using ItemFunction = void (*)(void *, Item);

    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual bool contains(const ScalarValueInputView &value) const = 0;
    virtual void for_each(void *context, ItemFunction function) const = 0;
};

template <typename T>
class TypedScalarValueInputView<Set<T>> : public TypedScalarValueInputViewBase<Set<T>, ScalarSetInputView> {
    using Stored = details::scalar_stored_t<Set<T>>;

  public:
    [[nodiscard]] std::size_t size() const noexcept override { return storage().items.size(); }
    [[nodiscard]] bool contains(const ScalarValueInputView &value) const override;
    void for_each(void *context, ScalarSetInputView::ItemFunction function) const override;

  private:
    [[nodiscard]] const Stored &storage() const noexcept { return *reinterpret_cast<const Stored *>(this->value_); }
};

static_assert(sizeof(ScalarSetInputView) == sizeof(ScalarInputView));

} // namespace bg
