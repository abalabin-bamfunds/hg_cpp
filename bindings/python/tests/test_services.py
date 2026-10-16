import gc
import weakref

import pytest

import bg


@bg.reference_service
def reference_value(key: int) -> bg.TS[int]:
    """A source-like service indexed by a scalar key."""


@bg.source_node
def reference_source() -> bg.TS[int]:
    return 42


@bg.graph
def reference_implementation(key: int) -> bg.TS[int]:
    return reference_source()


@bg.graph
def reference_client() -> bg.TS[int]:
    return reference_value(7)


def test_reference_service_uses_registered_graph_in_test_graph():
    catalogue = bg.ServiceCatalogue()
    catalogue.register_impl(reference_value, reference_implementation)
    profiler = bg.EvaluationProfiler()

    assert bg.test_graph(reference_client, catalogue=catalogue, recorder=profiler.recorder()) == [42]
    assert profiler.root_graph_id() is not None


def test_reference_service_selects_implementation_by_scalar_filter():
    @bg.source_node
    def alternate_source() -> bg.TS[int]:
        return 84

    @bg.graph
    def alternate_implementation(key: int) -> bg.TS[int]:
        return alternate_source()

    @bg.graph
    def alternate_client() -> bg.TS[int]:
        return reference_value(8)

    catalogue = bg.ServiceCatalogue()
    catalogue.register_impl(reference_value, reference_implementation, key=7)
    catalogue.register_impl(reference_value, alternate_implementation, key=8)

    assert bg.test_graph(reference_client, catalogue=catalogue) == [42]
    assert bg.test_graph(alternate_client, catalogue=catalogue) == [84]


def test_reference_service_uses_registered_graph_in_run_graph():
    values = []

    @bg.compute_node
    def collect(value: bg.TS[int]) -> None:
        values.append(value.value)

    @bg.graph
    def client() -> None:
        collect(reference_value(7))

    catalogue = bg.ServiceCatalogue()
    catalogue.register_impl(reference_value, reference_implementation)
    profiler = bg.EvaluationProfiler()

    bg.run_graph(client, catalogue=catalogue, recorder=profiler.recorder())

    assert values == [42]
    assert profiler.root_graph_id() is not None


def test_service_request_reply_with_python_tsd_implementation():
    @bg.service
    def double(value: bg.TS[int]) -> bg.TS[int]:
        """Return twice each request value."""

    @bg.compute_node
    def double_values(requests: bg.TSD[int, bg.TS[int]]) -> bg.TSD[int, bg.TS[int]]:
        return {key: requests[key].value * 2 for key in requests.valid_keys()}

    @bg.graph
    def implementation(value: bg.TSD[int, bg.TS[int]]) -> bg.TSD[int, bg.TS[int]]:
        return double_values(value)

    @bg.graph
    def client(value: bg.TS[int]) -> bg.TS[int]:
        return double(value)

    catalogue = bg.ServiceCatalogue()
    catalogue.register_impl(double, implementation)

    assert bg.test_graph(client, [2, 3, 5], catalogue=catalogue) == [4, 6, 10]


def test_request_reply_service_without_output_is_a_sink():
    seen = []

    @bg.service
    def observe(value: bg.TS[int]) -> None:
        pass

    @bg.compute_node
    def collect(requests: bg.TSD[int, bg.TS[int]]) -> None:
        for key in requests.valid_keys():
            seen.append(requests[key].value)

    @bg.graph
    def implementation(value: bg.TSD[int, bg.TS[int]]) -> None:
        collect(value)

    @bg.graph
    def client() -> None:
        observe(reference_source())

    catalogue = bg.ServiceCatalogue()
    catalogue.register_impl(observe, implementation)

    bg.run_graph(client, catalogue=catalogue)

    assert seen == [42]


def test_request_reply_service_supports_scalar_filter_and_installation_argument():
    @bg.service
    def transform(value: bg.TS[int], mode: int) -> bg.TS[int]:
        """A request/reply service selected by a scalar mode."""

    @bg.compute_node
    def transform_values(requests: bg.TSD[int, bg.TS[int]], factor: int) -> bg.TSD[int, bg.TS[int]]:
        return {key: requests[key].value * factor for key in requests.valid_keys()}

    @bg.graph
    def implementation(value: bg.TSD[int, bg.TS[int]], mode: int,
                        factor: int) -> bg.TSD[int, bg.TS[int]]:
        return transform_values(value, factor)

    @bg.graph
    def client(value: bg.TS[int]) -> bg.TS[int]:
        return transform(value, mode=2)

    catalogue = bg.ServiceCatalogue()
    catalogue.register_impl(transform, implementation, mode=2, factor=3)

    assert bg.test_graph(client, [2, 4], catalogue=catalogue) == [6, 12]


