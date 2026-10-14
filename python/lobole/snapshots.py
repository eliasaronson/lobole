# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Explicit, immutable edge measurements; no network access in properties."""
from dataclasses import dataclass
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from lobole.sdk import App, V


@dataclass(frozen=True)
class MeasuredEdge:
    index: int
    feature_id: int
    element: str
    kind: str
    center: 'V | None'
    radius: float | None
    length: float | None
    midpoint: 'V | None'

    @property
    def ref(self) -> dict:
        # A fresh dictionary: mutations cannot change the frozen record.
        return dict(feature_id=self.feature_id, element=self.element)


@dataclass(frozen=True)
class SnapshotRevision:
    session: str
    part_id: int
    instance: int
    version: int

    def as_dict(self) -> dict:
        return dict(session=self.session, part_id=self.part_id,
                    instance=self.instance, version=self.version)


@dataclass(frozen=True)
class MeasurementSnapshot:
    _app: 'App'
    revision: SnapshotRevision
    body: int
    space: str
    edges: tuple[MeasuredEdge, ...]
    _circular: bool
    _fields: tuple[str, ...]
    _max_elements: int

    @classmethod
    def capture(cls, app, part_id, body, circular=False,
                fields=('center', 'radius'), max_elements=2000):
        from lobole.sdk import V
        fields = tuple(fields)
        result = app.inspection.geometry_snapshot(
            part_id=part_id, body=body, circular=circular, fields=fields,
            max_elements=max_elements)
        edges = tuple(MeasuredEdge(
            e['index'], e['ref']['feature_id'], e['ref']['element'], e['kind'],
            V(e['center']) if 'center' in e else None, e.get('radius'),
            e.get('length'), V(e['midpoint']) if 'midpoint' in e else None)
            for e in result['elements'])
        return cls(app, SnapshotRevision(**result['revision']), result['body'],
                   result['space'], edges, circular, fields, max_elements)

    def validate(self) -> None:
        """One RPC; raises StaleSnapshot (lobole.errors) if replaced/edited.

        This is a point-in-time check, not a lock or a mutation precondition.
        """
        self._app.inspection.validate_snapshot(revision=self.revision.as_dict())

    def refresh(self) -> 'MeasurementSnapshot':
        """Return a new measurement set; this snapshot remains unchanged."""
        return self.capture(self._app, self.revision.part_id, self.body,
                            self._circular, self._fields, self._max_elements)
