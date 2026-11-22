// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/AlignTool.h"

#include "common/Logger.h"
#include "core/Command.h"
#include "core/ImportAlign.h"
#include "core/io/Serialization.h"
#include "ui/BackgroundJobs.h"
#include "ui/ExprInput.h"
#include "ui/FeatureEditor.h" // kAlignToolSlot
#include "ui/LengthText.h"    // ReadableLength
#include "ui/PanelLayout.h"   // LeftLabel
#include "ui/PreviewFeature.h"
#include "ui/Widgets.h" // DrawJobProgress

#include "imgui.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <exception>
#include <string>
#include <thread>

namespace ui {

// A best fit running on an `align` job: the worker writes the result, the
// job's finish lands it on the frame.
struct AlignToolRun {
  importalign::Result result;
  std::string error;
};

namespace {

using importalign::Mode;

constexpr const char* kModeLabels[] = {"Floor", "Three points",
                                       "Principal axes", "Scale", "Best fit"};
constexpr int kModes = 5;
// The slot of the best fit's onto pick, past every point's.
constexpr int kOntoSlot = 10;
// Best fit's pairs: pair i's point on the import, and where it goes on the
// body.
constexpr int kPairScanSlot = 20, kPairBodySlot = 30;
bool IsPairSlot(int slot) {
  return (slot >= kPairScanSlot && slot < kPairScanSlot + 3) ||
         (slot >= kPairBodySlot && slot < kPairBodySlot + 3);
}

ImportFeature* AlignedImport(AppState& state, size_t* index = nullptr) {
  const auto& t = state.alignTool;
  if (!t.open || state.document == nullptr ||
      state.document.get() != t.document)
    return nullptr;
  return dynamic_cast<ImportFeature*>(
      state.document->FindFeatureById(t.featureId, index));
}

// Stops the best fit in flight, if any: what it would land is no longer
// asked for.
void StopFit(AppState& state) {
  auto& t = state.alignTool;
  if (t.jobId != 0)
    (void)CancelBackgroundJobById(state, t.jobId, "the Align tool moved on");
  t.jobId = 0;
  t.run.reset();
}

void Reset(AppState& state) {
  if (state.slotPick.featureId == kAlignToolSlot)
    state.slotPick = {};
  StopFit(state);
  state.alignTool = AppState::AlignTool{};
}

// The bodies `id` made, as the document has them now.
std::vector<const Body*> BodiesOf(const Document& doc, uint64_t id) {
  std::vector<const Body*> out;
  for (const Body& b : doc.GetBodies())
    if (b.createdBy == id)
      out.push_back(&b);
  return out;
}

// Writes `p` onto the import and regenerates: the preview, off the undo
// stack, as a pending feature's parameters are.
void Show(AppState& state, ImportFeature& imp,
          const importplace::Placement& p) {
  if (imp.placement == p)
    return;
  imp.placement = p;
  // An alignment writes the placement whole: what it moved is no longer
  // what a text says (docs/interchange/README.md#placing-an-import).
  imp.translateExpr = {};
  imp.rotateExpr = {};
  imp.scaleExpr.clear();
  state.document->Regenerate();
}

std::string Prompt(Mode mode, size_t i) {
  switch (mode) {
  case Mode::Floor:
    return "Align: click the floor of the import (on an open scan, the rim "
           "of the hole where it stood)";
  case Mode::ThreePoints:
    return "Align: click point " + std::to_string(i + 1) +
           " of 3 on the floor plane";
  case Mode::Scale:
    return "Align: click " + std::string(i == 0 ? "one end" : "the other end") +
           " of a distance you know";
  case Mode::Principal:
    break;
  case Mode::BestFit:
    return "Align: click the body to lay the import onto";
  }
  return "";
}

std::string PairPrompt(int slot) {
  if (slot >= kPairBodySlot)
    return "Align: click where pair " +
           std::to_string(slot - kPairBodySlot + 1) +
           "'s point goes on the body";
  return "Align: click pair " + std::to_string(slot - kPairScanSlot + 1) +
         "'s point on the import";
}

void ArmPair(AppState& state, int slot) {
  // Picked where the import stood, not where a fit laid it over the body:
  // there the two surfaces lie on each other and a click cannot tell them
  // apart.
  auto& t = state.alignTool;
  if (ImportFeature* imp = AlignedImport(state); imp != nullptr && t.preview) {
    StopFit(state);
    Show(state, *imp, t.original);
    t.preview.reset();
    t.summary.clear();
  }
  state.slotPick = {};
  state.slotPick.featureId = kAlignToolSlot;
  state.slotPick.slot = slot;
  state.slotPick.kinds = SelectionKindBit(SelectionItem::Kind::Face);
  state.slotPick.prompt = PairPrompt(slot);
}

// Pairs picked: none, some, or all three of each.
size_t PairsPicked(const AppState::AlignTool& t) {
  size_t n = 0;
  for (int i = 0; i < 3; i++)
    n += (t.pairScan[i].has_value() ? 1 : 0) +
         (t.pairBody[i].has_value() ? 1 : 0);
  return n;
}

// The next unpicked pair point, in the order a person gives them: pair 1 on
// the import, then on the body, then pair 2.
std::optional<int> NextPairSlot(const AppState::AlignTool& t) {
  for (int i = 0; i < 3; i++) {
    if (!t.pairScan[i])
      return kPairScanSlot + i;
    if (!t.pairBody[i])
      return kPairBodySlot + i;
  }
  return std::nullopt;
}

void Arm(AppState& state, size_t i) {
  state.slotPick = {};
  state.slotPick.featureId = kAlignToolSlot;
  state.slotPick.slot = static_cast<int>(i);
  state.slotPick.kinds = SelectionKindBit(SelectionItem::Kind::Face);
  state.slotPick.prompt = Prompt(Mode(state.alignTool.mode), i);
}

void ArmOnto(AppState& state) {
  state.slotPick = {};
  state.slotPick.featureId = kAlignToolSlot;
  state.slotPick.slot = kOntoSlot;
  state.slotPick.kinds = SelectionKindBit(SelectionItem::Kind::Face);
  state.slotPick.prompt = Prompt(Mode::BestFit, 0);
}

void ArmNext(AppState& state) {
  const auto& t = state.alignTool;
  if (Mode(t.mode) == Mode::BestFit && t.ontoId == 0)
    ArmOnto(state);
  else if (t.points.size() < importalign::PointsFor(Mode(t.mode)))
    Arm(state, t.points.size());
  else if (state.slotPick.featureId == kAlignToolSlot)
    state.slotPick = {};
}

// What an alignment fitted, a line a fact, for the tool window: lengths in
// the document's unit with a floor (ui/LengthText.h), never an exponent.
// The reply's note names RPC parameters; the tool says what to do here.
std::string Summary(Mode mode, const nlohmann::json& r,
                    const units::System& sys) {
  const auto len = [&](const char* key) {
    return ReadableLength(r.value(key, 0.0), sys);
  };
  const auto pct = [&](double share) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f%%", 100.0 * share);
    return std::string(buf);
  };
  std::string out;
  switch (mode) {
  case Mode::Floor:
    if (r.value("fitted", std::string()) == "boundary_loop")
      out = "Fitted the rim: " +
            std::to_string(r.value("loop_vertices", size_t{0})) +
            " vertices\nRMS " + len("rms_mm") + ", max " + len("max_mm");
    else
      out = "Fitted " + std::to_string(r.value("triangles", size_t{0})) +
            " triangles, " + units::FormatNumber(r.value("area_mm2", 0.0), 4) +
            " " + units::AreaSuffix(sys) + "\nRMS " + len("rms_mm") + ", max " +
            len("max_mm") + ", within " + len("tolerance_mm");
    break;
  case Mode::ThreePoints:
    out = "The plane through the three points";
    break;
  case Mode::Principal: {
    const auto s = r.value("spread_mm", std::vector<double>{0, 0, 0});
    out = "Spread along X, Y, Z: " + ReadableLength(s.at(0), sys) + ", " +
          ReadableLength(s.at(1), sys) + ", " + ReadableLength(s.at(2), sys);
    break;
  }
  case Mode::Scale:
    out = "Measured " + len("measured_mm") + ": scaled by " +
          units::FormatNumber(r.value("factor", 1.0), 6);
    break;
  case Mode::BestFit:
    out =
        "Laid onto '" + r.value("onto", std::string()) + "'" +
        (r.value("winner", std::string()) == "pairs" ? " from the pairs" : "") +
        "\nMean " + len("mean_mm") + ", " + pct(r.value("within", 0.0)) +
        " within " + len("within_mm") + "\nMoved up to " + len("moved_mm");
    if (r.value("ambiguous", false) && r.contains("runner_up"))
      out += "\nAnother attitude, " +
             units::FormatAngle(
                 std::round(r.at("runner_up").value("turn_deg", 0.0)), sys) +
             " from this one, lies nearly as close: the shape barely tells "
             "them apart. If this is the wrong one, pick three Pairs below "
             "-- a point on the import and where it goes on the body -- to "
             "say which.";
    if (r.contains("pairs_scale_differ"))
      out += "\nThe pairs' distances differ by up to " +
             pct(r.at("pairs_scale_differ").get<double>()) +
             " between the import and the body: a best fit is rigid, so a "
             "wrong scale stays (the Scale mode fixes it).";
    break;
  }
  if (mode != Mode::BestFit && r.contains("note"))
    out += "\n" + r.at("note").get<std::string>();
  return out;
}

// `text` wrapped at its spaces only, to `width` pixels, each line dim or in
// `colour`: ImGui's own wrap breaks after a full stop, and "0." / "00458 mm"
// is a number nobody reads. A line break in `text` is kept.
void Wrapped(const std::string& text, float width,
             const ImVec4* colour = nullptr) {
  const auto draw = [&](const std::string& line) {
    if (colour != nullptr)
      ImGui::TextColored(*colour, "%s", line.c_str());
    else
      ImGui::TextDisabled("%s", line.c_str());
  };
  size_t start = 0;
  while (start <= text.size()) {
    size_t nl = text.find('\n', start);
    if (nl == std::string::npos)
      nl = text.size();
    const std::string para = text.substr(start, nl - start);
    std::string line;
    size_t i = 0;
    while (i <= para.size()) {
      size_t sp = para.find(' ', i);
      if (sp == std::string::npos)
        sp = para.size();
      const std::string word = para.substr(i, sp - i);
      const std::string tryLine = line.empty() ? word : line + " " + word;
      if (!line.empty() && ImGui::CalcTextSize(tryLine.c_str()).x > width) {
        draw(line);
        line = word;
      } else {
        line = tryLine;
      }
      i = sp + 1;
    }
    draw(line);
    start = nl + 1;
  }
}

// Lands a best fit that finished: previewed, or why not. Only the run the
// tool still waits on: one it stopped is dropped.
void LandFit(AppState& state, const std::shared_ptr<AlignToolRun>& run) {
  auto& t = state.alignTool;
  if (!t.open || t.run != run)
    return;
  t.jobId = 0;
  t.run.reset();
  ImportFeature* imp = AlignedImport(state);
  if (imp == nullptr)
    return;
  if (!run->error.empty()) {
    t.error = run->error;
    return;
  }
  if (run->result.cancelled) {
    t.error = "Cancelled: the import stays where it stood";
    return;
  }
  t.summary = Summary(Mode::BestFit, run->result.report, state.ActiveUnits());
  Show(state, *imp, run->result.placement);
  t.preview = run->result.placement;
}

// The best fit onto the tool's body, started on an `align` job: the
// triangles gathered here, the fit on the worker.
void StartFit(AppState& state, const ImportFeature& imp) {
  auto& t = state.alignTool;
  const Feature* onto = state.document->FindFeatureById(t.ontoId);
  const std::vector<const Body*> bodies = BodiesOf(*state.document, t.ontoId);
  if (onto == nullptr || bodies.empty()) {
    t.error = "the body to lay it onto is gone: pick another";
    t.ontoId = 0;
    ArmNext(state);
    return;
  }
  auto input = std::make_shared<const importalign::BestFitInput>(
      importalign::GatherBestFit(
          importalign::ImportBodies(*state.document, imp), bodies));
  importalign::Request rq;
  rq.mode = Mode::BestFit;
  // The pairs, when all three are picked: the one start they name.
  if (PairsPicked(t) == 6)
    for (int i = 0; i < 3; i++)
      rq.pairsMm.push_back({*t.pairScan[i], *t.pairBody[i]});
  if (std::string why = importalign::CheckBestFit(*input, rq); !why.empty()) {
    // The reply names its parameter; here it is the Pairs rows.
    if (why.rfind("pairs_mm: ", 0) == 0)
      why = "Pairs: " + why.substr(10);
    t.error = why;
    return;
  }
  auto run = std::make_shared<AlignToolRun>();
  auto bg = std::make_unique<BackgroundJob>();
  bg->kind = "align";
  bg->label = "Best fit onto '" + onto->GetName() + "'";
  bg->cancellable = true;
  bg->fraction.store(0.0);
  BackgroundJob* raw = bg.get();
  bg->thread = std::thread([run, raw, input, rq, current = t.original]() {
    importalign::Progress progress;
    progress.cancel = &raw->cancel;
    progress.fraction = [raw](double f) { raw->fraction.store(f); };
    try {
      std::string why;
      if (!importalign::BestFit(*input, current, rq, progress, run->result,
                                why))
        run->error = why;
    } catch (const std::exception& e) {
      run->error = e.what();
    }
    raw->done.store(true); // last
  });
  bg->finish = [run](AppState& s) {
    LandFit(s, run);
    return true;
  };
  t.run = run;
  t.jobId = StartBackgroundJob(state, std::move(bg)).id;
  LOG_INFO("tool: Align best fit onto '{}' started", onto->GetName());
}

// The alignment of the points as picked, previewed; or, while the mode
// lacks an input, the import where it was.
void Recompute(AppState& state) {
  auto& t = state.alignTool;
  ImportFeature* imp = AlignedImport(state);
  if (imp == nullptr)
    return;
  t.error.clear();
  t.summary.clear();
  const Mode mode = Mode(t.mode);
  // Always from where it stood: the points were picked there, and an
  // alignment of the preview would compound.
  Show(state, *imp, t.original);
  t.preview.reset();
  StopFit(state);
  if (mode == Mode::BestFit) {
    // Some pairs picked and not all: nothing to fit until they are, or
    // they are cleared.
    const size_t pairs = PairsPicked(t);
    if (t.ontoId != 0 && (pairs == 0 || pairs == 6))
      StartFit(state, *imp);
    return;
  }
  if (t.points.size() < importalign::PointsFor(mode))
    return;
  importalign::Request rq;
  rq.mode = mode;
  rq.pointsMm = t.points;
  rq.distanceMm = t.distanceMm;
  if (mode == Mode::Floor && t.ownTolerance)
    rq.toleranceMm = t.toleranceMm;
  importalign::Result res;
  std::string why;
  try {
    if (!importalign::Align(importalign::ImportBodies(*state.document, *imp),
                            t.original, rq, res, why)) {
      t.error = why;
      return;
    }
  } catch (const std::exception& e) {
    t.error = e.what();
    return;
  }
  t.summary = Summary(mode, res.report, state.ActiveUnits());
  // Re-resolved: the regeneration above may have moved the feature list's
  // storage, never the feature itself, but ask again rather than assume.
  imp = AlignedImport(state);
  if (imp == nullptr)
    return;
  Show(state, *imp, res.placement);
  t.preview = res.placement;
}

// A default derived from the import: a five-hundredth of its diagonal, as
// align_import's.
double DerivedTolerance(AppState& state, const ImportFeature& imp) {
  double diag = 0.0;
  glm::dvec3 lo(0.0), hi(0.0), blo, bhi;
  bool any = false;
  for (const Body* b : importalign::ImportBodies(*state.document, imp))
    if (BodyBounds(*b, blo, bhi)) {
      lo = any ? glm::min(lo, blo) : blo;
      hi = any ? glm::max(hi, bhi) : bhi;
      any = true;
    }
  if (any)
    diag = glm::length(hi - lo);
  return diag / 500.0;
}

} // namespace

