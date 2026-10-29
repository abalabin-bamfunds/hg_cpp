import bg
import pytest
from bg_web import (
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


@bg.schema
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

    @bg.compute_node
    def describe(decision: bg.TS[decision_type]) -> bg.TS[str]:
        value = decision.value
        if value.identity is not None:
            return value.identity.name
        return f"rejected:{value.rejection.status}:{bytes(value.rejection.body).decode()}"

    @bg.graph
    def accepted_graph() -> bg.TS[str]:
        return describe(bg.operators.const_(accepted))

    @bg.graph
    def rejected_graph() -> bg.TS[str]:
        return describe(bg.operators.const_(rejected))

    assert bg.run_graph(accepted_graph, capture_output=True) == ["alice"]
    assert bg.run_graph(rejected_graph, capture_output=True) == ["rejected:401:denied"]


def test_client_auth_schemas_keep_the_derived_type_through_a_base_input():
    values = [
        AnonymousAuth(),
        BasicAuth("alice", "secret"),
        DigestAuth("bob", "secret"),
        NegotiateAuth("carol", "secret"),
        NtlmAuth("dave", "secret"),
        BearerAuth("token-value"),
        HeaderAuth([("X-Token", "header-value")]),
    ]
    types = [type(value) for value in values]
    expected_fields = {
        AnonymousAuth: {},
        BasicAuth: {"username": "alice", "password": "secret"},
        DigestAuth: {"username": "bob", "password": "secret"},
        NegotiateAuth: {"username": "carol", "password": "secret"},
        NtlmAuth: {"username": "dave", "password": "secret"},
        BearerAuth: {"token": "token-value"},
        HeaderAuth: {"headers": (("X-Token", "header-value"),)},
    }

    @bg.compute_node
    def describe(value: bg.TS[Auth]) -> bg.TS[str]:
        observed = value.value
        assert any(type(observed) is schema for schema in types)
        for field, expected in expected_fields[type(observed)].items():
            assert getattr(observed, field) == expected
        return type(observed).__name__

    @bg.queue
    def auth_values(writer: bg.Queue = None) -> bg.TS[Auth]:
        for value in values:
            assert writer.write(value)

    @bg.graph
    def graph() -> bg.TS[str]:
        return describe(auth_values())

    assert bg.run_graph(graph, capture_output=True) == [schema.__name__ for schema in types]
