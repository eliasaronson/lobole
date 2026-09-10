#include "ui/ToleranceActions.h"

#include "core/Command.h"
#include "core/Document.h"
#include "core/FitStandards.h"
#include "core/Measure.h"
#include "core/Units.h"
#include "ui/AppState.h"
#include "ui/ExprInput.h"

#include <imgui.h>

#include <optional>
#include <string>

namespace ui {
namespace {

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
    if (have->isoClass.empty()) {
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
  if (!ImGui::CollapsingHeader("Tolerances"))
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
    // row wrapped in one can never be clicked by a test (docs/ui/
    // gui-coverage.md — it is why the Variables and Configurations panels went
    // untested for months). The ids below already carry the tolerance's own
    // id, which is what makes them unique.
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

    const std::string band = t.isoClass.empty()
                                 ? units::FormatNumber(t.lower) + " / " +
                                       units::FormatNumber(t.upper)
                                 : t.isoClass;
    std::string limits;
    if (!nominal) {
      // Kept, not dropped: the intent outlives the topology that carried it.
      limits = "unresolved";
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
