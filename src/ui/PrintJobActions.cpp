// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/PrintJobActions.h"
#include "ui/DerivedActions.h"

#include "core/Document.h"
#include "core/PrintAllowance.h"
#include "core/PrintCheck.h"
#include "core/features/DerivedFeature.h"
#include "core/io/SerializePrintSettings.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <unordered_map>

using json = nlohmann::json;

namespace ui {

namespace {

// The chord a job meshes a printed part at when its print states none:
// export_stl's default for an unprinted part.
constexpr double kDefaultDeflectionMm = 0.1;

std::string Mm(double v) {
  std::ostringstream s;
  s << std::round(v * 1000.0) / 1000.0;
  return s.str();
}

const DerivedFeature* DerivedOf(const Document& part, const Body& body) {
  if (body.createdBy == 0)
    return nullptr;
  return dynamic_cast<const DerivedFeature*>(
      part.FindFeatureById(body.createdBy));
}

std::string SlotName(AppState& state, const Document* part) {
  for (size_t i = 0; i < state.parts.size(); i++) {
    const PartSlot& slot = state.parts[i];
    if (slot.doc.get() == part || (static_cast<int>(i) == state.activePart &&
                                   state.document.get() == part))
      return slot.name;
  }
  return "?";
}

} // namespace

BodyPrint PrintOf(AppState& state, const Document& part, const Body& body) {
  BodyPrint out;
  const printset::PartSettings& own = part.PrintSettings();
  printset::Settings s = own.part;
  if (part.PrintProcess())
    out.process = &*part.PrintProcess();
  if (const DerivedFeature* d = DerivedOf(part, body)) {
    if (const Document* src = PartDocById(state, d->sourcePart);
        src != nullptr && src != &part) {
      out.source = src;
      printset::Settings from = src->PrintSettings().For(body.derivedFrom);
      from.direction.reset(); // the plate's frame is the bed's
      s = printset::Merge(s, from);
      if (out.process == nullptr && src->PrintProcess())
        out.process = &*src->PrintProcess();
    }
  }
  const auto it = own.bodies.find(body.createdBy);
  if (it != own.bodies.end())
    s = printset::Merge(s, it->second);
  out.settings = s;
  return out;
}

std::optional<printmass::Result> PrintedMassOf(AppState& state,
                                               const Document& part,
                                               const Body& body,
                                               std::string* why) {
  const BodyPrint bp = PrintOf(state, part, body);
  return printmass::EstimatePrinted(bp.process, bp.settings, body, why);
}

std::string SlicerFilamentType(const std::string& filament) {
  if (filament == "pla")
    return "PLA";
  if (filament == "petg")
    return "PETG";
  if (filament == "tpu")
    return "TPU";
  return {};
}

std::optional<std::string> FilamentOf(const Body& body,
                                      const printfit::Process* process,
                                      std::string* why) {
  if (!body.material.empty()) {
    if (!SlicerFilamentType(body.material).empty())
      return body.material;
    if (why != nullptr)
      *why = "body '" + body.name + "' is " + body.material +
             ", which is not a filament (pla, petg, tpu): set_material, or "
             "clear it to print in the process's material";
    return std::nullopt;
  }
  if (process != nullptr && !SlicerFilamentType(process->material).empty())
    return process->material;
  if (why != nullptr)
    *why = "body '" + body.name +
           "' has no material and no process material that is a filament";
  return std::nullopt;
}

namespace {

// The rectangle a Bambu polygon ["0x0", "18x0", ...] bounds; nullopt for a
// list that is not one.
std::optional<std::array<double, 4>> PolygonBox(const json& poly) {
  if (!poly.is_array() || poly.empty())
    return std::nullopt;
  std::array<double, 4> b{1e300, 1e300, -1e300, -1e300};
  for (const json& p : poly) {
    if (!p.is_string())
      return std::nullopt;
    const std::string s = p.get<std::string>();
    const size_t x = s.find('x');
    if (x == std::string::npos)
      return std::nullopt;
    try {
      const double u = std::stod(s.substr(0, x)),
                   v = std::stod(s.substr(x + 1));
      b[0] = std::min(b[0], u);
      b[1] = std::min(b[1], v);
      b[2] = std::max(b[2], u);
      b[3] = std::max(b[3], v);
    } catch (const std::exception&) {
      return std::nullopt;
    }
  }
  return b;
}

std::string Scalar(const json& v) {
  if (v.is_array() && !v.empty())
    return Scalar(v[0]);
  if (v.is_string())
    return v.get<std::string>();
  return v.dump();
}

// The config against the job: complete, for this printer's bed and zones
// and for this one filament. Empty, or the refusal.
std::string CheckConfig(const json& c, const printfit::Profile& prof,
                        const std::string& filamentType,
                        std::vector<std::string>& notes) {
  if (const std::string missing = printjob::MissingProjectKeys(c);
      !missing.empty())
    return "slicer_config is not a complete Bambu Studio project config (it "
           "lacks " +
           missing +
           "): Bambu applies a project's config over its own defaults, so a "
           "partial one slices on a printer nobody has. Use a project's "
           "Metadata/project_settings.config, as tools/bambu-config.py makes";
  const auto area = PolygonBox(c.at("printable_area"));
  if (!area)
    return "slicer_config printable_area is not a polygon: " +
           c.at("printable_area").dump();
  const auto& bed = *prof.bedMm;
  if (std::abs(area->at(2) - area->at(0) - bed[0]) > 1e-6 ||
      std::abs(area->at(3) - area->at(1) - bed[1]) > 1e-6)
    return "slicer_config is for a " + Mm(area->at(2) - area->at(0)) + " x " +
           Mm(area->at(3) - area->at(1)) + " mm bed, and print profile '" +
           prof.name + "' is " + Mm(bed[0]) + " x " + Mm(bed[1]) +
           ": another printer";
  try {
    // Lower than the profile's, bed_fit would pass a piece the slicer
    // refuses; higher is the profile being careful.
    const double h = std::stod(Scalar(c.at("printable_height")));
    if (h < bed[2] - 1e-6)
      return "slicer_config's printable_height is " + Mm(h) +
             " mm and print profile '" + prof.name + "' says " + Mm(bed[2]) +
             ": bed_fit would pass a piece the slicer refuses. Set the "
             "profile's bed_mm z to the slicer's";
  } catch (const std::exception&) {
    return "slicer_config printable_height is not a number";
  }
  if (c.contains("bed_exclude_area")) {
    const json& ex = c.at("bed_exclude_area");
    if (ex.is_array() && !ex.empty()) {
      const auto box = PolygonBox(ex);
      if (!box)
        return "slicer_config bed_exclude_area is not a polygon: " + ex.dump();
      const bool covered = std::any_of(
          prof.bedExcludeMm.begin(), prof.bedExcludeMm.end(),
          [&](const std::array<double, 4>& z) {
            return z[0] <= box->at(0) + 1e-6 && z[1] <= box->at(1) + 1e-6 &&
                   z[2] >= box->at(2) - 1e-6 && z[3] >= box->at(3) - 1e-6;
          });
      if (!covered)
        return "slicer_config keeps [" + Mm(box->at(0)) + ", " +
               Mm(box->at(1)) + ", " + Mm(box->at(2)) + ", " + Mm(box->at(3)) +
               "] mm of the bed clear, and print profile '" + prof.name +
               "' states no keep-out zone that covers it, so bed_fit could "
               "pass a piece the slicer refuses: add it to the profile's "
               "bed_exclude_mm";
    }
  }
  const json& ft = c.at("filament_type");
  if (!ft.is_array() || ft.size() != 1)
    return "slicer_config carries " +
           std::to_string(ft.is_array() ? ft.size() : 0) +
           " filaments; a job is one spool, so its config is one filament's";
  if (Scalar(ft) != filamentType)
    return "slicer_config is for " + Scalar(ft) + " and the job's pieces are " +
           filamentType + ": use that filament's config";
  try {
    const double layer = std::stod(Scalar(c.at("layer_height")));
    if (std::abs(layer - prof.layerHeightMm) > 1e-9)
      notes.push_back("slicer_config's layer_height is " + Mm(layer) +
                      " mm; every object carries the profile's " +
                      Mm(prof.layerHeightMm) + " mm");
  } catch (const std::exception&) {
    return "slicer_config layer_height is not a number";
  }
  return {};
}

// One body as a welded mesh in the plate's frame.
bool MeshObject(const Body& body, double deflection, printjob::Object& o) {
  const CADShape::Mesh mesh = body.shape.GenerateMeshDetached(deflection);
  if (mesh.indices.size() < 3)
    return false;
  struct KeyHash {
    size_t operator()(const std::array<uint32_t, 3>& k) const {
      return (static_cast<size_t>(k[0]) * 73856093u) ^
             (static_cast<size_t>(k[1]) * 19349663u) ^
             (static_cast<size_t>(k[2]) * 83492791u);
    }
  };
  std::unordered_map<std::array<uint32_t, 3>, uint32_t, KeyHash> index;
  std::vector<uint32_t> remap(mesh.vertices.size());
  for (size_t i = 0; i < mesh.vertices.size(); i++) {
    const glm::vec3& p = mesh.vertices[i].position;
    std::array<uint32_t, 3> k{};
    std::memcpy(k.data(), &p.x, sizeof(float));
    std::memcpy(k.data() + 1, &p.y, sizeof(float));
    std::memcpy(k.data() + 2, &p.z, sizeof(float));
    const auto [it, fresh] =
        index.emplace(k, static_cast<uint32_t>(o.vertices.size()));
    if (fresh)
      o.vertices.emplace_back(p.x, p.y, p.z);
    remap[i] = it->second;
  }
  for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    const std::array<uint32_t, 3> tri{remap[mesh.indices[t]],
                                      remap[mesh.indices[t + 1]],
                                      remap[mesh.indices[t + 2]]};
    if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2])
      continue; // welded to nothing
    o.triangles.push_back(tri);
  }
  return !o.triangles.empty();
}

