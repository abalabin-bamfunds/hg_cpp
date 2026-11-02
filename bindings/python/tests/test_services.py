import gc
import weakref
from typing import TypeVar

import pytest

import hg


@hg.reference_service
def reference_value(key: int) -> hg.TS[int]:
    """A source-like service indexed by a scalar key."""


@hg.source_node
def reference_source() -> hg.TS[int]:
    return 42


@hg.graph
def reference_implementation(key: int) -> hg.TS[int]:
    return reference_source()


@hg.graph
def reference_client() -> hg.TS[int]:
    return reference_value(7)


def test_reference_service_captures_registered_graph_output_in_run_graph():
    catalogue = hg.ServiceCatalogue()
    catalogue.register_impl(reference_value, reference_implementation)
    profiler = hg.EvaluationProfiler()

    assert hg.run_graph(reference_client, catalogue=catalogue, recorder=profiler.recorder()) == [42]
    assert profiler.root_graph_id() is not None


def test_native_service_export_checks_signature_family_and_identity():
    from hg._hg_test_extension import export_alternate_native_service, export_native_service

    @hg.service
    def declaration(value: hg.TS[int]) -> hg.TS[int]:
        pass

    export_native_service(declaration)
    export_native_service(declaration)
    with pytest.raises(ValueError, match="different native service"):
        export_alternate_native_service(declaration)

    @hg.service
    def wrong_count(value: hg.TS[int], extra: hg.TS[int]) -> hg.TS[int]:
        pass

    @hg.service
    def wrong_name(input: hg.TS[int]) -> hg.TS[int]:
        pass

    @hg.service
    def wrong_type(value: hg.TS[str]) -> hg.TS[int]:
        pass

    @hg.service
    def wrong_output(value: hg.TS[int]) -> hg.TS[str]:
        pass

    @hg.reference_service
    def wrong_family(key: int) -> hg.TS[int]:
        pass

    for service in (wrong_count, wrong_name, wrong_type, wrong_output):
        with pytest.raises(ValueError, match="service signature"):
            export_native_service(service)
    with pytest.raises(ValueError, match="service family"):
        export_native_service(wrong_family)


def test_reference_service_selects_implementation_by_scalar_filter():
    @hg.source_node
    def alternate_source() -> hg.TS[int]:
        return 84

    @hg.graph
    def alternate_implementation(key: int) -> hg.TS[int]:
        return alternate_source()

    @hg.graph
    def alternate_client() -> hg.TS[int]:
        return reference_value(8)

    catalogue = hg.ServiceCatalogue()
    catalogue.register_impl(reference_value, reference_implementation, key=7)
    catalogue.register_impl(reference_value, alternate_implementation, key=8)

    assert hg.run_graph(reference_client, catalogue=catalogue) == [42]
    assert hg.run_graph(alternate_client, catalogue=catalogue) == [84]


def test_reference_service_uses_registered_graph_in_run_graph():
    values = []

    @hg.compute_node
    def collect(value: hg.TS[int]) -> None:
        values.append(value.value)

    @hg.graph
    def client() -> None:
        collect(reference_value(7))

    catalogue = hg.ServiceCatalogue()
    catalogue.register_impl(reference_value, reference_implementation)
    profiler = hg.EvaluationProfiler()

    hg.run_graph(client, catalogue=catalogue, recorder=profiler.recorder())

    assert values == [42]
    assert profiler.root_graph_id() is not None


def test_service_request_reply_with_python_tsd_implementation():
    @hg.service
    def double(value: hg.TS[int]) -> hg.TS[int]:
        """Return twice each request value."""

    @hg.compute_node
    def double_values(requests: hg.TSD[int, hg.TS[int]]) -> hg.TSD[int, hg.TS[int]]:
        return {key: requests[key].value * 2 for key in requests.valid_keys()}

    @hg.graph
    def implementation(value: hg.TSD[int, hg.TS[int]]) -> hg.TSD[int, hg.TS[int]]:
        return double_values(value)

    @hg.graph
    def client(value: hg.TS[int]) -> hg.TS[int]:
        return double(value)

    catalogue = hg.ServiceCatalogue()
    catalogue.register_impl(double, implementation)

    assert hg.run_graph(client, [2, 3, 5], catalogue=catalogue) == [4, 6, 10]


