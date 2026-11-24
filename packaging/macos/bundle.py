#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Make Lobole.app from an install tree, with every library it loads inside.

    packaging/macos/bundle.py --version 0.3.0 --stage STAGE --out DIR \\
        --search "$LOBOLE_MACOS_DEPS/lib" --search /opt/homebrew/lib ...
    packaging/macos/bundle.py --verify DIR/Lobole.app

STAGE is `cmake --install build --prefix STAGE`. The result is
DIR/Lobole.app (docs/packaging/README.md#the-macos-app):

    Contents/MacOS/lobole              the executable
    Contents/Frameworks/*.dylib        every non-system library it loads,
                                       MoltenVK and the Vulkan loader too
    Contents/Resources/share/lobole/   what the install put in share/lobole
    Contents/Resources/vulkan/icd.d/   MoltenVK's manifest, which the loader
                                       reads from a bundle before the system

Every reference to a copied library is rewritten to @rpath/<name>, every
rpath outside the bundle is removed (one left behind would let a Mac with
Homebrew load ITS copy first), and the executable gets
@executable_path/../Frameworks. The last step reads every Mach-O back and
fails, naming the file and the reference, on anything that still points
outside the bundle or the system. Signing is not done here
(packaging/macos/sign.sh), because install_name_tool invalidates a
signature and every file must be signed after its last edit.
"""
import argparse
import json
import os
import pathlib
import plistlib
import re
import shutil
import subprocess
import sys

SYSTEM = ("/usr/lib/", "/System/")


def run(*cmd):
    return subprocess.run(cmd, check=True, capture_output=True,
                          text=True).stdout


def is_macho(path):
    with open(path, "rb") as f:
        magic = f.read(4)
    return magic in (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe",
                     b"\xfe\xed\xfa\xcf", b"\xbe\xba\xfe\xca")


def own_id(path):
    lines = run("otool", "-D", str(path)).splitlines()
    return lines[1].strip() if len(lines) > 1 else None


def deps(path):
    """The libraries `path` links, without its own id."""
    me = own_id(path)
    out = []
    for line in run("otool", "-L", str(path)).splitlines()[1:]:
        name = line.strip().split(" (compatibility")[0].strip()
        if name and name != me:
            out.append(name)
    return out


def rpaths(path):
    text = run("otool", "-l", str(path))
    return re.findall(r"cmd LC_RPATH\n\s*cmdsize \d+\n\s*path (.+?) \(offset",
                      text)


def expand(ref, origin, executable):
    """`ref` with @loader_path and @executable_path made absolute, where
    `origin` is the ORIGINAL location of the image that holds it."""
    ref = ref.replace("@loader_path", str(origin.parent))
    return ref.replace("@executable_path", str(executable.parent))


def resolve(ref, origin, executable, search):
    """The file the dynamic linker would load for `ref`, or None."""
    if ref.startswith("@rpath/"):
        tail = ref[len("@rpath/"):]
        for rp in rpaths(origin) + rpaths(executable):
            candidate = pathlib.Path(expand(rp, origin, executable)) / tail
            if candidate.exists():
                return candidate.resolve()
        for d in search:
            candidate = pathlib.Path(d) / tail
            if candidate.exists():
                return candidate.resolve()
        return None
    candidate = pathlib.Path(expand(ref, origin, executable))
    if candidate.exists():
        return candidate.resolve()
    for d in search:
        candidate = pathlib.Path(d) / pathlib.Path(ref).name
        if candidate.exists():
            return candidate.resolve()
    return None


def bundle_libraries(app, executable_src, extra, search):
    macos = app / "Contents" / "MacOS"
    frameworks = app / "Contents" / "Frameworks"
    frameworks.mkdir(parents=True, exist_ok=True)
    macos.mkdir(parents=True, exist_ok=True)
    exe = macos / "lobole"
    shutil.copy2(executable_src, exe)
    os.chmod(exe, 0o755)

    origin = {exe: pathlib.Path(executable_src).resolve()}
    copied = {}  # name in Frameworks -> the real file it came from
    queue = [exe]

    def take(src, name):
        if name in copied:
            if copied[name] != src:
                sys.exit(f"bundle: two libraries named {name}: "
                         f"{copied[name]} and {src}")
            return frameworks / name
        dst = frameworks / name
        shutil.copy2(src, dst)
        os.chmod(dst, 0o755)
        copied[name] = src
        origin[dst] = src
        queue.append(dst)
        return dst

    for lib in extra:
        lib = pathlib.Path(lib)
        take(lib.resolve(), lib.name)

    while queue:
        image = queue.pop()
        for ref in deps(image):
            if ref.startswith(SYSTEM):
                continue
            src = resolve(ref, origin[image], origin[exe], search)
            if src is None:
                sys.exit(f"bundle: {image.name} links {ref}, which is not "
                         f"found (searched its rpaths and {search})")
            name = pathlib.Path(ref).name
            take(src, name)
            run("install_name_tool", "-change", ref, f"@rpath/{name}",
                str(image))

    for image in [exe, *sorted(frameworks.iterdir())]:
        for rp in rpaths(image):
            run("install_name_tool", "-delete_rpath", rp, str(image))
        if image == exe:
            run("install_name_tool", "-add_rpath",
                "@executable_path/../Frameworks", str(image))
        else:
            run("install_name_tool", "-id", f"@rpath/{image.name}",
                str(image))
            run("install_name_tool", "-add_rpath", "@loader_path",
                str(image))
    print(f"bundle: {len(copied)} libraries in Contents/Frameworks")


def install_icd(app, brew_icd, moltenvk_name):
    manifest = json.loads(pathlib.Path(brew_icd).read_text())
    manifest["ICD"]["library_path"] = f"../../../Frameworks/{moltenvk_name}"
    icd = app / "Contents" / "Resources" / "vulkan" / "icd.d"
    icd.mkdir(parents=True, exist_ok=True)
    (icd / "MoltenVK_icd.json").write_text(json.dumps(manifest, indent=2) +
                                           "\n")


def make_icon(app, png):
    """Contents/Resources/lobole.icns from the 256 px PNG, through an
    iconset (sips scales, iconutil packs)."""
    work = app.parent / "lobole.iconset"
    work.mkdir(exist_ok=True)
    for size in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            px = size * scale
            suffix = "@2x" if scale == 2 else ""
            run("sips", "-z", str(px), str(px), str(png), "--out",
                str(work / f"icon_{size}x{size}{suffix}.png"))
    run("iconutil", "-c", "icns", str(work), "-o",
        str(app / "Contents" / "Resources" / "lobole.icns"))
    shutil.rmtree(work)


def write_plist(app, version, minimum):
    info = {
        "CFBundleExecutable": "lobole",
        "CFBundleIdentifier": "org.lobole.Lobole",
        "CFBundleName": "Loböle",
        "CFBundleDisplayName": "Loböle",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": version,
        "CFBundleVersion": version,
        "CFBundleIconFile": "lobole",
        "CFBundleInfoDictionaryVersion": "6.0",
        "LSMinimumSystemVersion": minimum,
        "LSApplicationCategoryType": "public.app-category.graphics-design",
        "NSHighResolutionCapable": True,
    }
    with open(app / "Contents" / "Info.plist", "wb") as f:
        plistlib.dump(info, f)


def verify(app):
    """Every Mach-O in the bundle loads only the system's libraries and the
    bundle's own; anything else is named and fails the build."""
    frameworks = app / "Contents" / "Frameworks"
    have = {p.name for p in frameworks.iterdir()}
    bad = []
    count = 0
    for path in sorted(app.rglob("*")):
        if not path.is_file() or path.is_symlink() or not is_macho(path):
            continue
        count += 1
        for ref in deps(path):
            if ref.startswith(SYSTEM):
                continue
            if ref.startswith("@rpath/") and ref[len("@rpath/"):] in have:
                continue
            bad.append(f"{path.relative_to(app)} -> {ref}")
        for rp in rpaths(path):
            if rp not in ("@executable_path/../Frameworks", "@loader_path"):
                bad.append(f"{path.relative_to(app)} rpath {rp}")
    if bad:
        sys.exit("bundle: references outside the bundle:\n  " +
                 "\n  ".join(bad))
    print(f"bundle: {count} Mach-O files, each loads only the system's "
          "libraries and the bundle's own")


def main():
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n")[0])
    ap.add_argument("--verify", type=pathlib.Path, metavar="APP",
                    help="only check an existing bundle's references")
    ap.add_argument("--version")
    ap.add_argument("--stage", type=pathlib.Path)
    ap.add_argument("--out", type=pathlib.Path)
    ap.add_argument("--search", action="append", default=[],
                    help="a directory to find an @rpath library in when the "
                         "image's own rpaths do not (repeatable)")
    ap.add_argument("--brew", default=os.environ.get("HOMEBREW_PREFIX",
                                                     "/opt/homebrew"))
    ap.add_argument("--minimum", default=os.environ.get(
        "MACOSX_DEPLOYMENT_TARGET"))
    args = ap.parse_args()
    if args.verify:
        verify(args.verify)
        return
    if not (args.version and args.stage and args.out):
        ap.error("--version, --stage and --out are required to make a bundle")
    if not args.minimum:
        sys.exit("bundle: no --minimum and no MACOSX_DEPLOYMENT_TARGET "
                 "(tools/macos-env.sh sets it)")

    brew = pathlib.Path(args.brew)
    app = args.out / "Lobole.app"
    if app.exists():
        shutil.rmtree(app)
    resources = app / "Contents" / "Resources"
    resources.mkdir(parents=True)

    stage_lib = args.stage / "lib"
    # Our own libraries (planegcs, designer_adaptive, lobole_thread_stack)
    # are linked, so the walk finds them under the names the executable
    # uses. The ICD is loaded by the Vulkan loader with dlopen, so nothing
    # links it: it is taken by name.
    brew_icd = brew / "etc" / "vulkan" / "icd.d" / "MoltenVK_icd.json"
    moltenvk = brew / "opt" / "molten-vk" / "lib" / "libMoltenVK.dylib"
    for need in (brew_icd, moltenvk):
        if not need.exists():
            sys.exit(f"bundle: {need} is missing (brew install molten-vk)")
    bundle_libraries(app, args.stage / "bin" / "lobole", [moltenvk],
                     [str(stage_lib), *args.search, str(brew / "lib")])
    install_icd(app, brew_icd, moltenvk.name)

    shutil.copytree(args.stage / "share" / "lobole",
                    resources / "share" / "lobole", symlinks=False)
    shutil.copytree(args.stage / "share" / "doc" / "lobole",
                    resources / "share" / "doc" / "lobole", symlinks=False)
    make_icon(app, args.stage / "share" / "icons" / "hicolor" / "256x256" /
              "apps" / "org.lobole.Lobole.png")
    write_plist(app, args.version, args.minimum)
    verify(app)


if __name__ == "__main__":
    main()
