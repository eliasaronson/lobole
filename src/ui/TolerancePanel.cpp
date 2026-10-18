// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/ToleranceActions.h"
#include "ui/Widgets.h"

#include "core/Command.h"
#include "core/Document.h"
#include "core/FitBands.h"
#include "core/FitStandards.h"
#include "core/Measure.h"
#include "core/Units.h"
#include "core/features/ThreadFeature.h"
#include "ui/AppState.h"
#include "ui/ExprInput.h"
#include "ui/PrintProfileActions.h"
#include "ui/SettingsWindow.h" // OpenPreferences

#include <imgui.h>

#include <cmath>

#include <cctype>
#include <optional>
#include <string>

namespace ui {
namespace {

// A printed press in words: the band its print profile gives the face
// (docs/tolerances/README.md#printed-press-fits), or why it has none.
std::string PrintedPressText(const Document& doc, const Tolerance& t,
                             const units::System& sys) {
  std::string why;
  const auto pb = fitband::PrintedPressBand(doc, t.ref, &why);
  if (!pb)
    return "printed press (" + why + ")";
  return "printed press: " +
         units::FormatLength(pb->band.basicMm + pb->band.lowerMm, sys) +
         " .. " +
         units::FormatLength(pb->band.basicMm + pb->band.upperMm, sys) +
         " (press " + units::FormatNumber(pb->band.pressMm) + " mm, from " +
         printfit::Label(*doc.PrintProcess()) + ")";
}

// What the current picks are a tolerance ON, or nothing.
//
// The mapping is the one the data model already makes: one named element is
// that face's SIZE, two are the DISTANCE between them. So the Measure tool's
// existing pick list needs no parallel notion of a "tolerance selection" —
// measuring a thing and saying what it may vary by are the same gesture, and
// this is the point at which the second one stops being read-only.
struct Subject {
  ElementRef a;
  ElementRef b; // unset for a size
  bool distance = false;
  std::optional<double> nominal;
  std::string why; // non-empty: nothing to tolerance, and this says why
};

Subject SubjectOf(AppState& state) {
  Subject s;
  const auto& picked = state.measure.picked;
  if (picked.empty() || picked.size() > 2)
    return s; // silent: no picks is not a refusal, it is the resting state
  for (const auto& e : picked) {
    // A whole body or a placed occurrence has no size a tolerance describes,
    // and a tolerance lives in the PART document — so an assembly-side or
    // job-side pick has nowhere to be stored even when it resolves.
    if (e.Whole() || e.occurrenceId != 0 || e.jobPartId != 0 ||
        !e.ref.IsSet()) {
      s.why = "A tolerance is about a face of this part — pick faces on a "
              "part tab, not whole bodies or placed occurrences.";
      return s;
    }
  }
  s.a = picked[0].ref;
  s.distance = picked.size() == 2;
  if (s.distance)
    s.b = picked[1].ref;

  const std::vector<Body>& bodies = state.document->GetBodies();
  if (s.distance) {
    if (const auto d = Measure::DistanceDetailed(bodies, s.a, s.b))
      s.nominal = d->distance;
    else
      s.why = "Those two faces have no measurable distance between them.";
    return s;
  }
  const auto el = Measure::Describe(bodies, s.a);
  if (!el) {
    s.why = "That element no longer resolves.";
    return s;
  }
  if (!el->hasRadius) {
    // Not a dead end, and saying so is the whole difference between a
    // refusal and a wall: a plane has no size of its own, but it is one half
    // of a length as soon as there is a second face.
    s.why = "A " + el->shape +
            " has no size of its own. Pick a second face and this becomes a "
            "tolerance on the distance between them.";
    return s;
  }
  s.nominal = el->radius * 2.0;
  return s;
}

std::string ClassOf(const AppState& state) {
  if (state.toleranceLetter[0] == '\0')
    return {};
  return std::string(state.toleranceLetter) +
         std::to_string(state.toleranceGrade);
}

// The letter pickers are built from the vendored tables, so the list IS the
// scope. The two are offered together, uppercase and lowercase, because
// nothing in the geometry says whether a cylinder is a bore or a shaft — the
// concavity that would tell us is not something Measure reports, and guessing
// it would put an H7 on a shaft and call it done.
void DrawClassPicker(AppState& state) {
  ImGui::SetNextItemWidth(90.0f);
  const char* shown =
      state.toleranceLetter[0] == '\0' ? "letter" : state.toleranceLetter;
  if (ImGui::BeginCombo("###tol_letter", shown)) {
    const auto offer = [&](const std::string& l, const char* side) {
      const bool sel = l == state.toleranceLetter;
      if (ImGui::Selectable((l + "  " + side).c_str(), sel)) {
        std::snprintf(state.toleranceLetter, sizeof(state.toleranceLetter),
                      "%s", l.c_str());
        state.toleranceMsg.clear();
      }
    };
    for (const std::string& l : fits::HoleLetters())
      offer(l, "bore");
    ImGui::Separator();
    for (const std::string& l : fits::ShaftLetters())
      offer(l, "shaft");
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip(
      "Uppercase is a bore, lowercase a shaft. Nothing in the geometry says "
      "which a cylinder is, so it is stated rather than guessed.");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(80.0f);
  const std::string grade = "IT" + std::to_string(state.toleranceGrade);
  if (ImGui::BeginCombo("###tol_grade", grade.c_str())) {
    for (int g = 4; g <= 13; g++) {
      const std::string name = "IT" + std::to_string(g);
      if (ImGui::Selectable(name.c_str(), g == state.toleranceGrade)) {
        state.toleranceGrade = g;
        state.toleranceMsg.clear();
      }
    }
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("The tolerance grade: how wide the band is. IT6-IT8 "
                        "are the usual fits, higher is coarser.");
}

// The band the current pickers imply at THIS size, or the reason there is
// none. Shown live rather than on Set, so a letter/grade pair the standard
// does not define says so before it is committed.
void DrawBandPreview(const AppState& state, double nominal,
                     const units::System& sys) {
  const std::string cls = ClassOf(state);
  if (cls.empty()) {
    ImGui::TextDisabled("Choose a letter and a grade.");
    return;
  }
  std::string why;
  const auto band = fits::Limits(nominal, cls, &why);
  if (!band) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.35f, 1.0f));
    ImGui::TextWrapped("%s", why.c_str());
    ImGui::PopStyleColor();
    return;
  }
  ImGui::Text("%s: %s .. %s", cls.c_str(),
              units::FormatLength(band->Min(nominal), sys).c_str(),
              units::FormatLength(band->Max(nominal), sys).c_str());
}

// On a printed part, what this face PRINTS at and what to model for the size
// it measures — the same sentences check_fit gives
// (docs/tolerances/README.md#printed-parts). Needs the class, because only
// the class says whether the face is a bore or a shaft.
void DrawPrintedPreview(const Document& doc, double modelled,
                        const std::string& cls) {
  const auto& proc = doc.PrintProcess();
  if (!proc || cls.empty())
    return;
  const bool hole = std::isupper(static_cast<unsigned char>(cls[0])) != 0;
  std::string why;
  const auto printed = PrintedForModelled(*proc, hole, modelled, cls, &why);
  const auto want = PrintedForBasic(*proc, hole, modelled, modelled, cls, &why);
  if (!printed || !want) {
    ImGui::TextWrapped("Printed: %s", why.c_str());
    return;
  }
  ImGui::TextWrapped("Printed (%s %s): as modelled it prints %s %.3f mm. %s",
                     proc->process.c_str(), proc->material.c_str(), "\xC3\x98",
                     printed->basic, want->Sentence(cls).c_str());
  if (!want->bandNote.empty())
    ImGui::TextDisabled("%s", want->bandNote.c_str());
}

} // namespace