def test_request_reply_service_without_output_is_a_sink():
    seen = []

    @hg.service
    def observe(value: hg.TS[int]) -> None:
        pass

    @hg.compute_node
    def collect(requests: hg.TSD[int, hg.TS[int]]) -> None:
        for key in requests.valid_keys():
            seen.append(requests[key].value)

    @hg.graph
    def implementation(value: hg.TSD[int, hg.TS[int]]) -> None:
        collect(value)

    @hg.graph
    def client() -> None:
        observe(reference_source())

    catalogue = hg.ServiceCatalogue()
    catalogue.register_impl(observe, implementation)

    hg.run_graph(client, catalogue=catalogue)

    assert seen == [42]


def test_request_reply_service_supports_scalar_filter_and_installation_argument():
    @hg.service
    def transform(value: hg.TS[int], mode: int) -> hg.TS[int]:
        """A request/reply service selected by a scalar mode."""

    @hg.compute_node
    def transform_values(requests: hg.TSD[int, hg.TS[int]], factor: int) -> hg.TSD[int, hg.TS[int]]:
        return {key: requests[key].value * factor for key in requests.valid_keys()}

    @hg.graph
    def implementation(value: hg.TSD[int, hg.TS[int]], mode: int,
                        factor: int) -> hg.TSD[int, hg.TS[int]]:
        return transform_values(value, factor)

    @hg.graph
    def client(value: hg.TS[int]) -> hg.TS[int]:
        return transform(value, mode=2)

    catalogue = hg.ServiceCatalogue()
    catalogue.register_impl(transform, implementation, mode=2, factor=3)

    assert hg.run_graph(client, [2, 4], catalogue=catalogue) == [6, 12]


def test_feedback_service_implementation_delays_request_reply():
    @hg.service
    def echo(value: hg.TS[int]) -> hg.TS[int]:
        """Return each request value."""

    @hg.compute_node
    def respond(requests: hg.TSD[int, hg.TS[int]]) -> hg.TSD[int, hg.TS[int]]:
        return {key: requests[key].value for key in requests.valid_keys()}

    @hg.graph
    def implementation(value: hg.TSD[int, hg.TS[int]]) -> hg.TSD[int, hg.TS[int]]:
        return respond(value)

    feedback = hg.feedback_service_implementation(echo, implementation)

    @hg.graph
    def client(value: hg.TS[int]) -> hg.TS[int]:
        return echo(value)

    catalogue = hg.ServiceCatalogue()
    catalogue.register_impl(echo, feedback)
    assert hg.run_graph(client, [2, 4], catalogue=catalogue) == [None, 2, 4]


def test_feedback_service_implementation_preserves_filter_installation_and_generic_types():
    VALUE = TypeVar("VALUE")

    @hg.service
    def transform(value: hg.TS[VALUE], mode: int) -> hg.TS[VALUE]:
        """Transform each request using an installation factor."""

    @hg.compute_node
    def transform_values(requests: hg.TSD[int, hg.TS[VALUE]], factor: int) -> hg.TSD[int, hg.TS[VALUE]]:
        return {key: requests[key].value * factor for key in requests.valid_keys()}

    @hg.graph
    def implementation(value: hg.TSD[int, hg.TS[VALUE]], mode: int,
                       factor: int) -> hg.TSD[int, hg.TS[VALUE]]:
        return transform_values(value, factor)

    feedback = hg.feedback_service_implementation(transform, implementation)

    @hg.graph
    def client(value: hg.TS[int]) -> hg.TS[int]:
        return transform(value, mode=2)

    catalogue = hg.ServiceCatalogue()
    catalogue.register_impl(transform, feedback, mode=2, factor=3)
    assert hg.run_graph(client, [2, 4], catalogue=catalogue) == [None, 6, 12]


def test_feedback_subscription_implementation_delays_keys_and_keeps_installation_argument():
    @hg.subscription_service
    def lookup(key: hg.TS[int, hg.final]) -> hg.TS[int]:
        """Return a value for every requested key."""

    @hg.compute_node
    def answer(keys: hg.TSS[int], offset: int) -> hg.TSD[int, hg.TS[int]]:
        return {key: key * 10 + offset for key in keys.value}

    @hg.graph
    def implementation(requests: hg.TSS[int], offset: int) -> hg.TSD[int, hg.TS[int]]:
        return answer(requests, offset)

    @hg.source_node
    def final_key() -> hg.TS[int, hg.final]:
        return 3

    @hg.graph
    def client(tick: hg.TS[int]) -> hg.TS[int]:
        _ = tick
        return lookup(final_key())

    catalogue = hg.ServiceCatalogue()
    catalogue.register_impl(lookup, hg.feedback_service_implementation(lookup, implementation), offset=5)
    assert hg.run_graph(client, [0], catalogue=catalogue) == [None, 35]


