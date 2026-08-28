#pragma once

#include <bg/scalar/view.hpp>

namespace bg {

class ScalarListMutableInputView : public ScalarListInputView {
  public:
    virtual void append(const ScalarValueInputView &value) const = 0;
    virtual void set(std::size_t index, const ScalarValueInputView &value) const = 0;
    virtual void erase(std::size_t index) const = 0;
    virtual void clear() const = 0;
};

template <typename T>
class TypedScalarListMutableInputView : public TypedScalarValueInputViewBase<List<T>, ScalarListMutableInputView> {
    using Stored = details::scalar_stored_t<List<T>>;

  public:
    [[nodiscard]] std::size_t size() const noexcept override { return storage().items.size(); }
    [[nodiscard]] ScalarListInputView::Item operator[](std::size_t index) const noexcept override;

    void append(const ScalarValueInputView &value) const override;
    void set(std::size_t index, const ScalarValueInputView &value) const override;
    void erase(std::size_t index) const override;
    void clear() const override;

  private:
    [[nodiscard]] Stored &storage() const noexcept { return *reinterpret_cast<Stored *>(this->value_); }
};

static_assert(sizeof(ScalarListMutableInputView) == sizeof(ScalarListInputView));

} // namespace bg