void DrawTolerancePick(AppState& state) {
  // Part tabs only: a tolerance is stored on the Document, and `commands` is
  // null anywhere there is no Document to undo against.
  if (state.document == nullptr || state.commands == nullptr)
    return;
  if (state.measure.picked.empty())
    return;

  ImGui::SeparatorText("Tolerance");
  Subject subject = SubjectOf(state);
  if (!subject.why.empty()) {
    ImGui::TextWrapped("%s", subject.why.c_str());
    return;
  }
  if (!subject.nominal)
    return;
  const double nominal = *subject.nominal;
  Document& doc = *state.document;
  const units::System sys = doc.Units();

  // What this subject already carries, if anything. Looked up by SUBJECT, the
  // key SetTolerance replaces on — a face's size and a distance measured from
  // it are different statements and must not shadow each other here either.
  if (const Tolerance* have = doc.ToleranceForSubject(subject.a, subject.b)) {
    std::string band;
    if (have->printedPress) {
      band = PrintedPressText(doc, *have, sys);
    } else if (have->isoClass.empty()) {
      band = units::FormatLength(nominal + have->lower, sys) + " .. " +
             units::FormatLength(nominal + have->upper, sys);
    } else {
      std::string why;
      if (const auto b = fits::Limits(nominal, have->isoClass, &why))
        band = have->isoClass + ": " +
               units::FormatLength(b->Min(nominal), sys) + " .. " +
               units::FormatLength(b->Max(nominal), sys);
      else
        band = have->isoClass + " (" + why + ")";
    }
    ImGui::TextWrapped("Already toleranced - %s", band.c_str());
    if (ImGui::Button("Clear###tol_clear")) {
      state.commands->Execute(std::make_unique<ClearToleranceCommand>(have->id),
                              doc);
      state.toleranceMsg.clear();
      return; // `have` is dangling
    }
    ImGui::SameLine();
  }

  // Which distance this band is on, said outright. The Measure tool can be
  // showing the FURTHEST separation or centre-to-centre while a tolerance is
  // always on the closest approach — Measure::DistanceDetailed has no kind,
  // and storing one would mean list_tolerances recomputing a different
  // nominal on reload. A number that silently disagrees with the readout two
  // lines above it is worse than a longer label.
  if (subject.distance)
    ImGui::Text("Length (closest approach) %s",
                units::FormatLength(nominal, sys).c_str());
  else
    ImGui::Text("\xC3\x98 %s", units::FormatLength(nominal, sys).c_str());

  Tolerance edit;
  edit.ref = subject.a;
  edit.ref2 = subject.b;
  bool ready = false;
  if (subject.distance) {
    // A class states how a shaft or a bore may vary and says nothing about a
    // gap, so a distance takes deviations outright. They are NOT seeded: a
    // band nobody chose is the plausible-and-wrong default this project
    // refuses, and there is no general-tolerance table vendored to derive one
    // from.
    ImGui::SetNextItemWidth(90.0f);
    // ExprInputDouble, not ImGui::InputDouble: the raw widget's Ctrl+click
    // entry runs a bare sscanf, so "0.2/2" commits as 0.2 — and a deviation
    // written as a fraction of something is exactly how a person types one
    // (tests/test_conventions.cpp gates this for every numeric field in ui/).
    ExprInputDouble(state, "###tol_lower", &state.toleranceLower, "%.4f");
    ImGui::SetItemTooltip("Lower deviation from the measured length, in mm "
                          "(negative is smaller). Takes an expression.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ExprInputDouble(state, "###tol_upper", &state.toleranceUpper, "%.4f");
    ImGui::SetItemTooltip("Upper deviation from the measured length, in mm");
    edit.lower = state.toleranceLower;
    edit.upper = state.toleranceUpper;
    ready = true;
  } else {
    DrawClassPicker(state);
    DrawBandPreview(state, nominal, sys);
    edit.isoClass = ClassOf(state);
    ready = !edit.isoClass.empty();
    DrawPrintedPreview(doc, nominal, edit.isoClass);
  }

  if (ImGui::Button("Set tolerance###tol_set")) {
    // Every refusal the RPC makes, made here too and in the same words. A
    // GUI that accepts what the script API refuses is the substitution this
    // codebase spent a workstream removing.
    std::string why;
    if (!ready) {
      why = "Choose a tolerance class first - a letter and a grade.";
    } else if (subject.distance && edit.lower > edit.upper) {
      why = "Lower must not exceed upper - a reversed band admits nothing "
            "and reads as a valid one.";
    } else if (subject.distance && edit.lower == 0.0 && edit.upper == 0.0) {
      why = "A band of zero admits nothing: no part is made to exactly "
            "one number. Give a lower and an upper deviation.";
    } else if (!subject.distance &&
               !fits::Limits(nominal, edit.isoClass, &why)) {
      // `why` is already the table's own refusal.
    } else {
      state.commands->Execute(
          std::make_unique<SetToleranceCommand>(std::move(edit)), doc);
      state.toleranceMsg.clear();
      return;
    }
    state.toleranceMsg = why;
  }
  if (!state.toleranceMsg.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.35f, 1.0f));
    ImGui::TextWrapped("%s", state.toleranceMsg.c_str());
    ImGui::PopStyleColor();
  }
}

