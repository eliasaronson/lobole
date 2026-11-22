# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every error the lobole package raises, in one hierarchy.

    LoboleError
    ├── ConnectionLost      the app went away (also a ConnectionError);
    │                       `.sent` says whether the call had gone out
    ├── CallTimedOut        no answer in time, and the app is still on it
    │                       (also a TimeoutError): do not send it again
    ├── Refused             the app did not do it, and changed nothing
    │   ├── MissingArgument     code missing_argument
    │   ├── UnknownArgument     code unknown_argument
    │   ├── InvalidArgument     code invalid_argument
    │   ├── StaleRevision       code stale_revision
    │   ├── StaleSnapshot       code stale_snapshot
    │   ├── BudgetExceeded      code budget_exceeded
    │   └── InternalError       code internal: the handler fell over
    ├── NonFiniteError      the call ran, and its answer held NaN/inf
    ├── ExpectationFailed   the call ran, and missed what it was said to make
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
    """The connection to the app is gone. `sent` is False when it was found
    gone before the call was written, so nothing reached the app and the
    call is safe to send again on a new connection; True when it went while
    the app had the call, and whether the call was applied is not known
    (docs/scripting/sdk.md#errors)."""

    def __init__(self, message, sent=True):
        super().__init__(message)
        self.sent = sent


class CallTimedOut(LoboleError, TimeoutError):
    """No answer within the call's timeout. The app is still running the
    call and will finish it, so sending it again would apply it twice;
    `method` is the call, `timeout` the seconds waited. The connection stays
    open, since closing it is what the app reads as the caller leaving (a
    check, or a solve without outlive_client, is cancelled then); the late
    answer is skipped by the next call, whose id it does not carry."""

    def __init__(self, message, method, timeout):
        super().__init__(message)
        self.method = method
        self.timeout = timeout


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


class DoesNotFit(Refused):
    """A Fillet, Chamfer or Shell whose size does not build: `observed` is
    the size asked, `fits` the answer the app searched for, {param (radius or
    thickness), asked_mm, fits_mm (None: nothing built), fails_mm, basis,
    element?}, and `fits_mm` the size to send instead -- or ask for it with
    `fit="reduce"` (docs/scripting/rpc.md#structured-errors)."""
    CODE = "does_not_fit"

    fits = property(lambda self: self._field("fits"))
    fits_mm = property(lambda self: (self._field("fits") or {}).get("fits_mm"))


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


class ExpectationFailed(LoboleError):
    """A batch's (or check_expect's) `expect` missed
    (docs/scripting/rpc.md#expect): `checks` are the judged numbers, each
    {quantity, expected, measured, unit, deviation_pct, pass}, and `misses`
    the sentences, one per miss. The call itself ran -- a batch's edits are
    applied, as one undo step, so the bodies can be read for why -- so this
    is a wrong result, not a refusal. `result` is the answer as sent."""

    def __init__(self, message, misses, result, checks=None):
        super().__init__(message)
        self.misses = misses
        self.result = result
        self.checks = checks or []


class SdkError(LoboleError):
    """A refusal from the SDK itself, never from the app: a handle from
    another document, a keyword renamed to carry its unit, a lookup that
    found nothing."""


_BY_CODE: dict[str, type[Refused]] = {
    cls.CODE: cls for cls in (
        MissingArgument, UnknownArgument, InvalidArgument, StaleRevision,
        StaleSnapshot, BudgetExceeded, InternalError, DeviceLost,
        DoesNotFit)
    if cls.CODE}


def refusal(message: str, diagnostic: dict | None = None) -> Refused:
    """The Refused subclass for `diagnostic`'s code, built."""
    code = (diagnostic or {}).get("code")
    cls = _BY_CODE.get(code, Refused) if isinstance(code, str) else Refused
    return cls(message, diagnostic)
