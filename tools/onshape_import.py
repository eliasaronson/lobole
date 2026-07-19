#!/usr/bin/env python3
"""Onshape → Designer parametric importer (interchange I3).

O1: authenticated API client + fetchers. API keys come from
~/.config/designer/onshape.env (ONSHAPE_ACCESS_KEY / ONSHAPE_SECRET_KEY;
real environment variables win when set) and are sent as HTTP Basic auth —
the personal-use path, no OAuth dance. Onshape wire units are METERS;
conversion to Designer's mm happens in the mapping layer, not here.

CLI (read-only plumbing for now):
    onshape_import.py info     <document-url>
    onshape_import.py elements <document-url>
    onshape_import.py features <document-url>            # part studio features
    onshape_import.py dump     <document-url> <out.json> # fixture recorder
"""
import base64
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

BASE = "https://cad.onshape.com/api/v6"
CRED_FILE = os.path.expanduser("~/.config/designer/onshape.env")


def load_credentials():
    """access,secret from the env file, overridden by real env vars."""
    creds = {}
    if os.path.exists(CRED_FILE):
        with open(CRED_FILE) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, _, value = line.partition("=")
                creds[key.strip()] = value.strip().strip('"').strip("'")
    access = os.environ.get("ONSHAPE_ACCESS_KEY", creds.get("ONSHAPE_ACCESS_KEY"))
    secret = os.environ.get("ONSHAPE_SECRET_KEY", creds.get("ONSHAPE_SECRET_KEY"))
    if not access or not secret:
        raise SystemExit(
            f"Onshape API keys missing - put ONSHAPE_ACCESS_KEY and "
            f"ONSHAPE_SECRET_KEY in {CRED_FILE} (dev-portal.onshape.com)")
    return access, secret


class DocRef:
    """A parsed document URL: document id + workspace/version/microversion +
    optional element id."""

    def __init__(self, did, wvm, wvmid, eid=None):
        self.did, self.wvm, self.wvmid, self.eid = did, wvm, wvmid, eid

    def __repr__(self):
        return f"d/{self.did}/{self.wvm}/{self.wvmid}" + (
            f"/e/{self.eid}" if self.eid else "")


def parse_url(url):
    m = re.search(
        r"/documents/([0-9a-f]{24})/([wvm])/([0-9a-f]{24})(?:/e/([0-9a-f]{24}))?",
        url)
    if not m:
        raise SystemExit(f"not an Onshape document URL: {url}")
    return DocRef(m.group(1), m.group(2), m.group(3), m.group(4))


class Client:
    def __init__(self):
        access, secret = load_credentials()
        token = base64.b64encode(f"{access}:{secret}".encode()).decode()
        self._auth = f"Basic {token}"

    def get(self, path, query=None):
        url = BASE + path
        if query:
            url += "?" + urllib.parse.urlencode(query)
        req = urllib.request.Request(url, headers={
            "Authorization": self._auth,
            "Accept": "application/json;charset=UTF-8; qs=0.09",
        })
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                return json.load(resp)
        except urllib.error.HTTPError as e:
            body = e.read().decode(errors="replace")[:400]
            raise SystemExit(f"Onshape API {e.code} on {path}: {body}")

    # ---- read endpoints the importer needs -------------------------------

    def document_info(self, ref):
        return self.get(f"/documents/{ref.did}")

    def elements(self, ref):
        return self.get(
            f"/documents/d/{ref.did}/{ref.wvm}/{ref.wvmid}/elements")

    def features(self, ref, eid=None):
        eid = eid or ref.eid
        return self.get(
            f"/partstudios/d/{ref.did}/{ref.wvm}/{ref.wvmid}/e/{eid}/features")

    def assembly_definition(self, ref, eid=None):
        eid = eid or ref.eid
        return self.get(
            f"/assemblies/d/{ref.did}/{ref.wvm}/{ref.wvmid}/e/{eid}",
            {"includeMateFeatures": "true",
             "includeMateConnectors": "true"})

    def variables(self, ref, eid=None):
        eid = eid or ref.eid
        return self.get(
            f"/variables/d/{ref.did}/{ref.wvm}/{ref.wvmid}/e/{eid}/variables")

    def mass_properties(self, ref, eid=None):
        eid = eid or ref.eid
        return self.get(
            f"/partstudios/d/{ref.did}/{ref.wvm}/{ref.wvmid}/e/{eid}/massproperties")

    def sketch_world_points(self, ref, eid=None):
        """{sketch featureId: {geom point id: [x,y,z] (meters)}} — the 3D
        side of the 2D↔3D correspondences the plane fit consumes."""
        eid = eid or ref.eid
        data = self.get(
            f"/partstudios/d/{ref.did}/{ref.wvm}/{ref.wvmid}/e/{eid}/sketches",
            {"output3D": "true"})
        out = {}
        for sk in data.get("sketches", []):
            pts = {}
            for g in sk.get("geomEntities", []):
                if g.get("entityType") == "point" and "point" in g:
                    pts[g["id"]] = g["point"]
            out[sk.get("featureId")] = pts
        return out