void DrawTolerancesSection(AppState& state) {
  if (!SectionHeader("Tolerances"))
    return;
  if (state.document == nullptr) {
    ImGui::TextDisabled("Tolerances belong to a part.");
    return;
  }
  Document& doc = *state.document;
  if (doc.GetTolerances().empty()) {
    ImGui::TextDisabled(
        "None yet. Measure a face and say what it may vary by.");
    return;
  }
  const units::System sys = doc.Units();
  const std::vector<Body>& bodies = doc.GetBodies();

  // Rows, not a table. This panel is 218 px wide by design (PanelTitles.h),
  // and ImGui table cells CLIP rather than elide — a limits column reading
  // "19.98 mm" when the value is "19.98 .. 19.993 mm" looks like one number
  // and is the same defect that once made 385169 g read as 385. A wrapped
  // line cannot lose its second half.
  uint64_t drop = 0;
  for (const Tolerance& t : doc.GetTolerances()) {
    // No PushID: an INTEGER scope is not addressable as a path segment, so a
    // row wrapped in one can never be clicked by a test
    // (docs/ui/testing.md#making-a-widget-addressable — it is why the Variables
    // and Configurations panels went untested for months). The ids below
    // already carry the tolerance's own id, which is what makes them unique.
    if (ImGui::SmallButton(("x###tol_del_" + std::to_string(t.id)).c_str()))
      drop = t.id;
    ImGui::SameLine();

    std::string what = Measure::HumanName(bodies, t.ref, sys);
    if (what.empty())
      what = t.ref.name;
    if (t.IsDistance()) {
      std::string to = Measure::HumanName(bodies, t.ref2, sys);
      what += " to " + (to.empty() ? t.ref2.name : to);
    }

    // The limits are recomputed against the CURRENT geometry rather than
    // stored — the same reason the class is stored and not the deviations:
    // one source of truth, and an edit upstream moves them.
    std::optional<double> nominal;
    if (t.IsDistance()) {
      if (const auto d = Measure::DistanceDetailed(bodies, t.ref, t.ref2))
        nominal = d->distance;
    } else if (const auto el = Measure::Describe(bodies, t.ref)) {
      if (el->hasRadius)
        nominal = el->radius * 2.0;
    }

    const std::string band = t.printedPress ? std::string("printed press")
                             : t.isoClass.empty()
                                 ? units::FormatNumber(t.lower) + " / " +
                                       units::FormatNumber(t.upper)
                                 : t.isoClass;
    std::string limits;
    if (!nominal) {
      // Kept, not dropped: the intent outlives the topology that carried it.
      limits = "unresolved";
    } else if (t.printedPress) {
      limits = PrintedPressText(doc, t, sys);
    } else if (t.isoClass.empty()) {
      limits = units::FormatLength(*nominal + t.lower, sys) + " .. " +
               units::FormatLength(*nominal + t.upper, sys);
    } else {
      std::string why;
      if (const auto b = fits::Limits(*nominal, t.isoClass, &why))
        limits = units::FormatLength(b->Min(*nominal), sys) + " .. " +
                 units::FormatLength(b->Max(*nominal), sys);
      else
        limits = why;
    }
    ImGui::TextWrapped("%s  -  %s  -  %s", what.c_str(), band.c_str(),
                       limits.c_str());
    // The stable names are the identity and stay one hover away, exactly as
    // they do on a measure row.
    ImGui::SetItemTooltip("%s%s", t.ref.name.c_str(),
                          t.IsDistance() ? (" -> " + t.ref2.name).c_str() : "");
    if (!t.comment.empty())
      ImGui::TextDisabled("   %s", t.comment.c_str());
  }
  if (drop != 0 && state.commands != nullptr)
    state.commands->Execute(std::make_unique<ClearToleranceCommand>(drop), doc);
}

} // namespace ui