// What the slicer lists an object as: a plate's piece by the name its
// Derived feature gives it ("dowel 7"), with the body's when that feature
// made several; any other body by its own name.
std::string ObjectName(const Document& part, const Body& body) {
  const Feature* f =
      body.createdBy != 0 ? part.FindFeatureById(body.createdBy) : nullptr;
  if (f == nullptr || dynamic_cast<const DerivedFeature*>(f) == nullptr)
    return body.name;
  int made = 0;
  for (const Body& b : part.GetBodies())
    made += !b.reference && b.createdBy == body.createdBy ? 1 : 0;
  return made > 1 ? f->GetName() + " / " + body.name : f->GetName();
}

json BambuJson(const std::vector<std::pair<std::string, std::string>>& kv) {
  json j = json::object();
  for (const auto& [k, v] : kv)
    j[k] = v;
  return j;
}

} // namespace

std::expected<JobBuilt, std::string>
BuildPrintJob(AppState& state, const std::vector<JobPlateAsk>& plates,
              const std::optional<json>& projectConfig,
              const std::string& jobName) {
  using std::unexpected;
  RefreshLinks(state); // the gate: docs/modeling/derived.md#copies-that-keep-up
  if (plates.empty())
    return unexpected("a job has at least one plate");
  if (plates.size() > 1 && !projectConfig)
    return unexpected(
        "a job of " + std::to_string(plates.size()) +
        " plates needs slicer_config: Bambu Studio keeps plates only in a "
        "project, and a project carries the slicer's whole config, which "
        "Loböle does not have (tools/bambu-config.py makes one from your "
        "Bambu Studio). One plate needs none: its 3MF opens with your "
        "presets");

  JobBuilt out;
  printjob::Job& job = out.job;
  const printfit::Profile* profile = nullptr;
  std::string filament;
  std::vector<std::string> notes;
  json plateRows = json::array();

  for (size_t k = 0; k < plates.size(); k++) {
    const Document& part = *plates[k].part;
    const std::string partName = SlotName(state, &part);
    const std::string label =
        "plate '" + plates[k].name + "' (part '" + partName + "')";
    const auto& proc = part.PrintProcess();
    if (!proc)
      return unexpected(label + " is not printed: set_print_process with a "
                                "profile on the plate's part");
    if (!proc->profile)
      return unexpected(label + " is printed on the built-in " +
                        printfit::Label(*proc) +
                        " table, which is no printer: print it with a "
                        "profile that states bed_mm and layer_height_mm");
    const printfit::Profile& prof = *proc->profile;
    if (!prof.bedMm)
      return unexpected(label + ": print profile '" + prof.name +
                        "' states no bed_mm");
    if (!(prof.layerHeightMm > 0.0))
      return unexpected(label + ": print profile '" + prof.name +
                        "' states no layer_height_mm, the layer every object "
                        "is sliced at");
    if (profile == nullptr) {
      profile = &prof;
    } else if (!(prof.bedMm == profile->bedMm &&
                 prof.bedExcludeMm == profile->bedExcludeMm &&
                 prof.layerHeightMm == profile->layerHeightMm)) {
      return unexpected(label + " is printed with '" + prof.name +
                        "', another printer or layer than the job's first "
                        "plate ('" +
                        profile->name + "'): one job is one printer");
    }

    std::vector<const Body*> bodies;
    for (const Body& b : part.GetBodies())
      if (!b.reference && b.shape.IsValid())
        bodies.push_back(&b);
    if (bodies.empty())
      return unexpected(label + " has nothing to print");

    // bed_fit, positional: the job puts every piece where the plate does.
    const glm::dvec3 bed((*prof.bedMm)[0], (*prof.bedMm)[1], (*prof.bedMm)[2]);
    printcheck::BedFit fit = printcheck::FitOnBed(
        bodies, glm::dvec3(0.0, 0.0, 1.0), bed, prof.bedExcludeMm);
    if (!fit.positional) {
      // No zones: the positions still are the job's, so on the bed.
      for (size_t i = 0; i < bodies.size(); i++) {
        glm::dvec3 lo, hi;
        bodies[i]->shape.GetBoundingBox(lo, hi, /*tight=*/true);
        if (lo.x < -1e-3 || lo.y < -1e-3 || hi.x > bed.x + 1e-3 ||
            hi.y > bed.y + 1e-3)
          fit.intrusions.push_back(
              {static_cast<int>(i), -1, {lo.x, lo.y}, {hi.x, hi.y}, 0.0});
      }
      if (!fit.intrusions.empty())
        fit.status = printcheck::Status::Fail;
    }
    if (fit.status != printcheck::Status::Pass) {
      std::string what;
      for (const printcheck::BedIntrusion& in : fit.intrusions) {
        what += (what.empty() ? "" : "; ") + std::string("'") +
                bodies[static_cast<size_t>(in.body)]->name + "' [" +
                Mm(in.minMm.x) + ", " + Mm(in.minMm.y) + ", " + Mm(in.maxMm.x) +
                ", " + Mm(in.maxMm.y) + "]";
        if (in.zone >= 0) {
          const auto& z = fit.zonesMm[static_cast<size_t>(in.zone)];
          what += " is in keep-out zone " + std::to_string(in.zone) + " [" +
                  Mm(z[0]) + ", " + Mm(z[1]) + ", " + Mm(z[2]) + ", " +
                  Mm(z[3]) + "]";
        } else {
          what += " is off the bed";
        }
      }
      if (what.empty())
        what = "its box " + Mm(fit.extentMm.x) + " x " + Mm(fit.extentMm.y) +
               " x " + Mm(fit.extentMm.z) + " mm does not fit the " +
               Mm(bed.x) + " x " + Mm(bed.y) + " x " + Mm(bed.z) + " mm bed";
      return unexpected(label + " fails bed_fit on '" + prof.name +
                        "' (the plate's x and y are the bed's, from its "
                        "front-left corner): " +
                        what);
    }

    printjob::Plate plate;
    plate.name = plates[k].name;
    json objects = json::array();
    const double deflection = printfit::StlDeflectionMm(*proc) > 0.0
                                  ? printfit::StlDeflectionMm(*proc)
                                  : kDefaultDeflectionMm;
    for (const Body* b : bodies) {
      const BodyPrint bp = PrintOf(state, part, *b);
      const std::string piece = label + ", piece '" + b->name + "'";
      if (const std::string why = printset::Validate(bp.settings); !why.empty())
        return unexpected(piece + ": " + why);
      std::string why;
      const auto f = FilamentOf(*b, bp.process, &why);
      if (!f)
        return unexpected(piece + ": " + why);
      if (filament.empty())
        filament = *f;
      else if (*f != filament)
        return unexpected(
            piece + " is " + *f + " and the job's other pieces are " +
            filament +
            ": a job is one spool. Put each filament's plates in a job "
            "of their own");
      printjob::Object o;
      o.name = ObjectName(part, *b);
      if (!MeshObject(*b, deflection, o))
        return unexpected(piece + " did not mesh");
      o.settings =
          printset::BambuObjectSettings(bp.settings, prof.layerHeightMm);
      json row{{"name", o.name},
               {"body", b->name},
               {"created_by", b->createdBy},
               {"settings", DocumentIO::PrintSettingsToJson(bp.settings)},
               {"described", printset::Describe(bp.settings)},
               {"slicer", BambuJson(o.settings)},
               {"triangles", o.triangles.size()}};
      if (bp.source != nullptr)
        row["source_part"] = SlotName(state, bp.source);
      objects.push_back(std::move(row));
      plate.objects.push_back(std::move(o));
    }
    json fitJson{
        {"status", "pass"},
        {"extent_mm", {fit.extentMm.x, fit.extentMm.y, fit.extentMm.z}},
        {"bed_mm", {bed.x, bed.y, bed.z}}};
    if (!prof.bedExcludeMm.empty())
      fitJson["exclude_mm"] = prof.bedExcludeMm;
    const glm::dvec2 at = printjob::PlateOrigin(static_cast<int>(k),
                                                static_cast<int>(plates.size()),
                                                glm::dvec2(bed.x, bed.y));
    plateRows.push_back({{"name", plates[k].name},
                         {"part", partName},
                         {"objects", std::move(objects)},
                         {"bed_fit", std::move(fitJson)},
                         {"slicer_origin_mm", {at.x, at.y}}});
    job.plates.push_back(std::move(plate));
  }

  const std::string ftype = SlicerFilamentType(filament);
  if (projectConfig) {
    if (const std::string why =
            CheckConfig(*projectConfig, *profile, ftype, notes);
        !why.empty())
      return unexpected(why);
    job.projectConfig = *projectConfig;
  }
  job.bedMm = glm::dvec2((*profile->bedMm)[0], (*profile->bedMm)[1]);
  out.summary = {
      {"name", jobName},
      {"form", projectConfig ? "project" : "model"},
      {"filament", filament},
      {"filament_type", ftype},
      {"profile", profile->name},
      {"layer_height_mm", profile->layerHeightMm},
      {"bed_mm", *profile->bedMm},
      {"plates", std::move(plateRows)},
      {"note",
       projectConfig
           ? "a Bambu Studio project: its plates, every piece an object with "
             "its own settings, and the slicer config it was handed"
           : "a 3MF of one plate without a slicer config: Bambu Studio "
             "opens it with your own printer, process and filament, and "
             "every object keeps its settings"}};
  if (projectConfig)
    out.summary["slicer_config"] = {
        {"printer", Scalar(projectConfig->at("printer_settings_id"))},
        {"process", Scalar(projectConfig->at("print_settings_id"))},
        {"filament", Scalar(projectConfig->at("filament_settings_id"))}};
  if (!profile->bedExcludeMm.empty())
    out.summary["bed_exclude_mm"] = profile->bedExcludeMm;
  if (!notes.empty())
    out.summary["notes"] = notes;
  job.record = out.summary;
  job.record["written_by"] = "Loböle export_print_job";
  return out;
}

} // namespace ui
