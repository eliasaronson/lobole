# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

from lobole.inspection import InspectionClient
from lobole.sdk import App


def mistakes(client: InspectionClient, app: App) -> None:
    client.geometry_snapshot(part_id='one', body=0)  # type-error
    client.geometry_snapshot(part_id=1)  # type-error
    client.geometry_snapshot(part_id=1, body=0, fields=['centre'])  # type-error
    client.geometry_snapshot(part_id=1, body=0, circular='yes')  # type-error
    client.geometry_snapshot(part_id=1, body=0, typo=2)  # type-error
    client.no_such_rpc()  # type-error
    app.inspection.no_such_rpc()  # type-error
    result = app.inspection.geometry_snapshot(part_id=1, body=0)
    result['nonsense']  # type-error
