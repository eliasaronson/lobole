# chess

A Staunton chess set and everything to machine it on the 6090: turned pieces
from rotary blanks, a board with epoxy-filled dark squares, and a jig that
holds the pieces upside down for their undersides. The plan and what is left
to do: [docs/plans/chess-set-dogfood.md](../../docs/plans/chess-set-dogfood.md).

    generators/chess/run_all.sh PORT      # everything, ~45 min

Each stage opens the project the one before saved; all of it lands in
`scratch/chess/` (projects, `nc/` programs, renders, reports, logs):

| script | makes | saves |
|---|---|---|
| `build_piece.py` | the `piece` part: a configuration per rank | `piece.dsgn` |
| `build_blanks.py` | the four rotary blanks, pieces Derived in their configurations on necks | `blanks.dsgn` |
| `build_board.py` | the board and the base jig | `set.dsgn` |
| `build_set.py` | the assembled set -- the piece part placed 32 times, each in its rank's configuration and its side's wood (maple, walnut) -- its BOM, the pieces' drawing (views turned upright, each height to its top's `extreme`) | `set.dsgn` |
| `cam.py` | every job, verified and posted | `chess.dsgn`, `nc/` |

`profiles.py` holds the turned profiles.

## Sizes

`king_h` (project, 85 mm) is the one literal. Each rank's height and base
diameter are FIDE fractions of it, set by its configuration; the square is
twice the pawn's base. Profiles are drawn against two anchors, the base
radius and the top (`piece_h - a * u`, `u = king_h / 85`), so an arc stays an
arc at any size; the set rebuilds from 70 to 110 mm. Every concave
junction -- line or arc either side -- is rounded to 1.6 mm (`FILLET_R`),
more than the 3 mm finishing ball's radius, and the grooves (the foot's, the
necks) are coves of at least 1.8 mm, so the finishing ball reaches every
surface and no rest pass is needed at the nominal size. A corner with no
room is listed on the run (`tight inside corner`).

## Blanks

Each blank is pieces strung base-first along X on necks the operator saws,
between a square grip (the chuck's jaws plus the clearance a cutter keeps
from them) and a square stub for the tailstock's centre. A neck is sized to
the top of the piece before it, so the fine tops (bishop, queen, king,
knight) go last, beside the tailstock. Each piece's underside is plugged in
the blank: the rotary cannot reach an end face. The plug fills the felt
recess and the weight's bore each to its own size; one cylinder the
recess's width down the bore's depth stood out of every base that narrows,
and the finishing ball left a ring at each of its corners.

## Jobs

- Blanks: turned (rough 6 mm flat, finish 3 mm ball), then indexed head
  cuts: crenels, the mitre, crown notches, the cross -- each ONE operation
  at the indices that face its floors (`index_a_deg`), with its top derived
  from what the turning left. The knights' blank
  alone adds a rest pass with a 1.5 mm ball, a program of its own: the
  head's outline turns tighter than the 3 mm ball at the mouth and under
  the chin.
- Board: dark-square pockets, a 2 mm pass round each square's corners, the
  frame's chamfer (V-bit), cut-out; a second setup faces it after the epoxy.
- Base jig: the seats and the cut-out; then one side's sixteen pieces in it,
  a setup per rank, felt recess and weight bore.

Every face an operation targets is asked for by what made it
(docs/modeling/toponaming.md): a crenel's floors are the faces
`made(..., "Crenels", "rook 1")` -- the piece part's pattern, through the
rook's Derived copy -- that face away from the axis. Every radial cut here is
extruded ACROSS the part, so its end caps (`role="floor"`) are its walls, and
its floor is a side face. The board's chamfered edges are the ones joining the slab's top
to its sides (`edges(between=...)`).

`MACHINE` in `cam.py` holds the rotary's mount as TEST values: the 6090's axis
letter, direction and steps per degree are not measured, so no rotary
program is for the machine until they are (step 0 of the plan).
