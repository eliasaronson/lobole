#include "ui/AssemblyActions.h"

#include "core/cam/CamToolpath.h" // StockBounds (the placement snap)
#include "imgui.h" // KeyCtrl: the placement drag snaps unless Ctrl says free
#include "ui/CamPanel.h" // ActiveCamSetup, RefreshCamToolpaths (the placement gizmo)
#include "ui/CamTool.h" // RunCamVerification
#include "ui/DocumentSlots.h"
#include "ui/JobActions.h" // JobPlacedBodies
#include "ui/ProjectActions.h"
#include "ui/SelectionPipeline.h"
#include "ui/ViewActions.h"

#include "ui/FeatureEditor.h" // PickSlot, kMateSessionSlot (U7c mate slots)

#include "cad/CADKernel.h" // CADKernel::ImportedModel (import)
#include "cad/CADMaterials.h"
#include "cad/CADMeshConverter.h"
#include "cad/FaceSelector.h"
#include "common/Logger.h"
#include "core/AssemblySolve.h"
#include "core/Command.h"
#include "core/Feature.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/drawing/DrawingCallout.h"
#include "core/drawing/DrawingDimension.h"
#include "core/drawing/DrawingExport.h"
#include "core/drawing/DrawingProjection.h"
#include "core/drawing/DrawingRender.h"
#include "core/features/BooleanFeature.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FastenerFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/HoleFeature.h"
#include "core/io/Autosaver.h"
#include "core/io/GitVault.h"
#include "core/io/ImportAssembly.h"
#include "core/io/Serialization.h"
#include "core/parts/AxisStack.h"
#include "core/parts/FastenerProfile.h"
#include "core/parts/FastenerTable.h"
#include "render/Camera.h"
#include "render/FieldRenderer.h"
#include "render/IBLTextures.h"
#include "render/MeshRenderer.h"
#include "render/OutlineRenderer.h"
#include "render/RayTracingRenderer.h"
#include "render/SketchRenderer.h"
#include "render/VulkanCommon.h"
#include "ui/DragMath.h"

#include "core/features/PatternFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/SketchFeature.h"
#include "core/sketch/SketchProfile.h"
#include "ui/PreviewFeature.h"
#include "ui/SketchInteraction.h" // SketchUVToScreen
#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_transform.hpp> // glm::translate
#include <glm/gtc/quaternion.hpp>       // glm::mat4_cast

#include "ui/AppActionsInternal.h"
#include "ui/AppState.h"
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <OSD.hxx>              // per-thread signal handler (prewarm workers)
#include <Standard_Failure.hxx> // ...and what it converts faults into

#include <atomic>
#include <thread>

// The bill of materials, and the fastener checks that read the same stack:
// what parts an assembly is made of, how many of each, and whether every
// bolt in it is long enough for the joint it spans.
//
// Pure functions of a document -- no renderer, no ImGui -- so unlike most of
// the assembly code this can be tested headlessly.
// Split out of AssemblyActions.cpp (docs/architecture.md#the-ui-actions-layer).
// AssemblyActions.h still declares everything it declared.

