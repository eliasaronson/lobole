#!/usr/bin/env python3
"""Typed RPC contracts against a running app (docs/scripting/rpc.md#typed-contracts).

- The server's `request_schema` for every typed or parameterless row equals
  the Python reading of the same row (tools/rpc_contract.py), which is what
  the generated stubs (tools/designer_rpc.py) are built from, and every
  `@vocabulary` a row names is one describe publishes.
- Every typed method refuses a missing required key, a value of the wrong
  type, an enum miss and a range miss before its handler runs, with the
  structured diagnostic, and the document does not move.
- An uncontracted method's unknown key and missing key carry a diagnostic too.
- The SDK refuses the same call client-side with the same diagnostic.
- The revision guard: a current revision passes and is answered with the new
  one, a stale or foreign one is refused and changes nothing, and `batch`
  guards the whole transaction.
"""
import os
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from designer_contracts import accepts  # noqa: E402
from designer_remote import RemoteError  # noqa: E402
from designer_rpc import COLUMNS  # noqa: E402
from designer_sdk import App  # noqa: E402
from rpc_contract import parse_column, request_schema  # noqa: E402
from smoke_app import launch  # noqa: E402

PROBES = [True, 'text', 1.5, -1, [], {}, None, [True]]


def refused(fn, code):
    try:
        fn()
    except RemoteError as e:
        assert e.diagnostic and e.diagnostic['code'] == code, (str(e), e.diagnostic)
        return e
    raise AssertionError(f'expected a {code} refusal')


def sample(schema):
    """A value the schema accepts."""
    if 'anyOf' in schema:
        return sample(schema['anyOf'][0])
    if 'enum' in schema:
        return schema['enum'][0]
    t = schema.get('type')
    if t == 'integer':
        return int(schema.get('minimum', schema.get('exclusiveMinimum', -1) + 1))
    if t == 'number':
        return float(schema.get('minimum', schema.get('exclusiveMinimum', 0) + 1))
    if t == 'object':
        if 'properties' in schema:
            return {k: sample(schema['properties'][k]) for k in schema['required']}
        return {}
    if t == 'array':
        return [sample(schema['items'])] * schema.get('minItems', 0)
    return {'string': 'x', 'boolean': True, 'null': None}.get(t, 0)


def world(raw):
    """What a refused call must leave as it was."""
    state = raw.call('get_state')
    return (len(state['features']), state['can_undo'], state['revision'],
            raw.call('list_parts'))


def check_schemas(raw, docs):
    vocab = {k: v for k, v in docs.items()
             if isinstance(v, list) and all(isinstance(x, str) for x in v)}
    rows = {m['name']: m for m in docs['methods']}
    inspection = {'geometry_snapshot', 'validate_snapshot'}
    server = {n for n, m in rows.items() if 'request_schema' in m} - inspection
    assert server == set(COLUMNS), (server ^ set(COLUMNS))
    def plain(s):
        # A vocabulary's remedy (`description`) is prose the server adds.
        if isinstance(s, dict):
            return {k: plain(v) for k, v in s.items() if k != 'description'}
        if isinstance(s, list):
            return [plain(v) for v in s]
        return s
    for name, column in COLUMNS.items():
        mine = request_schema(parse_column(column), vocab)
        theirs = plain(rows[name]['request_schema'])
        assert theirs == mine, (name, mine, theirs)
        assert rows[name]['params'] == [p['name'] for p in parse_column(column)]
    typed = [n for n, c in COLUMNS.items() if parse_column(c)]
    print(f'schemas: {len(COLUMNS)} contracted rows ({len(typed)} typed) '
          f'match the Python reading')
    return rows, typed


def check_refusals(raw, rows, typed):
    before = world(raw)
    cases = 0
    for name in typed:
        schema = rows[name]['request_schema']
        props, required = schema['properties'], schema['required']
        base = {k: sample(props[k]) for k in required}
        if required:
            e = refused(lambda: raw.call(name), 'missing_argument')
            d = e.diagnostic
            assert d['operation'] == name and d['param'] == required[0], d
            assert "missing required parameter" in str(e), str(e)
            cases += 1
        for key, prop in props.items():
            if not prop:
                continue  # `any`
            bad = next(v for v in PROBES if not accepts(prop, v))
            e = refused(lambda: raw.call(name, **{**base, key: bad}),
                        'invalid_argument')
            d = e.diagnostic
            assert d['param'] == key and d['path'] == f'/params/{key}', d
            assert d['observed'] == bad and d['operation'] == name, d
            assert f"'{key}' must be" in str(e), str(e)
            cases += 1
            enum = prop.get('enum') or next(
                (a['enum'] for a in prop.get('anyOf', []) if 'enum' in a), None)
            if enum and all(not accepts(a, 'no-such-value')
                            for a in prop.get('anyOf', [prop])):
                e = refused(lambda: raw.call(name, **{**base, key: 'no-such-value'}),
                            'invalid_argument')
                assert set(enum) <= set(e.diagnostic['accepted']), e.diagnostic
                cases += 1
            if 'minimum' in prop:
                e = refused(lambda: raw.call(name, **{**base, key: prop['minimum'] - 1}),
                            'invalid_argument')
                assert '>=' in str(e), str(e)
                cases += 1
    assert world(raw) == before, 'a refused call changed the document'
    print(f'refusals: {cases} cases over {len(typed)} typed methods, '
          f'nothing moved')


