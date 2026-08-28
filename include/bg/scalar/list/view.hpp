#pragma once

#include <bg/scalar/view.hpp>

namespace bg {

class ScalarListInputView : public ScalarValueInputView {
  public:
    VISITOR_SUPPORT(final)

    struct Item {
        const ScalarValueInputView &prototype;
        const void *data;
    };

    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual Item operator[](std::size_t index) const noexcept = 0;
};

template <typename T>
class TypedScalarValueInputView<List<T>> : public TypedScalarValueInputViewBase<List<T>, ScalarListInputView> {
    using Stored = details::scalar_stored_t<List<T>>;

  public:
    [[nodiscard]] std::size_t size() const noexcept override { return storage().items.size(); }
    [[nodiscard]] ScalarListInputView::Item operator[](std::size_t index) const noexcept override;

  private:
    [[nodiscard]] const Stored &storage() const noexcept { return *reinterpret_cast<const Stored *>(this->value_); }
};

static_assert(sizeof(ScalarListInputView) == sizeof(ScalarInputView));

} // namespace bg
