import hg


def test_whole_graph_wiring():
    @hg.source_node
    def price() -> hg.TS[int]:
        return 10

    @hg.compute_node
    def add_amount(value: hg.TS[int], amount: int = 3) -> hg.TS[int]:
        return value.value + amount

    @hg.compute_node
    def double(value: hg.TS[int]) -> hg.TS[int]:
        return value.value * 2

    @hg.compute_node
    def add(left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
        return left.value + right.value

    @hg.graph
    def adjusted(value: hg.TS[int], amount: int = 3) -> hg.TS[int]:
        return add_amount(value, amount=amount)

    @hg.graph
    def total_price() -> hg.TS[int]:
        price_value = price()
        adjusted_price = adjusted(price_value)
        doubled_price = double(adjusted_price)
        other_value = add_amount(price_value, amount=5)
        return add(doubled_price, other_value)

    assert hg.run_graph(total_price) == [41]
