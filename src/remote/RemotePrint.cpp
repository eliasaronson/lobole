// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "remote/RemotePrint.h"

#include "cad/CADMaterials.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/PrintedMass.h"
#include "core/io/PrintJob3mf.h"
#include "core/io/Serialization.h"
#include "core/io/SerializePrintSettings.h"
#include "ui/AppState.h"
#include "ui/PrintJobActions.h"

#include <filesystem>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

namespace remote {

namespace {

// The part a design-intent edit acts on: `part`, any part, else the active
// one (as set_print_process takes it).
Document& PartToEdit(ui::AppState& state, Document* active, const RpcParams& p,
                     const char* what) {
  if (p.contains("part"))
    return *ResolvePartParam(state, p.at("part"));
  if (active == nullptr)
    throw std::runtime_error(
        std::string(what) +
        ": no active part document — pass `part` (a name, stable id or "
        "slot index, see list_parts) to edit any part from this tab");
  return *active;
}

// The undo history `part` keeps: the active tab's, or its stashed slot's.
CommandStack& CommandsFor(ui::AppState& state, const Document& part) {
  if (state.document.get() == &part && state.commands)
    return *state.commands;
  for (ui::PartSlot& slot : state.parts)
    if (slot.doc.get() == &part) {
      if (!slot.commands)
        slot.commands = std::make_unique<CommandStack>();
      return *slot.commands;
    }
  throw std::runtime_error("that part has no undo history of its own (a "
                           "configuration's evaluated copy?) — switch to its "
                           "tab and edit it there");
}

static constexpr MethodDoc PrintMethods[] = {
    {"set_print_settings",
     "Set a printed part's SLICER settings, what export_print_job hands the "
     "slicer per object and what mass_properties' printed mass weighs: "
     "`walls` (wall loops), `top_layers`, `bottom_layers`, `infill_pct` "
     "(percent, 0-100), `infill_pattern` (rectilinear, aligned_rectilinear, "
     "grid, gyroid, honeycomb, cubic, triangles, line, concentric, "
     "lightning, adaptive_cubic, tri_hexagon, 3d_honeycomb, cross_hatch, "
     "hilbert_curve), `supports` (default off) and `brim` (none, outer, "
     "inner, both, ears; default none), and `direction`, the print "
     "direction in the part's frame (unitless, default +Z) for the printed "
     "mass of a part printed lying down. On the part, or with `body` (a "
     "name or the id of the feature that created it) as that body's "
     "override. Each given key is set and `null` unsets it; `clear: true` "
     "clears the part's (or that body's). A field left unstated is the "
     "slicer preset's. Refused: an unknown pattern or brim (naming the "
     "accepted), walls under 1, a layer count under 0, an infill outside "
     "0-100, and 100 % with a pattern that cannot print solid or with none "
     "(Bambu refuses grid there: rectilinear, aligned_rectilinear, "
     "concentric, hilbert_curve). Undoable; drives no geometry. `part` sets "
     "it on any part.",
     "part?:string|int, body?:string|int, clear?:bool, walls?:int|null, "
     "top_layers?:int|null, bottom_layers?:int|null, "
     "infill_pct?:number|null, infill_pattern?:string|null, "
     "supports?:bool|null, brim?:string|null, direction?:number[3]|null",
     remote::NoFlags},
    {"get_print_settings",
     "A part's slicer settings: the part's own (`part`), every body's "
     "override and, per body, what it resolves to (`settings`, a plate's "
     "piece reading its SOURCE part's under the plate's own) with a one-line "
     "description. Read-only; `part` reads any part from any tab.",
     "part?:string|int", remote::Deferrable,
     "part:object, bodies:{body:int, name:string, created_by:int, "
     "override?:object, settings:object, described:string, "
     "source_part?:string}[]"},
    {"export_print_job",
     "Write a PRINT JOB: one 3MF Bambu Studio opens and slices. `plates` "
     "lists the job's plates, each {part, name?}: a part whose bodies are "
     "the pieces where they lie on the bed (x, y from its front-left "
     "corner, as a generator's plate places them), every piece its own "
     "object with its per-object settings (set_print_settings; supports "
     "off and no brim unless set) and the profile's layer. Checked before "
     "anything is written: every plate printed with a profile that states "
     "bed_mm and layer_height_mm, one printer and layer for the job, every "
     "piece on the bed and clear of the profile's keep-out zones "
     "(bed_exclude_mm), every piece's settings valid and ONE filament for "
     "the whole job (a body's material when it is pla, petg or tpu, else "
     "the process's): a TPU plate is a job of its own. `slicer_config` (a "
     "path to a Bambu Studio project config, JSON, or the object) makes "
     "the 3MF a Bambu PROJECT that keeps its plates: Bambu applies a "
     "project's config over its own defaults, so it must be complete "
     "(tools/bambu-config.py makes one) and for this bed, keep-out zone "
     "and filament. Without it a job is one plate, which Bambu opens with "
     "your own presets. The answer lists every object with what it prints "
     "with, and `form` project | model.",
     "path:string, plates:object[], slicer_config?:string|object, "
     "name?:string",
     remote::NotInBatch},
};

std::string Lower(std::string s) {
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string SlotNameOf(ui::AppState& state, const Document* part) {
  for (size_t i = 0; i < state.parts.size(); i++) {
    const ui::PartSlot& slot = state.parts[i];
    if (slot.doc.get() == part || (static_cast<int>(i) == state.activePart &&
                                   state.document.get() == part))
      return slot.name;
  }
  return {};
}

json SettingsReport(ui::AppState& state, const Document& part) {
  const printset::PartSettings& ps = part.PrintSettings();
  json bodies = json::array();
  const auto& all = part.GetBodies();
  for (size_t i = 0; i < all.size(); i++) {
    const Body& b = all[i];
    if (b.reference)
      continue;
    const ui::BodyPrint bp = ui::PrintOf(state, part, b);
    json row{{"body", i},
             {"name", b.name},
             {"created_by", b.createdBy},
             {"settings", DocumentIO::PrintSettingsToJson(bp.settings)},
             {"described", printset::Describe(bp.settings)}};
    if (const auto it = ps.bodies.find(b.createdBy); it != ps.bodies.end())
      row["override"] = DocumentIO::PrintSettingsToJson(it->second);
    if (bp.source != nullptr)
      row["source_part"] = SlotNameOf(state, bp.source);
    bodies.push_back(std::move(row));
  }
  return {{"part", DocumentIO::PrintSettingsToJson(ps.part)},
          {"bodies", std::move(bodies)}};
}

json ReadConfig(const json& v) {
  if (v.is_object())
    return v;
  if (!v.is_string())
    throw std::runtime_error("slicer_config is a path to a Bambu Studio "
                             "project config (JSON) or that object");
  const std::string path = v.get<std::string>();
  std::ifstream in(path);
  if (!in)
    throw std::runtime_error("slicer_config: cannot read " + path);
  std::stringstream ss;
  ss << in.rdbuf();
  try {
    json j = json::parse(ss.str());
    if (!j.is_object())
      throw std::runtime_error("not an object");
    return j;
  } catch (const std::exception& e) {
    throw std::runtime_error("slicer_config " + path +
                             " is not a JSON object: " + e.what());
  }
}

} // namespace

std::span<const MethodDoc> PrintMethodDocs() { return PrintMethods; }

json PrintedMassJson(ui::AppState& state, const Document& part,
                     const Body& body, double density) {
  const ui::BodyPrint bp = ui::PrintOf(state, part, body);
  if (bp.process == nullptr)
    return nullptr;
  std::string why;
  const auto r =
      printmass::EstimatePrinted(bp.process, bp.settings, body, &why);
  if (!r)
    return {{"why", why}};
  json inertia = json::array();
  for (int a = 0; a < 3; a++)
    inertia.push_back({r->inertia[a][0], r->inertia[a][1], r->inertia[a][2]});
  json out{{"volume_mm3", r->effectiveMm3},
           {"solid_mm3", r->solidMm3},
           {"shell_mm3", r->shellMm3},
           {"core_mm3", r->coreMm3},
           {"center_of_mass_mm",
            {r->centerOfMass.x, r->centerOfMass.y, r->centerOfMass.z}},
           {"inertia", inertia},
           {"layers", r->layers},
           {"pixel_mm", r->pixelMm},
           {"settings", DocumentIO::PrintSettingsToJson(bp.settings)},
           {"estimate", true},
           {"method", printmass::MethodNote()}};
  // A body with no material prints in the filament: the process's.
  if (!(density > 0.0)) {
    std::string fwhy;
    if (const auto f = ui::FilamentOf(body, bp.process, &fwhy)) {
      const auto& mp = CADMaterials::MaterialLibrary::GetMaterial(
          CADMaterials::MaterialLibrary::MaterialTypeFromString(*f));
      density = mp.density;
      out["density_from"] = "the filament, " + *f;
    }
  }
  out["mass_g"] =
      density > 0.0 ? json(density * r->effectiveMm3 * 1e-6) : json(nullptr);
  return out;
}

bool HandlePrintRpc(ui::AppState& state, Document* doc, const std::string& m,
                    const RpcParams& p, json& out) {
  if (m == "set_print_settings") {
    Document& part = PartToEdit(state, doc, p, "set_print_settings");
    printset::PartSettings next = part.PrintSettings();
    uint64_t body = 0;
    std::string bodyName;
    if (p.contains("body")) {
      const std::set<uint64_t> ids = ResolvePartBodies(
          part, json::array({p.at("body")}), "set_print_settings body");
      body = *ids.begin();
      for (const Body& b : part.GetBodies())
        if (b.createdBy == body)
          bodyName = b.name;
    }
    printset::Settings target =
        body != 0 ? (next.bodies.contains(body) ? next.bodies.at(body)
                                                : printset::Settings{})
                  : next.part;
    const bool clear = p.value("clear", false);
    // Every key given, the wire form: null unsets.
    json given = DocumentIO::PrintSettingsToJson(target);
    bool any = false;
    // rpc-params: walls, top_layers, bottom_layers, infill_pct
    // rpc-params: infill_pattern, supports, brim, direction
    for (const char* key :
         {"walls", "top_layers", "bottom_layers", "infill_pct",
          "infill_pattern", "supports", "brim", "direction"}) {
      if (!p.contains(key))
        continue;
      any = true;
      if (p.at(key).is_null())
        given.erase(key);
      else
        given[key] = p.at(key);
    }
    if (clear && any)
      throw std::runtime_error("set_print_settings: `clear` or settings, "
                               "not both");
    if (!clear && !any)
      throw std::runtime_error(
          std::string("set_print_settings: nothing to set; give any of ") +
          DocumentIO::PrintSettingsKeys() + ", or clear: true");
    target =
        clear ? printset::Settings{}
              : DocumentIO::PrintSettingsFromJson(given, "set_print_settings");
    if (body != 0) {
      if (target.Empty())
        next.bodies.erase(body);
      else
        next.bodies[body] = target;
    } else {
      next.part = target;
    }
    // Each override must hold over the part's too.
    for (const auto& [id, b] : next.bodies)
      if (const std::string why =
              printset::Validate(printset::Merge(next.part, b));
          !why.empty()) {
        std::string name = std::to_string(id);
        for (const Body& bb : part.GetBodies())
          if (bb.createdBy == id)
            name = "'" + bb.name + "'";
        throw std::runtime_error("set_print_settings: body " + name +
                                 " over the part's: " + why);
      }
    if (!(next == part.PrintSettings()))
      CommandsFor(state, part)
          .Execute(std::make_unique<SetPrintSettingsCommand>(next), part);
    out = SettingsReport(state, part);
    if (!part.PrintProcess())
      out["note"] = "the part has no print process: the settings are kept, "
                    "and nothing prints them until it is printed "
                    "(set_print_process)";
    if (!bodyName.empty())
      out["set_on"] = bodyName;
    return true;
  }
  if (m == "get_print_settings") {
    // rpc-returns-from: src/remote/RemotePrint.cpp
    const Document& part = SubjectPart(state, doc, p);
    out = SettingsReport(state, part);
    return true;
  }
  if (m == "export_print_job") {
    const std::string path = p.at("path").get<std::string>();
    if (Lower(std::filesystem::path(path).extension().string()) != ".3mf")
      throw std::runtime_error("export_print_job: path is a .3mf file; got " +
                               path);
    if (std::string why; !DocumentIO::AtomicTargetOk(path, &why))
      throw std::runtime_error("export_print_job: " + why);
    const json& list = p.at("plates");
    if (!list.is_array() || list.empty())
      throw std::runtime_error("export_print_job: plates is a non-empty list "
                               "of {part, name?}");
    std::vector<ui::JobPlateAsk> plates;
    for (size_t i = 0; i < list.size(); i++) {
      const json& e = list[i];
      if (!e.is_object() || !e.contains("part"))
        throw std::runtime_error("export_print_job: plates[" +
                                 std::to_string(i) + "] is {part, name?}");
      for (const auto& [k, v] : e.items())
        if (k != "part" && k != "name")
          throw std::runtime_error("export_print_job: plates[" +
                                   std::to_string(i) + "]: unknown key '" + k +
                                   "'; accepted: part, name");
      Document* part = ResolvePartParam(state, e.at("part"));
      if (part == nullptr)
        throw std::runtime_error("export_print_job: plates[" +
                                 std::to_string(i) + "] names no part");
      ui::JobPlateAsk ask;
      ask.part = part;
      ask.name = e.contains("name") && e.at("name").is_string()
                     ? e.at("name").get<std::string>()
                     : SlotNameOf(state, part);
      plates.push_back(std::move(ask));
    }
    std::optional<json> config;
    if (p.contains("slicer_config"))
      config = ReadConfig(p.at("slicer_config"));
    const std::string name =
        p.value("name", std::filesystem::path(path).stem().string());
    auto built = ui::BuildPrintJob(state, plates, config, name);
    if (!built)
      throw std::runtime_error("export_print_job: " + built.error());
    std::string err;
    if (!DocumentIO::WriteTextAtomic(path, printjob::Archive(built->job), &err))
      throw std::runtime_error("export_print_job: could not write " + path +
                               ": " + err);
    out = std::move(built->summary);
    out["path"] = path;
    return true;
  }
  return false;
}

} // namespace remote
