#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Check a tag against VERSION and the metainfo, and print its release notes.

    tools/release-notes.py v0.1.0     # markdown notes on stdout, or a refusal

The AppStream metainfo's <release> entries are the release notes: Flathub and
software centres show them, and the GitHub release takes the same text from
here, so there is one place to write them. A tag is refused when it is not
`v` + VERSION, when the metainfo's newest release is not that version, or
when that release has no <description> — a release with no notes is one
nobody can tell apart from the last.
"""
import pathlib
import sys
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parent.parent
METAINFO = ROOT / "packaging/org.lobole.Lobole.metainfo.xml"


def text(el):
    return " ".join("".join(el.itertext()).split())


def main():
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} <tag>")
    tag = sys.argv[1]
    version = (ROOT / "VERSION").read_text().strip()
    if tag != f"v{version}":
        sys.exit(f"refused: tag {tag} is not v{version} (VERSION)")

    releases = ET.parse(METAINFO).getroot().findall("./releases/release")
    if not releases or releases[0].get("version") != version:
        newest = releases[0].get("version") if releases else "none"
        sys.exit(f"refused: the metainfo's newest <release> is {newest}, "
                 f"not {version}; add the release entry first")
    desc = releases[0].find("description")
    if desc is None or not text(desc):
        sys.exit(f"refused: <release version=\"{version}\"> has no "
                 "<description>; write the release notes there")

    out = []
    for el in desc:
        if el.tag == "p":
            out += [text(el), ""]
        elif el.tag in ("ul", "ol"):
            out += [f"- {text(li)}" for li in el.findall("li")] + [""]
    print("\n".join(out).rstrip())


if __name__ == "__main__":
    main()
