#!/usr/bin/env python3
"""The committed icon font is what tools/icons/build_icon_font.py makes.

The script draws Designer's CAD glyphs and writes deps/fonts/designer-icons.ttf
and deps/fonts/IconsDesigner.h together; the header records the SHA-256 of the
script it came from and of the font written beside it. Edit a glyph without
rebuilding, or touch the font by hand, and the two stop matching. Standard
library only: the drawing needs fontTools and shapely, and ctest and CI have
neither (docs/ui/README.md#icons).
"""
import hashlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def digest(rel):
    with open(os.path.join(ROOT, rel), "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def main():
    header = open(os.path.join(ROOT, "deps/fonts/IconsDesigner.h")).read()
    want = dict(re.findall(r"^// (source|ttf)-sha256: ([0-9a-f]{64})$", header,
                           re.M))
    have = {"source": digest("tools/icons/build_icon_font.py"),
            "ttf": digest("deps/fonts/designer-icons.ttf")}
    bad = [k for k in ("source", "ttf") if want.get(k) != have[k]]
    for k in bad:
        what = ("tools/icons/build_icon_font.py changed since the font was built"
                if k == "source" else
                "deps/fonts/designer-icons.ttf is not the font the header was "
                "built with")
        print(f"FAIL: {what}. Rebuild: nix-shell -p \"python3.withPackages(p: "
              "[p.fonttools p.shapely p.pillow])\" --run \"python3 "
              "tools/icons/build_icon_font.py\"")
    if not bad:
        print("icon font matches its source")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