namespace ui {

namespace {

// Check print (docs/tolerances/README.md#checking-a-print): the eight items of
// ui::RunPrintCheck, measured against limit, a failure amber. The same call
// check_print makes, so the button and the RPC cannot disagree.
void DrawPrintCheck(AppState& state, Document& doc) {
  ImGui::Separator();
  if (ImGui::SmallButton("Check print###print_check")) {
    state.printCheck.reset();
    state.printCheckError.clear();
    try {
      state.printCheck = RunPrintCheck(state, doc, nullptr, {});
      state.printCheckDoc = &doc;
      state.printCheckVersion = doc.Version();
    } catch (const std::exception& e) {
      state.printCheckError = e.what();
    }
  }
  ImGui::SetItemTooltip("On the bed, one piece per body, overhangs, "
                        "bridges, the gap between bodies, the STL's "
                        "chords, every piece on the bed with the air "
                        "over another, the plate inside the printer's "
                        "bed, and no piece standing too slender, against "
                        "this print");
  const ImVec4 amber(0.95f, 0.80f, 0.40f, 1.0f);
  if (!state.printCheckError.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, amber);
    ImGui::TextWrapped("%s", state.printCheckError.c_str());
    ImGui::PopStyleColor();
  }
  if (!state.printCheck || state.printCheckDoc != &doc)
    return;
  const PrintCheckRun& run = *state.printCheck;
  const printcheck::Report& r = run.report;
  ImGui::SameLine();
  if (state.printCheckVersion != doc.Version())
    ImGui::TextDisabled("the part has changed since");
  else
    ImGui::TextUnformatted(r.Ok() ? "prints" : "will not print as it is");
  const auto line = [&](const char* item, printcheck::Status st,
                        const std::string& text) {
    const bool fail = st == printcheck::Status::Fail;
    if (fail)
      ImGui::PushStyleColor(ImGuiCol_Text, amber);
    else if (st == printcheck::Status::Skipped)
      ImGui::PushStyleColor(ImGuiCol_Text,
                            ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s %s: %s", printcheck::StatusName(st), item,
                       text.c_str());
    if (fail || st == printcheck::Status::Skipped)
      ImGui::PopStyleColor();
  };
  const auto mm = [](double v) { return units::FormatNumber(v) + " mm"; };
  // Select the face a row is about, so the viewport points at it.
  const auto show = [&](const char* id, const printcheck::Where& w) {
    const auto& bodies = doc.GetBodies();
    const size_t bi =
        static_cast<size_t>(run.bodyIndex.at(static_cast<size_t>(w.body)));
    const Body& b = bodies.at(bi);
    const std::string* name = b.elementMap.NameOf(w.face);
    if (name == nullptr)
      return;
    ImGui::SameLine();
    if (ImGui::SmallButton(id)) {
      SelectionItem it;
      it.kind = SelectionItem::Kind::Face;
      it.element = ElementRef{b.createdBy, *name};
      state.selection.Replace(it);
    }
  };
  line("on the bed", r.bed.status,
       "lowest point " + mm(r.bed.lowestMm) + ", " +
           units::FormatNumber(r.bed.contactAreaMm2) + " mm² on it");
  line("one piece per body", r.shells.status,
       std::to_string(r.shells.pieces) + " for " +
           std::to_string(r.shells.expected) +
           (r.shells.watertight ? "" : ", not watertight"));
  line("overhang", r.overhang.status,
       r.overhang.status == printcheck::Status::Skipped
           ? r.overhang.note
           : units::FormatNumber(r.overhang.areaMm2) + " mm² past " +
                 units::FormatNumber(r.overhang.limitDeg) + "°, worst " +
                 units::FormatNumber(r.overhang.worstDeg) + "°");
  if (r.overhang.worst && r.overhang.worstDeg > 0.0)
    show("Show###print_check_overhang", *r.overhang.worst);
  line("bridges", r.bridges.status,
       r.bridges.faces.empty()
           ? std::string("none")
           : std::to_string(r.bridges.faces.size()) + ", widest " +
                 mm(r.bridges.worstSpanMm) + " of " + mm(r.bridges.limitMm));
  if (!r.bridges.faces.empty())
    show("Show###print_check_bridge", r.bridges.faces.front().where);
  line("gap between bodies", r.clearance.status,
       r.clearance.status == printcheck::Status::Skipped ? r.clearance.note
       : r.clearance.minMm
           ? mm(*r.clearance.minMm) + " of " + mm(r.clearance.requiredMm) +
                 " a side"
           : "no two bodies near, needs " + mm(r.clearance.requiredMm));
  line("STL chords", r.chords.status,
       r.chords.status == printcheck::Status::Skipped
           ? r.chords.note
           : mm(r.chords.deflectionMm) + " of " + mm(r.chords.limitMm));
  // Stacking: the pieces off the bed, then the closest pair.
  const printcheck::Stacking& st = r.stacking;
  const auto piece = [&](int pc) {
    std::string out;
    for (int b : st.pieces.at(static_cast<size_t>(pc)).bodies)
      out +=
          (out.empty() ? "" : " + ") +
          doc.GetBodies()
              .at(static_cast<size_t>(run.bodyIndex.at(static_cast<size_t>(b))))
              .name;
    return out;
  };
  std::string stack;
  if (st.status == printcheck::Status::Skipped)
    stack = st.note;
  else {
    if (!st.lifted.empty())
      stack = piece(st.lifted.front().piece) + " starts " +
              mm(st.lifted.front().heightMm) + " off the bed" +
              (st.lifted.size() > 1
                   ? " (" + std::to_string(st.lifted.size()) + " pieces)"
                   : "");
    if (!st.pairs.empty()) {
      const printcheck::Stacked& w = st.pairs.front();
      stack += (stack.empty() ? "" : "; ") + piece(w.above) + " over " +
               piece(w.below) + ": " + mm(w.minAirMm) + " of air for " +
               mm(st.limitMm) + ", " + units::FormatNumber(w.areaMm2) +
               " mm² under it";
    }
    if (stack.empty())
      stack = "every piece on the bed, " + mm(st.limitMm) + " of air kept";
  }
  line("stacking", st.status, stack);
  // The closest pair, else the highest piece off the bed.
  const printcheck::Where* worst =
      !st.pairs.empty() ? &st.pairs.front().where
      : !st.lifted.empty() && st.lifted.front().where.body >= 0
          ? &st.lifted.front().where
          : nullptr;
  if (worst != nullptr)
    show("Show###print_check_stacking", *worst);
  // The plate in the printer's build volume (the profile's bed_mm).
  const printcheck::BedFit& bf = r.bedFit;
  const auto xyz = [&](const glm::dvec3& v) {
    return units::FormatNumber(v.x) + " x " + units::FormatNumber(v.y) + " x " +
           units::FormatNumber(v.z) + " mm";
  };
  line("fits the bed", bf.status,
       bf.status == printcheck::Status::Skipped
           ? bf.note
           : xyz(bf.extentMm) + " in " + xyz(bf.bedMm) +
                 (bf.status == printcheck::Status::Fail
                      ? ", over by " + xyz(bf.shortfallMm)
                      : std::string()));
  // The tallest piece over what it stands on (the slender item).
  const printcheck::Slender& sl = r.slender;
  std::string slText = sl.note;
  if (sl.status != printcheck::Status::Skipped && !sl.pieces.empty()) {
    const printcheck::SlenderPiece& w = sl.pieces.front();
    const auto ratio = [](double v) {
      return std::isfinite(v) ? units::FormatNumber(v) + ":1"
                              : std::string("on an edge");
    };
    slText =
        units::FormatNumber(w.heightMm) + " mm on " +
        units::FormatNumber(w.footprintMm) + " mm" +
        (w.brimMm > 0.0 ? " + " + units::FormatNumber(w.brimMm) + " mm of brim"
                        : std::string()) +
        ", " + ratio(w.ratio) +
        (w.neck ? ", a neck of " + units::FormatNumber(w.neck->widthMm) +
                      " mm at " + units::FormatNumber(w.neck->atMm) + " mm, " +
                      ratio(w.neck->ratio)
                : std::string()) +
        " against " + units::FormatNumber(sl.limit) + ":1";
  }
  line("slender", sl.status, slText);
}

} // namespace