def test_feedback_service_implementation_retains_service_and_graph():
    @hg.service
    def local_service(value: hg.TS[int]) -> hg.TS[int]:
        pass

    @hg.graph
    def local_implementation(value: hg.TSD[int, hg.TS[int]]) -> hg.TSD[int, hg.TS[int]]:
        return value

    service_reference = weakref.ref(local_service)
    graph_reference = weakref.ref(local_implementation)
    feedback = hg.feedback_service_implementation(local_service, local_implementation)

    del local_service, local_implementation
    gc.collect()

    assert service_reference() is not None
    assert graph_reference() is not None

    del feedback
    gc.collect()

    assert service_reference() is None
    assert graph_reference() is None


def test_feedback_service_implementation_rejects_unsupported_signatures():
    @hg.reference_service
    def source() -> hg.TS[int]:
        pass

    @hg.service
    def sink(value: hg.TS[int]) -> None:
        pass

    @hg.service
    def echo_service(value: hg.TS[int]) -> hg.TS[int]:
        pass

    @hg.graph
    def outputless_implementation(value: hg.TSD[int, hg.TS[int]]) -> None:
        pass

    @hg.graph
    def implementation(value: hg.TSD[int, hg.TS[int]]) -> hg.TSD[int, hg.TS[int]]:
        return value

    with pytest.raises(ValueError, match="requires a service with requests and an output"):
        hg.feedback_service_implementation(source, implementation)

    with pytest.raises(ValueError, match="requires a service with requests and an output"):
        hg.feedback_service_implementation(sink, implementation)

    with pytest.raises(ValueError, match="implementation must have an output"):
        hg.feedback_service_implementation(echo_service, outputless_implementation)

    catalogue = hg.ServiceCatalogue()
    feedback = hg.feedback_service_implementation(echo_service, implementation)
    with pytest.raises(ValueError, match="different service"):
        catalogue.register_impl(sink, feedback)


def test_subscription_service_accepts_final_and_const_keys():
    @hg.subscription_service
    def lookup(key: hg.TS[int, hg.final]) -> hg.TS[int]:
        """Look up a value for a key that cannot change."""

    @hg.compute_node
    def make_responses(requests: hg.TSS[int]) -> hg.TSD[int, hg.TS[int]]:
        return {key: key * 10 for key in requests.value}

    @hg.graph
    def implementation(requests: hg.TSS[int]) -> hg.TSD[int, hg.TS[int]]:
        return make_responses(requests)

    @hg.source_node
    def final_key() -> hg.TS[int, hg.final]:
        return 9

    @hg.graph
    def client() -> hg.TS[int]:
        constant_result = lookup(hg.operators.const_(7))
        final_result = lookup(final_key())
        return hg.operators.add(constant_result, final_result)

    catalogue = hg.ServiceCatalogue()
    catalogue.register_impl(lookup, implementation)

    assert hg.run_graph(client, catalogue=catalogue) == [160]


def test_subscription_service_rejects_ordinary_key_at_call_site():
    @hg.subscription_service
    def lookup(key: hg.TS[int, hg.final]) -> hg.TS[int]:
        """Look up a value for a key that cannot change."""

    @hg.compute_node
    def make_responses(requests: hg.TSS[int]) -> hg.TSD[int, hg.TS[int]]:
        return {key: key for key in requests.value}

    @hg.graph
    def implementation(requests: hg.TSS[int]) -> hg.TSD[int, hg.TS[int]]:
        return make_responses(requests)

    @hg.source_node
    def ordinary_key() -> hg.TS[int]:
        return 7

    @hg.graph
    def client() -> hg.TS[int]:
        return lookup(ordinary_key())

    catalogue = hg.ServiceCatalogue()
    catalogue.register_impl(lookup, implementation)

    with pytest.raises(RuntimeError, match="argument does not satisfy its generic type"):
        hg.run_graph(client, catalogue=catalogue)


