# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Explicit iteration stages over the application's existing exact checks.

Reports describe a session revision, not a permanent certificate. Local currently
uses the full standing sweep: a conservative fallback until dependency-scoped
neighbor tracking can prove its coverage. Full includes every defined motion
study on the chosen assembly; it is not continuous collision detection.
"""
from dataclasses import dataclass
from typing import Literal
from lobole.sdk import Part, SdkError

Stage = Literal['preview', 'local', 'full']


@dataclass
class ValidationReport:
    data: dict

    def current(self, app):
        """Return a fresh status without mutating the historical evidence."""
        out = dict(self.data)
        if app.validation_revision() != out['revision']:
            out.update(status='stale', accepted=False)
        return out


def validate(app, *, stage: Stage = 'preview', assembly=None,
             max_pairs=20000, deadline_s=900) -> ValidationReport:
    if stage not in ('preview', 'local', 'full'):
        raise SdkError(f'unknown validation stage: {stage}')
    if stage != 'preview' and assembly is None:
        raise SdkError('local/full validation requires an explicit assembly')
    if assembly is not None:
        solved = assembly.solve()
        if solved['errored_mates'] or not solved.get('loops_closed', True):
            raise SdkError('assembly does not solve; resolve mates before validation')
    before = app.validation_revision()
    out = dict(stage=stage, revision=before, status='incomplete', accepted=False,
               geometry=dict(status='not_checked'),
               standing=dict(status='not_checked'), motion=dict(status='not_checked'))
    original = app.active()
    try:
        checks = []
        for slot in app.parts():
            part = Part(app, slot).activate()
            result = app.check_geometry()
            checks.append(dict(part_id=part.id, name=part.name, result=result))
        good = bool(checks) and not before['missing_references'] and all(
            c['result']['ok'] and c['result']['body_count'] > 0 and
            not c['result'].get('stale_derived') for c in checks)
        out['geometry'] = dict(status='passed' if good else 'failed', parts=checks,
                               scope='all_project_parts')
        if not good:
            out['status'] = 'failed'
            return ValidationReport(out)
        if stage != 'preview':
            assembly.activate()
            sweep = app.interference(mode='exact', max_pairs=max_pairs,
                                     deadline_s=deadline_s, _timeout=deadline_s+60)
            status = ('incomplete' if sweep.get('truncated') else
                      'failed' if sweep['clash'] else 'passed')
            out['standing'] = dict(status=status, scope='whole_visible_assembly',
                                    report=sweep)
            if status != 'passed':
                out['status'] = status
                return ValidationReport(out)
        if stage == 'full':
            studies = app.list_motion_studies()['studies']
            runs = []
            for study in studies:
                if study['clash_mode'] != 'exact' or not study['check_clash']:
                    raise SdkError('full validation requires exact collision-enabled studies; '
                                   'configure the study explicitly before running')
                r = app.run_motion_study(study=study['id'], _timeout=1800)
                runs.append(dict(study=study['id'], result=r))
            status = ('not_checked' if not runs else 'passed' if all(
                r['result']['collision_status'] == 'passed' for r in runs) else 'failed')
            out['motion'] = dict(status=status, scope='defined_samples', studies=runs)
        out['status'] = ('passed' if stage != 'full' or out['motion']['status'] == 'passed'
                         else 'incomplete' if out['motion']['status'] == 'not_checked' else 'failed')
        out['accepted'] = stage == 'full' and out['status'] == 'passed'
        return ValidationReport(out)
    finally:
        if app.validation_revision() != before:
            out.update(status='stale', accepted=False)
        app.set_active_part(part=original)
