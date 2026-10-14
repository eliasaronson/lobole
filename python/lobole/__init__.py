# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Drive a running Loböle instance from Python (docs/scripting/README.md).

    from lobole import connect
    app = connect()                 # the one running instance, or port=
    part = app.part("bracket")

`Client` is the plain JSON-RPC client (dict in, dict out); `App` is the SDK
over it, with the whole RPC surface generated from the running app's
`describe` and geometry handles instead of transcribed numbers.
"""
from lobole.client import Client, RemoteError
from lobole.sdk import App, SdkError, connect

__all__ = ["App", "Client", "RemoteError", "SdkError", "connect"]
