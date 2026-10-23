# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Drive a running Loböle instance from Python (docs/scripting/README.md).

    from lobole import connect
    app = connect()                 # the one running instance, or port=
    part = app.part("bracket")

`Client` is the plain JSON-RPC client (dict in, dict out); `App` is the SDK
over it, with the whole RPC surface generated from the running app's
`describe` and geometry handles instead of transcribed numbers. Every error
either raises is a `LoboleError`; a refused call is a `Refused`
(lobole.errors).
"""
from lobole.client import Client
from lobole.errors import (BudgetExceeded, ConnectionLost, DeviceLost,
                           InternalError, InvalidArgument, LoboleError,
                           MissingArgument,
                           NonFiniteError, Refused, SdkError, StaleRevision,
                           StaleSnapshot, UnknownArgument)
from lobole.sdk import App, connect

__all__ = [
    "App", "BudgetExceeded", "Client", "ConnectionLost", "DeviceLost",
    "InternalError",
    "InvalidArgument", "LoboleError", "MissingArgument", "NonFiniteError",
    "Refused", "SdkError", "StaleRevision", "StaleSnapshot",
    "UnknownArgument", "connect"]
