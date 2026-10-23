import bg
import pytest
from bg_web import AuthDecision, HttpResponse


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
