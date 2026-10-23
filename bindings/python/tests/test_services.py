import gc
import weakref
from typing import TypeVar

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


def test_reference_service_captures_registered_graph_output_in_run_graph():
    catalogue = bg.ServiceCatalogue()
    catalogue.register_impl(reference_value, reference_implementation)
    profiler = bg.EvaluationProfiler()

    assert bg.run_graph(reference_client, catalogue=catalogue, recorder=profiler.recorder()) == [42]
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

    assert bg.run_graph(reference_client, catalogue=catalogue) == [42]
    assert bg.run_graph(alternate_client, catalogue=catalogue) == [84]


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

    assert bg.run_graph(client, [2, 3, 5], catalogue=catalogue) == [4, 6, 10]


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

    assert bg.run_graph(client, [2, 4], catalogue=catalogue) == [6, 12]


def test_feedback_service_implementation_delays_request_reply():
    @bg.service
    def echo(value: bg.TS[int]) -> bg.TS[int]:
        """Return each request value."""

    @bg.compute_node
    def respond(requests: bg.TSD[int, bg.TS[int]]) -> bg.TSD[int, bg.TS[int]]:
        return {key: requests[key].value for key in requests.valid_keys()}

    @bg.graph
    def implementation(value: bg.TSD[int, bg.TS[int]]) -> bg.TSD[int, bg.TS[int]]:
        return respond(value)

    feedback = bg.feedback_service_implementation(echo, implementation)

    @bg.graph
    def client(value: bg.TS[int]) -> bg.TS[int]:
        return echo(value)

    catalogue = bg.ServiceCatalogue()
    catalogue.register_impl(echo, feedback)
    assert bg.run_graph(client, [2, 4], catalogue=catalogue) == [None, 2, 4]


def test_feedback_service_implementation_preserves_filter_installation_and_generic_types():
    VALUE = TypeVar("VALUE")

    @bg.service
    def transform(value: bg.TS[VALUE], mode: int) -> bg.TS[VALUE]:
        """Transform each request using an installation factor."""

    @bg.compute_node
    def transform_values(requests: bg.TSD[int, bg.TS[VALUE]], factor: int) -> bg.TSD[int, bg.TS[VALUE]]:
        return {key: requests[key].value * factor for key in requests.valid_keys()}

    @bg.graph
    def implementation(value: bg.TSD[int, bg.TS[VALUE]], mode: int,
                       factor: int) -> bg.TSD[int, bg.TS[VALUE]]:
        return transform_values(value, factor)

    feedback = bg.feedback_service_implementation(transform, implementation)

    @bg.graph
    def client(value: bg.TS[int]) -> bg.TS[int]:
        return transform(value, mode=2)

    catalogue = bg.ServiceCatalogue()
    catalogue.register_impl(transform, feedback, mode=2, factor=3)
    assert bg.run_graph(client, [2, 4], catalogue=catalogue) == [None, 6, 12]


def test_feedback_subscription_implementation_delays_keys_and_keeps_installation_argument():
    @bg.subscription_service
    def lookup(key: bg.TS[int, bg.final]) -> bg.TS[int]:
        """Return a value for every requested key."""

    @bg.compute_node
    def answer(keys: bg.TSS[int], offset: int) -> bg.TSD[int, bg.TS[int]]:
        return {key: key * 10 + offset for key in keys.value}

    @bg.graph
    def implementation(requests: bg.TSS[int], offset: int) -> bg.TSD[int, bg.TS[int]]:
        return answer(requests, offset)

    @bg.source_node
    def final_key() -> bg.TS[int, bg.final]:
        return 3

    @bg.graph
    def client(tick: bg.TS[int]) -> bg.TS[int]:
        _ = tick
        return lookup(final_key())

    catalogue = bg.ServiceCatalogue()
    catalogue.register_impl(lookup, bg.feedback_service_implementation(lookup, implementation), offset=5)
    assert bg.run_graph(client, [0], catalogue=catalogue) == [None, 35]


def test_feedback_service_implementation_retains_service_and_graph():
    @bg.service
    def local_service(value: bg.TS[int]) -> bg.TS[int]:
        pass

    @bg.graph
    def local_implementation(value: bg.TSD[int, bg.TS[int]]) -> bg.TSD[int, bg.TS[int]]:
        return value

    service_reference = weakref.ref(local_service)
    graph_reference = weakref.ref(local_implementation)
    feedback = bg.feedback_service_implementation(local_service, local_implementation)

    del local_service, local_implementation
    gc.collect()

    assert service_reference() is not None
    assert graph_reference() is not None

    del feedback
    gc.collect()

    assert service_reference() is None
    assert graph_reference() is None


