#!/usr/bin/env python3
"""Ownership, staged regeneration, stable identity, undo and stale-plan gates."""
import os
import sys
import tempfile
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'tools'))
from designer_sdk import App, SdkError
from designer_remote import Designer
from designer_components import claim, plan
from designer_validation import validate
from smoke_app import launch_app


def refused(fn):
    try:
        fn()
    except (RuntimeError, SdkError):
        return
    raise AssertionError('unsafe update accepted')


def main():
    with tempfile.TemporaryDirectory(prefix='designer_components_') as tmp:
        env = dict(os.environ, XDG_STATE_HOME=tmp, XDG_CONFIG_HOME=tmp+'/config')
        proc = launch_app(sys.argv[1], env=env)
        try:
            app = App(Designer.wait_for(proc.port))
            app.new()
            def builder(width):
                def build(a):
                    p = a.part('housing')
                    p.var('width', width)
                    p.box(w='width', h=3, d=4, name='Casting')
                    return p
                return build
            part = builder(2)(app)
            app.delete_part(part='Part 1')
            claim(part, namespace='test', key='housing', fingerprint='v1')
            preview = validate(app)
            assert preview.data['status'] == 'passed', preview.data
            assert preview.data['standing']['status'] == 'not_checked'
            assert not preview.data['accepted']
            ids = [f['id'] for f in part.state()['features']]
            rig = app.assembly('rig')
            placed = rig.place(part, fixed=True)
            def never(a):
                raise AssertionError('unchanged builder called')
            def update(fp, build):
                return plan(app, namespace='test', key='housing',
                            fingerprint=fp, builder=build)
            with update('v1', never) as staged:
                assert staged.report['action'] == 'reuse'
                assert staged.apply().id == part.id
            local = validate(app, stage='local', assembly=rig)
            assert local.data['status'] == 'passed', local.data
            assert local.data['motion']['status'] == 'not_checked'
            assert not local.data['accepted']
            full = validate(app, stage='full', assembly=rig)
            assert full.data['status'] == 'incomplete', full.data
            assert not full.data['accepted']
            overlap = rig.place(part, fixed=True)
            partial = validate(app, stage='local', assembly=rig, max_pairs=0)
            assert partial.data['status'] == 'incomplete', partial.data
            assert not partial.data['accepted']
            clash = validate(app, stage='local', assembly=rig)
            assert clash.data['status'] == 'failed', clash.data
            assert not clash.data['accepted']
            rig.activate()
            app.remove_occurrence(id=overlap.id)
            with update('v2', builder(5)) as staged:
                assert abs(part.volume()-24) < 1e-6
                assert staged.report['action'] == 'update'
                staged.apply()
                assert staged.report['regen']['executed'] == 0, staged.report
                assert staged.report['geometry_reused'], staged.report
            assert preview.current(app)['status'] == 'stale'
            assert abs(part.volume()-60) < 1e-6
            assert [f['id'] for f in part.state()['features']] == ids
            rig.activate()
            assert rig.occurrences()[0].id == placed.id
            part.activate()
            app.undo()
            assert abs(part.volume()-24) < 1e-6
            assert app.component_status(part_id=part.id)['generator']['fingerprint'] == 'v1'
            app.redo()
            assert abs(part.volume()-60) < 1e-6
            with update('v3', builder(7)) as staged:
                part.var('width', 6)
                refused(staged.apply)
            assert abs(part.volume()-72) < 1e-6
            refused(lambda: update('v3', builder(7)))
            part.activate()
            app.undo()
            assert app.component_status(part_id=part.id)['state'] == 'owned'
            before = len(app.parts())
            refused(lambda: update('v3', lambda a: a.part('wrong')))
            assert len(app.parts()) == before
            path = tmp+'/owned.dsgn'
            app.save_project(path=path)
            app.open_project(path=path)
            with update('v2', never) as staged:
                assert staged.apply().id == part.id
            print('component updates: reuse, staging, identity, baked apply, undo, stale refusal, persistence passed')
        finally:
            proc.terminate()
            proc.wait(timeout=10)
    return 0

if __name__ == '__main__':
    sys.exit(main())
