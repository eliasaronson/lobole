# Trademarks

This repository names other companies' products. Every such name is a
trademark or registered trademark of its owner, used here only to refer to
that product. Designer is **not affiliated with, endorsed by, or sponsored
by** any of them, and naming a product claims no compatibility,
certification or approval.

## This project's own name

The name and logo of this project are not licensed under the AGPL, and are not
granted by any contributor agreement. The licence covers the source code; it
says nothing about the name, and copyleft does not transfer a mark.

This is the ordinary arrangement for free software — Mozilla and the Linux
Foundation both hold their marks separately from the licences on their code —
and it exists for the reader's benefit rather than the author's: anyone
redistributing a modified build should give it a different name, so that a
user can tell whose build they are running and who to report a bug to.

Referring to the project by name is free and always will be: saying that your
software reads its files, drives it, forks it or replaces it is nominative use,
exactly as described below for everyone else's marks.

## Which names appear

- **Other CAD, CAM and design software:** FreeCAD, SolidWorks, Parasolid,
  Fusion, Creo, NX, Inventor, Blender, Kiri:Moto, KiCad, PrusaSlicer, Bambu
  and blendercam.
- **Machine controllers and machines:** Mach3, LinuxCNC, GRBL, Haas and
  Shapeoko.
- **Makers of real parts the example generators model:** Mercedes-Benz and
  Bosch (`generators/om615/`, `generators/pes_pump/`), Visaton and Markaudio
  (`generators/smart_speaker/`), and hardware such as the Raspberry Pi and
  ESP32 named as the part a model represents.

The list is of names that appear, not an exhaustive statement of whose marks
they are; each belongs to its respective owner.

## How they are used

All of it is **referential (nominative) use**: naming someone else's product
to talk about that product, and no more of the name than that takes. No
third-party logos or branding are used. Designer is described on its own
terms, and is not offered as a replica of, or a drop-in replacement for, any
of these products.

The uses fall into a few kinds:

1. **Interoperability.** A tool that reads or writes another system's files,
   or drives another system's machine, has to say which. The G-code posts are
   named after the controller dialect they emit (`mach3`, `linuxcnc`, `grbl`),
   the mesh exports name the viewers they are meant for, and
   `generators/chassis/sldprt_frame.py` reads geometry out of a SolidWorks
   part file without SolidWorks.
2. **Prior art.** Design documents and source comments cite what another
   system does, or why this one differs, so the decision can be checked later
   ("FreeCAD's simulator does X; this one does Y because…"). Some of those
   citations sit beside the code they explain, mostly in the CAM headers and
   the sketch and keymap code.
3. **Real-world subjects.** The example generators model real engines, pumps
   and loudspeaker drivers, and name the manufacturer and model so a reader
   can find the datasheet or manual the dimensions came from.
4. **Licence attribution.** PlaneGCS is vendored from FreeCAD, and the
   fastener dimension tables from the FreeCAD Fasteners Workbench, so the
   About dialog and [`THIRD_PARTY.md`](THIRD_PARTY.md) name FreeCAD. That
   attribution is an obligation, not a choice.

If you own one of these marks and think a use here goes beyond reference,
open an issue and it will be changed.