# ---------------------------------------------------------------------------


def cmd_info(client, ref):
    info = client.document_info(ref)
    print(f"document: {info.get('name')}  (owner: "
          f"{info.get('owner', {}).get('name')})")
    for el in client.elements(ref):
        print(f"  [{el.get('elementType'):<10}] {el.get('name'):<32} "
              f"e/{el.get('id')}")


def cmd_elements(client, ref):
    print(json.dumps(client.elements(ref), indent=2))


def cmd_features(client, ref):
    if not ref.eid:
        raise SystemExit("URL has no element id (/e/...) - pass a part studio")
    data = client.features(ref)
    feats = data.get("features", [])
    print(f"{len(feats)} features:")
    for f in feats:
        sup = "  [suppressed]" if f.get("suppressed") else ""
        print(f"  {f.get('featureType', '?'):<22} {f.get('name', '?')}{sup}")


def cmd_dump(client, ref, out_path):
    """Record everything the importer will consume — the fixture format the
    mapping layer's offline tests replay."""
    dump = {"document": client.document_info(ref),
            "elements": client.elements(ref),
            "partstudios": {}, "assemblies": {}}
    for el in dump["elements"]:
        eid, kind = el.get("id"), el.get("elementType")
        try:
            if kind == "PARTSTUDIO":
                dump["partstudios"][eid] = {
                    "name": el.get("name"),
                    "features": client.features(ref, eid),
                    "massproperties": client.mass_properties(ref, eid),
                }
            elif kind == "ASSEMBLY":
                dump["assemblies"][eid] = {
                    "name": el.get("name"),
                    "definition": client.assembly_definition(ref, eid),
                }
        except SystemExit as e:
            print(f"  warning: skipping {el.get('name')}: {e}", file=sys.stderr)
    with open(out_path, "w") as f:
        json.dump(dump, f, indent=1)
    print(f"wrote {out_path}: {len(dump['partstudios'])} part studios, "
          f"{len(dump['assemblies'])} assemblies")


# ===========================================================================
# O2: mapping layer — vertical slice: variables, sketches (lines/circles/
# arcs/points + constraints), extrudes. Everything else lands in the report.
# Onshape wire units are METERS (angles: radians in values, "deg" in
# expressions); Designer speaks mm / degrees.
# ===========================================================================

MM = 1000.0
UNIT_TO_MM = {"mm": 1.0, "cm": 10.0, "m": 1000.0, "in": 25.4, "inch": 25.4}