bool HasImportToAlign(const AppState& state) {
  if (state.document == nullptr)
    return false;
  for (size_t i = 0; i < state.document->FeatureCount(); i++)
    if (dynamic_cast<const ImportFeature*>(state.document->GetFeature(i)))
      return true;
  return false;
}

void OpenAlignTool(AppState& state, uint64_t importId) {
  if (state.document == nullptr)
    return;
  Document& doc = *state.document;
  const auto isImport = [&](uint64_t id) {
    return id != 0 && dynamic_cast<const ImportFeature*>(
                          doc.FindFeatureById(id)) != nullptr;
  };
  // What was picked first: a body, a face or a tree row of an Import.
  if (!isImport(importId))
    importId = 0;
  for (const SelectionItem& item : state.selection.items) {
    if (importId != 0)
      break;
    const uint64_t id = item.kind == SelectionItem::Kind::Face ||
                                item.kind == SelectionItem::Kind::Edge
                            ? item.element.featureId
                            : item.id;
    if (isImport(id))
      importId = id;
  }
  for (size_t i = doc.FeatureCount(); importId == 0 && i > 0; i--)
    if (dynamic_cast<const ImportFeature*>(doc.GetFeature(i - 1)))
      importId = doc.GetFeature(i - 1)->GetId();
  if (importId == 0)
    return;
  // One session at a time: a preview, an editor or another alignment ends.
  CancelPendingFeature(state);
  CancelAlignTool(state);
  state.editFeatureId = 0;
  auto* imp = dynamic_cast<ImportFeature*>(doc.FindFeatureById(importId));
  auto& t = state.alignTool;
  t.open = true;
  t.document = &doc;
  t.featureId = importId;
  t.mode = static_cast<int>(Mode::Floor);
  t.original = imp->placement;
  t.originalTranslateExpr = imp->translateExpr;
  t.originalRotateExpr = imp->rotateExpr;
  t.originalScaleExpr = imp->scaleExpr;
  LOG_INFO("tool: Align started on '{}'", imp->GetName());
  ArmNext(state);
}

