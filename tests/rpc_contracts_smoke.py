#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Typed RPC contracts against a running app (docs/scripting/rpc.md#typed-contracts).

- The server's `request_schema` for every typed or parameterless row equals
  the Python reading of the same row (tools/rpc_contract.py), which is what
  the generated stubs (python/lobole/rpc.py) are built from, and every
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

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from lobole.contracts import accepts  # noqa: E402
import generate_rpc_contracts  # noqa: E402
from lobole import errors, rpc  # noqa: E402
from lobole.errors import (InvalidArgument, MissingArgument,  # noqa: E402
                           Refused, UnknownArgument)
from lobole.rpc import COLUMNS  # noqa: E402
from lobole.sdk import App  # noqa: E402
from method_doc_scan import method_docs  # noqa: E402
from rpc_contract import parse_column, request_schema  # noqa: E402
from smoke_app import launch  # noqa: E402

PROBES = [True, 'text', 1.5, -1, [], {}, None, [True]]


def refused(fn, code):
    """The call is refused with `code`, raised as the class for that code
    (lobole.errors), whose attributes read the diagnostic."""
    try:
        fn()
    except Refused as e:
        assert e.diagnostic and e.diagnostic['code'] == code, (str(e), e.diagnostic)
        assert e.code == code and e.path == e.diagnostic['path'], e
        assert type(e).CODE == code, (type(e).__name__, code)
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


def built_columns(raw):
    """The contracted rows THIS build serves. COLUMNS is read from every
    Remote*.cpp; RemoteCam.cpp's table is compiled out of a CAM-free build
    (-DENABLE_CAM=OFF), which ping's `features` says -- and only those rows
    may be missing from one."""
    if raw.call('ping').get('features', {}).get('cam', True):
        return dict(COLUMNS)
    cam = method_docs([str(ROOT / 'src/remote/RemoteCam.cpp')])
    assert set(cam) & set(COLUMNS), 'RemoteCam.cpp has no contracted row'
    return {k: v for k, v in COLUMNS.items() if k not in cam}


def check_schemas(raw, docs):
    vocab = {k: v for k, v in docs.items()
             if isinstance(v, list) and all(isinstance(x, str) for x in v)}
    rows = {m['name']: m for m in docs['methods']}
    inspection = {'geometry_snapshot', 'validate_snapshot'}
    server = {n for n, m in rows.items() if 'request_schema' in m} - inspection
    columns = built_columns(raw)
    assert server == set(columns), (server ^ set(columns))
    def plain(s):
        # A vocabulary's remedy (`description`) is prose the server adds.
        if isinstance(s, dict):
            return {k: plain(v) for k, v in s.items() if k != 'description'}
        if isinstance(s, list):
            return [plain(v) for v in s]
        return s
    for name, column in columns.items():
        mine = request_schema(parse_column(column), vocab)
        theirs = plain(rows[name]['request_schema'])
        assert theirs == mine, (name, mine, theirs)
        assert rows[name]['params'] == [p['name'] for p in parse_column(column)]
    typed = [n for n, c in columns.items() if parse_column(c)]
    print(f'schemas: {len(columns)} contracted rows ({len(typed)} typed) '
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
        {'method': 'set_variable', 'params': {'name': 'never',
                                              'expression': '1'}},
        {'method': 'add_mate', 'params': {'a': {}, 'b': {}, 'type': 'revolut'}}]),
        'invalid_argument')
    assert e.diagnostic['call'] == 2 and e.diagnostic['param'] == 'type'
    assert 'never' not in [v['name'] for v in
                           raw.call('list_variables')['variables']]
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
    except Refused as e:  # the client-side preflight
        client = e
    finally:
        raw.call = real
    assert not sent, sent
    server = refused(lambda: raw.call('add_mate', type='revolut', a={}, b={}),
                     'invalid_argument')
    assert client.diagnostic == server.diagnostic, (client.diagnostic,
                                                    server.diagnostic)
    # The same CLASS too: a caller catching InvalidArgument cannot tell which
    # side refused.
    assert type(client) is type(server) is InvalidArgument, (
        type(client), type(server))
    # And each preflight code raises its class, as the server's would.
    for params, cls in ((dict(a={}, b={}, typo=1), UnknownArgument),
                        (dict(b={}), MissingArgument)):
        try:
            app.add_mate(**params)
            raise AssertionError(f'the SDK sent {params}')
        except cls as e:
            assert e.code == cls.CODE, e.diagnostic
    print('sdk: preflight refuses with the server\'s diagnostic and class')


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