def translate_expression(expr):
    """Onshape expression → Designer expression (mm/deg, bare variable
    names). Returns None when the expression uses something we can't
    translate (caller falls back / reports)."""
    if not expr or not expr.strip():
        return None
    s = expr.strip()
    s = re.sub(r"#([A-Za-z_]\w*)", r"\1", s)

    def unit_repl(m):
        return repr(float(m.group(1)) * UNIT_TO_MM[m.group(2)])

    s = re.sub(r"(\d+(?:\.\d+)?(?:[eE][-+]?\d+)?)\s*(mm|cm|m|in|inch)\b",
               unit_repl, s)
    s = re.sub(r"(\d+(?:\.\d+)?(?:[eE][-+]?\d+)?)\s*(?:deg|degree|degrees)\b",
               r"\1", s)
    if re.search(r"[A-Za-z_]\w*\s*\(", s):  # function calls: not our grammar
        return None
    return s


# ---- sketch entities ------------------------------------------------------

def _line_pts(geom, sp, ep):
    ax = geom["pntX"] + geom["dirX"] * sp
    ay = geom["pntY"] + geom["dirY"] * sp
    bx = geom["pntX"] + geom["dirX"] * ep
    by = geom["pntY"] + geom["dirY"] * ep
    return (ax, ay), (bx, by)


def map_sketch_entities(feature):
    """One Onshape sketch's entities → (designer entity list, id map,
    2D point table for the plane fit, notes). The id map goes from Onshape
    entityId → (index into the designer list, kind)."""
    entities, id_map, pts2d, notes = [], {}, {}, []
    for e in feature.get("entities", []):
        oid = e.get("entityId")
        geom = e.get("geometry", {})
        gt = geom.get("btType", e.get("btType"))
        construction = e.get("isConstruction", False)
        if gt == "BTCurveGeometryLine-117":
            a, b = _line_pts(geom, e["startParam"], e["endParam"])
            pts2d[f"{oid}.start"], pts2d[f"{oid}.end"] = a, b
            id_map[oid] = {"idx": len(entities), "kind": "line",
                           "end_idx": None}
            entities.append({"type": "line",
                             "a": [a[0] * MM, a[1] * MM],
                             "b": [b[0] * MM, b[1] * MM],
                             "construction": construction})
        elif gt == "BTCurveGeometryCircle-115" and e.get("btType") == "BTMSketchCurve-4":
            c = (geom["xCenter"], geom["yCenter"])
            pts2d[f"{oid}.center"] = c
            id_map[oid] = {"idx": len(entities), "kind": "circle",
                           "end_idx": None}
            entities.append({"type": "circle",
                             "center": [c[0] * MM, c[1] * MM],
                             "radius": geom["radius"] * MM,
                             "construction": construction})
        elif gt == "BTCurveGeometryCircle-115":  # segment of a circle = arc
            import math
            c = (geom["xCenter"], geom["yCenter"])
            base = math.atan2(geom.get("yDir", 0.0), geom.get("xDir", 1.0))
            sp, ep = e["startParam"], e["endParam"]
            if geom.get("clockwise"):
                start, end = base - ep, base - sp
            else:
                start, end = base + sp, base + ep
            pts2d[f"{oid}.center"] = c
            id_map[oid] = {"idx": len(entities), "kind": "arc",
                           "end_idx": None}
            entities.append({"type": "arc",
                             "center": [c[0] * MM, c[1] * MM],
                             "radius": geom["radius"] * MM,
                             "start_angle": start, "end_angle": end,
                             "construction": construction})
        elif e.get("btType") == "BTMSketchPoint-158":
            p = (e["x"], e["y"])
            pts2d[oid] = p
            id_map[oid] = {"idx": len(entities), "kind": "point",
                           "end_idx": None}
            entities.append({"type": "point", "p": [p[0] * MM, p[1] * MM],
                             "construction": construction})
        else:
            notes.append(f"entity {gt or e.get('btType')} skipped")
    return entities, id_map, pts2d, notes


