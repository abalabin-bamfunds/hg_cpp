import bg


def test_whole_graph_wiring():
    @bg.source_node
    def price() -> bg.TS[int]:
        return 10

    @bg.compute_node
    def add_amount(value: bg.TS[int], amount: int = 3) -> bg.TS[int]:
        return value.value + amount

    @bg.compute_node
    def double(value: bg.TS[int]) -> bg.TS[int]:
        return value.value * 2

    @bg.compute_node
    def add(left: bg.TS[int], right: bg.TS[int]) -> bg.TS[int]:
        return left.value + right.value

    @bg.graph
    def adjusted(value: bg.TS[int], amount: int = 3) -> bg.TS[int]:
        return add_amount(value, amount=amount)

    @bg.graph
    def total_price() -> bg.TS[int]:
        price_value = price()
        adjusted_price = adjusted(price_value)
        doubled_price = double(adjusted_price)
        other_value = add_amount(price_value, amount=5)
        return add(doubled_price, other_value)

    assert bg.run_graph(total_price) == [41]