def test_feedback_service_implementation_rejects_unsupported_signatures():
    @bg.reference_service
    def source() -> bg.TS[int]:
        pass

    @bg.service
    def sink(value: bg.TS[int]) -> None:
        pass

    @bg.service
    def echo_service(value: bg.TS[int]) -> bg.TS[int]:
        pass

    @bg.graph
    def outputless_implementation(value: bg.TSD[int, bg.TS[int]]) -> None:
        pass

    @bg.graph
    def implementation(value: bg.TSD[int, bg.TS[int]]) -> bg.TSD[int, bg.TS[int]]:
        return value

    with pytest.raises(ValueError, match="requires a service with requests and an output"):
        bg.feedback_service_implementation(source, implementation)

    with pytest.raises(ValueError, match="requires a service with requests and an output"):
        bg.feedback_service_implementation(sink, implementation)

    with pytest.raises(ValueError, match="implementation must have an output"):
        bg.feedback_service_implementation(echo_service, outputless_implementation)

    catalogue = bg.ServiceCatalogue()
    feedback = bg.feedback_service_implementation(echo_service, implementation)
    with pytest.raises(ValueError, match="different service"):
        catalogue.register_impl(sink, feedback)


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

    assert bg.run_graph(client, catalogue=catalogue) == [160]


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
        bg.run_graph(client, catalogue=catalogue)


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

    assert bg.run_graph(read_schema_field) == [13]


def test_service_without_registered_implementation_fails_during_wiring():
    catalogue = bg.ServiceCatalogue()
    with pytest.raises(RuntimeError, match="no matching implementation"):
        bg.run_graph(reference_client, catalogue=catalogue)


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


def test_multiservice_graph_implements_two_reference_services():
    @bg.reference_service
    def selected_reference(key: int) -> bg.TS[int]:
        pass

    @bg.reference_service
    def plain_reference() -> bg.TS[int]:
        pass

    @bg.source_node
    def selected_source() -> bg.TS[int]:
        return 10

    @bg.source_node
    def plain_source() -> bg.TS[int]:
        return 20

    @bg.graph
    def implementation() -> None:
        selected_reference.implementation_replies(selected_source(), 7)
        plain_reference.implementation_replies(plain_source())

    @bg.graph
    def client() -> bg.TS[int]:
        return bg.operators.add(selected_reference(7), plain_reference())

    catalogue = bg.ServiceCatalogue()
    registration = catalogue.register_multiservice(implementation)
    registration.register_impl(selected_reference, key=7)
    registration.register_impl(plain_reference)

    assert bg.run_graph(client, catalogue=catalogue) == [30]


def test_multiservice_graph_combines_subscription_and_reference_services():
    @bg.reference_service
    def clock() -> bg.TS[int]:
        pass

    @bg.subscription_service
    def lookup(key: bg.TS[int, bg.final]) -> bg.TS[int]:
        pass

    @bg.source_node
    def clock_source() -> bg.TS[int]:
        return 5

    @bg.compute_node
    def make_responses(keys: bg.TSS[int]) -> bg.TSD[int, bg.TS[int]]:
        return {key: key * 10 for key in keys.value}

    @bg.source_node
    def final_key() -> bg.TS[int, bg.final]:
        return 3

    @bg.graph
    def implementation() -> None:
        lookup.implementation_replies(make_responses(lookup.implementation_requests()))
        clock.implementation_replies(clock_source())

    @bg.graph
    def client() -> bg.TS[int]:
        return bg.operators.add(clock(), lookup(final_key()))

    catalogue = bg.ServiceCatalogue()
    registration = catalogue.register_multiservice(implementation)
    registration.register_impl(clock)
    registration.register_impl(lookup)

    assert bg.run_graph(client, catalogue=catalogue) == [35]


def test_multiservice_request_reply_uses_scalar_filter_and_installation_argument():
    @bg.service
    def transform(value: bg.TS[int], mode: int) -> bg.TS[int]:
        pass

    @bg.compute_node
    def transform_values(values: bg.TSD[int, bg.TS[int]], factor: int) -> bg.TSD[int, bg.TS[int]]:
        return {key: values[key].value * factor for key in values.valid_keys()}

    @bg.graph
    def implementation(factor: int) -> None:
        requests = transform.implementation_requests(2)
        values = bg.operators.getattr(requests, bg.symbol("value"))
        transform.implementation_replies(transform_values(values, factor), 2)

    @bg.graph
    def client(value: bg.TS[int]) -> bg.TS[int]:
        return transform(value, mode=2)

    catalogue = bg.ServiceCatalogue()
    registration = catalogue.register_multiservice(implementation, factor=3)
    registration.register_impl(transform, mode=2)

    assert bg.run_graph(client, [2, 4], catalogue=catalogue) == [6, 12]