def close_open_profiles(entities, id_map, tol=1e-3):
    """Onshape computes sketch regions from the curve GRAPH — curves split
    implicitly where they cross or touch. Designer loops need shared
    endpoints, so a profile whose edge ends on another line's interior (a
    T-junction) never closes. Split the host line at each dangling endpoint;
    an entity's original index keeps its start point, the appended second
    half owns the end point (id_map end_idx). Returns notes."""
    notes = []

    def key(p):
        return (round(p[0], 3), round(p[1], 3))

    for _ in range(16):  # each pass fixes one junction; bounded for safety
        from collections import Counter
        deg = Counter()
        for e in entities:
            if e["type"] == "line" and not e.get("construction"):
                deg[key(e["a"])] += 1
                deg[key(e["b"])] += 1
            elif e["type"] == "arc" and not e.get("construction"):
                pass  # arc endpoints not implicated in the fixtures yet
        dangling = [p for p, c in deg.items() if c == 1]
        split = False
        for p in dangling:
            for i, e in enumerate(entities):
                if e["type"] != "line" or e.get("construction"):
                    continue
                (ax, ay), (bx, by) = e["a"], e["b"]
                dx, dy = bx - ax, by - ay
                l2 = dx * dx + dy * dy
                if l2 < tol:
                    continue
                t = ((p[0] - ax) * dx + (p[1] - ay) * dy) / l2
                if t < 0.01 or t > 0.99:
                    continue  # endpoint, not interior
                px, py = ax + t * dx, ay + t * dy
                if abs(px - p[0]) > tol or abs(py - p[1]) > tol:
                    continue
                entities.append({"type": "line", "a": [px, py], "b": [bx, by],
                                 "construction": False})
                e["b"] = [px, py]
                for info in id_map.values():
                    if info["idx"] == i:
                        info["end_idx"] = len(entities) - 1
                notes.append("split a line at a T-junction to close a profile")
                split = True
                break
            if split:
                break
        if not split:
            break
    return notes


def fit_plane(pts2d, pts3d):
    """Least-squares world = O + u*U + v*V from 2D↔3D point pairs (meters).
    Returns {origin(mm), normal, uAxis} or None when under-determined."""
    pairs = [(pts2d[k], pts3d[k]) for k in pts2d if k in pts3d]
    if len(pairs) < 3:
        return None
    # Normal equations for A = [1, u, v] — one 3x3 solve per world axis.
    s1 = su = sv = suu = svv = suv = 0.0
    rhs = [[0.0] * 3 for _ in range(3)]  # per world axis: Σb, Σub, Σvb
    for (u, v), w in pairs:
        s1 += 1; su += u; sv += v
        suu += u * u; svv += v * v; suv += u * v
        for c in range(3):
            rhs[c][0] += w[c]; rhs[c][1] += u * w[c]; rhs[c][2] += v * w[c]
    m = [[s1, su, sv], [su, suu, suv], [sv, suv, svv]]

    def det3(a):
        return (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
                - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
                + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))

    d = det3(m)
    if abs(d) < 1e-12:
        return None  # collinear/degenerate
    origin, uax, vax = [0.0] * 3, [0.0] * 3, [0.0] * 3
    for c in range(3):
        for col, out in ((0, origin), (1, uax), (2, vax)):
            mm_ = [row[:] for row in m]
            for r in range(3):
                mm_[r][col] = rhs[c][r]
            out[c] = det3(mm_) / d
    norm = lambda p: sum(x * x for x in p) ** 0.5
    lu, lv = norm(uax), norm(vax)
    if lu < 1e-9 or lv < 1e-9:
        return None
    u = [x / lu for x in uax]
    v = [x / lv for x in vax]
    n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
         u[0] * v[1] - u[1] * v[0]]
    ln = norm(n)
    if ln < 1e-6:
        return None
    return {"origin": [c * MM for c in origin],
            "normal": [x / ln for x in n], "uAxis": u}


# ---- constraints ----------------------------------------------------------

