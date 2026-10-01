"""Explicit ownership and staged updates for single-part Python generators.

The application owns the manifest and undoable apply. This module plans builder
work and assigns stable feature IDs while creating a disposable staging part.
"""
from collections import defaultdict
from dataclasses import dataclass
import uuid

from designer_sdk import Part, SdkError


class _StagingPart(Part):
    def __init__(self, app, slot, features):
        super().__init__(app, slot)
        self._keys = {}
        ordinal = defaultdict(int)
        for f in features:
            stem = (f['type'], f.get('name', ''))
            key = (*stem, ordinal[stem])
            ordinal[stem] += 1
            self._keys[key] = f['id']
        self._seen = defaultdict(int)
        self._next = max((f['id'] for f in features), default=0) + 1

    def _stable_id(self, type_, name):
        stem = (type_, name)
        key = (*stem, self._seen[stem])
        self._seen[stem] += 1
        fid = self._keys.get(key)
        if fid is None:
            fid = self._next
            self._next += 1
        return fid

    def _add(self, feature):
        name = feature.get('name', '')
        if feature['type'] in _PRIMITIVES:
            # A primitive is stored as the features it expands to, a Sketch
            # named "<name> sketch" and the solid named <name>
            # (docs/modeling/README.md#primitives): pin both, as they are
            # what the manifest holds.
            solid = self._stable_id(_PRIMITIVES[feature['type']], name)
            sketch = self._stable_id('Sketch', name + ' sketch')
            return super()._add(dict(feature, ids=[solid, sketch]))
        return super()._add(dict(feature,
                                 id=self._stable_id(feature['type'], name)))


# What a primitive's solid is stored as.
_PRIMITIVES = {'Box': 'Extrude', 'Cylinder': 'Extrude', 'Sphere': 'Revolve'}


class _BuilderApp:
    def __init__(self, app, name, staging):
        self._app, self._name, self._staging = app, name, staging

    def part(self, name, create=True, reuse=False):
        # The staging part IS the builder's own part, whichever of create /
        # reuse the builder says (a builder shared with a full build may say
        # reuse=True); only opening someone else's is a different request.
        if not create:
            raise SdkError('a component builder builds its part; it cannot '
                           f'open an existing one ({name!r}, create=False)')
        if name != self._name:
            raise SdkError('a component builder must create exactly its declared part')
        self._staging.activate()
        return self._staging

    def __getattr__(self, key):
        return getattr(self._app, key)


@dataclass
class ComponentUpdate:
    app: object
    part: Part
    staging: Part | None
    fingerprint: str
    before: dict
    source: dict | None
    report: dict

    def apply(self):
        if self.staging is None:
            now = self.app.component_status(part_id=self.part.id)
            if now['revision'] != self.before['revision'] or now['state'] != 'owned':
                raise SdkError('component changed after the reuse plan')
            return self.part
        self.report = self.app.update_component(
            part_id=self.part.id, source_part_id=self.staging.id,
            fingerprint=self.fingerprint,
            expected_revision=self.before['revision'],
            source_revision=self.source['revision'], apply=True)
        self.discard()
        return self.part

    def discard(self):
        if self.staging is not None:
            self.app.delete_part(part=self.staging.id)
            self.staging = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.discard()


def claim(part, *, namespace, key, fingerprint):
    before = part.app.component_status(part_id=part.id)
    return part.app.claim_component(part_id=part.id, namespace=namespace,
                                    key=key, fingerprint=fingerprint,
                                    expected_revision=before['revision'])


def plan(app, *, namespace, key, fingerprint, builder):
    """Build only when needed; caller reviews .report before explicit apply().

    fingerprint must include all builder inputs and dependencies. The caller
    declares it: undeclared ambient inputs must not be treated as cacheable.
    Existing feature names/types plus ordinal within duplicates are stable keys;
    rename/retirement conflicts are refused rather than guessed.
    """
    before = app.component_status(namespace=namespace, key=key)
    if before['state'] != 'owned':
        raise SdkError(f'component {namespace}/{key}: {before["state"]}; '
                       'claim fresh output explicitly, resolve human edits before updating')
    slot = next((p for p in app.parts() if p['id'] == before['part_id']), None)
    if slot is None:
        raise SdkError('component part disappeared')
    part = Part(app, slot)
    if before['generator']['fingerprint'] == fingerprint:
        return ComponentUpdate(app, part, None, fingerprint, before, None,
                               dict(action='reuse', part_id=part.id, applied=False))
    features = part.state()['features']
    slot = app.add_part(name='staging-' + uuid.uuid4().hex)
    # add_part returns id plus index/name depending on the server version.
    staging = _StagingPart(app, dict(id=slot['id'], name=slot.get('name', 'staging')), features)
    staging.name = part.name
    try:
        made = builder(_BuilderApp(app, part.name, staging))
        if not isinstance(made, Part) or made.id != staging.id:
            raise SdkError('component builder did not return its staging part')
        source = app.component_status(part_id=staging.id)
        report = app.update_component(part_id=part.id, source_part_id=staging.id,
                                      fingerprint=fingerprint,
                                      expected_revision=before['revision'],
                                      source_revision=source['revision'], apply=False)
        return ComponentUpdate(app, part, staging, fingerprint, before, source, report)
    except BaseException:
        app.delete_part(part=staging.id)
        raise