def check_uncontracted(raw):
    e = refused(lambda: raw.call('add_relation', typo=1), 'unknown_argument')
    assert e.diagnostic['param'] == 'typo' and 'type' in e.diagnostic['accepted']
    assert "unknown parameter 'typo'" in str(e), str(e)
    e = refused(lambda: raw.call('component_status'), 'missing_argument')
    assert e.diagnostic['operation'] == 'component_status', e.diagnostic
    assert e.diagnostic['param'] == 'namespace', e.diagnostic
    # A batch pre-checks every call's values before the first one runs.
    e = refused(lambda: raw.call('batch', calls=[
        {'method': 'add_part', 'params': {'name': 'never'}},
        {'method': 'add_mate', 'params': {'a': {}, 'b': {}, 'type': 'revolut'}}]),
        'invalid_argument')
    assert e.diagnostic['call'] == 2 and e.diagnostic['param'] == 'type'
    assert 'never' not in [p['name'] for p in raw.call('list_parts')['parts']]
    print('uncontracted: unknown and missing keys are structured; batch pre-checks')


def check_sdk(raw):
    app = App(raw)
    sent = []
    real = raw.call

    def counting(method, **params):
        sent.append(method)
        return real(method, **params)
    raw.call = counting
    try:
        app.add_mate(type='revolut', a={}, b={})
        raise AssertionError('the SDK sent an invalid add_mate')
    except Exception as e:  # designer_contracts.ContractError
        client = e.diagnostic
    finally:
        raw.call = real
    assert not sent, sent
    server = refused(lambda: raw.call('add_mate', type='revolut', a={}, b={}),
                     'invalid_argument').diagnostic
    assert client == server, (client, server)
    print('sdk: preflight refuses with the server\'s diagnostic')


def check_revision_guard(raw):
    raw.call('add_part', name='guarded')
    made = raw.call('add_feature', feature={
        'type': 'Box', 'name': 'Block', 'on': 'Top', 'mode': 'new_body',
        'width': 20, 'depth': 10, 'height': 5})
    state = raw.call('get_state')
    rev = state['revision']
    assert rev['part_id'] == raw.call('list_parts')['active_id'], rev
    snap = raw.call('geometry_snapshot', part_id=rev['part_id'], body=0)
    assert snap['revision'] == rev, (snap['revision'], rev)
    slug = raw.call('describe')['material_slugs'][0]
    body = state['bodies'][0]['created_by']

    out = raw.call('set_material', feature_id=body, material=slug,
                   expect_revision=rev)
    new = out['revision']
    assert new['version'] > rev['version'], (rev, new)
    # The old revision is stale now; the refusal says why and moves nothing.
    before = world(raw)
    e = refused(lambda: raw.call('delete_feature', feature_id=made['id'],
                                 expect_revision=rev), 'stale_revision')
    assert 'regenerated since' in str(e) and e.diagnostic['expected'] == new
    assert world(raw) == before
    # Chained: the answer's revision guards the next write.
    out = raw.call('set_material', feature_id=body, material='',
                   expect_revision=new)
    chained = out['revision']
    # Another part's revision is refused naming the part.
    foreign = dict(chained, part_id=chained['part_id'] + 1000)
    e = refused(lambda: raw.call('set_material', feature_id=body,
                                 material=slug, expect_revision=foreign),
                'stale_revision')
    assert 'names part' in str(e), str(e)
    # A malformed revision is a type refusal, not a stale one.
    refused(lambda: raw.call('set_material', feature_id=body, material=slug,
                             expect_revision={'version': 1}),
            'missing_argument')
    # A batch is guarded once, before its first call.
    before = world(raw)
    refused(lambda: raw.call('batch', expect_revision=rev, calls=[
        {'method': 'set_material',
         'params': {'feature_id': body, 'material': slug}}]),
        'stale_revision')
    assert world(raw) == before
    out = raw.call('batch', expect_revision=chained, calls=[
        {'method': 'set_material',
         'params': {'feature_id': body, 'material': slug}}])
    assert out['revision']['version'] > chained['version'], out['revision']
    # A guard on another kind of tab is refused, naming it.
    raw.call('add_assembly', name='guard asm')
    e = refused(lambda: raw.call('batch', expect_revision=out['revision'],
                                 calls=[{'method': 'list_mates'}]),
                'stale_revision')
    assert 'an assembly' in str(e), str(e)
    print('revision guard: pass, chain, stale, foreign, batch, wrong tab')


def main():
    with tempfile.TemporaryDirectory(prefix='rpc-contracts-') as tmp:
        env = dict(os.environ, XDG_STATE_HOME=tmp,
                   XDG_CONFIG_HOME=tmp + '/config')
        with launch(sys.argv[1], env=env) as running:
            raw = running.designer
            raw.call('new_project')
            raw.call('add_part', name='probe')
            docs = raw.call('describe')
            rows, typed = check_schemas(raw, docs)
            check_refusals(raw, rows, typed)
            check_uncontracted(raw)
            check_sdk(raw)
            check_revision_guard(raw)
    print('rpc contracts smoke: OK')


if __name__ == '__main__':
    main()