def _resolve_ref(pid, id_map):
    """Onshape point/entity id → [designer entity index, which] or None.
    A line split at a T-junction keeps its start on the original index and
    its end on the appended half (end_idx)."""
    if pid in id_map:
        return [id_map[pid]["idx"], 0]
    base, _, suffix = pid.rpartition(".")
    if base in id_map:
        info = id_map[base]
        kind = info["kind"]
        which = {("line", "start"): 0, ("line", "end"): 1,
                 ("circle", "center"): 0, ("arc", "center"): 0,
                 ("arc", "start"): 1, ("arc", "end"): 2}.get((kind, suffix))
        if which is None:
            return None
        if kind == "line" and suffix == "end" and info["end_idx"] is not None:
            return [info["end_idx"], 1]
        return [info["idx"], which]
    return None


CONSTRAINT_KINDS = {
    "COINCIDENT": "coincident", "PARALLEL": "parallel",
    "PERPENDICULAR": "perpendicular", "EQUAL": "equal",
    "TANGENT": "tangent", "CONCENTRIC": "concentric",
    "MIDPOINT": "midpoint", "HORIZONTAL": "horizontal",
    "VERTICAL": "vertical", "LENGTH": "distance", "DISTANCE": "distance",
    "RADIUS": "radius", "DIAMETER": "diameter", "ANGLE": "angle",
}


def map_constraint(con, id_map):
    """→ (payload for add_constraint | None, note | None). The payload lacks
    sketch_id and uses entity INDEXES (the replayer swaps in real ids)."""
    ctype = con.get("constraintType")
    kind = CONSTRAINT_KINDS.get(ctype)
    if kind is None:
        return None, f"constraint {ctype} skipped"
    params = {p.get("parameterId"): p for p in con.get("parameters", [])}

    def sref(pid_key):
        p = params.get(pid_key)
        return _resolve_ref(p.get("value"), id_map) if p and p.get("value") else None

    refs = []
    if ctype == "MIDPOINT":
        # Onshape: midpoint point + two endpoints. Ours: point ON a line's
        # midpoint — only the same-segment form maps.
        mid = sref("localMidpoint")
        e1p = params.get("localEntity1", {}).get("value", "")
        e2p = params.get("localEntity2", {}).get("value", "")
        b1, _, s1x = e1p.rpartition(".")
        b2, _, s2x = e2p.rpartition(".")
        if not mid or b1 != b2 or b1 not in id_map:
            return None, "MIDPOINT (two-point form) skipped"
        refs = [mid, [id_map[b1]["idx"], 0]]
    elif ctype in ("LENGTH",):
        line = params.get("localFirst", {}).get("value")
        if line not in id_map or id_map[line]["kind"] != "line":
            return None, "LENGTH target unresolved"
        info = id_map[line]
        # A split line's length spans both halves — still just the distance
        # between the outer endpoints.
        end = [info["end_idx"], 1] if info["end_idx"] is not None \
            else [info["idx"], 1]
        refs = [[info["idx"], 0], end]
    else:
        for key in ("localFirst", "localSecond", "localThird",
                    "localEntity1", "localEntity2"):
            if key in params and params[key].get("value"):
                r = _resolve_ref(params[key]["value"], id_map)
                if r is None:
                    return None, f"{ctype} ref '{params[key]['value']}' unresolved"
                refs.append(r)
        if not refs:
            return None, f"{ctype} has no resolvable refs"

    payload = {"kind": kind, "refs": refs}
    # Dimensional value: expression preferred (variables survive), converted
    # literal otherwise. Onshape values are meters / radians.
    for qid in ("length", "radius", "angle", "distance"):
        q = params.get(qid)
        if q and q.get("btType") == "BTMParameterQuantity-147":
            expr = translate_expression(q.get("expression"))
            if expr is not None:
                payload["expr"] = expr
            else:
                import math
                v = q.get("value", 0.0)
                payload["value"] = (v * 180.0 / math.pi if kind == "angle"
                                    else v * MM)
            break
    return payload, None


# ---- features -------------------------------------------------------------

def _params_by_id(feature):
    return {p.get("parameterId"): p for p in feature.get("parameters", [])}


def _query_strings(param):
    return " ".join(q.get("queryString") or "" for q in
                    (param or {}).get("queries", []))