void CancelAlignTool(AppState& state) {
  if (!state.alignTool.open)
    return;
  // Put back only what the preview wrote: an RPC or an undo that changed
  // the placement since is not the tool's to overwrite.
  if (ImportFeature* imp = AlignedImport(state);
      imp != nullptr && state.alignTool.preview &&
      imp->placement == *state.alignTool.preview) {
    imp->placement = state.alignTool.original;
    imp->translateExpr = state.alignTool.originalTranslateExpr;
    imp->rotateExpr = state.alignTool.originalRotateExpr;
    imp->scaleExpr = state.alignTool.originalScaleExpr;
    state.document->Regenerate();
  }
  LOG_INFO("tool: Align cancelled");
  Reset(state);
}

bool AcceptAlignTool(AppState& state) {
  auto& t = state.alignTool;
  size_t index = 0;
  ImportFeature* imp = AlignedImport(state, &index);
  if (imp == nullptr || !t.preview || *t.preview == t.original ||
      !(imp->placement == *t.preview))
    return false;
  // One undo step from where it stood to where the preview put it.
  ImportFeature was(*imp);
  was.placement = t.original;
  was.translateExpr = t.originalTranslateExpr;
  was.rotateExpr = t.originalRotateExpr;
  was.scaleExpr = t.originalScaleExpr;
  const nlohmann::json before = DocumentIO::FeatureToJson(was);
  const nlohmann::json after = DocumentIO::FeatureToJson(*imp);
  LOG_INFO("tool: Align accepted ({})", importalign::ToString(Mode(t.mode)));
  Reset(state);
  state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after),
      *state.document);
  return true;
}

