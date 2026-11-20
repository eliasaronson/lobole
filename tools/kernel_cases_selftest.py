#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""`tools/kernel_cases.py self-test`: the translator, the grader and the
ratchet against planted recipes and answers, with no instance.

A planted recipe per row of the command table
(docs/modeling/README.md#kernel-cases) must translate to the calls written
beside it in tests/kernel_cases/selftest.json; an unknown command, an
unknown parameter and an ambiguous pick must each come out untranslated,
naming it; and planted answers (a pass, a volume off in the eighth digit, a
refusal that changed the part, a known case that passes, an edited vendored
file) must each be judged.
"""
import copy
import decimal
import json
import os
import shutil
import sys
import tempfile

import kernel_cases as kc
import kernel_cases_translate as kt

PLANTED = os.path.join(kc.CORPUS, "selftest.json")


class Fake:
    """Answers as an instance would, as far as the translator reads: ids
    in order, the bodies a feature leaves, one planar face or edge at any
    point unless the point is planted as ambiguous."""

    def __init__(self, ambiguous=()):
        self.next = 1
        self.bodies = []
        self.ambiguous = [list(p) for p in ambiguous]

    def call(self, method, **params):
        return self.request(method, params)

    def request(self, method, params=None):
        params = params or {}
        if method == "query_geometry":
            at = params.get("on_point")
            count = 2 if at in self.ambiguous else 1
            match = {"created_by": self.bodies[-1] if self.bodies else 0,
                     "element": f"{params['element_type']}@{at}",
                     "kind": "plane", "normal": [0.0, 0.0, 1.0]}
            return {"count": count, "ok": count == 1,
                    "matches": [match] * count}
        if method == "list_elements":
            return {"bodies": []}
        if method != "add_feature":
            raise AssertionError(f"the translator called {method}")
        f = params["feature"]
        fid, self.next = self.next, self.next + 1
        if f["type"] == "Boolean":
            self.bodies = [b for b in self.bodies
                           if b not in (f["target_id"], f["tool_id"])]
            self.bodies.append(fid)
        elif f.get("mode") == "new_body":
            self.bodies.append(fid)
        elif f["type"] in ("Split", "Pattern") and \
                f.get("source_id", f.get("body_id")) in self.bodies:
            self.bodies[self.bodies.index(
                f.get("source_id", f.get("body_id")))] = fid
        return {"id": fid, "regen": {"ok": True, "failed": []},
                "digest": {"bodies": [
                    {"created_by": b, "solids": 1, "volume_mm3": 1.0,
                     "bbox_min": [0, 0, 0], "bbox_max": [1, 1, 1]}
                    for b in self.bodies]}}


def translate(commands, ambiguous=()):
    """The features a recipe becomes, or the Untranslated reason."""
    tr = kt.Translator(Fake(ambiguous))
    try:
        for c in commands:
            tr.command(c)
    except kt.Untranslated as e:
        return f"untranslated: {e}"
    return [step["params"]["feature"] for step in tr.log]


def stated(text):
    return kc.Stated(decimal.Decimal(text))


def main():
    failures = []

    def check(what, ok, detail=""):
        if not ok:
            failures.append(f"{what}: {detail}")

    with open(PLANTED) as f:
        planted = json.load(f)
    if "--print" in sys.argv:  # what the rows translate to, to review
        for row in planted["rows"]:
            row["calls"] = translate(row["recipe"])
        with open(PLANTED, "w") as f:
            f.write(kc.compact(planted))
        return 0

    # 1. A recipe per row of the command table.
    seen = set()
    for row in planted["rows"]:
        got = translate(row["recipe"])
        check(f"row {row['row']!r}", got == row["calls"],
              f"translated to\n    {json.dumps(got)}\n  not\n    "
              f"{json.dumps(row['calls'])}")
        seen.update(c["command"] for c in row["recipe"])
    table = {n[2:].replace("_", ".") for n in dir(kt.Translator)
             if n.startswith("c_")}
    names = {c.replace("_", ".") for c in seen}
    check("every translated command has a planted recipe",
          table <= names, f"none for {sorted(table - names)}")

    # 2. What has no counterpart is named, never approximated.
    box = {"command": "solid.box",
           "params": {"length": 60, "width": 40, "height": 20}}
    for what, commands, ambiguous, want in (
        ("an unknown command",
         [box, {"command": "solid.emboss", "params": {}}], (),
         "untranslated: `solid.emboss`"),
        ("an unknown parameter",
         [box, {"command": "solid.fillet", "params": {
             "edges": [[30, 0, 20]], "radius": 5, "radius2": 1}}], (),
         "untranslated: `solid.fillet` with `radius2`"),
        ("an ambiguous pick",
         [box, {"command": "solid.fillet", "params": {
             "edges": [[30, 0, 20]], "radius": 5}}], [[30.0, 0.0, 20.0]],
         "untranslated: ambiguous pick: several edges at the point"),
    ):
        got = translate(commands, ambiguous)
        check(what, got == want, f"came out {got!r}, want {want!r}")

    # 3. Planted answers.
    exp = {"body_count": stated("1"), "volume_mm3": stated("9424.777960769"),
           "faces": stated("3")}
    right = {"body_count": 1, "volume": 9424.77796076938, "faces": 3}
    status, _, _ = kc.judge(exp, {"refusal": None, "measured": right})
    check("a right answer", status == "pass", status)
    off = dict(right, volume=9424.7779 + 1e-4)  # the eighth digit
    status, detail, _ = kc.judge(exp, {"refusal": None, "measured": off})
    check("a volume off in the eighth digit",
          status == "fail: volume" and "volume_mm3" in detail, status)
    status, _, _ = kc.judge(exp, {"refusal": None, "measured":
                                  dict(right, faces=4)})
    check("a right volume on one face too many", status == "fail: topology",
          status)
    status, _, _ = kc.judge(exp, {"refusal": None, "measured": right,
                                  "invalid": "body 'B': 3 free edge(s)"})
    check("an open body", status == "fail: invalid", status)
    said = "the round of radius 5 mm does not fit the 4 mm wall"
    refusal = {"message": said, "internal": False, "changed": False}
    status, _, _ = kc.judge(exp, {"refusal": refusal})
    check("a refusal of a case that has an answer",
          status == "fail: refused", status)
    must = dict(exp, outcome="refused", refusal="does not fit")
    status, _, _ = kc.judge(must, {"refusal": refusal})
    check("a refusal of a case that must be refused", status == "pass",
          status)
    status, _, _ = kc.judge(must, {"refusal": dict(refusal, changed=True)})
    check("a refusal that changed the part", status == "fail: invalid",
          status)
    status, _, _ = kc.judge(must, {"refusal": dict(refusal,
                                                   message="failed:")})
    check("a refusal that states no condition", status == "fail: internal",
          status)
    status, _, _ = kc.judge(must, {"refusal": None, "measured": right})
    check("a case that must be refused and built",
          status == "fail: accepted", status)
    rng = dict(exp, volume_mm3=[stated("9000"), stated("9424.7")])
    status, _, _ = kc.judge(rng, {"refusal": None, "measured": right})
    check("a volume past its range's end", status == "fail: volume", status)

    # 4. The ratchet.
    entry = "docs/known-bugs.md#planted"
    rows = {"a": "pass", "b": f"known: volume ({entry})",
            "c": "untranslated: `solid.emboss`"}

    def ran(**got):
        return {k: {"status": v, "detail": "", "seconds": 0.0, "rows": []}
                for k, v in got.items()}

    check("a run that matches its rows", not kc.ratchet(rows, ran(
        a="pass", b="fail: volume", c="untranslated: `solid.emboss`")))
    for what, got, word in (
        ("a pass that fails", {"a": "fail: invalid"}, "was pass"),
        ("a known case that passes", {"b": "pass"}, "PASSES"),
        ("a known case failing another way", {"b": "fail: crash"},
         "known as volume"),
        ("an untranslated case that translates", {"c": "pass"},
         "now translates"),
        ("a case with no row", {"d": "pass"}, "no row"),
    ):
        bad = kc.ratchet(rows, ran(**got))
        check(what, len(bad) == 1 and word in bad[0], str(bad))
    check("a vendored case missing from status.json",
          len(kc.check_rows(rows, ["a", "b", "c", "d"])) == 1)

    # 5. An edited vendored file.
    real = (kc.CORPUS, kc.SOURCE)
    scratch = tempfile.mkdtemp(prefix="kernel_cases_selftest_")
    try:
        case = os.path.join("solidcraft", "ladder", "01-primitives",
                            "001-box")
        os.makedirs(os.path.join(scratch, case))
        os.makedirs(os.path.join(scratch, "solidcore"))
        src = {"files": {}}
        for name in ("recipe.json", "expected.json"):
            shutil.copyfile(os.path.join(real[0], case, name),
                            os.path.join(scratch, case, name))
            src["files"][os.path.join(case, name)] = kc.sha256(
                os.path.join(scratch, case, name))
        kc.CORPUS, kc.SOURCE = scratch, os.path.join(scratch, "SOURCE")
        with open(kc.SOURCE, "w") as f:
            json.dump(src, f)
        kc.check_source()
        with open(os.path.join(scratch, case, "expected.json"), "a") as f:
            f.write(" ")
        try:
            kc.check_source()
            check("an edited vendored file", False, "was accepted")
        except kc.CorpusError as e:
            check("an edited vendored file", "edited" in str(e), str(e))
    finally:
        kc.CORPUS, kc.SOURCE = real
        shutil.rmtree(scratch, ignore_errors=True)

    # 6. The mesh check: a closed tetrahedron, and one with a face gone.
    import struct
    v = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1)]
    tris = [(0, 2, 1), (0, 1, 3), (1, 2, 3), (0, 3, 2)]
    for what, faces, want in (("a closed mesh", tris, (True, 1)),
                              ("a mesh with a face gone", tris[:3],
                               (False, 1))):
        fd, path = tempfile.mkstemp(suffix=".stl")
        with os.fdopen(fd, "wb") as f:
            f.write(b"\0" * 80 + struct.pack("<I", len(faces)))
            for t in faces:
                f.write(struct.pack("<12fH", 0, 0, 0,
                                    *[c for i in t for c in v[i]], 0))
        got = kc.stl_watertight(path, 1e-4)
        os.remove(path)
        check(what, got == want, str(got))

    for line in failures:
        print("FAIL " + line)
    print(f"kernel_cases self-test: {'FAILED' if failures else 'ok'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
