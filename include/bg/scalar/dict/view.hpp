#pragma once

#include <bg/scalar/view.hpp>

namespace bg {

class ScalarDictInputView : public ScalarValueInputView {
  public:
    VISITOR_SUPPORT(final)

    struct Item {
        const ScalarValueInputView &prototype;
        const void *data;
    };

    struct Entry {
        Item key;
        Item value;
    };

    using EntryFunction = void (*)(void *, Entry);

    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual bool contains(const ScalarValueInputView &key) const = 0;
    [[nodiscard]] virtual Item at(const ScalarValueInputView &key) const = 0;
    virtual void for_each(void *context, EntryFunction function) const = 0;
};

template <typename K, typename V>
class TypedScalarValueInputView<Dict<K, V>> : public TypedScalarValueInputViewBase<Dict<K, V>, ScalarDictInputView> {
    using Stored = details::scalar_stored_t<Dict<K, V>>;

  public:
    [[nodiscard]] std::size_t size() const noexcept override { return storage().items.size(); }
    [[nodiscard]] bool contains(const ScalarValueInputView &key) const override;
    [[nodiscard]] ScalarDictInputView::Item at(const ScalarValueInputView &key) const override;
    void for_each(void *context, ScalarDictInputView::EntryFunction function) const override;

  private:
    [[nodiscard]] const Stored &storage() const noexcept { return *reinterpret_cast<const Stored *>(this->value_); }
};

static_assert(sizeof(ScalarDictInputView) == sizeof(ScalarInputView));

} // namespace bg
