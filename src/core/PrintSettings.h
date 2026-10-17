// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// A printed part's SLICER settings (docs/tolerances/README.md#slicer-settings):
// walls, top and bottom layers, infill, supports and brim. Part of the
// design, not of the slicer: a pendulum's period is computed for the plastic
// it is printed with, and printed at the slicer's 15 % it swings at another.
//
// They live on the part beside its print process (printfit::Process), and a
// BODY may override any of them, keyed by the id of the feature that created
// it (Body::createdBy) -- the key set_material uses. A field that is not
// stated is the slicer preset's, and the job leaves it to the slicer.
//
// No I/O. What reads them: export_print_job (one object per body, these as
// its per-object settings) and the printed mass (core/PrintedMass.h), which
// needs walls, both shells and the infill stated.
namespace printset {

// The infill patterns a job can name, in display order.
enum class Infill {
  Rectilinear,
  AlignedRectilinear,
  Grid,
  Gyroid,
  Honeycomb,
  Cubic,
  Triangles,
  Line,
  Concentric,
  Lightning,
  AdaptiveCubic,
  TriHexagon,
  Honeycomb3d,
  CrossHatch,
  HilbertCurve,
};
[[nodiscard]] const std::vector<Infill>& AllInfills();
// "rectilinear" | "aligned_rectilinear" | "grid" | "gyroid" | ...
[[nodiscard]] std::string InfillName(Infill i);
// nullopt for a name not carried (the caller refuses naming InfillNames()).
[[nodiscard]] std::optional<Infill> InfillFromName(const std::string& name);
[[nodiscard]] const std::string& InfillNames();
// Bambu Studio's name for it (`sparse_infill_pattern`).
[[nodiscard]] const char* BambuInfillName(Infill i);
// Whether the pattern can fill a region SOLID: the slicer refuses any other
// at 100 % (it is not supposed to work there, and Bambu Studio asks to
// switch it to rectilinear). The patterns its top surfaces may use.
[[nodiscard]] bool PrintsSolid(Infill i);
[[nodiscard]] std::string SolidInfillNames();

enum class Brim { None, Outer, Inner, Both, Ears };
[[nodiscard]] const std::vector<Brim>& AllBrims();
// "none" | "outer" | "inner" | "both" | "ears"
[[nodiscard]] std::string BrimName(Brim b);
[[nodiscard]] std::optional<Brim> BrimFromName(const std::string& name);
[[nodiscard]] const std::string& BrimNames();
// Bambu Studio's `brim_type`.
[[nodiscard]] const char* BambuBrimName(Brim b);

// Every field optional: unset is the slicer preset's (supports and brim
// excepted -- a job prints with neither unless one is set).
struct Settings {
  std::optional<int> walls;        // wall loops, >= 1
  std::optional<int> topLayers;    // >= 0
  std::optional<int> bottomLayers; // >= 0
  std::optional<double> infillPct; // 0..100
  std::optional<Infill> infill;
  std::optional<bool> supports;
  std::optional<Brim> brim;
  // The print direction in the PART's frame, unit: the way up the bed when
  // the part is printed. Unset is the part's +Z. Only the printed mass reads
  // it (a plate's pieces are placed on their plate, whose +Z is the bed's).
  std::optional<glm::dvec3> direction;
  [[nodiscard]] bool Empty() const;
  bool operator==(const Settings&) const = default;
};

// `over`'s fields where it states them, else `under`'s.
[[nodiscard]] Settings Merge(const Settings& under, const Settings& over);

// Empty when `s` may be stored; otherwise the refusal: walls below 1, a
// layer count below 0, an infill outside 0..100 %, a direction that is not
// a finite non-zero vector, or 100 % with a pattern that cannot print solid
// (or with no pattern: the preset's may be one that cannot).
[[nodiscard]] std::string Validate(const Settings& s);

// A part's settings and its bodies' overrides.
struct PartSettings {
  Settings part;
  std::map<uint64_t, Settings> bodies; // by Body::createdBy
  [[nodiscard]] bool Empty() const;
  // The part's merged with the override of the body `createdBy` made.
  [[nodiscard]] Settings For(uint64_t createdBy) const;
  bool operator==(const PartSettings&) const = default;
};

// What the slicer gets for one object: Bambu Studio's per-object keys, in a
// stable order. Supports and brim always (off and none unless set); a shell
// count with its thickness floor zeroed, so the count is what prints (the
// preset's top_shell_thickness would otherwise raise it); every other key
// only when stated.
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
BambuObjectSettings(const Settings& s, double layerHeightMm);

// "walls 3, top/bottom 5/3, 100 % rectilinear, no supports, no brim":
// how a report names what an object prints with. Unstated fields read
// "preset".
[[nodiscard]] std::string Describe(const Settings& s);

} // namespace printset
