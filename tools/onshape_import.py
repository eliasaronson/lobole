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
    else:
        raise SystemExit(f"unknown command {cmd!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
