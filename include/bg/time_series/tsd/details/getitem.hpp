#pragma once

namespace bg {

class NodeTemplate;
class TimeSeriesDictionaryType;

namespace details {

[[nodiscard]] const NodeTemplate &dictionary_item_node_template(const TimeSeriesDictionaryType &source,
                                                                bool time_series_key);

} // namespace details
} // namespace bg