namespace {

// The part's slicer settings (core/PrintSettings.h): each field stated or
// left to the slicer preset, the pattern and brim picked from the lists the
// RPC accepts, so this cannot store what set_print_settings refuses. A body's
// override is made with set_print_settings {body}; here it is counted.
void DrawSlicerSettings(AppState& state, Document& doc) {
  static std::string refused;
  ImGui::SeparatorText("Slicer settings");
  const printset::PartSettings& have = doc.PrintSettings();
  printset::Settings next = have.part;
  bool edited = false;
  const auto count = [&](const char* label, const char* id,
                         std::optional<int>& v, int fallback) {
    bool on = v.has_value();
    const std::string box = std::string("###slicer_on_") + id;
    if (ImGui::Checkbox(box.c_str(), &on)) {
      v = on ? std::optional<int>(fallback) : std::nullopt;
      edited = true;
    }
    ImGui::SetItemTooltip("Stated here, or left to the slicer preset");
    ImGui::SameLine();
    if (!v) {
      ImGui::TextDisabled("%s: preset", label);
      return;
    }
    int n = *v;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    const std::string field = std::string(label) + "###slicer_" + id;
    if (ExprInputInt(state, field.c_str(), &n) && n != *v) {
      v = n;
      edited = true;
    }
  };
  count("Walls", "walls", next.walls, 2);
  count("Top layers", "top", next.topLayers, 5);
  count("Bottom layers", "bottom", next.bottomLayers, 3);
  {
    bool on = next.infillPct.has_value();
    if (ImGui::Checkbox("###slicer_on_infill", &on)) {
      next.infillPct = on ? std::optional<double>(15.0) : std::nullopt;
      edited = true;
    }
    ImGui::SetItemTooltip("Stated here, or left to the slicer preset");
    ImGui::SameLine();
    if (!next.infillPct) {
      ImGui::TextDisabled("Infill: preset");
    } else {
      double pct = *next.infillPct;
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
      if (ExprInputDouble(state, "Infill %###slicer_infill", &pct, "%.0f") &&
          pct != *next.infillPct) {
        next.infillPct = pct;
        edited = true;
      }
    }
  }
  {
    const std::string cur =
        next.infill ? printset::InfillName(*next.infill) : "preset";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    if (ImGui::BeginCombo("Pattern###slicer_pattern", cur.c_str())) {
      if (ImGui::Selectable("preset###slicer_pattern_preset", !next.infill)) {
        next.infill.reset();
        edited = true;
      }
      for (printset::Infill i : printset::AllInfills()) {
        const std::string label = printset::InfillName(i) +
                                  "###slicer_pattern_" +
                                  printset::InfillName(i);
        if (ImGui::Selectable(label.c_str(), next.infill == i)) {
          next.infill = i;
          edited = true;
        }
      }
      ImGui::EndCombo();
    }
  }
  {
    bool supports = next.supports.value_or(false);
    if (ImGui::Checkbox("Supports###slicer_supports", &supports)) {
      next.supports = supports;
      edited = true;
    }
    ImGui::SameLine();
    const printset::Brim brim = next.brim.value_or(printset::Brim::None);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    if (ImGui::BeginCombo("Brim###slicer_brim",
                          printset::BrimName(brim).c_str())) {
      for (printset::Brim b : printset::AllBrims()) {
        const std::string label =
            printset::BrimName(b) + "###slicer_brim_" + printset::BrimName(b);
        if (ImGui::Selectable(label.c_str(), b == brim)) {
          next.brim = b;
          edited = true;
        }
      }
      ImGui::EndCombo();
    }
  }
  if (edited) {
    printset::PartSettings all = have;
    all.part = next;
    std::string why = printset::Validate(next);
    for (const auto& [id, b] : all.bodies)
      if (why.empty())
        why = printset::Validate(printset::Merge(next, b));
    if (!why.empty()) {
      refused = why;
    } else {
      refused.clear();
      if (state.commands != nullptr && !(all == have))
        state.commands->Execute(
            std::make_unique<SetPrintSettingsCommand>(std::move(all)), doc);
    }
  }
  if (!refused.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
    ImGui::TextWrapped("%s", refused.c_str());
    ImGui::PopStyleColor();
  }
  if (!have.bodies.empty())
    ImGui::TextDisabled("%zu bod%s override%s these (set_print_settings "
                        "{body})",
                        have.bodies.size(),
                        have.bodies.size() == 1 ? "y" : "ies",
                        have.bodies.size() == 1 ? "s" : "");
}

} // namespace