def test_guarantee_annotations_apply_to_each_time_series_shape():
    @hg.schema
    class Schema:
        value: hg.TS[int, hg.final]

    @hg.graph
    def shaped_inputs(
        values: hg.TSS[int, hg.final],
        items: hg.TSL[hg.TS[int], 2, hg.final],
        mapping: hg.TSD[int, hg.TS[int], hg.final],
        bundle: hg.TSB[Schema, hg.final],
        reference: hg.REF[hg.TS[int], hg.final],
    ) -> None:
        pass

    assert isinstance(shaped_inputs, hg.GraphDefinition)

    @hg.source_node
    def make_bundle() -> hg.TSB[Schema]:
        return {"value": 13}

    @hg.compute_node
    def read_final_field(value: hg.TS[int, hg.final]) -> hg.TS[int]:
        return value.value

    @hg.graph
    def read_schema_field() -> hg.TS[int]:
        field = hg.operators.getattr(make_bundle(), hg.symbol("value"))
        return read_final_field(field)

    assert hg.run_graph(read_schema_field) == [13]


def test_service_without_registered_implementation_fails_during_wiring():
    catalogue = hg.ServiceCatalogue()
    with pytest.raises(RuntimeError, match="no matching implementation"):
        hg.run_graph(reference_client, catalogue=catalogue)


def test_service_catalogue_retains_declarations_and_graph_wrappers():
    @hg.reference_service
    def local_service() -> hg.TS[int]:
        """A locally declared reference service."""

    @hg.graph
    def local_implementation() -> hg.TS[int]:
        return reference_source()

    service_reference = weakref.ref(local_service)
    implementation_reference = weakref.ref(local_implementation)
    catalogue = hg.ServiceCatalogue()
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
    @hg.reference_service
    def selected_reference(key: int) -> hg.TS[int]:
        pass

    @hg.reference_service
    def plain_reference() -> hg.TS[int]:
        pass

    @hg.source_node
    def selected_source() -> hg.TS[int]:
        return 10

    @hg.source_node
    def plain_source() -> hg.TS[int]:
        return 20

    @hg.graph
    def implementation() -> None:
        selected_reference.implementation_replies(selected_source(), 7)
        plain_reference.implementation_replies(plain_source())

    @hg.graph
    def client() -> hg.TS[int]:
        return hg.operators.add(selected_reference(7), plain_reference())

    catalogue = hg.ServiceCatalogue()
    registration = catalogue.register_multiservice(implementation)
    registration.register_impl(selected_reference, key=7)
    registration.register_impl(plain_reference)

    assert hg.run_graph(client, catalogue=catalogue) == [30]


def test_multiservice_graph_combines_subscription_and_reference_services():
    @hg.reference_service
    def clock() -> hg.TS[int]:
        pass

    @hg.subscription_service
    def lookup(key: hg.TS[int, hg.final]) -> hg.TS[int]:
        pass

    @hg.source_node
    def clock_source() -> hg.TS[int]:
        return 5

    @hg.compute_node
    def make_responses(keys: hg.TSS[int]) -> hg.TSD[int, hg.TS[int]]:
        return {key: key * 10 for key in keys.value}

    @hg.source_node
    def final_key() -> hg.TS[int, hg.final]:
        return 3

    @hg.graph
    def implementation() -> None:
        lookup.implementation_replies(make_responses(lookup.implementation_requests()))
        clock.implementation_replies(clock_source())

    @hg.graph
    def client() -> hg.TS[int]:
        return hg.operators.add(clock(), lookup(final_key()))

    catalogue = hg.ServiceCatalogue()
    registration = catalogue.register_multiservice(implementation)
    registration.register_impl(clock)
    registration.register_impl(lookup)

    assert hg.run_graph(client, catalogue=catalogue) == [35]


def test_multiservice_request_reply_uses_scalar_filter_and_installation_argument():
    @hg.service
    def transform(value: hg.TS[int], mode: int) -> hg.TS[int]:
        pass

    @hg.compute_node
    def transform_values(values: hg.TSD[int, hg.TS[int]], factor: int) -> hg.TSD[int, hg.TS[int]]:
        return {key: values[key].value * factor for key in values.valid_keys()}

    @hg.graph
    def implementation(factor: int) -> None:
        requests = transform.implementation_requests(2)
        values = hg.operators.getattr(requests, hg.symbol("value"))
        transform.implementation_replies(transform_values(values, factor), 2)

    @hg.graph
    def client(value: hg.TS[int]) -> hg.TS[int]:
        return transform(value, mode=2)

    catalogue = hg.ServiceCatalogue()
    registration = catalogue.register_multiservice(implementation, factor=3)
    registration.register_impl(transform, mode=2)

    assert hg.run_graph(client, [2, 4], catalogue=catalogue) == [6, 12]