void AlignPointPicked(AppState& state, const SelectionItem& item) {
  auto& t = state.alignTool;
  ImportFeature* imp = AlignedImport(state);
  if (imp == nullptr) {
    state.slotPick = {};
    return;
  }
  if (IsPairSlot(state.slotPick.slot)) {
    const int slot = state.slotPick.slot;
    const bool onBody = slot >= kPairBodySlot;
    const uint64_t want = onBody ? t.ontoId : t.featureId;
    if (item.kind != SelectionItem::Kind::Face ||
        item.element.featureId != want) {
      const Feature* onto = state.document->FindFeatureById(t.ontoId);
      state.slotPick.miss =
          onBody ? "that is not on '" +
                       (onto != nullptr ? onto->GetName() : std::string()) +
                       "': click where the point goes on it"
                 : "that is not on '" + imp->GetName() +
                       "': click a point on the import";
      return;
    }
    // The import's point where it stood before the preview moved it, which
    // is where the fit starts from; the body does not move.
    glm::dvec3 p = state.slotPick.hitPoint;
    if (!onBody && t.preview)
      p = glm::dvec3(t.original.Matrix() * glm::inverse(t.preview->Matrix()) *
                     glm::dvec4(p, 1.0));
    (onBody ? t.pairBody : t.pairScan)[size_t(slot % 10)] = p;
    state.slotPick = {};
    if (const std::optional<int> next = NextPairSlot(t))
      ArmPair(state, *next);
    Recompute(state);
    return;
  }
  if (state.slotPick.slot == kOntoSlot) {
    const uint64_t id =
        item.kind == SelectionItem::Kind::Face ? item.element.featureId : 0;
    if (id == 0 || id == t.featureId || BodiesOf(*state.document, id).empty()) {
      state.slotPick.miss = id == t.featureId
                                ? "that is the import itself: click the body "
                                  "to lay it onto"
                                : "click a face of the body to lay the "
                                  "import onto";
      return;
    }
    if (id != t.ontoId)
      t.pairBody = {}; // they were on the other body
    t.ontoId = id;
    state.slotPick = {};
    Recompute(state);
    return;
  }
  if (item.kind != SelectionItem::Kind::Face ||
      item.element.featureId != t.featureId) {
    state.slotPick.miss =
        "that is not on '" + imp->GetName() + "': click a point on the import";
    return;
  }
  // Clicked where the bodies are shown; kept where they stood before the
  // preview moved them, which is where every point is computed from.
  glm::dvec3 p = state.slotPick.hitPoint;
  if (t.preview)
    p = glm::dvec3(t.original.Matrix() * glm::inverse(t.preview->Matrix()) *
                   glm::dvec4(p, 1.0));
  const size_t slot = static_cast<size_t>(std::max(0, state.slotPick.slot));
  if (slot < t.points.size())
    t.points[slot] = p;
  else
    t.points.push_back(p);
  // The true distance starts as the one measured, so nothing scales until
  // it is typed: a default derived from the picks, never a literal.
  if (Mode(t.mode) == Mode::Scale && t.points.size() == 2 &&
      !(t.distanceMm > 0.0))
    t.distanceMm = glm::length(t.points[1] - t.points[0]);
  state.slotPick = {};
  ArmNext(state);
  Recompute(state);
}

