"""HTTP authentication decision values."""

from functools import cache
from typing import get_type_hints

import bg

from . import HttpResponse


def _policy_graphs(policy):
    if not isinstance(policy, bg._ServiceDefinition):
        raise TypeError("authentication policy must be a bg.service declaration")

    parameters = tuple(policy.signature.parameters.values())
    if len(parameters) != 1:
        raise TypeError("an HTTP authentication policy must take one request input")

    annotations = get_type_hints(policy.function, include_extras=True)
    parameter = parameters[0]
    if parameter.name not in annotations or "return" not in annotations:
        raise TypeError("an HTTP authentication policy requires request and decision type annotations")

    def submit_request(request):
        client_id = bg.service_client_id()
        policy.client_requests(client_id, request)
        return client_id

    submit_request.__name__ = f"{policy.function.__name__}_http_policy_request"
    submit_request.__annotations__ = {
        "request": annotations[parameter.name],
        "return": bg.TS[int, bg.const_],
    }

    def receive_reply(client_id):
        return policy.client_replies(client_id)

    receive_reply.__name__ = f"{policy.function.__name__}_http_policy_reply"
    receive_reply.__annotations__ = {
        "client_id": bg.TS[int, bg.const_],
        "return": annotations["return"],
    }
    return bg.graph(submit_request), bg.graph(receive_reply)


@cache
def _auth_decision_schema(identity_type):
    def initialize(self, identity=None, rejection=None):
        if (identity is None) == (rejection is None):
            raise ValueError("an authentication decision requires exactly one of identity or rejection")
        if rejection is not None and not isinstance(rejection, HttpResponse):
            raise TypeError("rejection must be an HttpResponse")
        self.identity = identity
        self.rejection = rejection

    @classmethod
    def accept(decision_type, identity):
        return decision_type(identity=identity)

    @classmethod
    def reject(decision_type, response):
        return decision_type(rejection=response)

    name = f"AuthDecision_{identity_type!r}"
    decision_type = type(
        name,
        (),
        {
            "__module__": __name__,
            "__qualname__": name,
            "__annotations__": {
                "identity": identity_type | None,
                "rejection": HttpResponse | None,
            },
            "__init__": initialize,
            "accept": accept,
            "reject": reject,
        },
    )
    return bg.schema(decision_type)


class AuthDecision:
    """Cached schema class for accepted identities and rejected responses."""

    def __class_getitem__(cls, identity_type):
        return _auth_decision_schema(identity_type)