def map_extrude(feature, sketch_features):
    """→ (designer Extrude dict (sans sketch_id) + '_sketch' featureId,
    notes). None when the form can't map."""
    notes = []
    p = _params_by_id(feature)
    op = p.get("operationType", {}).get("value", "NEW")
    mode = {"NEW": "new_body", "ADD": "add", "REMOVE": "cut"}.get(op)
    if mode is None:
        return None, [f"extrude operation {op} unsupported"]

    # Which sketch: its featureId appears inside the profile query string.
    qs = _query_strings(p.get("entities"))
    matches = [f for f in sketch_features if f["featureId"] in qs]
    if len(matches) != 1:
        return None, ["extrude profile sketch unresolved"]
    sketch = matches[0]

    out = {"type": "Extrude", "name": feature.get("name", "Extrude"),
           "mode": mode, "_sketch": sketch["featureId"]}

    bound = p.get("endBound", {}).get("value", "BLIND")
    depth = p.get("depth", {})
    expr = translate_expression(depth.get("expression"))
    if expr is not None:
        out["distance_expr"] = expr
        out["distance"] = 1.0  # placeholder; expr drives it
    else:
        out["distance"] = depth.get("value", 0.0) * MM
    if bound == "SYMMETRIC" or p.get("midplane", {}).get("value"):
        out["end_mode"] = "symmetric"
    elif bound != "BLIND":
        notes.append(f"end bound {bound} approximated as blind")
    if p.get("oppositeDirection", {}).get("value"):
        out["reversed"] = True
    if p.get("hasSecondDirection", {}).get("value"):
        notes.append("second direction dropped")
    if p.get("hasDraft", {}).get("value"):
        notes.append("draft dropped")
    return out, notes


# ===========================================================================
# Replay: walk a part studio's history and rebuild it in a live Designer
# over the RPC — variables first, then sketches (baked world planes,
# constraints) and extrudes. Everything unmappable is REPORTED, never
# silently dropped.
# ===========================================================================

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


class Report:
    def __init__(self):
        self.rows = []

    def add(self, feature, status, note=""):
        self.rows.append((feature, status, note))

    def show(self):
        counts = {}
        print("\n--- import report ---")
        for feature, status, note in self.rows:
            counts[status] = counts.get(status, 0) + 1
            print(f"  {status:<8} {feature:<28} {note}")
        total = len(self.rows)
        summary = ", ".join(f"{v} {k}" for k, v in sorted(counts.items()))
        print(f"  ({total} features: {summary})")


