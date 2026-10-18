#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A reference image is a PNG, JPEG or BMP up to 16384 px a side; anything
else is refused, naming what is accepted.

stb_image decodes these, and a project file carries them, so opening someone
else's document decodes bytes they chose: only the decoders the app offers
are compiled in (src/common/ImageLimits.h). A file it could not read used to
be added anyway, zero pixels wide, and never drawn.

Usage: reference_image_formats_smoke.py <path-to-designer-binary>
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smoke_app import launch  # noqa: E402
from underlay_show_always_smoke import write_png  # noqa: E402

# A 1 x 1 GIF and a 1 x 1 uncompressed TGA: formats stb_image can read, and
# the app no longer offers.
GIF = (b"GIF89a\x01\x00\x01\x00\x80\x00\x00\xff\x00\x00\x00\x00\x00,"
       b"\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02D\x01\x00;")
TGA = (bytes([0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 24, 0])
       + b"\x00\x00\xff")


def expect(condition, message):
    if not condition:
        raise AssertionError(message)
    print(f"  ok: {message}", flush=True)


def main():
    with launch(sys.argv[1]) as app:
        d = app.client
        d.request("new_project", {})
        plan = d.call("add_feature", feature={
            "type": "Sketch", "name": "Plan",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "line", "a": [0, 0], "b": [10, 0]}]})["id"]

        def add(path):
            try:
                return d.request("add_reference_image",
                                 {"sketch_id": plan, "path": path})
            except Exception as e:  # a refusal, said in the error
                return {"error": str(e)}

        png = os.path.join(app.tmp, "ok.png")
        write_png(png, 4, 3, (255, 0, 0))
        r = add(png)
        expect("error" not in r and r.get("width_px") == 4,
               f"a PNG is accepted: {r}")

        for name, data in [("x.gif", GIF), ("x.tga", TGA)]:
            path = os.path.join(app.tmp, name)
            with open(path, "wb") as f:
                f.write(data)
            r = add(path)
            expect("PNG, JPEG or BMP" in str(r.get("error")),
                   f"{name} is refused, naming what is accepted: {r}")

        wide = os.path.join(app.tmp, "wide.png")
        write_png(wide, 16385, 1, (255, 0, 0))
        r = add(wide)
        expect("16384" in str(r.get("error")),
               f"a PNG wider than 16384 px is refused: {r}")

        images = [f for f in d.call("get_state")["features"]
                  if f.get("id") == plan][0].get("images", [])
        expect(len(images) == 1,
               f"the refused files left no image behind ({len(images)})")
    print("reference_image_formats_smoke: OK")


if __name__ == "__main__":
    main()
