# The Loböle mark

"Lö" in square strokes on a 24-unit grid. The master is
[`assets/icons/org.lobole.Lobole.svg`](../icons/org.lobole.Lobole.svg), and
nothing else is drawn by hand: every file below is made from it, so a change
to the master reaches them all on the next run.

The name and the mark are not licensed under the AGPL; see
[TRADEMARKS.md](../../TRADEMARKS.md).

## The rules the master keeps

- Strokes 2.4 units wide, square caps.
- The stave's top is level with the tops of the dots, and the foot ends
  flush with the o's right edge.
- The o stands 2.1 units clear of both the stave and the foot.
- The mark is centred on the tile.
- Colours from the app: the tile is the viewport (`#23262e`), the mark the
  sketch line colour (`#8cd9ff`, `kActiveSketchColor` in
  `src/ui/SceneOverlays.cpp`), and a lighter rim (`#3d4250`) keeps the
  tile's edge visible on a dark panel, where the tile alone is 1.1:1. The
  mark on the tile is 9.7:1.

## The files

| file | what | made by |
|---|---|---|
| `assets/icons/org.lobole.Lobole.svg` | the master; installed as the scalable desktop icon | by hand |
| `assets/icons/org.lobole.Lobole.png` | 256 px, the desktop's raster fallback | `brand.py` |
| `assets/icons/org.lobole.Lobole.ico` | Windows, 16-256 px: the executable, its windows, the installer | `brand.py` |
| `lobole-lockup-dark.svg`, `lobole-lockup-light.svg` | the tile and "Loböle", for a dark and a light ground | `brand.py` |
| `social-preview.png` | GitHub's 1280×640 card, set by hand under the repository's Settings → General → Social preview | `brand.py` |
| `docs/gallery/lobole-log-end.jpg`, `lobole-badge.jpg` | the mark modelled and path-traced in Loböle | `generators/logo/build.py` |

`generators/logo/mark.py` reads the master into outlines: each stroked path
becomes the outline of its stroke, and anything the master does not use
(curves, relative commands, diagonal strokes) is refused rather than read as
something near it.

## Making them again

```
# the icons alone
nix-shell -p librsvg imagemagick --run "python3 generators/logo/brand.py"

# with the lockups and the social preview: the wordmark font, fetched once
curl -LO 'https://github.com/google/fonts/raw/main/ofl/archivo/Archivo%5Bwdth%2Cwght%5D.ttf'
nix-shell -p librsvg imagemagick \
    'python3.withPackages (p: [ p.fonttools p.uharfbuzz ])' --run \
    "python3 generators/logo/brand.py --font 'Archivo[wdth,wght].ttf'"

# the renders, on a scratch instance (tools/headless-preview.sh --start PORT)
python3 generators/logo/build.py --port PORT
```

The wordmark is Archivo SemiBold, the tagline Archivo Regular, by the Archivo
Project Authors under the SIL Open Font License 1.1. `brand.py` shapes the
text with HarfBuzz (so the font's kerning applies) and writes it as outlines,
so the font itself is not distributed with the project.
