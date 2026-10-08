# Engineering catalogs

Loböle accepts local CSV and JSON bearing datasets. Import validates and
normalizes reference data; it does not create geometry, choose a fit, or certify
manufacturability. Vendor APIs, PDF extraction and cutting-tool datasets are
future adapters, not supported input formats today.

## Try it

`examples/catalogs/synthetic-bearings.csv` contains invented demonstration values,
released as CC0-1.0. They are not vendor specifications or design recommendations.

In the **Variables** panel, expand **Engineering catalogs → Import bearing data**.
Enter the file path, catalog name, revision, source, usage terms and input units.
Preview the rows, then install the previewed revision. Column mapping is optional
when the file already uses the canonical names below. Installed records can be
browsed and checked against manually entered seat dimensions.

The same workflow is available through the Python SDK:

```python
from designer_remote import Designer
from designer_sdk import App

app = App(Designer(8852))  # your own running instance
preview = app.catalogs.preview_file(
    "examples/catalogs/synthetic-bearings.csv",
    id="demo-bearings", revision="1",
    source="Loböle synthetic example", license="CC0-1.0",
    units="mm", storage="reference",
)
print(preview.summary)
bearings = preview.install()
record = bearings.find("DEMO-25")
print(record.fields)
report = record.check_seat(
    shaft_diameter=25, housing_diameter=52,
    shoulder_diameter=32, fillet_radius=0.5,
)
assert report["status"] == "pass"
```

Preview has no project side effects. SDK installation rereads the file and refuses
changed content. The GUI installs its validated in-memory preview. Installation
and resolution participate in project undo/redo, including on assembly tabs,
and do not regenerate geometry. Repeating an unchanged import creates no undo step.

## Input schema

CSV needs a header. JSON accepts an array of record objects. Units are explicitly
`mm` or `in`; all supported dimensions and limits use that unit. Imported records
are normalized to millimetres. Mixed units and unit suffixes in numeric cells are
not supported. Optional fields may be absent, null or blank in CSV.

| Field | Meaning |
| --- | --- |
| `designation` | Unique, nonempty part identifier within this revision |
| `bore_diameter`, `outer_diameter`, `width` | Required positive nominal dimensions; outer diameter must exceed bore |
| `shaft_min`, `shaft_max` | Optional pair of permitted shaft diameters |
| `housing_min`, `housing_max` | Optional pair of permitted housing diameters |
| `shoulder_min`, `shoulder_max` | Optional pair of permitted shoulder diameters for the specified seat |
| `fillet_radius_max` | Optional nonnegative maximum seat fillet radius |
| `step_reference` | Opaque reference text; never fetched or bundled automatically |

Supply both ends of any interval. Unknown columns are listed in
`unsupported_fields`; their values are discarded. No fits or tolerances are
inferred from nominal dimensions. Limits must be appropriate to the application:
a supplier's abutment dimensions are not automatically a shaft fit specification.
The shoulder field describes one seat; inner/outer-ring shoulder semantics and
more complex arrangements need a future schema.

Map a provider's columns explicitly, for example:

```python
mapping = {
    "designation": "Product", "bore_diameter": "d",
    "outer_diameter": "D", "width": "B",
}
```

Imports are limited to 8 MiB and 10,000 records, with at most 256 revisions per
project. Duplicate identifiers, malformed rows and invalid limits are rejected
before installation. Queries are paginated; `bearings.records(page_size=50)`
iterates pages automatically.

## Provenance, sharing and revisions

Every revision requires a catalog name, revision, source and license/usage-terms
text. These are user declarations, not a license verification service. Use data
whose terms permit your intended use; availability for download alone does not
establish redistribution permission.

**Reference** is the default storage mode. Records are available during the
session, but saved projects and reference exports contain only metadata, record
count and a SHA-256 content fingerprint. The importer does not persist the local
source-file path. Reopened references remain unresolved until the matching data
is explicitly imported. Source, terms and any explicitly entered reference text
should be written with the intended recipients in mind.

**Embedded** is an explicit choice to include normalized records in saved/shared
projects. Choose it when permitted by the dataset terms. Embedded export is
refused for a revision installed as reference-only. Import under a new revision
if the content, source, terms or storage policy changes; existing revisions never
silently update. Handles pin catalog name, revision and normalized content hash.
The hash detects content changes; it does not authenticate a supplier.

```python
bearings.export("bearing-reference.json", mode="reference")
# An embedded revision can also export with mode="embedded".
restored = app.catalogs.import_package("bearing-reference.json")
# Restored reference is unresolved until matching records are reimported.
```

Exported `.json` packages carry `format: "designer-catalog"`, package version 1,
and one catalog. Package import defaults to reference storage even when records
are included. Pass `storage="embedded"` explicitly to retain them in saved projects.
No network requests, credentials, supplier integrations or third-party datasets
are bundled by this feature.

## Interpreting checks

`record.check_seat(...)` (RPC `catalog_requirements`) returns independent
`pass`, `fail` or `unverified` findings for
shaft, housing, shoulder and fillet dimensions, with the supplied limits and
observations. Missing limits or observations are unverified. Any failed check
makes the overall status fail; otherwise missing evidence makes it unverified.
All supplied observations are in mm. The API accepts zero fillet radius; the GUI
currently uses zero to mean unspecified. These observations are temporary and
are not saved with the project; a placed bearing's `check_seat` is what a
[requirement](../requirements/README.md#measures) can hold.

`record.check_seat(occurrence=N)` (RPC `check_seat {occurrence, catalog,
revision, hash}`) does the same for a PLACED bearing, with the observations
measured off the seats it is mated to instead of typed, beside the checks
ISO 15 itself supports and SKF's abutment limits, which are built in
([README.md](README.md#fits-and-seats)). An imported catalog is how another
supplier's limits are judged on top.

A passing report means only that these four supplied numbers satisfy these
supplied limits. It does not evaluate loads, speed, temperature, bearing life,
fit selection or, for typed observations, the accuracy of the model's actual
geometry. Persistent requirements are not built.

## Where the code is

`src/core/catalog/Catalog.{h,cpp}` (parsing, normalisation, packages,
`SeatRequirements`), `src/ui/CatalogPanel.cpp` and `src/ui/CatalogActions.cpp`
(the Variables-panel section and its undo history), `src/remote/RemoteCatalog.cpp`
(RPCs: `preview_catalog`, `import_catalog`, `list_catalogs`, `query_catalog`,
`catalog_requirements`, `export_catalog`; parameters via `describe`), and
`App.catalogs` (`Catalogs`, `CatalogImport`) in `tools/designer_sdk.py`.
Tests: `tests/test_catalog.cpp`, `tests/catalog_smoke.py`.
