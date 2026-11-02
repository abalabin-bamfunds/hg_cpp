import hg
import pytest
from hg_web import (
    AnonymousAuth,
    Auth,
    AuthDecision,
    BasicAuth,
    BearerAuth,
    DigestAuth,
    HeaderAuth,
    HttpResponse,
    NegotiateAuth,
    NtlmAuth,
)


@hg.schema
class User:
    name: str


def test_auth_decision_schema_construction_and_native_conversion():
    decision_type = AuthDecision[User]
    assert AuthDecision[User] is decision_type
    assert AuthDecision[str] is not decision_type

    accepted = decision_type.accept(User("alice"))
    rejected = decision_type.reject(HttpResponse(status=401, body=b"denied"))

    with pytest.raises(ValueError, match="exactly one"):
        decision_type()
    with pytest.raises(ValueError, match="exactly one"):
        decision_type(identity=User("alice"), rejection=HttpResponse(status=401))
    with pytest.raises(TypeError, match="HttpResponse"):
        decision_type.reject("denied")

    @hg.compute_node
    def describe(decision: hg.TS[decision_type]) -> hg.TS[str]:
        value = decision.value
        if value.identity is not None:
            return value.identity.name
        return f"rejected:{value.rejection.status}:{bytes(value.rejection.body).decode()}"

    @hg.graph
    def accepted_graph() -> hg.TS[str]:
        return describe(hg.operators.const_(accepted))

    @hg.graph
    def rejected_graph() -> hg.TS[str]:
        return describe(hg.operators.const_(rejected))

    assert hg.run_graph(accepted_graph, capture_output=True) == ["alice"]
    assert hg.run_graph(rejected_graph, capture_output=True) == ["rejected:401:denied"]


def test_client_auth_schemas_keep_the_derived_type_through_a_base_input():
    values = [
        AnonymousAuth(),
        BasicAuth("alice", "secret"),
        DigestAuth("bob", "secret"),
        NegotiateAuth(),
        NtlmAuth("dave", "secret"),
        BearerAuth("token-value"),
        HeaderAuth([("X-Token", "header-value")]),
    ]
    types = [type(value) for value in values]
    expected_fields = {
        AnonymousAuth: {},
        BasicAuth: {"username": "alice", "password": "secret"},
        DigestAuth: {"username": "bob", "password": "secret"},
        NegotiateAuth: {},
        NtlmAuth: {"username": "dave", "password": "secret"},
        BearerAuth: {"token": "token-value"},
        HeaderAuth: {"headers": (("X-Token", "header-value"),)},
    }

    @hg.compute_node
    def describe(value: hg.TS[Auth]) -> hg.TS[str]:
        observed = value.value
        assert any(type(observed) is schema for schema in types)
        for field, expected in expected_fields[type(observed)].items():
            assert getattr(observed, field) == expected
        return type(observed).__name__

    @hg.queue
    def auth_values(writer: hg.Queue = None) -> hg.TS[Auth]:
        for value in values:
            assert writer.write(value)

    @hg.graph
    def graph() -> hg.TS[str]:
        return describe(auth_values())

    assert hg.run_graph(graph, capture_output=True) == [schema.__name__ for schema in types]
