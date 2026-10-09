#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Catalog import, pinned SDK references, privacy, persistence and undo."""
import json
import os
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from designer_sdk import App
from smoke_app import launch


def refused(fn, text):
    try:
        fn()
    except RuntimeError as e:
        assert text in str(e), str(e)
    else:
        raise AssertionError("expected refusal: " + text)


def main(binary):
    with tempfile.TemporaryDirectory(prefix="catalog-test-") as temp, launch(binary) as running:
        app = App(running.designer)
        app.new()
        part = app.part("test block")
        part.box("Top", width=10, depth=10, height=10, at=(0, 0), centered=False)
        file = Path(temp) / "bearings.csv"
        original = "designation,bore_diameter,outer_diameter,width,shaft_min,shaft_max,notes\nB-25,25,52,15,24.99,25.01,not a verified rule\n"
        file.write_text(original)
        options = dict(id="synthetic", revision="1", source="synthetic fixture", license="CC0-1.0", units="mm")
        plan = app.catalogs.preview_file(file, **options)
        assert app.list_catalogs()["catalogs"] == []
        assert plan.summary["sample"][0]["unsupported_fields"] == ["notes"]
        file.write_text(original.replace(",15,", ",16,"))
        refused(plan.install, "changed after preview")
        file.write_text(original)
        c = plan.install()
        assert c.find("B-25").fields["bore_diameter"] == 25
        assert len(list(c.records(page_size=1))) == 1
        r = c.find("B-25").check_seat(shaft_diameter=25)
        assert r["status"] == "unverified" and r["checks"][0]["status"] == "pass"
        assert c.find("B-25").check_seat(shaft_diameter=26)["status"] == "fail"
        refused(lambda: c.find("absent"), "not found")
        app.undo()
        assert not app.list_catalogs()["catalogs"]
        app.redo()
        assert len(app.list_catalogs()["catalogs"]) == 1
        # A repeat is not another undo step.
        app.catalogs.import_file(file, **options)
        app.undo()
        assert not app.list_catalogs()["catalogs"]
        app.redo()
        reference_export = Path(temp) / "reference.json"
        c.export(reference_export, mode="reference")
        assert '"records"' not in reference_export.read_text()
        refused(lambda: c.export(Path(temp)/"blocked.json",mode="embedded"), "embedded export requires")
        assert not (Path(temp)/"blocked.json").exists()
        # Flat single-part document persists references without private rows.
        flat = str(Path(temp) / "single.dsgn")
        app.save_project(path=flat)
        # A suite run keeps its "recently opened" to ITSELF: the list lives in
        # the state directory, and seven temp projects from one afternoon's
        # runs turned up in the developer's File menu before smoke_app gave
        # each run its own (the autosave recovery copy shares that directory).
        recents = (Path(running.tmp) / "state" / "lobole"
                   / "recent-projects.json")
        assert recents.exists(), f"the recents went somewhere else: {recents}"
        assert "single.dsgn" in recents.read_text()
        app.new()
        app.open_project(path=flat)
        assert app.list_catalogs()["catalogs"][0]["resolved"] is False
        refused(lambda: c.find("B-25"), "unresolved")
        c = app.catalogs.import_file(file, **options)
        assert c.find("B-25").fields["width"] == 15
        file.write_text(original.replace(",15,", ",16,"))
        refused(lambda: app.catalogs.import_file(file, **options), "revision content changed")
        # New revision retains the old pinned records.
        v2 = app.catalogs.import_file(file, **dict(options, revision="2", storage="embedded"))
        assert c.find("B-25").fields["width"] == 15
        assert v2.find("B-25").fields["width"] == 16
        # Project catalogs are accessible and undoable on an assembly tab.
        app.assembly("assembly")
        app.undo()
        assert len(app.list_catalogs()["catalogs"]) == 1
        app.redo()
        multi = str(Path(temp) / "multi.dsgn")
        app.save_project(path=multi)
        index = json.loads((Path(multi)/"project.json").read_text())
        assert "records" not in index["catalogs"][0]
        assert index["catalogs"][1]["records"][0]["width"] == 16
        app.new()
        app.open_project(path=multi)
        v2 = app.catalogs.open("synthetic","2")
        assert v2.find("B-25").fields["width"] == 16
        package = Path(temp) / "embedded.json"
        v2.export(package,mode="embedded")
        assert json.loads(package.read_text())["catalogs"][0]["records"]
        # A stale handle must not query different content after replacement.
        refused(lambda: app.query_catalog(catalog="synthetic",revision="2",hash="bad",designation="B-25"), "stale")
        app.new()
        restored = app.catalogs.import_package(package, storage="embedded")
        assert restored.find("B-25").fields["width"] == 16
        reference = app.catalogs.import_package(reference_export)
        refused(lambda: reference.find("B-25"), "unresolved")
        print("catalog_smoke: passed")


if __name__ == "__main__":
    main(sys.argv[1])
