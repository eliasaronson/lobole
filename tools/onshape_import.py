#!/usr/bin/env python3
"""Onshape → Designer parametric importer (interchange I3).

Rebuilds an Onshape part studio's feature history in a running Designer over
the JSON-RPC remote, so the model arrives EDITABLE (variables drive
dimensions, sketches keep their constraints) rather than as frozen geometry.

Credentials: ~/.config/designer/onshape.env (ONSHAPE_ACCESS_KEY /
ONSHAPE_SECRET_KEY; real environment variables win), sent as HTTP Basic —
the personal-use path, no OAuth. Onshape meters API calls ANNUALLY (2,500/user
on a free account) and a separate API Agreement governs non-browser access:
read docs/interchange/onshape-terms.md before relying on this. Onshape wire units are METERS; conversion
to mm happens in the mapping layer.

Maps today: variables, sketches (lines/circles/arcs/points/splines + most
constraints), extrude / revolve / loft / sweep / boolean, feature mirror
(replayed), face-attached sketches (plane from the host cap face). Sketch
B-splines import (Onshape interpolates its points; we solve for the control
poles whose Designer curve passes through them). 3D fitSplines import as
Curve3D, re-fitted through a dense sample of the curve Onshape built.
Every unmapped feature
(fillet, chamfer, splitPart, patterns) and every skipped
constraint is REPORTED with a reason, never silently dropped.

CLI:
    onshape_import.py info        <document-url>
    onshape_import.py features    <document-url>          # list a studio's features
    onshape_import.py dump        <document-url> <out.json>  # offline fixture
    onshape_import.py replay      <part-studio-url> [port]   # live rebuild (~5 calls)
    onshape_import.py replay-dump <dump.json> <studio> [port] # rebuild offline, 0 calls
"""
import base64
import json
import os
import time
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

    _last_fs = [0.0]  # wall-clock of the last FS call (proactive throttle)

    def _eval_fs(self, ref, eid, script, rollback):
        url = (f"/partstudios/d/{ref.did}/{ref.wvm}/{ref.wvmid}/e/{eid}"
               f"/featurescript?"
               + urllib.parse.urlencode({"rollbackBarIndex": rollback}))
        # Onshape's FS-eval rate limit is strict; keep >=4s between calls so
        # a whole dump stays under it, then back off further on a 429.
        gap = time.time() - self._last_fs[0]
        if gap < 4.0:
            time.sleep(4.0 - gap)
        # Back off and retry on 429 / the HTML throttle page rather than
        # losing the geometry.
        for attempt in range(6):
            req = urllib.request.Request(
                BASE + url, data=json.dumps({"script": script}).encode(),
                method="POST", headers={
                    "Authorization": self._auth,
                    "Content-Type": "application/json",
                    "Accept": "application/json;charset=UTF-8; qs=0.09"})
            try:
                with urllib.request.urlopen(req, timeout=90) as resp:
                    self._last_fs[0] = time.time()
                    return json.load(resp)
            except urllib.error.HTTPError as e:
                if e.code in (429, 503) and attempt < 5:
                    time.sleep(10 * (attempt + 1))  # 10,20,30,40,50s
                    continue
                raise SystemExit(f"FeatureScript eval failed: "
                                 f"{e.read().decode(errors='replace')[:200]}")
            except (json.JSONDecodeError, urllib.error.URLError):
                if attempt < 5:
                    time.sleep(10 * (attempt + 1))
                    continue
                raise SystemExit("FeatureScript eval failed (throttled)")
            finally:
                self._last_fs[0] = time.time()

    _EDGE_MIDS_FS = """function(context is Context, queries){
      var res = [];
      for (var e in evaluateQuery(context, qEverything(EntityType.EDGE))){
        var mp = evEdgeTangentLine(context, {"edge": e, "parameter": 0.5}).origin;
        res = append(res, [mp[0].value * 1000, mp[1].value * 1000,
                           mp[2].value * 1000]);
      }
      return res;
    }"""

    def sketch_planes(self, ref, eid, sketch_fids, rollback):
        """{sketch featureId: {origin(mm), normal, xAxis}} — the sketch's
        TRUE plane frame from Onshape (evOwnerSketchPlane). Ground truth,
        so a face-attached sketch's normal direction is unambiguous (a
        least-squares fit can't tell a normal from its flip, which sent
        face cuts the wrong way). One FS call for the whole studio;
        `rollback` = the studio's feature count (final state)."""
        ids = ",".join('"%s"' % f for f in sketch_fids)
        script = (
            'function(context is Context, queries){ var out = {}; '
            'for (var fid in [' + ids + ']){ '
            'var es = qCreatedBy(makeId(fid), EntityType.EDGE); '
            'if (size(evaluateQuery(context, es)) == 0){ continue; } '
            'var pl = evOwnerSketchPlane(context, '
            '{"entity": qNthElement(es, 0)}); '
            'out[fid] = [pl.origin[0].value*1000, pl.origin[1].value*1000, '
            'pl.origin[2].value*1000, pl.normal[0], pl.normal[1], '
            'pl.normal[2], pl.x[0], pl.x[1], pl.x[2]]; } return out; }')
        res = self._eval_fs(ref, eid, script, rollback)
        out = {}
        for entry in res.get("result", {}).get("value", []):
            key = entry.get("key", {}).get("value")
            nums = [x.get("value") for x in entry.get("value", {}).get("value", [])]
            if key and len(nums) == 9:
                out[key] = {"origin": nums[0:3], "normal": nums[3:6],
                            "xAxis": nums[6:9]}
        return out

    def feature_vertices(self, ref, eid, feature_id, rollback):
        """World-mm vertices created by a feature (qCreatedBy), at a rollback
        state. Projected onto the sketch normal, their span gives an
        up-to/through extrude's true depth without resolving a target query."""
        script = (
            'function(context is Context, queries){ var vs = evaluateQuery('
            'context, qCreatedBy(makeId("' + feature_id + '"), '
            'EntityType.VERTEX)); var res = []; for (var v in vs){ var p = '
            'evVertexPoint(context, {"vertex": v}); res = append(res, '
            '[p[0].value*1000, p[1].value*1000, p[2].value*1000]); } '
            'return res; }')
        res = self._eval_fs(ref, eid, script, rollback)
        return [[v.get("value") for v in item.get("value", [])]
                for item in res.get("result", {}).get("value", [])]

    def curve_sample_points(self, ref, eid, feature_id, rollback, n=17):
        """`n` world-mm points along the curve a feature created (qCreatedBy,
        EntityType.EDGE), at a rollback state.

        Why sample the RESULT rather than read the feature's own through-
        points: those are a query of vertices/mate connectors, and this
        importer has learned repeatedly that Onshape queries do not resolve
        at rollback (see up_to_verts, consumed_edges, face_diffs — all of
        them measure geometry for the same reason). A fit spline re-fitted
        through a dense sample of itself is the same curve to within the
        sample spacing, and it cannot fail to resolve."""
        script = (
            'function(context is Context, queries){ var es = evaluateQuery('
            'context, qCreatedBy(makeId("' + feature_id + '"), '
            'EntityType.EDGE)); if (size(es) == 0) { return []; } '
            'var res = []; for (var i = 0; i < ' + str(n) + '; i += 1){ '
            'var t = i / ' + str(n - 1) + '; var l = evEdgeTangentLine('
            'context, {"edge": es[0], "parameter": t}); res = append(res, '
            '[l.origin[0].value*1000, l.origin[1].value*1000, '
            'l.origin[2].value*1000]); } return res; }')
        res = self._eval_fs(ref, eid, script, rollback)
        return [[v.get("value") for v in item.get("value", [])]
                for item in res.get("result", {}).get("value", [])]

    _FACE_FS = """function(context is Context, queries){
      var res = [];
      for (var f in evaluateQuery(context, qEverything(EntityType.FACE))){
        try silent {
          var c = evApproximateCentroid(context, {"entities": f});
          var ar = evArea(context, {"entities": f}).value;
          var nrm = [0, 0, 0];
          try silent { var pl = evPlane(context, {"face": f});
                       nrm = [pl.normal[0], pl.normal[1], pl.normal[2]]; }
          res = append(res, [c[0].value*1000, c[1].value*1000, c[2].value*1000,
                             nrm[0], nrm[1], nrm[2], ar*1e6]);
        }
      }
      return res;
    }"""

    def all_faces(self, ref, eid, rollback):
        """Every face's [centroid(mm) x3, normal x3, area mm2] at a rollback
        — before/after diffs resolve shell openings (a big face becomes a
        thin rim: same centroid, area collapses) and draft faces (same
        centroid, normal tilts), sidestepping Onshape's face-query
        fragility. Non-planar faces report a zero normal."""
        res = self._eval_fs(ref, eid, self._FACE_FS, rollback)
        return [[v.get("value") for v in item.get("value", [])]
                for item in res.get("result", {}).get("value", [])]

    def all_edge_midpoints(self, ref, eid, rollback):
        """Every edge's midpoint (mm, world) at a rollback state. Two of
        these (before/after a fillet) diff to the consumed = filleted edges,
        sidestepping Onshape's fragile topology-query resolution."""
        res = self._eval_fs(ref, eid, self._EDGE_MIDS_FS, rollback)
        out = []
        for item in res.get("result", {}).get("value", []):
            out.append([v.get("value") for v in item.get("value", [])])
        return out

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
                elif g.get("entityType") == "circle":
                    if "center" in g:
                        pts.setdefault(g["id"] + ".center", g["center"])
                    if "radius" in g:  # smuggled: [r] under a reserved key
                        pts[g["id"] + "::radius"] = [g["radius"]]
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


def _diff_edges(pre, post, tol=0.5):
    """Midpoints present in `pre` but absent (moved) in `post` — the edges
    a fillet/chamfer consumed (tangent-propagated ones included)."""
    def near(m):
        return any(abs(m[0] - q[0]) < tol and abs(m[1] - q[1]) < tol
                   and abs(m[2] - q[2]) < tol for q in post)
    return [m for m in pre if not near(m)]


FILLET_TYPES = ("fillet", "chamfer")


def consumed_edges_for(client, ref, eid, features):
    """{feature index: [consumed midpoint mm, ...]} for each fillet/chamfer,
    from a before/after edge-midpoint diff (2 FS calls per feature)."""
    feats = features.get("features", [])
    out = {}
    for i, f in enumerate(feats):
        if f.get("featureType") not in FILLET_TYPES or f.get("suppressed"):
            continue
        try:
            pre = client.all_edge_midpoints(ref, eid, i)
            post = client.all_edge_midpoints(ref, eid, i + 1)
        except SystemExit as e:
            print(f"  warning: no edges for {f.get('name')}: {e}",
                  file=sys.stderr)
            continue
        out[str(i)] = _diff_edges(pre, post)
    return out