def test_split_request_reply_client_binds_scalar_instance_arguments():
    @bg.service
    def echo(value: bg.TS[int], mode: str = "fast") -> bg.TS[int]:
        pass

    @bg.graph
    def implementation() -> None:
        requests = echo.implementation_requests("fast")
        echo.implementation_replies(bg.operators.getattr(requests, bg.symbol("value")), "fast")

    @bg.graph
    def client(value: bg.TS[int]) -> bg.TS[int]:
        client_id = bg.service_client_id()
        replies = echo.client_replies(client_id)
        echo.client_requests(client_id, value, mode="fast")
        return replies

    catalogue = bg.ServiceCatalogue()
    registration = catalogue.register_multiservice(implementation)
    registration.register_impl(echo, mode="fast")

    assert bg.test_graph(client, [3, 7], catalogue=catalogue) == [3, 7]


def test_multiservice_outputless_service_uses_request_port_without_reply():
    @bg.service
    def observe(value: bg.TS[int]) -> None:
        pass

    seen = []

    @bg.compute_node
    def collect(values: bg.TSD[int, bg.TS[int]]) -> None:
        for key in values.valid_keys():
            seen.append(values[key].value)

    @bg.graph
    def implementation() -> None:
        requests = observe.implementation_requests()
        collect(bg.operators.getattr(requests, bg.symbol("value")))

    @bg.graph
    def client(value: bg.TS[int]) -> None:
        observe(value)

    catalogue = bg.ServiceCatalogue()
    registration = catalogue.register_multiservice(implementation)
    registration.register_impl(observe)

    bg.run_graph(client, [4, 6], catalogue=catalogue)

    assert seen == [4, 6]


def test_multiservice_registration_rejects_unknown_and_duplicate_claims():
    @bg.reference_service
    def selected(key: int) -> bg.TS[int]:
        pass

    @bg.graph
    def implementation() -> None:
        pass

    catalogue = bg.ServiceCatalogue()
    registration = catalogue.register_multiservice(implementation)

    with pytest.raises(TypeError):
        registration.register_impl(selected, 1)

    with pytest.raises(ValueError, match="must name a service scalar parameter"):
        registration.register_impl(selected, missing=1)

    with pytest.raises(ValueError, match="incompatible type"):
        registration.register_impl(selected, key="seven")

    registration.register_impl(selected, key=1)
    with pytest.raises(ValueError, match="repeats a service claim"):
        registration.register_impl(selected, key=1)

    @bg.reference_service
    def rejected_service() -> bg.TS[int]:
        pass

    rejected_reference = weakref.ref(rejected_service)
    with pytest.raises(ValueError, match="must name a service scalar parameter"):
        registration.register_impl(rejected_service, missing=1)

    del rejected_service
    gc.collect()
    assert rejected_reference() is None


def test_multiservice_graph_rejects_an_unregistered_request_port():
    @bg.reference_service
    def registered() -> bg.TS[int]:
        pass

    @bg.reference_service
    def unregistered() -> bg.TS[int]:
        pass

    @bg.source_node
    def source() -> bg.TS[int]:
        return 1

    @bg.graph
    def implementation() -> None:
        registered.implementation_replies(source())
        unregistered.implementation_replies(source())

    @bg.graph
    def client() -> bg.TS[int]:
        return registered()

    catalogue = bg.ServiceCatalogue()
    registration = catalogue.register_multiservice(implementation)
    registration.register_impl(registered)

    with pytest.raises(RuntimeError, match="unregistered service port"):
        bg.run_graph(client, catalogue=catalogue)


def test_multiservice_catalogue_retains_claim_and_graph_wrappers():
    @bg.reference_service
    def local_service() -> bg.TS[int]:
        pass

    @bg.source_node
    def local_source() -> bg.TS[int]:
        return 1

    @bg.graph
    def local_implementation() -> None:
        local_service.implementation_replies(local_source())

    service_reference = weakref.ref(local_service)
    implementation_reference = weakref.ref(local_implementation)
    catalogue = bg.ServiceCatalogue()
    registration = catalogue.register_multiservice(local_implementation)
    registration.register_impl(local_service)

    del local_service, local_implementation
    gc.collect()

    assert service_reference() is not None
    assert implementation_reference() is not None

    del catalogue
    gc.collect()

    assert service_reference() is not None
    assert implementation_reference() is not None

    del registration
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
