# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

from lobole.inspection import InspectionClient
from lobole.sdk import App


def inspect(client: InspectionClient, app: App) -> float:
    result = client.geometry_snapshot(part_id=1, body=0, circular=True,
                                      fields=['center', 'radius'])
    same = app.inspection.geometry_snapshot(part_id=1, body=0)
    client.validate_snapshot(revision=same['revision'])
    return result['elements'][0]['radius']