def true_planes_for(client, ref, eid, features):
    """{sketch featureId: {origin, normal, xAxis}} — one FS call per studio."""
    feats = features.get("features", [])
    fids = [f["featureId"] for f in feats
            if f.get("featureType") == "newSketch" and not f.get("suppressed")]
    if not fids:
        return {}
    try:
        return client.sketch_planes(ref, eid, fids, len(feats))
    except SystemExit as e:
        print(f"  warning: no sketch planes: {e}", file=sys.stderr)
        return {}


def _param_val(feature, pid):
    for p in feature.get("parameters", []):
        if p.get("parameterId") == pid:
            return p.get("value")
    return None


def _face_c_match(m, pts, ct=0.6):
    """True if any face in `pts` shares m's centroid AND area (a face that
    only moved/tilted still matches; one that opened or vanished doesn't)."""
    for q in pts:
        if (abs(m[0] - q[0]) < ct and abs(m[1] - q[1]) < ct
                and abs(m[2] - q[2]) < ct
                and abs(m[6] - q[6]) < max(1.0, 0.05 * m[6])):
            return True
    return False


def _nearest_face(m, pts, ct=0.6):
    best = None
    for q in pts:
        d = ((m[0] - q[0]) ** 2 + (m[1] - q[1]) ** 2 + (m[2] - q[2]) ** 2) ** 0.5
        if d < ct and (best is None or d < best[0]):
            best = (d, q)
    return best[1] if best else None


FACE_FEATURE_TYPES = ("shell", "draft", "externalThread")


def face_diffs_for(client, ref, eid, features):
    """Per shell/draft/thread feature: the affected face centroids (mm), by
    a before/after face diff (2 FS calls each). Shell = faces that opened
    (centroid+area no longer match); draft = faces whose normal tilted;
    thread = the cylindrical face it split (normal went to 0 / area changed
    at the same centroid — a thread splits the cylinder)."""
    feats = features.get("features", [])
    out = {}
    for i, f in enumerate(feats):
        t = f.get("featureType")
        if t not in FACE_FEATURE_TYPES or f.get("suppressed"):
            continue
        try:
            pre = client.all_faces(ref, eid, i)
            post = client.all_faces(ref, eid, i + 1)
        except SystemExit as e:
            print(f"  warning: no faces for {f.get('name')}: {e}",
                  file=sys.stderr)
            continue
        if t == "shell":
            out[str(i)] = [m[:3] for m in pre if not _face_c_match(m, post)]
        elif t == "draft":
            drafted = []
            for m in pre:
                q = _nearest_face(m, post)
                if q is None:
                    continue
                dot = m[3] * q[3] + m[4] * q[4] + m[5] * q[5]
                if dot < 0.9995:  # normal tilted -> this face was drafted
                    drafted.append(m[:3])
            out[str(i)] = drafted
        else:  # externalThread: the cylindrical face it acted on
            out[str(i)] = [m[:3] for m in pre if not _face_c_match(m, post)]
    return out


def up_to_verts_for(client, ref, eid, features):
    """{feature index: [created vertex mm, ...]} for each up-to/through
    extrude (1 FS call each). The span of these along the sketch normal is
    the extrude's true depth — Onshape's target-face queries don't resolve
    at rollback, so measure the result instead."""
    feats = features.get("features", [])
    out = {}
    for i, f in enumerate(feats):
        if f.get("featureType") != "extrude" or f.get("suppressed"):
            continue
        bound = _param_val(f, "endBound")
        if bound in (None, "BLIND", "SYMMETRIC"):
            continue
        try:
            out[str(i)] = client.feature_vertices(ref, eid, f["featureId"],
                                                  i + 1)
        except SystemExit as e:
            print(f"  warning: no verts for {f.get('name')}: {e}",
                  file=sys.stderr)
    return out


def curve_points_for(client, ref, eid, features):
    """{feature index: [sampled curve point mm, ...]} for each 3D-curve
    feature (1 FS call each), so replay-dump maps them offline."""
    out = {}
    for i, f in enumerate(features.get("features", [])):
        if f.get("featureType") not in _CURVE_FEATURE_TYPES or f.get("suppressed"):
            continue
        try:
            pts = client.curve_sample_points(ref, eid, f["featureId"], i + 1)
            if pts:
                out[str(i)] = pts
        except SystemExit as e:
            print(f"  warning: no curve points for {f.get('name')}: {e}",
                  file=sys.stderr)
    return out


def cmd_dump(client, ref, out_path):
    """Record everything the importer will consume — the fixture format
    offline tests AND `replay-dump` consume, so iterating on the mapping
    costs zero API calls."""
    dump = {"version": 2,
            "document": client.document_info(ref),
            "elements": client.elements(ref),
            "partstudios": {}, "assemblies": {}, "variables": {}}
    for el in dump["elements"]:
        eid, kind = el.get("id"), el.get("elementType")
        try:
            if kind == "PARTSTUDIO":
                feats = client.features(ref, eid)
                dump["partstudios"][eid] = {
                    "name": el.get("name"),
                    "features": feats,
                    "sketches": client.sketch_world_points(ref, eid),
                    "massproperties": client.mass_properties(ref, eid),
                    # Consumed-edge midpoints per fillet/chamfer (2 FS calls
                    # each) so replay-dump resolves edges offline.
                    "consumed_edges": consumed_edges_for(client, ref, eid,
                                                         feats),
                    # Created vertices per up-to/through extrude → true depth.
                    "up_to_verts": up_to_verts_for(client, ref, eid, feats),
                    # Ground-truth sketch plane frames (unambiguous normals).
                    "true_planes": true_planes_for(client, ref, eid, feats),
                    # Affected face centroids per shell/draft/thread.
                    "face_diffs": face_diffs_for(client, ref, eid, feats),
                    # Sampled points along each 3D curve (fitSpline).
                    "curve_points": curve_points_for(client, ref, eid, feats),
                }
            elif kind == "ASSEMBLY":
                dump["assemblies"][eid] = {
                    "name": el.get("name"),
                    "definition": client.assembly_definition(ref, eid),
                }
            elif kind == "VARIABLESTUDIO":
                dump["variables"][eid] = client.variables(ref, eid)
        except SystemExit as e:
            print(f"  warning: skipping {el.get('name')}: {e}", file=sys.stderr)
    with open(out_path, "w") as f:
        json.dump(dump, f, indent=1)
    print(f"wrote {out_path}: {len(dump['partstudios'])} part studios, "
          f"{len(dump['assemblies'])} assemblies, "
          f"{len(dump['variables'])} variable studios")


class DumpSource:
    """The Replayer's data source when reading a recorded dump instead of
    the live API — mapping iteration without spending API calls."""

    def __init__(self, path, eid_or_name):
        with open(path) as f:
            self.dump = json.load(f)
        self.eid = None
        for eid, ps in self.dump.get("partstudios", {}).items():
            if eid == eid_or_name or ps.get("name") == eid_or_name:
                self.eid = eid
                break
        if self.eid is None:
            names = [p.get("name") for p in
                     self.dump.get("partstudios", {}).values()]
            raise SystemExit(f"part studio {eid_or_name!r} not in dump; "
                             f"has: {names}")

    def document_info(self):
        return self.dump.get("document", {})

    def elements(self):
        return self.dump.get("elements", [])

    def variables(self, eid):
        return self.dump.get("variables", {}).get(eid, [])

    def features(self, eid=None):
        return self.dump["partstudios"][eid or self.eid]["features"]

    def sketch_world_points(self, eid=None):
        ps = self.dump["partstudios"][eid or self.eid]
        if "sketches" not in ps:
            raise SystemExit("dump predates v2 (no sketch world points) - "
                             "re-dump once with the current tool")
        return ps["sketches"]

    def consumed_edges(self, feature_index, eid=None):
        ps = self.dump["partstudios"][eid or self.eid]
        return ps.get("consumed_edges", {}).get(str(feature_index))

    def up_to_verts(self, feature_index, eid=None):
        ps = self.dump["partstudios"][eid or self.eid]
        return ps.get("up_to_verts", {}).get(str(feature_index))

    def true_plane(self, sketch_fid, eid=None):
        ps = self.dump["partstudios"][eid or self.eid]
        return ps.get("true_planes", {}).get(sketch_fid)

    def face_diff(self, feature_index, eid=None):
        ps = self.dump["partstudios"][eid or self.eid]
        return ps.get("face_diffs", {}).get(str(feature_index))

    def curve_points(self, feature_index, eid=None):
        ps = self.dump["partstudios"][eid or self.eid]
        return ps.get("curve_points", {}).get(str(feature_index))