def test_split_request_reply_client_binds_scalar_instance_arguments():
    @hg.service
    def echo(value: hg.TS[int], mode: str = "fast") -> hg.TS[int]:
        pass

    @hg.graph
    def implementation() -> None:
        requests = echo.implementation_requests("fast")
        echo.implementation_replies(hg.operators.getattr(requests, hg.symbol("value")), "fast")

    @hg.graph
    def client(value: hg.TS[int]) -> hg.TS[int]:
        client_id = hg.service_client_id()
        replies = echo.client_replies(client_id)
        echo.client_requests(client_id, value, mode="fast")
        return replies

    catalogue = hg.ServiceCatalogue()
    registration = catalogue.register_multiservice(implementation)
    registration.register_impl(echo, mode="fast")

    assert hg.run_graph(client, [3, 7], catalogue=catalogue, capture_output=True) == [3, 7]


def test_multiservice_outputless_service_uses_request_port_without_reply():
    @hg.service
    def observe(value: hg.TS[int]) -> None:
        pass

    seen = []

    @hg.compute_node
    def collect(values: hg.TSD[int, hg.TS[int]]) -> None:
        for key in values.valid_keys():
            seen.append(values[key].value)

    @hg.graph
    def implementation() -> None:
        requests = observe.implementation_requests()
        collect(hg.operators.getattr(requests, hg.symbol("value")))

    @hg.graph
    def client(value: hg.TS[int]) -> None:
        observe(value)

    catalogue = hg.ServiceCatalogue()
    registration = catalogue.register_multiservice(implementation)
    registration.register_impl(observe)

    hg.run_graph(client, [4, 6], catalogue=catalogue)

    assert seen == [4, 6]


def test_multiservice_registration_rejects_unknown_and_duplicate_claims():
    @hg.reference_service
    def selected(key: int) -> hg.TS[int]:
        pass

    @hg.graph
    def implementation() -> None:
        pass

    catalogue = hg.ServiceCatalogue()
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

    @hg.reference_service
    def rejected_service() -> hg.TS[int]:
        pass

    rejected_reference = weakref.ref(rejected_service)
    with pytest.raises(ValueError, match="must name a service scalar parameter"):
        registration.register_impl(rejected_service, missing=1)

    del rejected_service
    gc.collect()
    assert rejected_reference() is None


def test_multiservice_graph_rejects_an_unregistered_request_port():
    @hg.reference_service
    def registered() -> hg.TS[int]:
        pass

    @hg.reference_service
    def unregistered() -> hg.TS[int]:
        pass

    @hg.source_node
    def source() -> hg.TS[int]:
        return 1

    @hg.graph
    def implementation() -> None:
        registered.implementation_replies(source())
        unregistered.implementation_replies(source())

    @hg.graph
    def client() -> hg.TS[int]:
        return registered()

    catalogue = hg.ServiceCatalogue()
    registration = catalogue.register_multiservice(implementation)
    registration.register_impl(registered)

    with pytest.raises(RuntimeError, match="unregistered service port"):
        hg.run_graph(client, catalogue=catalogue)


def test_multiservice_catalogue_retains_claim_and_graph_wrappers():
    @hg.reference_service
    def local_service() -> hg.TS[int]:
        pass

    @hg.source_node
    def local_source() -> hg.TS[int]:
        return 1

    @hg.graph
    def local_implementation() -> None:
        local_service.implementation_replies(local_source())

    service_reference = weakref.ref(local_service)
    implementation_reference = weakref.ref(local_implementation)
    catalogue = hg.ServiceCatalogue()
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

        @hg.reference_service
        def missing_annotation(value):
            pass

    with pytest.raises(TypeError, match="explicit TS"):

        @hg.reference_service
        def scalar_output() -> int:
            pass

    with pytest.raises(ValueError, match="source-like"):

        @hg.reference_service
        def missing_reference_output() -> None:
            pass
