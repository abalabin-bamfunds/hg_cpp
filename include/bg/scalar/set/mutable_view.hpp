#pragma once

#include <bg/scalar/view.hpp>

namespace bg {

class ScalarSetMutableInputView : public ScalarSetInputView {
  public:
    [[nodiscard]] virtual bool insert(const ScalarValueInputView &value) const = 0;
    [[nodiscard]] virtual bool erase(const ScalarValueInputView &value) const = 0;
    virtual void clear() const = 0;
};

template <typename T>
class TypedScalarSetMutableInputView : public TypedScalarValueInputViewBase<Set<T>, ScalarSetMutableInputView> {
    using Stored = details::scalar_stored_t<Set<T>>;

  public:
    [[nodiscard]] std::size_t size() const noexcept override { return storage().items.size(); }
    [[nodiscard]] bool contains(const ScalarValueInputView &value) const override;
    void for_each(void *context, ScalarSetInputView::ItemFunction function) const override;
    [[nodiscard]] bool insert(const ScalarValueInputView &value) const override;
    [[nodiscard]] bool erase(const ScalarValueInputView &value) const override;
    void clear() const override;

  private:
    [[nodiscard]] Stored &storage() const noexcept { return *reinterpret_cast<Stored *>(this->value_); }
};

static_assert(sizeof(ScalarSetMutableInputView) == sizeof(ScalarSetInputView));

} // namespace bg