class LiveSource:
    """The same interface over the live API (~5 calls per part studio)."""

    def __init__(self, client, ref):
        self.client, self.ref, self.eid = client, ref, ref.eid

    def document_info(self):
        return self.client.document_info(self.ref)

    def elements(self):
        return self.client.elements(self.ref)

    def variables(self, eid):
        return self.client.variables(self.ref, eid)

    def features(self, eid=None):
        return self.client.features(self.ref, eid or self.eid)

    def sketch_world_points(self, eid=None):
        return self.client.sketch_world_points(self.ref, eid or self.eid)

    def consumed_edges(self, feature_index, eid=None):
        # Live: compute the before/after edge diff on demand (2 FS calls).
        try:
            pre = self.client.all_edge_midpoints(self.ref, eid or self.eid,
                                                 feature_index)
            post = self.client.all_edge_midpoints(self.ref, eid or self.eid,
                                                  feature_index + 1)
        except SystemExit:
            return None
        return _diff_edges(pre, post)

    def up_to_verts(self, feature_index, eid=None):
        feats = self.features().get("features", [])
        if feature_index >= len(feats):
            return None
        try:
            return self.client.feature_vertices(
                self.ref, eid or self.eid, feats[feature_index]["featureId"],
                feature_index + 1)
        except SystemExit:
            return None

    def true_plane(self, sketch_fid, eid=None):
        if not hasattr(self, "_planes"):
            feats = self.features().get("features", [])
            fids = [f["featureId"] for f in feats
                    if f.get("featureType") == "newSketch"]
            try:
                self._planes = self.client.sketch_planes(
                    self.ref, eid or self.eid, fids, len(feats))
            except SystemExit:
                self._planes = {}
        return self._planes.get(sketch_fid)

    def face_diff(self, feature_index, eid=None):
        feats = self.features().get("features", [])
        if feature_index >= len(feats):
            return None
        try:
            pre = self.client.all_faces(self.ref, eid or self.eid,
                                        feature_index)
            post = self.client.all_faces(self.ref, eid or self.eid,
                                         feature_index + 1)
        except SystemExit:
            return None
        t = feats[feature_index].get("featureType")
        if t == "draft":
            out = []
            for m in pre:
                q = _nearest_face(m, post)
                if q and (m[3] * q[3] + m[4] * q[4] + m[5] * q[5]) < 0.9995:
                    out.append(m[:3])
            return out
        return [m[:3] for m in pre if not _face_c_match(m, post)]

    def curve_points(self, feature_index, eid=None):
        feats = self.features().get("features", [])
        if feature_index >= len(feats):
            return None
        try:
            return self.client.curve_sample_points(
                self.ref, eid or self.eid, feats[feature_index]["featureId"],
                feature_index + 1)
        except SystemExit:
            return None


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

    # A number directly before a unit folds into its value ("145.4 mm").
    s = re.sub(r"(\d+(?:\.\d+)?(?:[eE][-+]?\d+)?)\s*(mm|cm|m|in|inch)\b",
               unit_repl, s)
    s = re.sub(r"(\d+(?:\.\d+)?(?:[eE][-+]?\d+)?)\s*(?:deg|degree|degrees)\b",
               r"\1", s)
    # A unit after ")" or an identifier is an IMPLICIT MULTIPLY, e.g.
    # "(145.4+1) mm" — needs an explicit "*" or it's a syntax error (the bug
    # that collapsed a driver-hole diameter variable to 0).
    s = re.sub(r"([\)\w])\s+(mm|cm|m|in|inch)\b",
               lambda m: "%s * %s" % (m.group(1), UNIT_TO_MM[m.group(2)]), s)
    s = re.sub(r"([\)\w])\s+(?:deg|degree|degrees)\b", r"\1 * 1.0", s)
    # Any remaining bare unit token ("2.5 * mm").
    s = re.sub(r"\b(mm|cm|m|in|inch)\b",
               lambda m: repr(UNIT_TO_MM[m.group(1)]), s)
    s = re.sub(r"\b(?:deg|degree|degrees)\b", "1.0", s)
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


# ---- B-spline interpolation ------------------------------------------------
# Onshape's spline INTERPOLATES its points; Designer's BSpline is control-POLE
# based (passes through only its endpoints). Solve for the poles P whose
# Designer curve, evaluated at the Greville abscissae, equals the data points:
#   M . P = D,  M[i][j] = N_{j,p}(greville_i).
# The knot vector + degree must match Designer's BSplineKnots / BSplineDegree
# so the reconstructed curve is exactly what Designer draws (verified to
# machine precision against the header de Boor evaluator).

def _bspline_degree(n, degree=3):
    return 0 if n < 2 else min(degree, n - 1)


def _bspline_knots(n, p):
    interior = n - p - 1
    return ([0.0] * (p + 1) + [float(i) for i in range(1, interior + 1)] +
            [float(interior + 1)] * (p + 1))


def _bspline_basis(j, p, t, U):
    if p == 0:
        if U[j] <= t < U[j + 1]:
            return 1.0
        return 1.0 if (t == U[-1] and U[j] < U[j + 1] == U[-1]) else 0.0
    a = b = 0.0
    d1 = U[j + p] - U[j]
    if d1 > 1e-12:
        a = (t - U[j]) / d1 * _bspline_basis(j, p - 1, t, U)
    d2 = U[j + p + 1] - U[j + 1]
    if d2 > 1e-12:
        b = (U[j + p + 1] - t) / d2 * _bspline_basis(j + 1, p - 1, t, U)
    return a + b


def _solve_linear(M, rhs):
    """Gaussian elimination with partial pivoting; M is n x n, rhs n x k."""
    n = len(M)
    A = [M[i][:] + rhs[i][:] for i in range(n)]
    k = len(rhs[0])
    for col in range(n):
        piv = max(range(col, n), key=lambda r: abs(A[r][col]))
        A[col], A[piv] = A[piv], A[col]
        pv = A[col][col]
        if abs(pv) < 1e-12:
            raise ValueError("singular collocation matrix")
        A[col] = [x / pv for x in A[col]]
        for r in range(n):
            if r != col and abs(A[r][col]) > 1e-15:
                f = A[r][col]
                A[r] = [x - f * y for x, y in zip(A[r], A[col])]
    return [A[r][n:n + k] for r in range(n)]


def bspline_interp_poles(points, degree=3):
    """points: [(x, y), ...] the curve must pass through. Returns control
    poles [(x, y), ...] for Designer's clamped B-spline (same count as points).
    """
    n = len(points)
    if n < 3:
        return [list(p) for p in points]  # 2 pts -> a line; poles == points
    p = _bspline_degree(n, degree)
    U = _bspline_knots(n, p)
    tau = [sum(U[i + 1:i + p + 1]) / p for i in range(n)]  # Greville abscissae
    M = [[_bspline_basis(j, p, tau[i], U) for j in range(n)] for i in range(n)]
    rhs = [[points[i][0], points[i][1]] for i in range(n)]
    sol = _solve_linear(M, rhs)
    return [[sol[i][0], sol[i][1]] for i in range(n)]


def map_sketch_entities(feature):
    """One Onshape sketch's entities → (designer entity list, id map,
    2D point table for the plane fit, notes). The id map goes from Onshape
    entityId → (index into the designer list, kind)."""
    entities, id_map, pts2d, notes = [], {}, {}, []
    point_lookup = {}  # pointId -> (entity index, coordinate key)
    for e in feature.get("entities", []):
        oid = e.get("entityId")
        geom = e.get("geometry", {})
        gt = geom.get("btType", e.get("btType"))
        construction = e.get("isConstruction", False)
        if gt == "BTCurveGeometryLine-117":
            a, b = _line_pts(geom, e["startParam"], e["endParam"])
            pts2d[f"{oid}.start"], pts2d[f"{oid}.end"] = a, b
            point_lookup[f"{oid}.start"] = (len(entities), "a")
            point_lookup[f"{oid}.end"] = (len(entities), "b")
            id_map[oid] = {"idx": len(entities), "kind": "line",
                           "end_idx": None}
            entities.append({"type": "line",
                             "a": [a[0] * MM, a[1] * MM],
                             "b": [b[0] * MM, b[1] * MM],
                             "construction": construction})
        elif gt == "BTCurveGeometryCircle-115" and e.get("btType") == "BTMSketchCurve-4":
            c = (geom["xCenter"], geom["yCenter"])
            pts2d[f"{oid}.center"] = c
            point_lookup[f"{oid}.center"] = (len(entities), "center")
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
            point_lookup[f"{oid}.center"] = (len(entities), "center")
            # Onshape's .start/.end are param-order; a clockwise arc maps to
            # our CCW model with the endpoints SWAPPED.
            swapped = bool(geom.get("clockwise"))
            point_lookup[f"{oid}.start"] = \
                (len(entities), "end" if swapped else "start")
            point_lookup[f"{oid}.end"] = \
                (len(entities), "start" if swapped else "end")
            id_map[oid] = {"idx": len(entities), "kind": "arc",
                           "end_idx": None, "swapped": swapped}
            entities.append({"type": "arc",
                             "center": [c[0] * MM, c[1] * MM],
                             "radius": geom["radius"] * MM,
                             "start_angle": start, "end_angle": end,
                             "construction": construction})
        elif (gt == "BTCurveGeometryEllipse-1189" and
              e.get("btType") == "BTMSketchCurve-4"):
            import math
            c = (geom["xCenter"], geom["yCenter"])
            rot = math.atan2(geom.get("yDir", 0.0), geom.get("xDir", 1.0))
            pts2d[f"{oid}.center"] = c
            point_lookup[f"{oid}.center"] = (len(entities), "center")
            id_map[oid] = {"idx": len(entities), "kind": "ellipse",
                           "end_idx": None}
            entities.append({"type": "ellipse",
                             "center": [c[0] * MM, c[1] * MM],
                             "r_major": geom["radius"] * MM,
                             "r_minor": geom["minorRadius"] * MM,
                             "rotation": rot, "construction": construction})
        elif gt == "BTCurveGeometryEllipse-1189":  # segment = elliptical arc
            import math
            c = (geom["xCenter"], geom["yCenter"])
            rot = math.atan2(geom.get("yDir", 0.0), geom.get("xDir", 1.0))
            sp, ep = e["startParam"], e["endParam"]
            # The ellipse parameter matches Designer's EllipseArcEvalUV (verified
            # against sketch connectivity); `clockwise` only reverses the sweep,
            # so a CW arc sp->ep is the CCW arc ep->sp in our CCW-always model.
            start_p, end_p = (ep, sp) if geom.get("clockwise") else (sp, ep)
            while end_p <= start_p:
                end_p += 2.0 * math.pi
            pts2d[f"{oid}.center"] = c
            point_lookup[f"{oid}.center"] = (len(entities), "center")
            id_map[oid] = {"idx": len(entities), "kind": "ellipse_arc",
                           "end_idx": None}
            entities.append({"type": "ellipse_arc",
                             "center": [c[0] * MM, c[1] * MM],
                             "r_major": geom["radius"] * MM,
                             "r_minor": geom["minorRadius"] * MM,
                             "rotation": rot, "start_param": start_p,
                             "end_param": end_p, "construction": construction})
        elif e.get("btType") == "BTMSketchPoint-158":
            p = (e["x"], e["y"])
            pts2d[oid] = p
            point_lookup[oid] = (len(entities), "p")
            id_map[oid] = {"idx": len(entities), "kind": "point",
                           "end_idx": None}
            entities.append({"type": "point", "p": [p[0] * MM, p[1] * MM],
                             "construction": construction})
        elif gt == "BTCurveGeometryInterpolatedSpline-116":
            ip = geom.get("interpolationPoints", [])
            pts = [(ip[k], ip[k + 1]) for k in range(0, len(ip) - 1, 2)]
            if geom.get("isPeriodic") and len(pts) >= 2:
                pts = pts + [pts[0]]  # close the loop -> self-closed spline
            if len(pts) < 2:
                notes.append("spline with <2 points skipped")
                continue
            # Onshape interpolates its points; convert to the control poles
            # whose Designer curve passes through them.
            poles = bspline_interp_poles(pts)
            pts2d[f"{oid}.start"], pts2d[f"{oid}.end"] = pts[0], pts[-1]
            point_lookup[f"{oid}.start"] = (len(entities), "spole0")
            point_lookup[f"{oid}.end"] = \
                (len(entities), f"spole{len(poles) - 1}")
            id_map[oid] = {"idx": len(entities), "kind": "spline",
                           "end_idx": None}
            entities.append({"type": "spline",
                             "poles": [[p[0] * MM, p[1] * MM] for p in poles],
                             "degree": 3,
                             "construction": construction})
        else:
            notes.append(f"entity {gt or e.get('btType')} skipped")
    return entities, id_map, pts2d, point_lookup, notes