namespace ui {

namespace {
// How far along the axis the LAST material is — the maximum exit, not the
// last segment's.
//
// `TraverseAxis` orders by where each solid is entered, which is not the same
// order as where they are left once anything overlaps: a plate moved down past
// a nut is entered after the washer and left after the nut. Reading the end of
// the ordered list said a stale screw still reached, which is precisely the
// case the check exists for.
double LastExit(const std::vector<parts::StackSegment>& stack) {
  double out = 0.0;
  for (const parts::StackSegment& seg : stack)
    out = std::max(out, seg.exit);
  return out;
}
} // namespace

BomTable ComputeBom(AppState& state) {
  BomTable table;
  if (!state.assembly)
    return table;
  // A5: aggregate over the LEAF parts of the whole tree, so a sub-assembly
  // placed twice doubles its parts' quantities. The per-unit BOM needs no
  // code of its own — it is this same function on that assembly's tab.
  for (const assembly::FlatInstance& inst : FlattenActiveAssembly(state)) {
    if (!inst.visible)
      continue; // hidden or suppressed, here or above: out of the BOM
    BomRow* row = nullptr;
    for (BomRow& r : table.rows)
      if (r.partId == inst.partId) {
        row = &r;
        break;
      }
    if (row) {
      row->quantity++;
      continue;
    }
    BomRow fresh;
    fresh.partId = inst.partId;
    fresh.quantity = 1;
    for (const PartSlot& slot : state.parts)
      if (slot.kind == DocKind::Part && slot.id == inst.partId)
        fresh.partName = slot.name;
    const Document* part = PartDocById(state, inst.partId);
    if (const FastenerFeature* screw = SoleFastenerOf(part)) {
      fresh.standard = screw->standard;
      fresh.designation = screw->Designation();
    }
    if (!part) {
      fresh.partName = "(missing part)"; // dangling reference — mass unknown
      table.warnings.push_back("'" + inst.path +
                               "' references a missing part — mass unknown");
    } else if (const auto mass = Measure::MassOfBodies(part->GetBodies())) {
      fresh.unitVolume = mass->volume;
      fresh.massKnown = mass->massKnown;
      fresh.unitMassG = mass->massG;
      for (size_t i = 0; i < mass->bodies.size(); i++) {
        if (i == 0)
          fresh.material = mass->bodies[i].material;
        else if (mass->bodies[i].material != fresh.material) {
          fresh.material = "mixed";
          break;
        }
      }
      for (const Measure::BodyMass& bm : mass->bodies)
        if (!bm.massKnown)
          table.warnings.push_back(
              "part '" + fresh.partName + "': body '" + bm.name + "' has " +
              (bm.material.empty() ? "no material" : "no density") +
              " — mass unknown");
    }
    table.rows.push_back(std::move(fresh));
  }
  for (const BomRow& r : table.rows) {
    if (r.massKnown)
      table.totalMassG += r.unitMassG * r.quantity;
    else
      table.massKnown = false;
  }
  if (!table.massKnown)
    table.totalMassG = 0.0;
  return table;
}

FastenerReport CheckFasteners(AppState& state) {
  FastenerReport report;
  std::vector<FastenerCheck>& out = report.findings;
  if (!state.assembly)
    return report;
  const std::vector<assembly::FlatInstance> flat = FlattenActiveAssembly(state);

  // Every body once, tagged with the instance it came from, so a fastener can
  // be left out of its OWN traversal without leaving out its twin.
  std::vector<parts::PlacedBody> all;
  std::vector<size_t> owner;
  for (size_t i = 0; i < flat.size(); i++) {
    if (flat[i].part == nullptr || !flat[i].visible)
      continue;
    for (const Body& body : flat[i].part->GetBodies()) {
      if (body.reference)
        continue; // a derived ghost is not the occurrence's matter
      parts::PlacedBody pb;
      pb.body = &body;
      pb.world = flat[i].world;
      pb.part = flat[i].path.empty() ? body.name : flat[i].path;
      pb.partId = flat[i].partId;
      pb.occurrence = flat[i].rootOccurrenceId;
      all.push_back(pb);
      owner.push_back(i);
    }
  }

  for (size_t i = 0; i < flat.size(); i++) {
    const FastenerFeature* screw = SoleFastenerOf(flat[i].part);
    if (screw == nullptr || !flat[i].visible)
      continue;
    const parts::Profile* profile = parts::ProfileFor(screw->standard);
    if (profile == nullptr || profile->kind != parts::Kind::Screw)
      continue; // a nut or a washer spans nothing; the screw is the claim
    const std::optional<double> nominal = parts::NominalDiameter(screw->size);
    if (!nominal) {
      // A screw this build cannot size. Not a finding — nothing is known to
      // be wrong — but not an examination either.
      report.skipped++;
      continue;
    }
    report.examined++;
    // Probed past the LOOSEST hole this size is drilled to, not past the
    // thread: a placed fastener knows its size and its axis but not the bore
    // it went through, and a ray offset by the thread radius runs down the
    // inside of the clearance and finds nothing. A hole wider than "Loose"
    // would make this cry wolf, which is the right way round for a check.
    const std::optional<double> bore =
        parts::WidestClearance(screw->size, nullptr);
    const double probe = bore ? *bore * 0.5 : *nominal * 0.5;

    // Its own placement IS the derivation: the bearing face is the frame's
    // z = 0 and the shank runs +Z, so the rim and the axis come back out of
    // the matrix with nothing stored.
    const glm::vec3 origin(flat[i].world * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    const glm::vec3 axis = glm::normalize(
        glm::vec3(flat[i].world * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)));

    std::vector<parts::PlacedBody> others;
    for (size_t b = 0; b < all.size(); b++)
      if (owner[b] != i)
        others.push_back(all[b]);

    FastenerCheck finding;
    finding.occurrence = flat[i].rootOccurrenceId;
    finding.part = screw->Designation();

    const double reach = 10000.0;
    const std::vector<parts::StackSegment> stack = parts::TraverseAxis(
        others, glm::dvec3(origin), glm::dvec3(axis), probe, reach);
    if (stack.empty()) {
      finding.why = "spans nothing — there is no material on its axis, so it "
                    "is holding the two halves of the air together";
      out.push_back(finding);
      continue;
    }
    // Everything the shank has to reach through, WASHERS AND NUT INCLUDED:
    // they are occurrences on the same axis, so re-traversing finds them the
    // same way it found them when the length was derived.
    //
    // Bounded by its OWN NUT when it has one. A joint ends where its nut
    // ends — that is what a nut is — and the axis beyond is somebody else's
    // material. Without this the check re-derived the untruncated stack and
    // called three correctly sized alternator through-bolts 21.6 mm short,
    // because the bolt-circle line carries on into a cast rib the bolts were
    // deliberately not spanning (docs/parts/README.md). A check that cries wolf
    // on a correct model is worse than no check.
    //
    // Derived rather than recorded: the nut's own placement says where the
    // joint ends, so a far-end override needs to store nothing to be
    // respected here.
    // What actually STOPS this screw, on the axis rather than beside it.
    // Cast here rather than at the end: it is half the answer to where the
    // joint ends, and reading it only afterwards is how the grip came to
    // ignore blind holes entirely.
    const std::vector<parts::StackSegment> onAxis = parts::TraverseAxis(
        others, glm::dvec3(origin), glm::dvec3(axis), 0.0, reach);
    double nutEnds = 0.0;
    const auto riderOn = [&](uint64_t occurrence) -> const parts::Profile* {
      for (size_t b = 0; b < all.size(); b++)
        if (all[b].occurrence == occurrence)
          if (const FastenerFeature* f = SoleFastenerOf(flat[owner[b]].part))
            return parts::ProfileFor(f->standard);
      return nullptr;
    };
    // ITS nut: the FIRST one along the axis, and any jam nut tight against
    // it. Taking the last one found the nut of the bolt facing this one from
    // the other side of the cart — 646 mm away, past the joint, past the
    // grease tray's rail in between — and the joint then read as blind on
    // that rail, so four correct side-shelf bolts were reported as having no
    // thread in a part they do not touch.
    for (const parts::StackSegment& seg : stack) {
      const parts::Profile* rider = riderOn(seg.occurrence);
      if (rider == nullptr || rider->kind != parts::Kind::Nut)
        continue;
      if (nutEnds <= 0.0)
        nutEnds = seg.exit;
      else if (seg.enter <= nutEnds + 1e-3)
        nutEnds = std::max(nutEnds, seg.exit);
      else
        break;
    }
    // A thread ends a joint the way a nut does — and a screw into a tapped
    // BOSS has no nut to end it, so without this the re-derivation ran on
    // down the axis and reported "0 mm of thread" in whatever it found
    // there. The threads are in the model; nothing was reading them.
    const double threadEnds = parts::ThreadEndAlong(
        others, glm::dvec3(origin), glm::dvec3(axis), screw->size);
    // The SAME answer fit_fastener derived from, from the same helper.
    const parts::JointExtent extent =
        parts::JointEnd(stack, onAxis, nutEnds, threadEnds);
    const double grip = extent.end;

    // A nut BURIED IN A PART, which is the other way a joint comes apart and
    // which the rule above would otherwise hide. Bounding the grip at the nut
    // says "the joint ends here" — true only while the nut is clear of the
    // parts. Move a plate in a redesign and leave the hardware behind and the
    // nut ends up INSIDE it: the screw still reaches its own nut, so the
    // length is not what is wrong and nothing else here would say a word.
    //
    // Straddling is the test, not "material beyond the nut". Material beyond
    // a nut is ordinary — the alternator's bolt circle runs on into a cast
    // rib the bolts deliberately do not span — but a part that STARTS before
    // the nut ends and CONTINUES past it has the nut embedded in it, and
    // there is no correct model that looks like that.
    if (nutEnds > 0.0) {
      for (const parts::StackSegment& seg : stack) {
        if (riderOn(seg.occurrence) != nullptr)
          continue; // hardware; the nut cannot be buried in its own washer
        if (seg.enter >= nutEnds - 1e-3 || seg.exit <= nutEnds + 1e-3)
          continue;
        finding.why = "has its nut buried in " + seg.part +
                      ": that part runs "
                      "from " +
                      Mm(seg.enter) + " to " + Mm(seg.exit) +
                      " down the axis and the nut ends at " + Mm(nutEnds) +
                      ", so the nut is inside the material instead of "
                      "bearing on it and the joint clamps nothing. Something "
                      "moved out from under this fastener — re-fit it from "
                      "the rim.";
        break;
      }
      if (!finding.why.empty()) {
        out.push_back(finding);
        continue;
      }
    }
    // TWO DIFFERENT RULES, and applying the through one to a blind joint is
    // what made every blind-hole screw in a model read as too short.
    //
    // A through screw must SPAN its stack: anything less and the nut is not
    // reached. A blind screw must not BOTTOM OUT, and is derived by snapping
    // DOWN to a stocked length, so it is shorter than its hole is deep by
    // construction — "it does not reach the far side" is not a defect there,
    // it is the definition. What can be wrong with a blind screw is that too
    // little thread is engaged in the part it threads into.
    if (!extent.blind) {
      if (screw->length < grip - 1e-3) {
        finding.why = "is " + Mm(grip - screw->length) +
                      " too short: its stack is now " + Mm(grip) +
                      " and it is " + Mm(screw->length) +
                      " under the head. Re-fit it, or re-derive from the rim.";
        out.push_back(finding);
      }
      continue;
    }
    if (screw->length > grip + 1e-3) {
      finding.why = "bottoms out: the hole is " + Mm(grip) +
                    " deep and the screw is " + Mm(screw->length) +
                    " long, so it lands on the floor and clamps nothing.";
      out.push_back(finding);
      continue;
    }
    // Enough thread in the part it threads into — the same bar fit_fastener
    // holds a derivation to (~1x the nominal diameter), asked of the screw
    // that is actually there.
    if (const parts::StackSegment* threads = parts::SegmentAt(stack, grip)) {
      const double engaged = screw->length - threads->enter;
      if (engaged < *nominal - 1e-3) {
        finding.why = "has only " + Mm(std::max(engaged, 0.0)) +
                      " of thread in " + threads->part + ": an " + screw->size +
                      " needs " + Mm(*nominal) + " to hold, and the hole is " +
                      Mm(grip - threads->enter) +
                      " deep. Use a longer screw, or drill and tap deeper.";
        out.push_back(finding);
      }
    }
  }
  return report;
}

std::string BomToCsv(const BomTable& bom) {
  auto field = [](const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos)
      return s;
    std::string quoted = "\"";
    for (char c : s) {
      if (c == '"')
        quoted += '"';
      quoted += c;
    }
    return quoted + "\"";
  };
  // `standard` and `designation` are the two columns a purchase order needs
  // and the panel has no room for: blank on a part somebody made, filled on
  // a bought one, so a filter on either splits the list in two.
  std::string csv = "part,quantity,standard,designation,material,"
                    "unit_volume_mm3,unit_mass_g,total_mass_g\n";
  char buf[64];
  auto num = [&buf](double v) {
    snprintf(buf, sizeof buf, "%.6g", v);
    return std::string(buf);
  };
  for (const BomRow& r : bom.rows) {
    csv += field(r.partName) + "," + std::to_string(r.quantity) + "," +
           field(r.standard) + "," + field(r.designation) + "," +
           field(r.material) + "," + num(r.unitVolume) + ",";
    if (r.massKnown)
      csv += num(r.unitMassG) + "," + num(r.unitMassG * r.quantity);
    else
      csv += ",";
    csv += "\n";
  }
  csv += "total,,,,,,,";
  if (bom.massKnown)
    csv += num(bom.totalMassG);
  csv += "\n";
  return csv;
}

// At `ui` scope because FastenerActions.cpp formats the same figures in
// its own refusals, and two spellings of "how many millimetres" drift.

// Millimetres as a person writes them: "12 mm", "3.5 mm" — not the six
// decimal places std::to_string gives, which makes a refusal read like a
// stack trace.
std::string Mm(double v) {
  std::ostringstream out;
  out.precision(4);
  // -0.0 prints as "-0", and a refusal reading "the axis meets its face -0 mm
  // out" reads as a broken message rather than a measurement. (-0.0 == 0.0 is
  // true, so this assignment is what normalises the sign.)
  if (v == 0.0)
    v = 0.0;
  out << v << " mm";
  return out.str();
}

} // namespace ui