void DrawPrintProcessSection(AppState& state) {
  if (state.document == nullptr)
    return;
  Document& doc = *state.document;
  const auto& have = doc.PrintProcess();
  if (!SectionHeader("Print process", -1, have.has_value()))
    return;
  const auto set = [&](std::optional<printfit::Process> next) {
    if (state.commands != nullptr && !(have == next))
      state.commands->Execute(
          std::make_unique<SetPrintProcessCommand>(std::move(next)), doc);
  };
  // "Not printed", then the user's profiles, then the built-in tables as
  // the defaults. Nothing else is offered, so the picker cannot choose what
  // set_print_process would refuse
  // (docs/tolerances/README.md#print-profiles).
  const std::string current =
      !have           ? std::string("Not printed")
      : have->profile ? have->profile->name
                      : have->process + " " + have->material + " (default)";
  ImGui::SetNextItemWidth(-1.0f);
  if (ImGui::BeginCombo("###print_process", current.c_str())) {
    if (ImGui::Selectable("Not printed###print_none", !have))
      set(std::nullopt);
    for (const printfit::Profile& pr : state.printProfiles) {
      const bool on = have && have->profile && have->profile->name == pr.name;
      const std::string label = pr.name + "###print_prof_" + pr.name;
      if (ImGui::Selectable(label.c_str(), on) && !on)
        set(printfit::FromProfile(pr));
      ImGui::SetItemTooltip("%s %s", pr.table.process.c_str(),
                            pr.table.material.c_str());
    }
    if (!state.printProfiles.empty())
      ImGui::Separator();
    for (const printfit::Table& t : printfit::Tables()) {
      const printfit::Process pr{t.process, t.material, std::nullopt};
      const std::string label = t.process + " " + t.material +
                                " (default)###print_" + t.process + "_" +
                                t.material;
      if (ImGui::Selectable(label.c_str(), have && *have == pr) &&
          !(have && *have == pr))
        set(pr);
    }
    ImGui::EndCombo();
  }
  if (state.printProfiles.empty()) {
    ImGui::TextDisabled("No print profiles: keep your printer's in");
    ImGui::SameLine();
    if (ImGui::SmallButton("Preferences › Print profiles###print_prefs"))
      OpenPreferences(state, "Print profiles");
  }
  if (!have) {
    ImGui::TextDisabled("Declare it and the fit checks say what to model "
                        "for a print.");
    return;
  }
  // How the part's copy stands against the store, SAID -- the CAM setup's
  // machine block, for a profile (CamTree.cpp).
  const ImVec4 amber(0.95f, 0.80f, 0.40f, 1.0f);
  const printfit::ProfileMatch match =
      printfit::MatchProfile(state.printProfiles, *have);
  const std::string name = have->profile ? have->profile->name : "";
  switch (match) {
  case printfit::ProfileMatch::Builtin:
  case printfit::ProfileMatch::Same:
    break;
  case printfit::ProfileMatch::Differs:
    ImGui::PushStyleColor(ImGuiCol_Text, amber);
    ImGui::TextWrapped("Modelled with a different '%s' than yours.",
                       name.c_str());
    ImGui::PopStyleColor();
    if (ImGui::SmallButton("Use mine###print_use_mine"))
      set(printfit::FromProfile(
          *printfit::FindProfile(state.printProfiles, name)));
    ImGui::SetItemTooltip("Replace the part's copy with your '%s' (undoable)",
                          name.c_str());
    ImGui::SameLine();
    break;
  case printfit::ProfileMatch::Missing:
    ImGui::PushStyleColor(ImGuiCol_Text, amber);
    ImGui::TextWrapped("'%s' is not one of your profiles.", name.c_str());
    ImGui::PopStyleColor();
    break;
  }
  if (match == printfit::ProfileMatch::Differs ||
      match == printfit::ProfileMatch::Missing) {
    // ONE call site for both cases (widget_ids_lint): the label says which.
    if (ImGui::SmallButton(match == printfit::ProfileMatch::Differs
                               ? "Save as mine###print_save_mine"
                               : "Save it###print_save_mine")) {
      std::string error;
      if (!SavePrintProfile(state, *have->profile, &error))
        state.printProfileError = error;
      else
        state.printProfileError.clear();
    }
    ImGui::SetItemTooltip("Write the part's copy to your print profiles as "
                          "'%s'",
                          name.c_str());
  }
  if (!state.printProfileError.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
    ImGui::TextWrapped("%s", state.printProfileError.c_str());
    ImGui::PopStyleColor();
  }
  if (const printfit::Table* t = printfit::Find(*have)) {
    for (const printfit::Row& r : t->rows) {
      std::string range =
          r.toMm >= 1e8    ? ("from " + units::FormatNumber(r.fromMm) + " mm")
          : r.fromMm > 0.0 ? (units::FormatNumber(r.fromMm) + "-" +
                              units::FormatNumber(r.toMm) + " mm")
                           : ("under " + units::FormatNumber(r.toMm) + " mm");
      if (r.fromMm == 0.0 && r.toMm >= 1e8)
        range = "any size";
      ImGui::TextWrapped("%s %s%s mm on the diameter (%s)",
                         printfit::PrintedName(r.kind).c_str(),
                         r.allowanceMm >= 0.0 ? "+" : "",
                         units::FormatNumber(r.allowanceMm).c_str(),
                         range.c_str());
      ImGui::SetItemTooltip("%s", r.source.c_str());
    }
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", t->conditions.c_str());
    ImGui::PopStyleColor();
  }
  // The built-in tables are defaults; a profile is already the user's
  // measurement, and telling them to confirm it again is noise.
  if (!have->profile) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.35f, 1.0f));
    ImGui::TextWrapped("%s", printfit::ConfirmNote().c_str());
    ImGui::PopStyleColor();
  }
  // The part's threads, each with what the print wants it modelled at.
  for (size_t i = 0; i < doc.FeatureCount(); i++)
    if (const auto* th = dynamic_cast<const ThreadFeature*>(doc.GetFeature(i)))
      if (const auto r = PrintedThreadFor(doc, *th))
        ImGui::TextWrapped("%s", r->Sentence().c_str());
  DrawSlicerSettings(state, doc);
  DrawPrintCheck(state, doc);
}

} // namespace ui