def _entity_coord(e, key, value=None):
    """Read (value=None) or write a point on an entity by point_lookup key.
    Splines address a pole by "spole<k>"; everything else is a named field."""
    if e["type"] == "spline" and key.startswith("spole"):
        k = int(key[5:])
        if value is None:
            return e["poles"][k]
        e["poles"][k] = value
        return value
    if value is None:
        return e[key]
    e[key] = value
    return value


def project_to_plane(world_mm, plane):
    """World point (mm) -> sketch UV in the fitted plane's frame."""
    o, u, n = plane["origin"], plane["uAxis"], plane["normal"]
    v = [n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2],
         n[0] * u[1] - n[1] * u[0]]
    d = [world_mm[0] - o[0], world_mm[1] - o[1], world_mm[2] - o[2]]
    return [d[0] * u[0] + d[1] * u[1] + d[2] * u[2],
            d[0] * v[0] + d[1] * v[1] + d[2] * v[2]]


def align_plane_frame(entities, point_lookup, world_pts, plane):
    """Adjust the candidate plane's ORIGIN and U axis so the STORED 2D
    coordinates land on the world correspondences unchanged — entities keep
    Onshape's sketch frame, so HORIZONTAL/VERTICAL semantics survive (the
    entity-rotating variant broke them). 1 pair: translation only. 2+:
    in-plane rotation + translation (2D Kabsch). Tries the flipped normal
    too and keeps whichever residual is smaller (mirrored face frames).
    Returns (plane, pair count) or (None, 0)."""
    import math

    def solve(pl):
        pairs = []
        for pid, (idx, key) in point_lookup.items():
            w = world_pts.get(pid)
            if w is None or len(w) != 3 or key in ("start", "end"):
                continue
            uv = project_to_plane([c * MM for c in w], pl)
            pairs.append((_entity_coord(entities[idx], key), uv))
        if not pairs:
            return None, 0.0, 0
        if len(pairs) == 1:
            (p, q) = pairs[0]
            theta, t = 0.0, [q[0] - p[0], q[1] - p[1]]
        else:
            n = len(pairs)
            cp = [sum(p[0] for p, _ in pairs) / n,
                  sum(p[1] for p, _ in pairs) / n]
            cq = [sum(q[0] for _, q in pairs) / n,
                  sum(q[1] for _, q in pairs) / n]
            num = den = 0.0
            for p, q in pairs:
                px, py = p[0] - cp[0], p[1] - cp[1]
                qx, qy = q[0] - cq[0], q[1] - cq[1]
                num += px * qy - py * qx
                den += px * qx + py * qy
            theta = math.atan2(num, den)
            c, s = math.cos(theta), math.sin(theta)
            t = [cq[0] - (c * cp[0] - s * cp[1]),
                 cq[1] - (s * cp[0] + c * cp[1])]
        c, s = math.cos(theta), math.sin(theta)
        resid = 0.0
        for p, q in pairs:
            rx = c * p[0] - s * p[1] + t[0] - q[0]
            ry = s * p[0] + c * p[1] + t[1] - q[1]
            resid = max(resid, math.hypot(rx, ry))
        # Fold rotation+translation into the frame: world = O' + U'·u + V'·v
        u, nrm = pl["uAxis"], pl["normal"]
        v = [nrm[1] * u[2] - nrm[2] * u[1], nrm[2] * u[0] - nrm[0] * u[2],
             nrm[0] * u[1] - nrm[1] * u[0]]
        new_u = [c * u[i] + s * v[i] for i in range(3)]
        origin = [pl["origin"][i] + t[0] * u[i] + t[1] * v[i]
                  for i in range(3)]
        return ({"origin": origin, "normal": nrm, "uAxis": new_u},
                resid, len(pairs))

    flipped = {"origin": plane["origin"],
               "normal": [-c for c in plane["normal"]],
               "uAxis": plane["uAxis"]}
    best = (None, float("inf"), 0)
    for cand in (plane, flipped):
        got = solve(cand)
        if got is not None and got[0] is not None and got[1] < best[1]:
            best = got
    return (best[0], best[2]) if best[0] is not None else (None, 0)


def snap_to_world(entities, point_lookup, world_pts, plane):
    """The feature list's 2D geometry is a STORED SNAPSHOT that can lag the
    current variable values (Onshape re-solves at regen; a wall_thickness
    edit does not rewrite stored sketches). The /sketches endpoint reflects
    the CURRENT solved state — snap every corresponding point onto it so
    imported sketches start at truth instead of relying on re-derived
    dimension constraints to pull them there."""
    import math
    moved = 0.0
    arc_pts = {}  # idx -> {"start"/"end"/"center": uv}
    for pid, (idx, key) in point_lookup.items():
        w = world_pts.get(pid)
        if not w:
            continue
        uv = project_to_plane([c * MM for c in w], plane)
        e = entities[idx]
        if e["type"] == "arc":
            arc_pts.setdefault(idx, {})[key] = uv
            if key == "center":
                d = math.dist(e["center"], uv)
                moved = max(moved, d)
                e["center"] = uv
            continue
        old = _entity_coord(e, key)
        d = ((old[0] - uv[0]) ** 2 + (old[1] - uv[1]) ** 2) ** 0.5
        moved = max(moved, d)
        _entity_coord(e, key, uv)
    # Circles: snap radii from the world data (stale stored radii pull
    # concentric geometry when a diameter variable changed).
    for pid, w in world_pts.items():
        if pid.endswith("::radius") and len(w) == 1:
            base = pid[:-len("::radius")]
            info_pid = base + ".center"
            for lp, (idx, key) in point_lookup.items():
                if lp == info_pid and entities[idx]["type"] == "circle":
                    entities[idx]["radius"] = w[0] * MM
    # Arcs: snapped endpoints re-derive the angles (the stored angles were
    # in the stale frame); radius follows the snapped points.
    for idx, pts in arc_pts.items():
        e = entities[idx]
        c = e["center"]
        if "start" in pts and "end" in pts:
            r = (math.dist(c, pts["start"]) + math.dist(c, pts["end"])) / 2
            e["radius"] = r
            e["start_angle"] = math.atan2(pts["start"][1] - c[1],
                                          pts["start"][0] - c[0])
            e["end_angle"] = math.atan2(pts["end"][1] - c[1],
                                        pts["end"][0] - c[0])
    return moved


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
                # End ownership: the ORIGINAL end point b now lives on the
                # appended half — but only re-point entities whose end is
                # currently owned by the segment we just split. (A line split
                # twice keeps its true end on the FIRST appended half; the
                # second split shortens the original again and must not
                # steal ownership back.)
                for info in id_map.values():
                    owner = info["end_idx"] if info["end_idx"] is not None \
                        else info["idx"]
                    if owner == i:
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
    pairs = [(pts2d[k], pts3d[k]) for k in pts2d
             if k in pts3d and len(pts3d[k]) == 3]
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
        if kind == "arc" and info.get("swapped") and suffix in ("start", "end"):
            which = 3 - which  # clockwise source: endpoints swapped in ours
        if kind == "line" and suffix == "end" and info["end_idx"] is not None:
            return [info["end_idx"], 1]
        return [info["idx"], which]
    return None


CONSTRAINT_KINDS = {
    "COINCIDENT", "PARALLEL", "PERPENDICULAR", "EQUAL", "TANGENT",
    "CONCENTRIC", "MIDPOINT", "HORIZONTAL", "VERTICAL", "LENGTH",
    "DISTANCE", "RADIUS", "DIAMETER", "ANGLE",
}


def _classify_ref(pid, id_map):
    """→ (ref [idx, which], is_point, kind) or None. A bare entity id is the
    whole curve; suffixed ids are its points."""
    r = _resolve_ref(pid, id_map)
    if r is None:
        return None
    if pid in id_map:
        kind = id_map[pid]["kind"]
        return r, kind == "point", kind
    base = pid.rpartition(".")[0]
    return r, True, id_map[base]["kind"]