class Replayer:
    def __init__(self, client, ref, port):
        from designer_remote import Designer
        self.client, self.ref = client, ref
        self.designer = Designer(port)
        self.report = Report()
        self.sketch_ids = {}  # onshape sketch featureId -> designer feature id

    def import_variables(self):
        """Variable studios (and anything in scope) → document variables,
        ordered as given so cross-references evaluate."""
        elements = self.client.elements(self.ref)
        count = 0
        for el in elements:
            if el.get("elementType") != "VARIABLESTUDIO":
                continue
            for scope in self.client.variables(self.ref, el["id"]):
                for var in scope.get("variables", []):
                    name, expr = var.get("name"), var.get("expression")
                    translated = translate_expression(expr)
                    if not name or translated is None:
                        self.report.add(f"variable {name}", "skipped",
                                        f"expression {expr!r} untranslatable")
                        continue
                    try:
                        self.designer.call("set_variable", name=name,
                                           expression=translated,
                                           comment=var.get("description", ""))
                        count += 1
                    except Exception as e:
                        self.report.add(f"variable {name}", "skipped", str(e))
        if count:
            print(f"imported {count} variables")

    def import_sketch(self, feature, world_points):
        name = feature.get("name", "Sketch")
        entities, id_map, pts2d, notes = map_sketch_entities(feature)
        if not entities:
            self.report.add(name, "skipped", "no mappable entities")
            return
        notes += close_open_profiles(entities, id_map)
        plane = fit_plane(pts2d, world_points.get(feature["featureId"], {}))
        if plane is None:
            self.report.add(name, "skipped", "sketch plane unresolved "
                            "(too few 2D-3D correspondences)")
            return
        resp = self.designer.call("add_feature", feature={
            "type": "Sketch", "name": name, "plane": plane,
            "entities": entities})
        self.sketch_ids[feature["featureId"]] = resp["id"]
        designer_ids = resp["entity_ids"]

        applied = failed = 0
        for con in feature.get("constraints", []):
            payload, note = map_constraint(con, id_map)
            if payload is None:
                notes.append(note)
                continue
            payload["refs"] = [[designer_ids[idx], which]
                               for idx, which in payload["refs"]]
            try:
                self.designer.call("add_constraint",
                                   sketch_id=resp["id"], **payload)
                applied += 1
            except Exception as e:
                failed += 1
                notes.append(f"{con.get('constraintType')}: "
                             f"{str(e).splitlines()[0][:60]}")
        detail = f"{len(entities)} entities, {applied} constraints"
        if failed or notes:
            uniq = sorted(set(n for n in notes if n))
            detail += f"; {failed + len(uniq)} issues: " + "; ".join(uniq[:4])
            self.report.add(name, "partial", detail)
        else:
            self.report.add(name, "mapped", detail)

    def import_extrude(self, feature, sketch_features):
        name = feature.get("name", "Extrude")
        mapped, notes = map_extrude(feature, sketch_features)
        if mapped is None:
            self.report.add(name, "skipped", "; ".join(notes))
            return
        sketch_id = self.sketch_ids.get(mapped.pop("_sketch"))
        if sketch_id is None:
            self.report.add(name, "skipped", "profile sketch was not imported")
            return
        mapped["sketch_id"] = sketch_id
        try:
            self.designer.call("add_feature", feature=mapped)
        except Exception as e:
            self.report.add(name, "skipped", str(e).splitlines()[0][:80])
            return
        self.report.add(name, "partial" if notes else "mapped",
                        "; ".join(notes))

    def run(self):
        info = self.client.document_info(self.ref)
        elements = {e["id"]: e for e in self.client.elements(self.ref)}
        studio = elements.get(self.ref.eid, {})
        tab = f"{studio.get('name', 'Onshape import')}"
        print(f"importing '{tab}' from '{info.get('name')}' "
              f"into a new part tab...")
        self.designer.call("add_part", name=tab)
        self.import_variables()

        data = self.client.features(self.ref)
        feats = data.get("features", [])
        world = self.client.sketch_world_points(self.ref)
        sketches = [f for f in feats if f.get("featureType") == "newSketch"]
        for f in feats:
            ftype, name = f.get("featureType"), f.get("name", "?")
            if f.get("suppressed"):
                self.report.add(name, "skipped", "suppressed in Onshape")
            elif ftype == "newSketch":
                self.import_sketch(f, world)
            elif ftype == "extrude":
                self.import_extrude(f, sketches)
            elif ftype == "cPlane":
                self.report.add(name, "mapped",
                                "construction plane (baked into sketch planes)")
            else:
                self.report.add(name, "skipped",
                                f"feature type '{ftype}' not mapped yet")
        self.report.show()


def cmd_replay(client, ref, port):
    if not ref.eid:
        raise SystemExit("URL has no element id (/e/...) - pass a part studio")
    Replayer(client, ref, port).run()


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    cmd, url = sys.argv[1], sys.argv[2]
    client = Client()
    ref = parse_url(url)
    if cmd == "info":
        cmd_info(client, ref)
    elif cmd == "elements":
        cmd_elements(client, ref)
    elif cmd == "features":
        cmd_features(client, ref)
    elif cmd == "dump":
        cmd_dump(client, ref, sys.argv[3])
    elif cmd == "replay":
        port = int(sys.argv[3]) if len(sys.argv) > 3 else 8799
        cmd_replay(client, ref, port)
    else:
        raise SystemExit(f"unknown command {cmd!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
