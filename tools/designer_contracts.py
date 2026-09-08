"""Runtime validation of the documented inspection JSON Schema subset."""
import math


class ContractError(RuntimeError):
    """Client preflight failure; diagnostic has the same shape as server errors."""
    def __init__(self, operation, code, path, expected, observed):
        self.diagnostic = dict(operation=operation, code=code, path=path,
                               expected=expected, observed=observed)
        super().__init__(f'{operation}: {code} at {path}; expected {expected}')


def validate(schema, value, operation, path='/params'):
    def fail(expected=schema, code='invalid_argument', at=path, observed=value):
        raise ContractError(operation, code, at, expected, observed)

    kind = schema['type']
    number = (isinstance(value, (int, float)) and not isinstance(value, bool)
              and (isinstance(value, int) or math.isfinite(value)))
    valid = {'object': isinstance(value, dict),
             'array': isinstance(value, (list, tuple)),
             'string': isinstance(value, str),
             'boolean': isinstance(value, bool),
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
    if kind == 'string' and len(value) < schema.get('minLength', 0):
        fail()
    if kind == 'object':
        props = schema['properties']
        for key in schema.get('required', []):
            if key not in value:
                fail(props[key], 'missing_argument', path + '/' + key, None)
        for key, item in value.items():
            if key not in props:
                fail(props, 'unknown_argument', path + '/' + key, item)
            validate(props[key], item, operation, path + '/' + key)
    if kind == 'array':
        if len(value) < schema.get('minItems', 0):
            fail()
        if 'maxItems' in schema and len(value) > schema['maxItems']:
            fail()
        for i, item in enumerate(value):
            validate(schema['items'], item, operation, path + '/' + str(i))
            if schema.get('uniqueItems') and item in value[:i]:
                fail()