def map_constraint(con, id_map, entities=None):
    """→ (payload for add_constraint | None, note | None), with STRICT
    arity/kind rules: Onshape constraints that reference external model
    geometry arrive with a ref missing — sending a partial constraint makes
    the solver "satisfy" it by MOVING geometry, which is far worse than
    skipping. (The drift check downstream guards this invariant.)"""
    ctype = con.get("constraintType")
    if ctype not in CONSTRAINT_KINDS:
        return None, f"constraint {ctype} skipped"
    params = {p.get("parameterId"): p for p in con.get("parameters", [])}

    def quantity(payload, kind):
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
                return

    # Gather present local refs in parameter order.
    refs = []
    for key in ("localFirst", "localSecond", "localThird", "localMidpoint",
                "localEntity1", "localEntity2"):
        p = params.get(key)
        if p and p.get("value"):
            c = _classify_ref(p["value"], id_map)
            if c is None:
                return None, f"{ctype} ref '{p['value']}' unresolved"
            refs.append((key, c))

    def payload(kind, reflist):
        out = {"kind": kind, "refs": [r for r, _, _ in reflist]}
        quantity(out, kind)
        return out, None

    if ctype == "COINCIDENT":
        if len(refs) != 2:
            return None, "COINCIDENT with an external ref skipped"
        (_, a), (_, b) = refs
        if a[1] and b[1]:
            return payload("coincident", [a, b])
        if a[1] != b[1]:  # one point + one whole curve = on-curve
            point, entity = (a, b) if a[1] else (b, a)
            return payload("point_on_line", [point, entity])
        return None, "COINCIDENT (curve-curve form) skipped"
    if ctype == "DISTANCE":
        if len(refs) != 2:
            return None, "DISTANCE with an external ref skipped"
        (_, a), (_, b) = refs
        if a[1] and b[1]:
            return payload("distance", [a, b])
        if a[1] != b[1] and (a[2] == "line" or b[2] == "line"):
            point, line = (a, b) if a[1] else (b, a)
            return payload("distance_pl", [point, line])
        return None, "DISTANCE (curve-curve form) skipped"
    if ctype == "MIDPOINT":
        # Ours: a point AT a line's midpoint. Onshape's two-point form and
        # virtual ".midpoint" references don't map.
        if len(refs) == 2:
            (_, a), (_, b) = refs
            if a[1] and not b[1] and b[2] == "line":
                # A line split at a T-junction is no longer the line whose
                # midpoint Onshape meant.
                line_pid = params.get("localEntity2", params.get(
                    "localSecond", {})).get("value")
                info = id_map.get(line_pid)
                if info is not None and info.get("end_idx") is not None:
                    return None, "MIDPOINT on a split line skipped"
                return payload("midpoint", [a, b])
        return None, "MIDPOINT (unsupported form) skipped"
    if ctype in ("HORIZONTAL", "VERTICAL"):
        kind = ctype.lower()
        if len(refs) == 1 and not refs[0][1][1] and refs[0][1][2] == "line":
            return payload(kind, [refs[0][1]])
        if len(refs) == 2 and refs[0][1][1] and refs[1][1][1]:
            return payload(kind, [refs[0][1], refs[1][1]])
        return None, f"{ctype} (unsupported form) skipped"
    if ctype == "LENGTH":
        line = params.get("localFirst", {}).get("value")
        if line not in id_map or id_map[line]["kind"] != "line":
            return None, "LENGTH target unresolved"
        info = id_map[line]
        end = [info["end_idx"], 1] if info["end_idx"] is not None \
            else [info["idx"], 1]
        out = {"kind": "distance", "refs": [[info["idx"], 0], end]}
        quantity(out, "distance")
        return out, None
    if ctype in ("RADIUS", "DIAMETER"):
        if len(refs) == 1 and refs[0][1][2] in ("circle", "arc"):
            return payload(ctype.lower(), [refs[0][1]])
        return None, f"{ctype} target unresolved"
    if ctype == "ANGLE":
        if len(refs) == 2 and all(r[2] == "line" and not r[1]
                                  for _, r in refs):
            out, note = payload("angle", [refs[0][1], refs[1][1]])
            # Onshape's angle convention differs from PlaneGCS's signed
            # line1->line2 angle. The geometry is already at truth (snapped),
            # so for literal values MEASURE the angle the way our solver
            # does and use that — the dimension then holds the drawn state.
            # A units-only expression ("11 deg" -> "11") is still a
            # literal — only variable-driven angles keep their expression.
            if out is not None and "expr" in out and \
                    not re.search(r"[A-Za-z_]", out["expr"]):
                out.pop("expr")
            # Onshape's angle value (its convention + the supplementary forms
            # it stores, e.g. "180 - feet_angle") does NOT equal PlaneGCS's
            # signed line1->line2 angle, and the relationship isn't a simple
            # sign flip — a mistranslated angle rotates the line into a
            # bowtie. The geometry is already at truth (snapped), so MEASURE
            # the exact signed angle our solver wants and use it as a literal.
            # (Trade-off: that one angle stops tracking its variable; correct
            # geometry beats a parametric angle that solves wrong.)
            if out is not None and entities is not None:
                import math
                e1 = entities[out["refs"][0][0]]
                e2 = entities[out["refs"][1][0]]
                if e1.get("type") == "line" and e2.get("type") == "line":
                    d1 = (e1["b"][0] - e1["a"][0], e1["b"][1] - e1["a"][1])
                    d2 = (e2["b"][0] - e2["a"][0], e2["b"][1] - e2["a"][1])
                    cross = d1[0] * d2[1] - d1[1] * d2[0]
                    dot = d1[0] * d2[0] + d1[1] * d2[1]
                    out.pop("expr", None)
                    out["value"] = math.degrees(math.atan2(cross, dot))
            return out, note
        return None, "ANGLE (unsupported form) skipped"
    # parallel / perpendicular / equal / tangent / concentric: two curves.
    if len(refs) == 2 and not refs[0][1][1] and not refs[1][1][1]:
        return payload(ctype.lower(), [refs[0][1], refs[1][1]])
    return None, f"{ctype} (unsupported form) skipped"


# ---- features -------------------------------------------------------------

def _params_by_id(feature):
    return {p.get("parameterId"): p for p in feature.get("parameters", [])}


def _query_strings(param):
    return " ".join(q.get("queryString") or "" for q in
                    (param or {}).get("queries", []))


# Onshape feature types that produce a free 3D curve. Designer models these
# as Curve3D (format v42) — see docs/modeling/curves-3d-plan.md.
_CURVE_FEATURE_TYPES = ("fitSpline",)


def map_fit_spline(feature, points):
    """→ (designer Curve3D dict, notes). None when the form can't map.

    `points` are world-mm samples along the curve Onshape built (see
    Client.curve_sample_points for why the result is measured rather than
    the through-point query resolved). Designer re-interpolates them, which
    reproduces the same curve: a fit spline through a dense sample of itself
    is that spline, to within the sample spacing."""
    notes = []
    if not points:
        return None, ["fitSpline curve points unresolved"]
    if len(points) < 2:
        return None, [f"fitSpline has {len(points)} point(s), needs 2"]
    if any(len(pt) != 3 for pt in points):
        return None, ["fitSpline points are not 3D"]
    # isPeriodic is Onshape's name for a closed loop. Absent (an older
    # document, or a form that does not offer it) reads as open, which is
    # the overwhelmingly common case and is what the geometry shows anyway.
    closed = bool(_param_val(feature, "isPeriodic"))
    if closed:
        # A periodic sample repeats its first point last; Designer's `closed`
        # flag re-closes it, so leaving the duplicate in would pin a zero-
        # length span through the seam.
        if len(points) > 2 and _same_point(points[0], points[-1]):
            points = points[:-1]
        notes.append("closed (periodic) spline")
    out = {"type": "Curve3D", "name": feature.get("name", "Curve"),
           "kind": "fit_spline",
           "points": [[round(c, 6) for c in pt] for pt in points],
           "degree": 3, "closed": closed}
    return out, notes


