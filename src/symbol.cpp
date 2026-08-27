#include <bg/util/symbol.hpp>

#include <memory>
#include <mutex>
#include <unordered_set>

namespace bg::util {
namespace {

class symbol_table {
  public:
    [[nodiscard]] const std::string *intern(std::string_view text) {
        const std::lock_guard lock(mutex_);
        const auto entry = values_.emplace(text).first;

        return std::addressof(*entry);
    }

  private:
    std::mutex mutex_;
    std::unordered_set<std::string> values_;
};

symbol_table &symbols() {
    static symbol_table value;

    return value;
}

} // namespace

symbol::symbol(std::string_view text) : text_(intern(text)) {}

const std::string *symbol::intern(std::string_view text) {
    return symbols().intern(text);
}

} // namespace bg::util
