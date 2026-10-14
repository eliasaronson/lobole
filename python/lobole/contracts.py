# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Runtime validation of the documented contract JSON Schema subset.

The client-side twin of ValidateInspectionValue (src/remote/InspectionContracts.h):
the same walk in the same order, building the same diagnostic, so a preflight
refusal and the server's name the same fault field for field
(docs/scripting/rpc.md#structured-errors).
"""
import json
import math

from lobole.errors import Refused, refusal


def _accepted(schema):
    out = list(schema.get('enum', []))
    for alt in schema.get('anyOf', []):
        out += alt.get('enum', [])
    return out


def _num(v):
    return str(int(v)) if float(v).is_integer() and abs(v) < 1e18 else repr(v)


def describe(schema):
    """A schema in words, as the server's DescribeSchema says it."""
    if 'anyOf' in schema:
        return ' or '.join(describe(a) for a in schema['anyOf'])
    if 'enum' in schema:
        vals = [v if isinstance(v, str) else json.dumps(v) for v in schema['enum']]
        if len(vals) == 1:
            return f'"{vals[0]}"'
        more = f', +{len(vals) - 12} more' if len(vals) > 12 else ''
        return 'one of: ' + ', '.join(vals[:12]) + more
    if 'type' not in schema:
        return 'any value'
    t = schema['type']
    s = dict(integer='an integer', number='a number', string='a string',
             boolean='true or false', object='an object', null='null',
             array='an array').get(t, t)
    for key, op in (('minimum', '>='), ('exclusiveMinimum', '>'),
                    ('maximum', '<='), ('exclusiveMaximum', '<')):
        if key in schema:
            s += f' {op} {_num(schema[key])}'
    if 'minLength' in schema:
        s += f' of at least {_num(schema["minLength"])} character(s)'
    if t == 'object' and 'properties' in schema:
        s += ' {' + ', '.join(schema['properties']) + '}'
    if t == 'array':
        lo, hi = 'minItems' in schema, 'maxItems' in schema
        if lo and hi and schema['minItems'] == schema['maxItems']:
            s += f' of {_num(schema["minItems"])}'
        elif lo or hi:
            s += (f' of {_num(schema["minItems"]) if lo else "0"}..'
                  f'{_num(schema["maxItems"]) if hi else "any"}')
        if 'items' in schema:
            s += (' × ' if lo or hi else ' of ') + describe(schema['items'])
        if schema.get('uniqueItems'):
            s += ', no repeats'
    return s


def contract_refusal(operation, code, path, expected, observed, message=None,
                     accepted=None):
    """A client preflight refusal: the Refused subclass the server would
    raise for `code`, with the diagnostic it would send (lobole.errors)."""
    diagnostic = dict(operation=operation, code=code, path=path,
                      expected=expected, observed=observed)
    if path.startswith('/params/'):
        diagnostic['param'] = path[len('/params/'):].split('/')[0]
    if accepted:
        diagnostic['accepted'] = accepted
    return refusal(message or
                   f'{operation}: {code} at {path}; expected {expected}',
                   diagnostic)


def _where(path):
    return 'its params' if path == '/params' else f"'{path[len('/params/'):]}'"


def _shown(value):
    try:
        s = json.dumps(value)
    except (TypeError, ValueError):
        s = repr(value)
    return s if len(s) <= 80 else s[:77] + '...'


def _is_number(value):
    return (isinstance(value, (int, float)) and not isinstance(value, bool)
            and (isinstance(value, int) or math.isfinite(value)))


def accepts(schema, value):
    try:
        validate(schema, value, '', '/params')
        return True
    except Refused:
        return False


def validate(schema, value, operation, path='/params'):
    def fail(expected=schema):
        # A vocabulary's remedy for the usual mistake rides as `description`.
        hint = expected.get('description') or next(
            (a['description'] for a in expected.get('anyOf', [])
             if a.get('description')), '')
        raise contract_refusal(
            operation, 'invalid_argument', path, expected, value,
            f'{operation}: {_where(path)} must be {describe(expected)}; '
            f'got {_shown(value)}' + (f'. {hint}' if hint else ''),
            _accepted(expected))

    if 'anyOf' in schema:
        if any(accepts(alt, value) for alt in schema['anyOf']):
            return
        fail()
    number = _is_number(value)
    if 'type' in schema:
        kind = schema['type']
        valid = {'object': isinstance(value, dict),
                 'array': isinstance(value, (list, tuple)),
                 'string': isinstance(value, str),
                 'boolean': isinstance(value, bool),
                 'null': value is None,
                 'number': number,
                 'integer': number and (isinstance(value, int) or value.is_integer())}
        if not valid[kind]:
            fail()
    if 'enum' in schema and value not in schema['enum']:
        fail()
    if number:
        if 'minimum' in schema and value < schema['minimum']:
            fail()
        if 'maximum' in schema and value > schema['maximum']:
            fail()
        if 'exclusiveMinimum' in schema and value <= schema['exclusiveMinimum']:
            fail()
        if 'exclusiveMaximum' in schema and value >= schema['exclusiveMaximum']:
            fail()
    if isinstance(value, str) and len(value) < schema.get('minLength', 0):
        fail()
    if isinstance(value, dict) and 'properties' in schema:
        props = schema['properties']
        # Sorted: the server's objects iterate in key order. Unknown keys
        # first, then missing ones, then each value -- the server's order.
        for key in sorted(value):
            # An open object (a result's `{..., *}`) carries keys it does
            # not list. Every request schema closes its objects, so this is
            # still the server's walk for everything the server validates.
            if key not in props and \
                    schema.get('additionalProperties', True) is False:
                at = path + '/' + key
                raise contract_refusal(
                    operation, 'unknown_argument', at, props, value[key],
                    f'{operation}: unknown parameter {_where(at)}; it takes: '
                    + ', '.join(sorted(props)), sorted(props))
        for key in schema.get('required', []):
            if key not in value:
                at = path + '/' + key
                raise contract_refusal(
                    operation, 'missing_argument', at, props[key], None,
                    f'{operation}: missing required parameter {_where(at)} '
                    f'({describe(props[key])})')
        for key in sorted(value):
            if key in props:
                validate(props[key], value[key], operation, path + '/' + key)
    if isinstance(value, (list, tuple)):
        if len(value) < schema.get('minItems', 0):
            fail()
        if 'maxItems' in schema and len(value) > schema['maxItems']:
            fail()
        for i, item in enumerate(value):
            if 'items' in schema:
                validate(schema['items'], item, operation, path + '/' + str(i))
            if schema.get('uniqueItems') and item in value[:i]:
                fail()
