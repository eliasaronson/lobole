#!/usr/bin/env python3
"""Wire/client parity, bounded frozen measurements, and stale-document safety."""
import dataclasses
import os
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from designer_contracts import ContractError, validate
from designer_remote import RemoteError
from designer_sdk import App, V
from smoke_app import launch


def refusal(fn, code):
    try:
        fn()
    except (ContractError, RemoteError) as error:
        assert error.diagnostic['code'] == code, error.diagnostic
        return error.diagnostic
    raise AssertionError(f'expected {code}')


class ChangedContractRemote:
    """An independently upgraded server, otherwise exposing the same API."""
    def __init__(self, remote):
        self.remote = remote

    def call(self, method, **params):
        result = self.remote.call(method, **params)
        if method == 'describe':
            result['inspection_contract']['hash'] = 'different contract'
        return result


class CountedRemote:
    def __init__(self, remote):
        self.remote, self.calls = remote, 0

    def call(self, method, **params):
        self.calls += 1
        return self.remote.call(method, **params)


def main():
    with tempfile.TemporaryDirectory(prefix='inspection-contracts-') as tmp:
        env = dict(os.environ, XDG_STATE_HOME=tmp, XDG_CONFIG_HOME=tmp + '/config')
        with launch(sys.argv[1], env=env) as running:
            raw = running.designer
            counted = CountedRemote(raw)
            app = App(counted)
            app.new()
            part = app.part('angled cylinder')
            part.var('diameter', 12)
            axis = V(1, 2, 3).unit()
            origin = V(7, 11, 19)
            body = part.cylinder(center=origin, direction=axis, d='diameter', h=23)
            # Read another part without switching the visible tab.
            other = app.part('visible part')
            other.var('diameter', 40)
            active = app.active()
            snap = body.snapshot(circular=True)
            assert app.active() == active == other.id
            assert len(snap.edges) == 2
            assert snap.space == 'part'
            expected = [origin, origin + axis * 23]
            for edge in snap.edges:
                assert min((edge.center - point).length() for point in expected) < 1e-4
                assert abs(edge.radius - 6) < 1e-5
                live = app.frame(part=part.id, **edge.ref)
                assert (edge.center - V(live['origin'])).length() < 1e-5
                copied = edge.ref
                copied['element'] = 'tampered'
                assert edge.ref['element'] != 'tampered'
            before = counted.calls
            for _ in range(100):
                for edge in snap.edges:
                    assert edge.center.length() > 0
                    assert edge.ref['feature_id'] > 0
            assert counted.calls == before, 'snapshot properties performed RPCs'
            try:
                snap.edges[0].radius = 42
                raise AssertionError('mutable measurement')
            except dataclasses.FrozenInstanceError:
                pass
            snap.validate()
            # Request and response conform to the same published schema.
            docs = raw.call('describe')
            methods = {row['name']: row for row in docs['methods']}
            for method in ('geometry_snapshot', 'validate_snapshot'):
                assert set(methods[method]['params']) == set(
                    methods[method]['request_schema']['properties'])
            assert len(docs['inspection_contract']['hash']) == 64
            good = dict(part_id=part.id, body=0)
            upgraded = App(ChangedContractRemote(counted))
            before = counted.calls
            refusal(lambda: upgraded.inspection.geometry_snapshot(**good),
                    'unsupported_contract')
            assert counted.calls == before
            # Explicit dynamic/raw access remains available across versions.
            assert upgraded.call('geometry_snapshot', **good)['space'] == 'part'
            result = raw.call('geometry_snapshot', **good)
            validate(methods['geometry_snapshot']['result_schema'], result,
                     'geometry_snapshot', '/result')
            assert all('center' in e for e in result['elements'])
            explicit_defaults = raw.call('geometry_snapshot', **good,
                circular=False, fields=['center', 'radius'], max_elements=2000)
            assert result == explicit_defaults
            length_only = raw.call('geometry_snapshot', **good, fields=['length'])
            assert all('length' in e and 'center' not in e for e in length_only['elements'])
            for invalid_params in (False, [], 'bad'):
                refusal(lambda: raw.request('geometry_snapshot', invalid_params), 'invalid_argument')
            bad_requests = [
                ({'body': 0}, 'missing_argument'),
                ({**good, 'body': True}, 'invalid_argument'),
                ({**good, 'body': -1}, 'invalid_argument'),
                ({**good, 'part_id': '1'}, 'invalid_argument'),
                ({**good, 'part_id': 2**64}, 'invalid_argument'),
                ({**good, 'circular': 1}, 'invalid_argument'),
                ({**good, 'fields': ['bogus']}, 'invalid_argument'),
                ({**good, 'fields': ['center', 'center']}, 'invalid_argument'),
                ({**good, 'fields': []}, 'invalid_argument'),
                ({**good, 'typo': 1}, 'unknown_argument'),
                ({**good, 'max_elements': 10001}, 'invalid_argument'),
            ]
            for request, code in bad_requests:
                before = counted.calls
                client = refusal(lambda: app.call('geometry_snapshot', **request), code)
                assert counted.calls == before, 'preflight sent an invalid request'
                server = refusal(lambda: raw.call('geometry_snapshot', **request), code)
                assert client == server, (client, server)
            bad_call = dict(method='geometry_snapshot', params=dict(good, body=True))
            failed_batch = refusal(lambda: raw.call('batch', calls=[bad_call]), 'batch_failed')
            assert failed_batch['failures'][0]['diagnostic']['path'] == '/params/body'
            partial = raw.call('batch', calls=[bad_call], stop_on_error=False)
            assert partial['failed'][0]['diagnostic']['code'] == 'invalid_argument'
            # JSON Schema integer semantics: 0.0 is accepted, bool is not.
            raw.call('geometry_snapshot', part_id=float(part.id), body=0.0)
            for number in (float('nan'), float('inf'), -float('inf')):
                refusal(lambda: app.call('geometry_snapshot', **good, max_elements=number),
                        'invalid_argument')
                try:
                    raw.call('geometry_snapshot', **good, max_elements=number)
                    raise AssertionError('non-finite number serialized')
                except ValueError:
                    pass
            refusal(lambda: raw.call('geometry_snapshot', **good, max_elements=1),
                    'budget_exceeded')
            refusal(lambda: raw.call('geometry_snapshot', part_id=999999, body=0),
                    'unknown_part')
            refusal(lambda: raw.call('geometry_snapshot', part_id=part.id, body=999),
                    'unknown_body')
            broken = snap.revision.as_dict()
            broken['session'] = 'different server'
            refusal(lambda: raw.call('validate_snapshot', revision=broken), 'stale_snapshot')
            bad_revision = dict(broken, typo=True)
            c = refusal(lambda: app.inspection.validate_snapshot(revision=bad_revision), 'unknown_argument')
            r = refusal(lambda: raw.call('validate_snapshot', revision=bad_revision), 'unknown_argument')
            assert c == r
            part.var('diameter', 18)
            assert other.value_of('diameter') == 40
            assert part.vars()['diameter']['value'] == 18
            refusal(snap.validate, 'stale_snapshot')
            refreshed = snap.refresh()
            refreshed.validate()
            assert all(abs(e.radius - 9) < 1e-5 for e in refreshed.edges)
            assert all(abs(e.radius - 6) < 1e-5 for e in snap.edges)
            saved = tmp + '/snapshot-project.dsgn'
            app.save_project(path=saved)
            app.open_project(path=saved)
            refusal(refreshed.validate, 'stale_snapshot')
            reopened = app.part('angled cylinder', create=False).body().snapshot(circular=True)
            reopened.validate()
            # A new project can reuse IDs/version counters; identity must differ.
            app.new()
            replacement = app.part('replacement')
            replacement.cylinder(d=12, h=23)
            refusal(reopened.validate, 'stale_snapshot')
            print('inspection contracts: parity, centers, zero-RPC reads, budgets, stale revisions passed')


if __name__ == '__main__':
    main()