void DrawAlignToolWindow(AppState& state) {
  auto& t = state.alignTool;
  ImportFeature* imp = AlignedImport(state);
  if (imp == nullptr) {
    Reset(state); // the document or the import went away
    return;
  }
  BeginToolWindowFrame(state, "Align");
  ImGui::TextDisabled("Import: %s", imp->GetName().c_str());
  int mode = t.mode;
  LeftLabel("Mode");
  ImGui::SetNextItemWidth(-1);
  if (ImGui::Combo("##align_mode", &mode, kModeLabels, kModes) &&
      mode != t.mode) {
    t.mode = mode;
    t.points.clear();
    t.distanceMm = 0.0;
    t.ontoId = 0;
    t.pairScan = {};
    t.pairBody = {};
    ArmNext(state);
    Recompute(state); // principal needs no point: it previews at once
  }
  const Mode m = Mode(t.mode);
  const size_t need = importalign::PointsFor(m);
  for (size_t i = 0; i < need; i++) {
    char text[128];
    const bool armed = state.slotPick.featureId == kAlignToolSlot &&
                       state.slotPick.slot == static_cast<int>(i);
    if (i < t.points.size())
      std::snprintf(text, sizeof text, "Point %zu: %.4g, %.4g, %.4g%s", i + 1,
                    t.points[i].x, t.points[i].y, t.points[i].z,
                    armed ? " (click again)" : "");
    else
      std::snprintf(text, sizeof text, "Point %zu: %s", i + 1,
                    armed ? "click it in the view" : "not picked");
    // One id a point, whatever the row says: tests click ###align_pt<i>.
    const std::string label =
        text + std::string("###align_pt") + std::to_string(i);
    if (ImGui::Button(label.c_str()))
      Arm(state, i);
  }
  if (m == Mode::BestFit) {
    const bool armed = state.slotPick.featureId == kAlignToolSlot &&
                       state.slotPick.slot == kOntoSlot;
    const Feature* onto =
        t.ontoId != 0 ? state.document->FindFeatureById(t.ontoId) : nullptr;
    const std::string label =
        (onto != nullptr
             ? "Onto: " + onto->GetName() + (armed ? " (click again)" : "")
             : std::string("Onto: ") +
                   (armed ? "click it in the view" : "not picked")) +
        "###align_onto";
    if (ImGui::Button(label.c_str()))
      ArmOnto(state);
    if (BackgroundJob* job =
            t.jobId != 0 ? FindBackgroundJobById(state, t.jobId) : nullptr) {
      JobProgress bar;
      bar.status = job->label;
      bar.fraction = job->fraction.load();
      bar.cancellable = job->cancellable;
      bar.cancelAsked = job->cancel.load();
      bar.cancelId = "###align_cancel_fit";
      bar.cancelTip = "Stop fitting: the import stays where it stood";
      if (DrawJobProgress(bar))
        (void)CancelBackgroundJobById(state, t.jobId, "cancelled in the tool");
    }
    // Pairs, optional: three points on the import and where each goes on
    // the body, which name the attitude when the shape does not.
    if (onto != nullptr) {
      ImGui::Spacing();
      ImGui::TextUnformatted("Pairs");
      ImGui::SameLine();
      ImGui::TextDisabled("optional: which way it lies");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Three points on the import, each with where it "
                          "goes on '%s': the fit starts from them alone",
                          onto->GetName().c_str());
      const auto point = [&](const char* side, int slot,
                             const std::optional<glm::dvec3>& p) {
        const bool armed = state.slotPick.featureId == kAlignToolSlot &&
                           state.slotPick.slot == slot;
        const std::string label = std::string(side) +
                                  (armed ? ": click it"
                                   : p   ? ": picked"
                                         : ": pick") +
                                  "###align_pair" + std::to_string(slot);
        const bool pressed = ImGui::Button(label.c_str());
        if (p && ImGui::IsItemHovered()) {
          char at[96];
          std::snprintf(at, sizeof at, "%.4g, %.4g, %.4g (click to pick again)",
                        p->x, p->y, p->z);
          ImGui::SetTooltip("%s", at);
        }
        if (pressed)
          ArmPair(state, slot);
      };
      for (int i = 0; i < 3; i++) {
        ImGui::PushID(i);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%d", i + 1);
        ImGui::SameLine();
        point("Import", kPairScanSlot + i, t.pairScan[size_t(i)]);
        ImGui::SameLine();
        point("Body", kPairBodySlot + i, t.pairBody[size_t(i)]);
        ImGui::SameLine();
        const bool any = t.pairScan[size_t(i)] || t.pairBody[size_t(i)];
        ImGui::BeginDisabled(!any);
        const std::string clear = "x###align_pair_clear" + std::to_string(i);
        if (ImGui::SmallButton(clear.c_str())) {
          t.pairScan[size_t(i)].reset();
          t.pairBody[size_t(i)].reset();
          if (IsPairSlot(state.slotPick.slot))
            state.slotPick = {};
          Recompute(state);
        }
        ImGui::EndDisabled();
        if (any && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          ImGui::SetTooltip("Clear pair %d", i + 1);
        ImGui::PopID();
      }
    }
  }
  if (m == Mode::Floor) {
    bool own = t.ownTolerance;
    if (ImGui::Checkbox("Own tolerance###align_own_tol", &own)) {
      t.ownTolerance = own;
      if (own && !(t.toleranceMm > 0.0))
        t.toleranceMm = DerivedTolerance(state, *imp);
      Recompute(state);
    }
    if (t.ownTolerance) {
      LeftLabel("Tolerance (mm)");
      ImGui::SetNextItemWidth(-1);
      double tol = t.toleranceMm;
      if (ExprInputDouble(state, "##align_tol", &tol, "%.4g") && tol > 0.0 &&
          std::isfinite(tol) && tol != t.toleranceMm) {
        t.toleranceMm = tol;
        Recompute(state);
      }
    } else {
      ImGui::TextDisabled("Tolerance: a 500th of the import's size");
    }
  }
  if (m == Mode::Scale && t.points.size() == 2) {
    LeftLabel("True distance (mm)");
    ImGui::SetNextItemWidth(-1);
    double d = t.distanceMm;
    if (ExprInputDouble(state, "##align_distance", &d, "%.6g") && d > 0.0 &&
        std::isfinite(d) && d != t.distanceMm) {
      t.distanceMm = d;
      Recompute(state);
    }
  }
  if (state.slotPick.featureId == kAlignToolSlot &&
      !state.slotPick.miss.empty())
    ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.3f, 1.0f), "%s",
                       state.slotPick.miss.c_str());
  if (!t.error.empty()) {
    const ImVec4 red(0.9f, 0.4f, 0.4f, 1.0f);
    Wrapped(t.error, 360.0f, &red);
  } else if (!t.summary.empty()) {
    Wrapped(t.summary, 360.0f);
  }
  ImGui::Separator();
  const bool ready = t.preview.has_value() && !(*t.preview == t.original);
  if (!ready && t.error.empty()) {
    if (m == Mode::BestFit) {
      const size_t pairs = PairsPicked(t);
      ImGui::TextDisabled(t.ontoId == 0  ? "Pick the body to lay it onto"
                          : t.jobId != 0 ? "Fitting..."
                          : pairs > 0 && pairs < 6
                              ? "Pick the rest of the pairs, or clear them"
                              : "It stands this way already");
    } else if (t.points.size() < need)
      ImGui::TextDisabled("Pick point %zu to continue", t.points.size() + 1);
    else if (m == Mode::Scale)
      ImGui::TextDisabled("Type the true distance to continue");
    else
      ImGui::TextDisabled("It stands this way already");
  }
  ImGui::BeginDisabled(!ready);
  ImGui::PushStyleColor(ImGuiCol_Button,
                        ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  const bool accept = ImGui::Button("Accept###tw_accept");
  ImGui::PopStyleColor();
  ImGui::EndDisabled();
  ImGui::SameLine();
  const bool cancel = ImGui::Button("Cancel###tw_cancel");
  ImGui::End();
  if (accept)
    AcceptAlignTool(state);
  else if (cancel)
    CancelAlignTool(state);
}

} // namespace ui