def _same_point(a, b, tol=1e-6):
    return all(abs(x - y) < tol for x, y in zip(a, b))


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
    def __init__(self, source, port):
        from designer_remote import Designer
        self.src = source
        self.designer = Designer(port)
        self.report = Report()
        self.sketch_ids = {}    # onshape sketch featureId -> designer id
        self.sketch_entities = {}  # featureId -> {onshape eid: designer eid}
        self.sketch_planes = {}    # featureId -> plane dict (mm)
        self.solid_ids = {}        # solid featureId -> designer feature id
        self.extrudes = {}         # featureId -> {plane, distance, reversed}
        self.vars = {}             # variable name -> value (mm)

    # ---- query helpers (all local: qCompressed is base64+zlib) ----------

    @staticmethod
    def query_text(param):
        """Every queryString of a query-list parameter, decompressed where
        Onshape compressed it — readable operationIds / sketchEntityIds."""
        texts = []
        for q in (param or {}).get("queries", []):
            qs = q.get("queryString") or ""
            texts.append(qs)
            m = re.search(r'qCompressed\(1\.0,"(.+?)",id\)', qs)
            if m:
                body = re.sub(r"^[&%][0-9a-zA-Z]*\$", "", m.group(1))
                try:
                    import zlib
                    texts.append(zlib.decompress(
                        base64.b64decode(body)).decode("latin1"))
                except Exception:
                    pass
        return " ".join(texts)

    def sketches_in(self, param, sketch_features):
        text = self.query_text(param)
        return [f for f in sketch_features if f["featureId"] in text]

    def entity_refs_in(self, param, sketch_feature):
        """Designer entity ids of this sketch referenced by the query."""
        text = self.query_text(param)
        table = self.sketch_entities.get(sketch_feature["featureId"], {})
        return [did for oid, did in table.items() if oid in text]

    # ---- feature imports ------------------------------------------------

    def import_variables(self):
        count = 0
        for el in self.src.elements():
            if el.get("elementType") != "VARIABLESTUDIO":
                continue
            for scope in self.src.variables(el["id"]):
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
        try:
            for v in self.designer.call("list_variables").get("variables", []):
                self.vars[v.get("name")] = v.get("value", 0.0)
        except Exception:
            pass

    def eval_expr(self, expr, default=0.0):
        """Evaluate a TRANSLATED (mm/deg) expression with document
        variables — arithmetic only."""
        if expr is None:
            return default
        try:
            return float(eval(expr, {"__builtins__": {}}, dict(self.vars)))
        except Exception:
            return default

    def face_plane(self, feature):
        """Plane of a face-attached sketch: the query names the host
        feature's cap face — for an imported extrude that plane is the
        profile plane offset by the (evaluated) distance."""
        p = _params_by_id(feature)
        text = self.query_text(p.get("sketchPlane"))
        for fid, info in self.extrudes.items():
            if fid not in text:
                continue
            base = info["plane"]
            is_start = bool(re.search(r"isStart.{0,3}T", text))
            offset = 0.0 if is_start else info["distance"]
            if info.get("reversed"):
                offset = -offset
            n = base["normal"]
            return {"origin": [base["origin"][i] + n[i] * offset
                               for i in range(3)],
                    "normal": n, "uAxis": base["uAxis"]}
        return None

    def import_sketch(self, feature, world_points):
        name = feature.get("name", "Sketch")
        entities, id_map, pts2d, point_lookup, notes = \
            map_sketch_entities(feature)
        if not entities:
            self.report.add(name, "skipped", "no mappable entities")
            return
        world = world_points.get(feature["featureId"], {})
        plane = fit_plane(pts2d, world)
        if plane is None:
            # Few 2D-3D correspondences (a face-attached hole sketch): use
            # Onshape's GROUND-TRUTH plane frame — its normal direction is
            # unambiguous, so face cuts go the right way (a least-squares
            # fit or a host-face guess can't tell a normal from its flip).
            tp = self.src.true_plane(feature["featureId"])
            if tp is not None:
                plane = {"origin": tp["origin"], "normal": tp["normal"],
                         "uAxis": tp["xAxis"]}
                notes.append("plane from Onshape sketch frame")
            else:
                plane = self.face_plane(feature)
                if plane is None:
                    self.report.add(name, "skipped", "sketch plane unresolved "
                                    "(too few 2D-3D correspondences)")
                    return
                plane, pairs = align_plane_frame(entities, point_lookup,
                                                 world, plane)
                if plane is None:
                    self.report.add(name, "skipped",
                                    "sketch frame could not be aligned")
                    return
                notes.append(f"plane from host face, aligned by {pairs} pt(s)")
        snapped = snap_to_world(entities, point_lookup, world, plane)
        if snapped > 0.05:
            notes.append(f"stored sketch lagged current values by "
                         f"{snapped:.2f} mm (snapped to world)")
        notes += close_open_profiles(entities, id_map)
        resp = self.designer.call("add_feature", feature={
            "type": "Sketch", "name": name, "plane": plane,
            "entities": entities})
        self.sketch_ids[feature["featureId"]] = resp["id"]
        self.sketch_planes[feature["featureId"]] = plane
        designer_ids = resp["entity_ids"]
        self.sketch_entities[feature["featureId"]] = {
            oid: designer_ids[info["idx"]] for oid, info in id_map.items()}

        payloads, kinds = [], []
        for con in feature.get("constraints", []):
            payload, note = map_constraint(con, id_map, entities)
            if payload is None:
                notes.append(note)
                continue
            payload["refs"] = [[designer_ids[idx], which]
                               for idx, which in payload["refs"]]
            payloads.append(payload)
            kinds.append(con.get("constraintType"))
        # Fast path: apply every constraint in ONE call (one solve + regen
        # instead of one per constraint). Fall back to per-constraint only
        # when the batch fails, to isolate and report the offender.
        applied = failed = 0
        try:
            if payloads:
                self.designer.call("add_constraints", sketch_id=resp["id"],
                                   constraints=payloads)
            applied = len(payloads)
        except Exception:
            for payload, kind in zip(payloads, kinds):
                try:
                    self.designer.call("add_constraint",
                                       sketch_id=resp["id"], **payload)
                    applied += 1
                except Exception as e:
                    failed += 1
                    notes.append(f"{kind}: {str(e).splitlines()[0][:60]}")

        # Drift check (local RPC): Onshape handed us SOLVED geometry, so a
        # partial constraint set must not move anything. Movement = a
        # mistranslated constraint (wrong refs or value) — surface it.
        drift = self._sketch_drift(resp["id"], entities)
        if drift is not None and drift > 0.01:
            notes.append(f"solver moved geometry {drift:.2f} mm - check "
                         "constraint translation")
        detail = f"{len(entities)} entities, {applied} constraints"
        if failed or notes:
            uniq = sorted(set(n for n in notes if n))
            detail += "; " + "; ".join(uniq[:4])
            self.report.add(name, "partial", detail)
        else:
            self.report.add(name, "mapped", detail)

    def _sketch_drift(self, sketch_id, sent_entities):
        try:
            state = self.designer.call("get_state")
            feat = next((f for f in state.get("features", [])
                         if f.get("id") == sketch_id), None)
            got = feat.get("entities", []) if feat else []
            worst = 0.0
            for sent, cur in zip(sent_entities, got):
                if sent.get("construction"):
                    continue  # construction geometry resizing isn't drift
                for key in ("a", "b", "p", "center"):
                    if key in sent and key in cur:
                        dx = sent[key][0] - cur[key][0]
                        dy = sent[key][1] - cur[key][1]
                        worst = max(worst, (dx * dx + dy * dy) ** 0.5)
            return worst
        except Exception:
            return None

    def _add_solid(self, name, mapped, notes):
        try:
            resp = self.designer.call("add_feature", feature=mapped)
        except Exception as e:
            self.report.add(name, "skipped", str(e).splitlines()[0][:80])
            return None
        self.report.add(name, "partial" if notes else "mapped",
                        "; ".join(notes))
        return resp.get("id")

    def import_extrude(self, feature, sketch_features, feature_index=None):
        name = feature.get("name", "Extrude")
        p = _params_by_id(feature)
        op = p.get("operationType", {}).get("value", "NEW")
        mode = {"NEW": "new_body", "ADD": "add", "REMOVE": "cut"}.get(op)
        if mode is None:
            self.report.add(name, "skipped", f"operation {op} unsupported")
            return
        matches = self.sketches_in(p.get("entities"), sketch_features)
        if len(matches) != 1:
            self.report.add(name, "skipped", "profile sketch unresolved")
            return
        sketch_id = self.sketch_ids.get(matches[0]["featureId"])
        if sketch_id is None:
            self.report.add(name, "skipped", "profile sketch was not imported")
            return
        notes = []
        out = {"type": "Extrude", "name": name, "mode": mode,
               "sketch_id": sketch_id}
        depth = p.get("depth", {})
        expr = translate_expression(depth.get("expression"))
        if expr is not None:
            out["distance_expr"] = expr
            out["distance"] = 1.0
        else:
            out["distance"] = depth.get("value", 0.0) * MM
        bound = p.get("endBound", {}).get("value", "BLIND")
        plane = self.sketch_planes.get(matches[0]["featureId"])
        if bound == "SYMMETRIC" or p.get("midplane", {}).get("value"):
            out["end_mode"] = "symmetric"
        elif bound != "BLIND":
            # Up-to / through: Onshape's target-face query doesn't resolve at
            # rollback, so measure the true depth from the vertices the
            # extrude created (span along the sketch normal). A blind extrude
            # of that depth reproduces the geometry (loses only the
            # parametric follows-the-target link).
            span = self._up_to_depth(feature_index, plane)
            if span is not None:
                out.pop("distance_expr", None)
                out["distance"] = span["depth"]
                out["reversed"] = span["reversed"]
                notes.append(f"{bound.lower().replace('_','-')} measured as "
                             f"{span['depth']:.1f}mm blind")
            elif mode == "cut":
                # A cut creates no new vertices to measure. A SYMMETRIC
                # over-long cut removes only the material it overlaps and
                # can't miss the panel by going the wrong way (a one-sided
                # blind cut does, when the sketch normal is ambiguous) —
                # correct for through-all and the common up-to-next panel.
                out.pop("distance_expr", None)
                out["distance"] = 10000.0
                out["end_mode"] = "symmetric"
                notes.append(f"{bound.lower().replace('_','-')} cut "
                             "approximated as through-all")
            else:
                self.report.add(name, "skipped",
                                f"end bound {bound}: could not measure depth")
                return
        if bound in ("BLIND", "SYMMETRIC") and p.get(
                "oppositeDirection", {}).get("value"):
            out["reversed"] = True
        if p.get("hasSecondDirection", {}).get("value"):
            notes.append("second direction dropped")
        if p.get("hasDraft", {}).get("value"):
            notes.append("draft dropped")
        did = self._add_solid(name, out, notes)
        if did is not None:
            self.solid_ids[feature["featureId"]] = did
            dist = self.eval_expr(out.get("distance_expr"),
                                  out.get("distance", 0.0))
            self.extrudes[feature["featureId"]] = {
                "plane": self.sketch_planes.get(matches[0]["featureId"]),
                "distance": dist,
                "reversed": out.get("reversed", False),
                "extrude": out,             # designer Extrude dict (sans sketch)
                "sketch_feature": matches[0]}  # onshape sketch feature

    def _up_to_depth(self, feature_index, plane):
        """Depth (mm) + reversed flag of an up-to/through extrude, from the
        span of its created vertices along the sketch normal. None when no
        vertices are available."""
        if feature_index is None or plane is None:
            return None
        verts = self.src.up_to_verts(feature_index)
        if not verts:
            return None
        o, n = plane["origin"], plane["normal"]
        dists = [(v[0] - o[0]) * n[0] + (v[1] - o[1]) * n[1]
                 + (v[2] - o[2]) * n[2] for v in verts]
        lo, hi = min(dists), max(dists)
        # The profile sits at ~0; the extrude reaches to lo or hi. Take the
        # larger magnitude as the depth, its sign as the direction.
        far = hi if abs(hi) >= abs(lo) else lo
        if abs(far) < 1e-6:
            return None
        return {"depth": abs(far), "reversed": far < 0}

    def import_revolve(self, feature, sketch_features):
        name = feature.get("name", "Revolve")
        p = _params_by_id(feature)
        op = p.get("operationType", {}).get("value", "NEW")
        mode = {"NEW": "new_body", "ADD": "add", "REMOVE": "cut"}.get(op)
        if mode is None:
            self.report.add(name, "skipped", f"operation {op} unsupported")
            return
        profiles = self.sketches_in(p.get("entities"), sketch_features)
        if len(profiles) != 1:
            self.report.add(name, "skipped", "profile sketch unresolved")
            return
        sketch_id = self.sketch_ids.get(profiles[0]["featureId"])
        # Axis: a line of some imported sketch, named in the axis query.
        axis = None
        for sk in self.sketches_in(p.get("axis"), sketch_features):
            for did in self.entity_refs_in(p.get("axis"), sk):
                axis = {"kind": "sketch_line",
                        "sketch_id": self.sketch_ids.get(sk["featureId"]),
                        "entity_id": did}
                break
        if sketch_id is None or axis is None or axis["sketch_id"] is None:
            self.report.add(name, "skipped", "axis or profile unresolved")
            return
        notes = []
        out = {"type": "Revolve", "name": name, "mode": mode,
               "sketch_id": sketch_id, "axis": axis}
        if p.get("fullRevolve", {}).get("value", True):
            out["angle_deg"] = 360.0
        else:
            ang = p.get("angle", {})
            expr = translate_expression(ang.get("expression"))
            if expr is not None:
                out["angle_expr"] = expr
                out["angle_deg"] = 360.0
            else:
                import math
                out["angle_deg"] = ang.get("value", 0.0) * 180.0 / math.pi
        if p.get("oppositeDirection", {}).get("value"):
            out["reversed"] = True
        did = self._add_solid(name, out, notes)
        if did is not None:
            self.solid_ids[feature["featureId"]] = did

    def import_loft(self, feature, sketch_features):
        name = feature.get("name", "Loft")
        p = _params_by_id(feature)
        op = p.get("operationType", {}).get("value", "NEW")
        mode = {"NEW": "new_body", "ADD": "add", "REMOVE": "cut"}.get(op)
        arr = p.get("sheetProfilesArray", {})
        sketch_ids = []
        for item in arr.get("items", []):
            for ip in item.get("parameters", []):
                if ip.get("btType") != "BTMParameterQueryList-148":
                    continue
                matches = self.sketches_in(ip, sketch_features)
                if len(matches) == 1:
                    did = self.sketch_ids.get(matches[0]["featureId"])
                    if did is not None and did not in sketch_ids:
                        sketch_ids.append(did)
        if mode is None or len(sketch_ids) < 2:
            self.report.add(name, "skipped",
                            f"needs 2+ resolved profile sketches "
                            f"(got {len(sketch_ids)})")
            return
        notes = []
        if arr and len(arr.get("items", [])) != len(sketch_ids):
            notes.append(f"{len(arr.get('items', []))} profiles in Onshape, "
                         f"{len(sketch_ids)} resolved")
        if p.get("addGuides", {}).get("value"):
            notes.append("guide curves dropped")
        did = self._add_solid(name, {"type": "Loft", "name": name,
                                     "mode": mode,
                                     "sketch_ids": sketch_ids}, notes)
        if did is not None:
            self.solid_ids[feature["featureId"]] = did

    def import_sweep(self, feature, sketch_features):
        name = feature.get("name", "Sweep")
        p = _params_by_id(feature)
        op = p.get("operationType", {}).get("value", "NEW")
        mode = {"NEW": "new_body", "ADD": "add", "REMOVE": "cut"}.get(op)
        profiles = self.sketches_in(p.get("profiles"), sketch_features)
        paths = self.sketches_in(p.get("path"), sketch_features)
        paths = [f for f in paths if f not in profiles] or paths
        if mode is None or len(profiles) != 1 or len(paths) != 1:
            self.report.add(name, "skipped", "profile or path unresolved")
            return
        profile_id = self.sketch_ids.get(profiles[0]["featureId"])
        path_id = self.sketch_ids.get(paths[0]["featureId"])
        if profile_id is None or path_id is None:
            self.report.add(name, "skipped", "profile or path not imported")
            return
        did = self._add_solid(name, {"type": "Sweep", "name": name,
                                     "mode": mode, "profile_id": profile_id,
                                     "path_id": path_id}, [])
        if did is not None:
            self.solid_ids[feature["featureId"]] = did

    # ---- mirror (feature replay) ---------------------------------------

    BASE_PLANES = {  # query token -> (point mm, unit normal)
        "Frontplane": ((0, 0, 0), (0, 0, 1)),
        "Topplane": ((0, 0, 0), (0, 1, 0)),
        "Rightplane": ((0, 0, 0), (1, 0, 0)),
    }

    def mirror_plane_geom(self, param):
        """The mirror plane as (point mm, unit normal). Base planes only for
        now; a datum/face mirror plane returns None (reported)."""
        text = self.query_text(param)
        for token, geom in self.BASE_PLANES.items():
            if token in text:
                return geom
        return None

    @staticmethod
    def _reflect(v, p0, n, is_point):
        base = [v[i] - p0[i] for i in range(3)] if is_point else list(v)
        d = 2 * (base[0] * n[0] + base[1] * n[1] + base[2] * n[2])
        r = [base[i] - d * n[i] for i in range(3)]
        return [r[i] + (p0[i] if is_point else 0) for i in range(3)]

    def reflected_plane(self, plane, p0, n):
        return {"origin": self._reflect(plane["origin"], p0, n, True),
                "normal": self._reflect(plane["normal"], p0, n, False),
                "uAxis": self._reflect(plane["uAxis"], p0, n, False)}

    @staticmethod
    def reflect_entities_v(entities):
        """Reflection negates the sketch V axis (V = N x U flips under a
        reflection), so the SAME 2D coords on the reflected plane land at the
        mirrored world point once every v is negated. Arcs additionally
        reverse winding (angles negate, start/end swap)."""
        import copy
        out = []
        for e in entities:
            e = copy.deepcopy(e)
            for key in ("a", "b", "p", "center"):
                if key in e:
                    e[key] = [e[key][0], -e[key][1]]
            if e.get("type") == "arc":
                s, en = -e["start_angle"], -e["end_angle"]
                e["start_angle"], e["end_angle"] = en, s  # swap: winding flips
            out.append(e)
        return out

    def import_mirror(self, feature):
        name = feature.get("name", "Mirror")
        p = _params_by_id(feature)
        if p.get("patternType", {}).get("value") != "FEATURE":
            self.report.add(name, "skipped", "only feature-mirror supported")
            return
        geom = self.mirror_plane_geom(p.get("mirrorPlane"))
        if geom is None:
            self.report.add(name, "skipped",
                            "mirror plane is not a base plane (datum/face "
                            "mirror needs datum tracking)")
            return
        p0, n = geom
        inst = next((pp for pp in feature["parameters"]
                     if pp.get("parameterId") == "instanceFunction"), {})
        fids = inst.get("featureIds", [])
        done = notes = 0
        skipped = []
        for fid in fids:
            spec = self.extrudes.get(fid)
            if spec is None:
                skipped.append("a mirrored feature was not an imported "
                               "extrude")
                continue
            # Reflect the source sketch and re-apply the same extrude op.
            plane = self.reflected_plane(spec["plane"], p0, n)
            src_feat = spec["sketch_feature"]
            ents, id_map, pts2d, point_lookup, _ = \
                map_sketch_entities(src_feat)
            world = self.src.sketch_world_points().get(
                src_feat["featureId"], {})
            # Snap to truth on the ORIGINAL plane, then reflect the coords.
            snap_to_world(ents, point_lookup, world, spec["plane"])
            ents = self.reflect_entities_v(ents)
            close_open_profiles(ents, id_map)
            try:
                sk = self.designer.call("add_feature", feature={
                    "type": "Sketch", "name": f"{name}: {src_feat.get('name')}",
                    "plane": plane, "entities": ents})
            except Exception as e:
                skipped.append(str(e).splitlines()[0][:50])
                continue
            ex = dict(spec["extrude"])
            ex.pop("_sketch", None)
            ex["name"] = f"{name}: {src_feat.get('name')}"
            ex["sketch_id"] = sk["id"]
            try:
                self.designer.call("add_feature", feature=ex)
                done += 1
            except Exception as e:
                skipped.append(str(e).splitlines()[0][:50])
        if done and not skipped:
            self.report.add(name, "mapped", f"replayed {done} mirrored "
                            f"feature(s) across the plane")
        elif done:
            self.report.add(name, "partial",
                            f"replayed {done}; " + "; ".join(sorted(set(skipped))[:3]))
        else:
            self.report.add(name, "skipped", "; ".join(sorted(set(skipped))[:3])
                            or "no mirrored features resolved")

    def _match_faces(self, centroids, tol=1.0):
        """Match Onshape face centroids (mm) to the current Designer body's
        faces by centroid. Returns {created_by: [face name, ...]} for the
        best-matching body (faces must be on one body for shell/draft)."""
        try:
            bodies = self.designer.call("list_elements").get("bodies", [])
        except Exception:
            return {}
        by_body = {}
        for m in centroids:
            best = None
            for b in bodies:
                for f in b.get("faces", []):
                    q = f.get("centroid")
                    if not q:
                        continue
                    d = ((m[0] - q[0]) ** 2 + (m[1] - q[1]) ** 2
                         + (m[2] - q[2]) ** 2) ** 0.5
                    if best is None or d < best[0]:
                        best = (d, b.get("created_by"), f.get("name"))
            if best and best[0] < tol:
                by_body.setdefault(best[1], []).append(best[2])
        return by_body

    def import_shell(self, feature, feature_index):
        """Shell: the opened faces (present before, collapsed to a rim after)
        matched to the Designer body they're on, applied as a Shell with
        those open faces + the wall thickness."""
        name = feature.get("name", "Shell")
        p = _params_by_id(feature)
        cents = self.src.face_diff(feature_index)
        if not cents:
            self.report.add(name, "skipped",
                            "could not resolve the opened faces")
            return
        by_body = self._match_faces(cents)
        if not by_body:
            self.report.add(name, "skipped",
                            f"none of {len(cents)} opened faces matched")
            return
        created_by, names = max(by_body.items(), key=lambda kv: len(kv[1]))
        out = {"type": "Shell", "name": name, "body_id": created_by,
               "open_faces": [{"feature_id": created_by, "element": n}
                              for n in sorted(set(names))]}
        thick = p.get("thickness", {})
        expr = translate_expression(thick.get("expression"))
        if expr is not None:
            out["thickness_expr"] = expr
            out["thickness"] = 1.0
        else:
            out["thickness"] = thick.get("value", 0.001) * MM
        err = self._add_and_check(out)
        if err is None:
            self.report.add(name, "mapped",
                            f"{len(out['open_faces'])} open face(s)")
        else:
            self.report.add(name, "skipped", err.splitlines()[0][:70])

    NEUTRAL_PLANES = {  # unit normal -> Designer base plane
        (0, 0, 1): "xy", (0, 0, -1): "xy", (0, 1, 0): "xz", (0, -1, 0): "xz",
        (1, 0, 0): "yz", (-1, 0, 0): "yz"}

    def import_draft(self, feature, feature_index):
        """Draft: the tapered faces (normal tilts after) matched to their
        Designer body, applied with the neutral base plane + angle."""
        name = feature.get("name", "Draft")
        p = _params_by_id(feature)
        cents = self.src.face_diff(feature_index)
        if not cents:
            self.report.add(name, "skipped",
                            "could not resolve the drafted faces")
            return
        by_body = self._match_faces(cents)
        if not by_body:
            self.report.add(name, "skipped", "no drafted face matched")
            return
        created_by, names = max(by_body.items(), key=lambda kv: len(kv[1]))
        # Neutral plane: resolve its normal to a base plane. The query
        # doesn't resolve at rollback, so read it from the true-plane FS via
        # the neutralPlane's own face; fall back to xy.
        base = "xy"
        npn = self.src.neutral_normal(feature_index) \
            if hasattr(self.src, "neutral_normal") else None
        if npn:
            key = tuple(round(x) for x in npn)
            base = self.NEUTRAL_PLANES.get(key, "xy")
        out = {"type": "Draft", "name": name, "body_id": created_by,
               "neutral_plane": base,
               "faces": [{"feature_id": created_by, "element": n}
                         for n in sorted(set(names))]}
        ang = p.get("angle", {})
        expr = translate_expression(ang.get("expression"))
        if expr is not None:
            out["angle_expr"] = expr
            out["angle_deg"] = 5.0
        else:
            import math
            out["angle_deg"] = math.degrees(ang.get("value", 0.0))
        notes = []
        if npn is None:
            notes.append("neutral plane assumed xy")
        err = self._add_and_check(out)
        if err is None:
            self.report.add(name, "partial" if notes else "mapped",
                            "; ".join(notes) or f"{len(out['faces'])} face(s)")
        else:
            self.report.add(name, "skipped", err.splitlines()[0][:70])

    def import_thread(self, feature, feature_index):
        """External thread: the cylindrical face it split, matched to the
        Designer body, applied as a modeled Thread of the given depth."""
        name = feature.get("name", "Thread")
        p = _params_by_id(feature)
        cents = self.src.face_diff(feature_index)
        if not cents:
            self.report.add(name, "skipped",
                            "could not resolve the threaded face")
            return
        by_body = self._match_faces(cents)
        if not by_body:
            self.report.add(name, "skipped", "threaded face did not match")
            return
        created_by, names = max(by_body.items(), key=lambda kv: len(kv[1]))
        # One cylindrical face; pick the first match. Designer only does
        # COSMETIC threads (no cut geometry), so record it as such — the
        # thread grooves the modeled Onshape thread carries are dropped.
        out = {"type": "Thread", "name": name,
               "face": {"feature_id": created_by, "element": names[0]},
               "modeled": False, "pitch": 1.25}
        depth = p.get("threadDepth", {})
        expr = translate_expression(depth.get("expression"))
        if expr is not None:
            out["length_expr"] = expr
            out["length"] = 10.0
        else:
            out["length"] = depth.get("value", 0.0) * MM
        err = self._add_and_check(out)
        if err is None:
            self.report.add(name, "partial",
                            "cosmetic thread (Designer has no modeled thread)")
        else:
            self.report.add(name, "skipped", err.splitlines()[0][:60])

    def import_fit_spline(self, feature, feature_index):
        """A 3D fit spline → Designer's Curve3D (format v42).

        Until 2026-08-25 this branch reported a skip claiming Designer had no
        3D curve type, which had been false since v42. The skip outlived the
        gap it guarded because its stated reason still read plausible, so
        nobody re-read it — docs/loose-ends-2026-08-25.md L1."""
        name = feature.get("name", "Curve")
        pts = self.src.curve_points(feature_index)
        out, notes = map_fit_spline(feature, pts)
        if out is None:
            self.report.add(name, "skipped", "; ".join(notes))
            return
        err = self._add_and_check(out)
        if err is None:
            self.report.add(name, "mapped", "; ".join(notes) or
                            f"{len(out['points'])} fit points")
        else:
            self.report.add(name, "skipped", err.splitlines()[0][:60])

    def import_fillet(self, feature, feature_index):
        """Fillet/chamfer: resolve the consumed (blended) edges by geometric
        diff (Onshape queries don't resolve reliably), match them to the
        current Designer body's edges by midpoint, and apply the blend."""
        name = feature.get("name", feature.get("featureType", "Fillet"))
        ftype = feature.get("featureType")
        p = _params_by_id(feature)
        mids = self.src.consumed_edges(feature_index)
        if not mids:
            self.report.add(name, "skipped",
                            "could not resolve the blended edges "
                            "(no edge geometry)")
            return
        # Designer body edges, world mm — match each consumed midpoint.
        try:
            bodies = self.designer.call("list_elements").get("bodies", [])
        except Exception as e:
            self.report.add(name, "skipped", str(e).splitlines()[0][:60])
            return
        # Collect (created_by, edge name) per body for the nearest edge to
        # each consumed midpoint; keep the body that matched the most.
        by_body = {}
        for m in mids:
            best = None
            for b in bodies:
                for e in b.get("edges", []):
                    q = e.get("midpoint")
                    if not q:
                        continue
                    d = ((m[0] - q[0]) ** 2 + (m[1] - q[1]) ** 2
                         + (m[2] - q[2]) ** 2) ** 0.5
                    if best is None or d < best[0]:
                        best = (d, b.get("created_by"), e.get("name"))
            if best and best[0] < 0.5:  # within 0.5 mm
                by_body.setdefault(best[1], []).append(best[2])
        if not by_body:
            self.report.add(name, "skipped",
                            f"none of {len(mids)} blended edges matched a "
                            "Designer edge")
            return
        created_by, edge_names = max(by_body.items(), key=lambda kv: len(kv[1]))
        edge_names = sorted(set(edge_names))

        # Radius / chamfer distance: chamfers here are 45deg (width), fillets
        # a radius. Expression preferred so a variable keeps driving it.
        rad_param = p.get("width") if ftype == "chamfer" else p.get("radius")
        out = {"type": "Chamfer" if ftype == "chamfer" else "Fillet",
               "name": name,
               "edges": [{"feature_id": created_by, "element": e}
                         for e in edge_names]}
        expr = translate_expression((rad_param or {}).get("expression"))
        if expr is not None:
            out["radius_expr"] = expr
            out["radius"] = 1.0
        else:
            out["radius"] = (rad_param or {}).get("value", 0.001) * MM
        notes = []
        if len(edge_names) < len(mids):
            notes.append(f"{len(edge_names)}/{len(mids)} edges matched")
        if ftype == "chamfer" and p.get("chamferType", {}).get(
                "value") not in (None, "OFFSET_ANGLE", "EQUAL_OFFSETS"):
            notes.append(f"chamfer type {p['chamferType']['value']} "
                         "approximated as 45deg")

        err = self._add_and_check(out)
        if err is None:
            self.report.add(name, "partial" if notes else "mapped",
                            "; ".join(notes) or f"{len(edge_names)} edges")
            return
        self.designer.call("undo")  # remove the failed all-edges blend
        # OCC rejects the whole blend if ONE edge is too tight for the radius
        # (Onshape's kernel is more tolerant). Keep the edges that take the
        # blend individually — a partial blend beats a failed feature.
        good = []
        for en in edge_names:
            probe = dict(out)
            probe["edges"] = [{"feature_id": created_by, "element": en}]
            probe["name"] = f"{name} probe"
            probe_err = self._add_and_check(probe)
            self.designer.call("undo")  # revert the probe either way
            if probe_err is None:
                good.append(en)
        if not good:
            self.report.add(name, "skipped",
                            "no edge accepted the blend (" +
                            err.splitlines()[0][:50] + ")")
            return
        out["edges"] = [{"feature_id": created_by, "element": e} for e in good]
        final_err = self._add_and_check(out)
        if final_err is None:
            self.report.add(name, "partial",
                            f"{len(good)}/{len(edge_names)} edges took the "
                            "blend (rest too tight for the radius)")
        else:
            self.report.add(name, "skipped", final_err.splitlines()[0][:60])

    def _add_and_check(self, feature):
        """add_feature + return the feature's regen error string, or None on
        success (add_feature succeeds even when the feature errors)."""
        try:
            resp = self.designer.call("add_feature", feature=feature)
        except Exception as e:
            return str(e).splitlines()[0][:80]
        fid = resp.get("id")
        st = self.designer.call("get_state")
        f = next((x for x in st.get("features", []) if x.get("id") == fid),
                 None)
        return (f or {}).get("error") or None

    def import_boolean(self, feature):
        name = feature.get("name", "Boolean")
        p = _params_by_id(feature)
        op = {"UNION": "union", "SUBTRACTION": "subtract",
              "INTERSECTION": "intersect"}.get(
                  p.get("operationType", {}).get("value"))
        if op is None:
            self.report.add(name, "skipped", "boolean operation unsupported")
            return
        def resolve(param):
            text = self.query_text(param)
            return [did for fid, did in self.solid_ids.items() if fid in text]
        targets = resolve(p.get("targets"))
        tools = [d for d in resolve(p.get("tools")) if d not in targets]
        chain = targets + tools if op != "subtract" else targets + tools
        if op == "subtract" and (not targets or not tools):
            self.report.add(name, "skipped", "boolean bodies unresolved")
            return
        if len(chain) < 2:
            self.report.add(name, "skipped", "boolean bodies unresolved")
            return
        # Ours is pairwise: fold the tools onto the target left to right.
        current = chain[0]
        notes = []
        for i, tool in enumerate(chain[1:]):
            resp_id = self._add_solid(
                f"{name}" if i == len(chain) - 2 else f"{name} ({i + 1})",
                {"type": "Boolean", "name": name, "op": op,
                 "target_id": current, "tool_id": tool}, notes)
            if resp_id is None:
                return
            current = resp_id
        self.solid_ids[feature["featureId"]] = current

    def run(self):
        info = self.src.document_info()
        elements = {e["id"]: e for e in self.src.elements()}
        studio = elements.get(self.src.eid, {})
        tab = f"{studio.get('name', 'Onshape import')}"
        print(f"importing '{tab}' from '{info.get('name')}' "
              f"into a new part tab...")
        self.designer.call("add_part", name=tab)
        self.import_variables()

        feats = self.src.features().get("features", [])
        world = self.src.sketch_world_points()
        sketches = [f for f in feats if f.get("featureType") == "newSketch"]
        for fi, f in enumerate(feats):
            ftype, name = f.get("featureType"), f.get("name", "?")
            if f.get("suppressed"):
                self.report.add(name, "skipped", "suppressed in Onshape")
            elif ftype == "newSketch":
                self.import_sketch(f, world)
            elif ftype == "extrude":
                self.import_extrude(f, sketches, fi)
            elif ftype == "revolve":
                self.import_revolve(f, sketches)
            elif ftype == "loft":
                self.import_loft(f, sketches)
            elif ftype == "sweep":
                self.import_sweep(f, sketches)
            elif ftype == "booleanBodies":
                self.import_boolean(f)
            elif ftype == "mirror":
                self.import_mirror(f)
            elif ftype in ("fillet", "chamfer"):
                self.import_fillet(f, fi)
            elif ftype == "shell":
                self.import_shell(f, fi)
            elif ftype == "draft":
                self.import_draft(f, fi)
            elif ftype == "externalThread":
                self.import_thread(f, fi)
            elif ftype == "cPlane":
                self.report.add(name, "mapped",
                                "construction plane (baked into sketch planes)")
            elif ftype == "splitPart":
                # Designer has a Split feature, but Onshape's knife is a query
                # (a face/plane) that doesn't resolve at rollback — needs the
                # geometric measure-the-cut approach (like fillets). Deferred.
                self.report.add(name, "skipped",
                                "splitPart: knife query unresolved (Designer "
                                "Split exists; geometric knife mapping TODO)")
            elif ftype == "fitSpline":
                self.import_fit_spline(f, fi)
            else:
                self.report.add(name, "skipped",
                                f"feature type '{ftype}' not mapped yet")
        self.report.show()


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    cmd, arg = sys.argv[1], sys.argv[2]
    if cmd == "replay-dump":
        # replay-dump <dump.json> <eid-or-name> [port] — zero API calls.
        source = DumpSource(arg, sys.argv[3])
        port = int(sys.argv[4]) if len(sys.argv) > 4 else 8799
        Replayer(source, port).run()
        return 0
    client = Client()
    ref = parse_url(arg)
    if cmd == "info":
        cmd_info(client, ref)
    elif cmd == "elements":
        cmd_elements(client, ref)
    elif cmd == "features":
        cmd_features(client, ref)
    elif cmd == "dump":
        cmd_dump(client, ref, sys.argv[3])
    elif cmd == "replay":
        if not ref.eid:
            raise SystemExit("URL has no element id - pass a part studio")
        port = int(sys.argv[3]) if len(sys.argv) > 3 else 8799
        Replayer(LiveSource(client, ref), port).run()
    else:
        raise SystemExit(f"unknown command {cmd!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