def check_error_classes():
    """Every code the app sends that has a class gets it; an unknown code, or
    none, is still a Refused, so a newer app raises something catchable."""
    for cls in (MissingArgument, UnknownArgument, InvalidArgument,
                errors.StaleRevision, errors.StaleSnapshot,
                errors.BudgetExceeded, errors.InternalError,
                errors.DeviceLost):
        e = errors.refusal('x', {'code': cls.CODE})
        assert type(e) is cls and isinstance(e, errors.LoboleError), cls
    for diagnostic in ({'code': 'a_code_from_a_newer_app'}, {}, None):
        assert type(errors.refusal('x', diagnostic)) is Refused, diagnostic
    # A dropped connection is ours AND the builtin a retry loop catches.
    assert issubclass(errors.ConnectionLost, ConnectionError)
    assert not issubclass(errors.NonFiniteError, Refused)  # the call RAN
    print('errors: one class per code, Refused for the rest')


def _sample(schema):
    """A value `schema` accepts, for the keys a fault-free spec needs."""
    if "anyOf" in schema:
        return _sample(schema["anyOf"][0])
    if "enum" in schema:
        return schema["enum"][0]
    t = schema.get("type")
    return {"integer": 1, "number": 1.0, "boolean": True, "string": "x",
            "object": {}, "array": [], "null": None}.get(t, 1)


def _wrong(schema):
    """A value `schema` refuses: a type it does not take."""
    t = schema.get("type")
    return [1] if t != "array" else "not-a-list"


def check_feature_contracts(raw):
    """Every feature type's contract
    (docs/scripting/rpc.md#typed-feature-specs): describe publishes the
    column the generator types the SDK from,
    and the server refuses an unknown field, a wrong type, an enum miss and
    a missing required field by its path under /params/feature, with the
    class for the code, the diagnostic the SDK's preflight gives, and
    nothing built."""
    app = App(raw)
    types = {t["type"]: t for t in raw.call("describe")["feature_types"]}
    expected = generate_rpc_contracts.feature_contracts()
    assert set(types) == set(expected), set(types) ^ set(expected)
    before = len(raw.call("get_state")["features"])
    faults = 0
    for name, row in types.items():
        assert row["contract"] == expected[name], (name, row["contract"])
        schema = rpc.FEATURE_SCHEMAS[name]
        props, required = schema["properties"], schema["required"]
        base = {k: _sample(props[k]) for k in required}
        cases = [("unknown_argument", dict(base, zz_not_a_field=1))]
        for k, sub in props.items():
            if k == "type":
                continue
            if "enum" in sub:
                cases.append(("invalid_argument", dict(base, **{k: "zz"})))
            elif sub.get("type") in ("integer", "number", "boolean",
                                     "string", "array"):
                cases.append(("invalid_argument",
                              dict(base, **{k: _wrong(sub)})))
        for k in required:
            if k != "type":
                cases.append(("missing_argument",
                              {x: v for x, v in base.items() if x != k}))
        for code, spec in cases:
            server = refused(lambda: raw.call("add_feature", feature=spec),
                             code)
            assert server.path.startswith("/params/feature"), server.path
            try:
                app.add_feature(feature=spec)
                raise AssertionError(f"the SDK sent {spec}")
            except Refused as client:
                assert type(client) is type(server), (type(client), server)
                assert client.diagnostic == server.diagnostic, (
                    client.diagnostic, server.diagnostic)
            faults += 1
    assert len(raw.call("get_state")["features"]) == before
    print(f"feature contracts: {len(types)} types, {faults} faults refused "
          "alike by the server and the SDK, nothing built")


def main():
    check_error_classes()
    with tempfile.TemporaryDirectory(prefix='rpc-contracts-') as tmp:
        env = dict(os.environ, XDG_STATE_HOME=tmp,
                   XDG_CONFIG_HOME=tmp + '/config')
        with launch(sys.argv[1], env=env) as running:
            raw = running.client
            raw.call('new_project')
            raw.call('add_part', name='probe')
            docs = raw.call('describe')
            rows, typed = check_schemas(raw, docs)
            check_refusals(raw, rows, typed)
            check_uncontracted(raw)
            check_sdk(raw)
            check_feature_contracts(raw)
            check_revision_guard(raw)
    print('rpc contracts smoke: OK')


if __name__ == '__main__':
    main()
