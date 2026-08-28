#pragma once

#include <bg/scalar/view.hpp>

namespace bg {

class ScalarDictMutableInputView : public ScalarDictInputView {
  public:
    virtual void insert_or_assign(const ScalarValueInputView &key, const ScalarValueInputView &value) const = 0;
    [[nodiscard]] virtual bool erase(const ScalarValueInputView &key) const = 0;
    virtual void clear() const = 0;
};

template <typename K, typename V>
class TypedScalarDictMutableInputView : public TypedScalarValueInputViewBase<Dict<K, V>, ScalarDictMutableInputView> {
    using Stored = details::scalar_stored_t<Dict<K, V>>;

  public:
    [[nodiscard]] std::size_t size() const noexcept override { return storage().items.size(); }
    [[nodiscard]] bool contains(const ScalarValueInputView &key) const override;
    [[nodiscard]] ScalarDictInputView::Item at(const ScalarValueInputView &key) const override;
    void for_each(void *context, ScalarDictInputView::EntryFunction function) const override;
    void insert_or_assign(const ScalarValueInputView &key, const ScalarValueInputView &value) const override;
    [[nodiscard]] bool erase(const ScalarValueInputView &key) const override;
    void clear() const override;

  private:
    [[nodiscard]] Stored &storage() const noexcept { return *reinterpret_cast<Stored *>(this->value_); }
};

static_assert(sizeof(ScalarDictMutableInputView) == sizeof(ScalarDictInputView));

} // namespace bg