def test_subscription_service_accepts_final_and_const_keys():
    @bg.subscription_service
    def lookup(key: bg.TS[int, bg.final]) -> bg.TS[int]:
        """Look up a value for a key that cannot change."""

    @bg.compute_node
    def make_responses(requests: bg.TSS[int]) -> bg.TSD[int, bg.TS[int]]:
        return {key: key * 10 for key in requests.value}

    @bg.graph
    def implementation(requests: bg.TSS[int]) -> bg.TSD[int, bg.TS[int]]:
        return make_responses(requests)

    @bg.source_node
    def final_key() -> bg.TS[int, bg.final]:
        return 9

    @bg.graph
    def client() -> bg.TS[int]:
        constant_result = lookup(bg.operators.const_(7))
        final_result = lookup(final_key())
        return bg.operators.add(constant_result, final_result)

    catalogue = bg.ServiceCatalogue()
    catalogue.register_impl(lookup, implementation)

    assert bg.test_graph(client, catalogue=catalogue) == [160]


def test_subscription_service_rejects_ordinary_key_at_call_site():
    @bg.subscription_service
    def lookup(key: bg.TS[int, bg.final]) -> bg.TS[int]:
        """Look up a value for a key that cannot change."""

    @bg.compute_node
    def make_responses(requests: bg.TSS[int]) -> bg.TSD[int, bg.TS[int]]:
        return {key: key for key in requests.value}

    @bg.graph
    def implementation(requests: bg.TSS[int]) -> bg.TSD[int, bg.TS[int]]:
        return make_responses(requests)

    @bg.source_node
    def ordinary_key() -> bg.TS[int]:
        return 7

    @bg.graph
    def client() -> bg.TS[int]:
        return lookup(ordinary_key())

    catalogue = bg.ServiceCatalogue()
    catalogue.register_impl(lookup, implementation)

    with pytest.raises(RuntimeError, match="argument does not satisfy its generic type"):
        bg.test_graph(client, catalogue=catalogue)


def test_guarantee_annotations_apply_to_each_time_series_shape():
    @bg.schema
    class Schema:
        value: bg.TS[int, bg.final]

    @bg.graph
    def shaped_inputs(
        values: bg.TSS[int, bg.final],
        items: bg.TSL[bg.TS[int], 2, bg.final],
        mapping: bg.TSD[int, bg.TS[int], bg.final],
        bundle: bg.TSB[Schema, bg.final],
        reference: bg.REF[bg.TS[int], bg.final],
    ) -> None:
        pass

    assert isinstance(shaped_inputs, bg.GraphDefinition)

    @bg.source_node
    def make_bundle() -> bg.TSB[Schema]:
        return {"value": 13}

    @bg.compute_node
    def read_final_field(value: bg.TS[int, bg.final]) -> bg.TS[int]:
        return value.value

    @bg.graph
    def read_schema_field() -> bg.TS[int]:
        field = bg.operators.getattr(make_bundle(), bg.symbol("value"))
        return read_final_field(field)

    assert bg.test_graph(read_schema_field) == [13]


def test_service_without_registered_implementation_fails_during_wiring():
    catalogue = bg.ServiceCatalogue()
    with pytest.raises(RuntimeError, match="no matching implementation"):
        bg.test_graph(reference_client, catalogue=catalogue)


def test_service_catalogue_retains_declarations_and_graph_wrappers():
    @bg.reference_service
    def local_service() -> bg.TS[int]:
        """A locally declared reference service."""

    @bg.graph
    def local_implementation() -> bg.TS[int]:
        return reference_source()

    service_reference = weakref.ref(local_service)
    implementation_reference = weakref.ref(local_implementation)
    catalogue = bg.ServiceCatalogue()
    catalogue.register_impl(local_service, local_implementation)

    del local_service, local_implementation
    gc.collect()

    assert service_reference() is not None
    assert implementation_reference() is not None

    del catalogue
    gc.collect()

    assert service_reference() is None
    assert implementation_reference() is None


def test_service_declarations_require_annotations_and_explicit_ts_output():
    with pytest.raises(TypeError, match="requires a type annotation"):

        @bg.reference_service
        def missing_annotation(value):
            pass

    with pytest.raises(TypeError, match="explicit TS"):

        @bg.reference_service
        def scalar_output() -> int:
            pass

    with pytest.raises(ValueError, match="source-like"):

        @bg.reference_service
        def missing_reference_output() -> None:
            pass
