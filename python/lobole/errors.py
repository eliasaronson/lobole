# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every error the lobole package raises, in one hierarchy.

    LoboleError
    ├── ConnectionLost      the app went away mid-call (also a ConnectionError)
    ├── Refused             the app did not do it, and changed nothing
    │   ├── MissingArgument     code missing_argument
    │   ├── UnknownArgument     code unknown_argument
    │   ├── InvalidArgument     code invalid_argument
    │   ├── StaleRevision       code stale_revision
    │   ├── StaleSnapshot       code stale_snapshot
    │   ├── BudgetExceeded      code budget_exceeded
    │   └── InternalError       code internal: the handler fell over
    ├── NonFiniteError      the call ran, and its answer held NaN/inf
    └── SdkError            misuse the SDK caught without asking the app

A refusal carries the server's structured diagnostic
(docs/scripting/rpc.md#structured-errors) as `.diagnostic`, and its fields as
attributes: `.code`, `.path`, `.param`, `.expected`, `.observed`,
`.accepted`, `.call`. A refusal the client catches before the round trip (a
typed contract, docs/scripting/sdk.md#the-generated-layer) raises the same
class with the same diagnostic the app would have sent, so a caller cannot
tell which side refused. An internal error is a refusal too: the app rolls
the call back like any other.

A code this client does not know is a plain `Refused`, so an app newer than
the client still raises something catchable by the base class.
"""


class LoboleError(RuntimeError):
    """Anything the lobole package raises on purpose."""


class ConnectionLost(LoboleError, ConnectionError):
    """The app closed the connection before answering."""


class Refused(LoboleError):
    """The app (or the client, on the app's behalf) refused the call, and
    nothing changed. `diagnostic` is the server's structured one, or None
    for a refusal that only has its sentence."""

    #: The `code` this class stands for; None on the base class.
    CODE: str | None = None

    def __init__(self, message, diagnostic=None):
        super().__init__(message)
        self.diagnostic = diagnostic

    def _field(self, key):
        return (self.diagnostic or {}).get(key)

    code = property(lambda self: self._field("code"))
    path = property(lambda self: self._field("path"))
    param = property(lambda self: self._field("param"))
    expected = property(lambda self: self._field("expected"))
    observed = property(lambda self: self._field("observed"))
    accepted = property(lambda self: self._field("accepted"))
    call = property(lambda self: self._field("call"))


class MissingArgument(Refused):
    CODE = "missing_argument"


class UnknownArgument(Refused):
    CODE = "unknown_argument"


class InvalidArgument(Refused):
    """A wrong type, an enum miss, a range miss or a wrong shape."""
    CODE = "invalid_argument"


class StaleRevision(Refused):
    """`expect_revision` named a revision the document has moved past."""
    CODE = "stale_revision"


class StaleSnapshot(Refused):
    """An inspection snapshot from before the document last changed."""
    CODE = "stale_snapshot"


class BudgetExceeded(Refused):
    """An inspection answer would be larger than the budget it was given."""
    CODE = "budget_exceeded"


class InternalError(Refused):
    """The handler fell over rather than refused (an exception that is not a
    deliberate refusal); `observed` names its type. Rolled back all the
    same — and a bug in the app, worth a report."""
    CODE = "internal"


class DeviceLost(Refused):
    """The app's graphics device was lost and a rebuild in place could not
    save it (the rebuild failed, or it was lost again within 60 s), so a
    method that draws (a screenshot, a still, a face picked by mesh, a UI
    action) is refused. While a rebuild runs, requests wait instead. The
    document is intact and every other method still answers; the app asks
    its user to restart (docs/rendering/README.md#a-lost-device)."""
    CODE = "device_lost"


class NonFiniteError(LoboleError):
    """The answer held NaN or infinity, which JSON writes as null: the server
    listed where under `nonfinite`
    (docs/scripting/rpc.md#what-a-response-carries). The call itself ran --
    a mutating one has happened -- so this is a wrong answer, not a refusal.
    `result` is the answer as sent."""

    def __init__(self, message, pointers, result):
        super().__init__(message)
        self.pointers = pointers
        self.result = result


class SdkError(LoboleError):
    """A refusal from the SDK itself, never from the app: a handle from
    another document, a keyword renamed to carry its unit, a lookup that
    found nothing."""


_BY_CODE: dict[str, type[Refused]] = {
    cls.CODE: cls for cls in (
        MissingArgument, UnknownArgument, InvalidArgument, StaleRevision,
        StaleSnapshot, BudgetExceeded, InternalError, DeviceLost)
    if cls.CODE}


def refusal(message: str, diagnostic: dict | None = None) -> Refused:
    """The Refused subclass for `diagnostic`'s code, built."""
    code = (diagnostic or {}).get("code")
    cls = _BY_CODE.get(code, Refused) if isinstance(code, str) else Refused
    return cls(message, diagnostic)
