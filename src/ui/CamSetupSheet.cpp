// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifdef HAVE_CAM

#include "ui/CamSetupSheet.h"

#include "cad/CADMaterials.h"
#include "common/Logger.h"
#include "core/Conventions.h"
#include "core/cam/CamNames.h"
#include "core/cam/CamPost.h"
#include "core/cam/CamSim.h"
#include "core/cam/CamToolpath.h"
#include "core/cam/CamVerify.h"
#include "render/Camera.h"
#include "ui/AppState.h"
#include "ui/CamJobs.h"
#include "ui/CamPanel.h"
#include "ui/CamPlayback.h"
#include "ui/JobActions.h"
#include "ui/RendererSync.h"
#include "ui/ViewActions.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>

namespace ui {

namespace {

std::string Mm(double v, int dp = 1) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.*f", dp, v);
  std::string s(buf);
  if (s == "-0.0" || s == "-0.00" || s == "-0.000")
    s = s.substr(1);
  return s;
}

std::string Minutes(double m) {
  char buf[32];
  if (m < 0.05)
    return "<0.1";
  std::snprintf(buf, sizeof(buf), "%.1f", m);
  return buf;
}

std::string Today() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t); // the CRT's, arguments the other way round
#else
  localtime_r(&t, &tm);
#endif
  char buf[16];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
  return buf;
}

const char* OpTypeWords(cam::OpType t) {
  switch (t) {
  case cam::OpType::Profile:
    return "profile";
  case cam::OpType::Pocket:
    return "pocket";
  case cam::OpType::Face:
    return "face";
  case cam::OpType::Drill:
    return "drill / bore";
  case cam::OpType::Adaptive:
    return "adaptive clear";
  case cam::OpType::EdgeRound:
    return "edge break";
  case cam::OpType::SurfaceParallel:
    return "surface raster";
  case cam::OpType::SurfaceWaterline:
    return "surface waterline";
  case cam::OpType::SurfaceRotary:
    return "rotary surface";
  case cam::OpType::Engrave:
    return "engrave";
  case cam::OpType::ThreadMill:
    return "thread mill";
  case cam::OpType::Count:
    break;
  }
  return "?";
}

const char* ToolWords(const cam::Tool& t) {
  switch (t.type) {
  case cam::ToolType::Flat:
    return "flat";
  case cam::ToolType::Ball:
    return "ball";
  case cam::ToolType::VBit:
    return "V-bit";
  case cam::ToolType::Drill:
    return "drill";
  case cam::ToolType::ThreadMill:
    return "thread mill";
  }
  return "?";
}

// Where X0 Y0 is, in the words the panel's corner selector uses.
std::string ZeroCorner(cam::OriginAt at) {
  switch (at) {
  case cam::OriginAt::StockMin:
    return "the stock's near-left corner (its lowest X and Y)";
  case cam::OriginAt::StockCentre:
    return "the stock's centre in X and Y";
  case cam::OriginAt::ModelMin:
    return "the part's near-left corner (its lowest X and Y)";
  case cam::OriginAt::StockMax:
    return "the stock's far-right corner (its highest X and Y)";
  }
  return "?";
}

// The lines each `(op N)` starts on in the program, in order, for "run from
// line": an op with a rough and a finish is two toolpaths and two lines.
std::map<uint64_t, std::vector<int>> OpLines(const std::string& gcode) {
  std::map<uint64_t, std::vector<int>> out;
  std::istringstream in(gcode);
  int n = 0;
  for (std::string line; std::getline(in, line);) {
    n++;
    if (line.rfind("(op ", 0) == 0 && line.back() == ')') {
      try {
        out[std::stoull(line.substr(4))].push_back(n);
      } catch (...) {
      }
    }
  }
  return out;
}

} // namespace

std::string SetupProgramName(const AppState& state, const cam::Setup& setup) {
  if (state.projectPath.empty())
    return {};
  const std::filesystem::path project(state.projectPath);
  const std::string stem = project.stem().string() + " - " + setup.name;
  return setup.splitOutput ? stem + "-T<n>.nc (one per tool)" : stem + ".nc";
}

bool SetupSheetOf(AppState& state, const cam::Setup& setup,
                  const std::vector<cam::Toolpath>& paths,
                  const std::string& program, cam::SetupSheet& sheet,
                  std::string* error) {
  // The program the sheet describes, posted: a setup that does not post has
  // no program to send with a sheet.
  std::string gcode, why;
  if (!cam::PostProcess(setup.machine.post, setup, paths, PostOpts(setup),
                        gcode, &why)) {
    if (error != nullptr)
      *error =
          "the setup does not post, so there is no program to describe: " + why;
    return false;
  }
  const std::map<uint64_t, std::vector<int>> opLines = OpLines(gcode);
  // The line each toolpath starts on: the k-th `(op N)` for the k-th of op
  // N's toolpaths that posts anything.
  std::vector<int> pathLine(paths.size(), 0);
  {
    std::map<uint64_t, size_t> seen;
    for (size_t i = 0; i < paths.size(); i++) {
      if (!paths[i].error.empty() || paths[i].Empty())
        continue;
      const auto it = opLines.find(paths[i].operationId);
      const size_t k = seen[paths[i].operationId]++;
      if (it != opLines.end() && k < it->second.size())
        pathLine[i] = it->second[k];
    }
  }
  const glm::dvec3 o = setup.origin;
  const std::vector<Body>& bodies = JobBodies(state);
  const cam::JobDocument* job = ActiveJob(state);
  const std::string jobName =
      state.activePart >= 0 &&
              static_cast<size_t>(state.activePart) < state.parts.size()
          ? state.parts[static_cast<size_t>(state.activePart)].name
          : std::string("Job");

  sheet = {};
  sheet.title = jobName + " - " + setup.name;
  const std::string project =
      state.projectPath.empty()
          ? std::string("unsaved project")
          : std::filesystem::path(state.projectPath).filename().string();
  sheet.subtitle = "Setup sheet - " + project + " - " + Today();
  sheet.footer = program.empty() ? sheet.title : program;

  // --- facts -----------------------------------------------------------
  using Fact = cam::SetupSheet::Fact;
  sheet.facts.push_back({"Program", program.empty() ? "(not saved)" : program});
  sheet.facts.push_back(
      {"Machine", (setup.machine.name.empty() ? std::string("the job's own")
                                              : setup.machine.name) +
                      ", post " + setup.machine.post});
  // The parts and what they are made of: the grade the material library
  // carries (6082-T6), as a drawing's title block says it.
  std::string parts, material;
  std::set<std::string> grades;
  if (job != nullptr)
    for (const cam::JobPart& part : job->GetParts()) {
      parts += (parts.empty() ? "" : ", ") + part.name;
      if (const Document* doc = JobPartDocument(state, part))
        for (const Body& b : doc->GetBodies())
          if (!b.reference && !b.material.empty())
            grades.insert(CADMaterials::MaterialLibrary::GradeOf(b.material));
    }
  for (const std::string& g : grades)
    material += (material.empty() ? "" : ", ") + g;
  sheet.facts.push_back({"Part", parts.empty() ? "-" : parts});
  glm::dvec3 lo(0.0), hi(0.0);
  const bool haveStock = cam::StockBounds(setup, bodies, lo, hi);
  if (haveStock) {
    const glm::dvec3 size = hi - lo;
    std::string how = setup.stock.mode == cam::StockMode::Fixed
                          ? std::string("as stated")
                          : "the part plus " + Mm(setup.stock.margin.x) +
                                " / " + Mm(setup.stock.margin.y) + " a side, " +
                                Mm(setup.stock.margin.z) + " over and " +
                                Mm(setup.stock.bottomMargin) + " under";
    sheet.facts.push_back({"Stock", Mm(size.x) + " x " + Mm(size.y) + " x " +
                                        Mm(size.z) + " mm (X x Y x Z), " +
                                        how});
  }
  sheet.facts.push_back(
      {"Material", material.empty() ? "not stated on the part" : material});
  // The zero: which corner, which face, and what to set when touching off
  // on the blank before the face op has cut down to it.
  std::string zero = "X0 Y0 at " + ZeroCorner(setup.originAt) + "; Z0 ";
  if (setup.topFace.IsSet())
    zero += "on the face picked as the top";
  else
    zero += "on the top of the stock";
  if (haveStock && hi.z - o.z > 1e-3)
    zero += ", " + Mm(hi.z - o.z, 3) +
            " mm below the blank's top: touch off on the blank and set Z+" +
            Mm(hi.z - o.z, 3);
  else
    zero += ": touch off there and set Z0";
  sheet.facts.push_back({"Work zero", zero + " (G54)"});
  sheet.facts.push_back(
      {"Safe Z", "Z" + Mm(setup.safeZ - o.z, 2) + " (above the stock" +
                     (setup.fixtures.empty() ? ")" : " and the fixtures)")});
  std::string held;
  if (setup.heldIn == cam::Setup::HeldIn::Rotary || cam::EmitsRotary(setup))
    held = "in the rotary, A" + Mm(setup.rotationA, 1);
  else
    held = "on the bed";
  if (setup.fixtures.empty())
    held += "; no fixtures modelled -- nothing holding the stock was checked";
  sheet.facts.push_back({"Held", held});
  for (const cam::Fixture& f : setup.fixtures) {
    const glm::dvec3 w = f.pos - o;
    sheet.facts.push_back(
        {f.enabled ? "Fixture" : "Fixture (off)",
         (f.name.empty() ? "fixture " + std::to_string(f.id) : f.name) +
             " at X" + Mm(w.x, 2) + " Y" + Mm(w.y, 2) + " Z" + Mm(w.z, 2) +
             (std::abs(f.rotZdeg) > 1e-9
                  ? ", turned " + Mm(f.rotZdeg, 0) + " deg"
                  : std::string())});
  }

  // --- the plan, from above, in the program's coordinates ----------------
  if (haveStock) {
    const auto rect = [&](double x0, double y0, double x1, double y1) {
      return cam::SetupSheet::Polyline{{x0 - o.x, y0 - o.y},
                                       {x1 - o.x, y0 - o.y},
                                       {x1 - o.x, y1 - o.y},
                                       {x0 - o.x, y1 - o.y}};
    };
    sheet.plan.stock = rect(lo.x, lo.y, hi.x, hi.y);
    // The part's outline: its section halfway up, where a plate shows its
    // rim and its holes.
    const std::vector<Body> framed = cam::BodiesInSetupFrame(setup, bodies);
    glm::dvec3 plo(1e300), phi(-1e300);
    for (const Body& b : framed) {
      glm::dvec3 a(0.0), c(0.0);
      b.shape.GetBoundingBox(a, c);
      plo = glm::min(plo, a);
      phi = glm::max(phi, c);
    }
    if (plo.z < phi.z)
      for (const cam::Loop& l :
           cam::SectionLoops(framed, (plo.z + phi.z) * 0.5)) {
        cam::SetupSheet::Polyline line;
        for (const glm::dvec2& q : l.pts)
          line.push_back({q.x - o.x, q.y - o.y});
        sheet.plan.part.push_back(std::move(line));
      }
    std::vector<std::string> missing;
    for (const cam::FixtureSolid& f :
         JobFixtureSolids(state, setup, /*enabledOnly=*/false, &missing)) {
      glm::dvec3 flo(1e300), fhi(-1e300);
      for (const Body& b : f.bodies) {
        glm::dvec3 a(0.0), c(0.0);
        b.shape.GetBoundingBox(a, c);
        flo = glm::min(flo, a);
        fhi = glm::max(fhi, c);
      }
      if (flo.x < fhi.x)
        sheet.plan.fixtures.push_back(rect(flo.x, flo.y, fhi.x, fhi.y));
    }
    sheet.plan.caption =
        "The program's X0 Y0 is the ring, " + ZeroCorner(setup.originAt) +
        "; X+ to the right, Y+ up, looking down the spindle. Dashed: the "
        "stock, " +
        Mm(hi.x - lo.x) + " x " + Mm(hi.y - lo.y) +
        " mm. Solid: the part's outline halfway up. Grey: what holds it" +
        (setup.fixtures.empty() ? " (nothing is modelled)." : ".");
  }

  // --- tools, in the order they cut -------------------------------------
  std::vector<int> order;
  std::map<int, std::vector<const cam::Toolpath*>> byTool;
  for (const cam::Toolpath& tp : paths) {
    if (!tp.error.empty() || tp.Empty())
      continue;
    if (std::find(order.begin(), order.end(), tp.toolNumber) == order.end())
      order.push_back(tp.toolNumber);
    byTool[tp.toolNumber].push_back(&tp);
  }
  const auto opName = [&](uint64_t id) {
    for (const cam::Operation& op : setup.operations)
      if (op.id == id)
        return op.name;
    return std::string();
  };
  cam::SetupSheet::Table tools;
  tools.heading = "Tools, in the order they are loaded (stickout: tip to "
                  "the holder; ? = not stated, and not checked)";
  tools.columns = {
      "T",   "Tool",          "Type", "Dia", "Flutes", "Stickout / holder",
      "rpm", "Feed / plunge", "Cuts", "Min"};
  tools.widths = {0.04, 0.19, 0.11, 0.06, 0.06, 0.12, 0.07, 0.11, 0.19, 0.05};
  for (const int number : order) {
    const cam::Tool* t = setup.FindTool(number);
    if (t == nullptr)
      continue;
    std::string cuts;
    std::set<uint64_t> seen;
    double minutes = 0.0;
    std::vector<cam::Toolpath> mine;
    for (const cam::Toolpath* tp : byTool[number]) {
      mine.push_back(*tp);
      if (seen.insert(tp->operationId).second)
        cuts += (cuts.empty() ? "" : ", ") + std::to_string(tp->operationId) +
                " " + opName(tp->operationId) + (tp->finish ? " (finish)" : "");
    }
    minutes = cam::CuttingMinutes(setup, mine);
    std::string type = ToolWords(*t);
    if (t->type == cam::ToolType::VBit || t->type == cam::ToolType::Drill ||
        t->type == cam::ToolType::ThreadMill)
      type += " " + Mm(t->tipAngleDeg, 0) + "°";
    const std::string stickout =
        (t->overallLength > 0.0 ? Mm(t->overallLength, 1) : std::string("?")) +
        " / " +
        (t->collarDiameter > 0.0 ? "Ø" + Mm(t->collarDiameter, 0)
                                 : std::string("?"));
    tools.rows.push_back({std::to_string(t->number), t->name, type,
                          Mm(t->diameter, 2), std::to_string(t->flutes),
                          stickout, Mm(t->spindleRpm, 0),
                          Mm(t->feedCut, 0) + " / " + Mm(t->feedPlunge, 0),
                          cuts, Minutes(minutes)});
  }
  sheet.tables.push_back(std::move(tools));

  // --- operations, in cutting order ------------------------------------
  cam::SetupSheet::Table ops;
  ops.heading = "Operations, in cutting order";
  ops.columns = {"Op",          "Name", "Type", "T",
                 "Z from / to", "Line", "Min",  "Notes"};
  ops.widths = {0.05, 0.19, 0.11, 0.04, 0.13, 0.07, 0.06, 0.35};
  for (size_t pi = 0; pi < paths.size(); pi++) {
    const cam::Toolpath& tp = paths[pi];
    const cam::Operation* op = nullptr;
    for (const cam::Operation& candidate : setup.operations)
      if (candidate.id == tp.operationId)
        op = &candidate;
    if (op == nullptr)
      continue;
    std::string notes;
    const auto note = [&](const std::string& n) {
      notes += (notes.empty() ? "" : "; ") + n;
    };
    if (!tp.error.empty())
      note("NOT IN THE PROGRAM: " + tp.error);
    for (const std::string& s : tp.skipped)
      note("skipped " + s);
    if (tp.finish)
      note("the finish of op " + std::to_string(op->id) + "'s rough");
    for (const cam::Toolpath::Fit& f : tp.fits)
      if (f.depth)
        note((f.internal ? "floor " : "shoulder ") + Mm(f.nominal, 3) + " " +
             f.label + ", aimed at " + Mm(f.aim, 4));
      else
        note((f.internal ? "bore " : "boss ") + std::string("Ø") +
             Mm(f.nominal, 3) + " " + f.label + " " + Mm(f.Min(), 4) + "-" +
             Mm(f.Max(), 4) + ", aimed at " + Mm(f.aim, 4));
    if (!tp.comps.empty())
      note(std::string(tp.comps.front().left ? "G41" : "G42") + " D" +
           std::to_string(tp.toolNumber) + " wear comp");
    if (!tp.strategy.empty())
      note(tp.strategy);
    ops.rows.push_back(
        {std::to_string(op->id), op->name, OpTypeWords(op->type),
         std::to_string(tp.toolNumber),
         "Z" + Mm(op->topZ - o.z, 2) + " / Z" + Mm(op->bottomZ - o.z, 2),
         pathLine[pi] == 0 ? std::string("-") : std::to_string(pathLine[pi]),
         tp.error.empty() ? Minutes(cam::CuttingMinutes(setup, {tp})) : "-",
         notes});
  }
  sheet.tables.push_back(std::move(ops));

  // --- the cycle --------------------------------------------------------
  const double minutes = cam::CuttingMinutes(setup, paths);
  const size_t changes = order.empty() ? 0 : order.size() - 1;
  sheet.facts.push_back(
      {"Cycle time",
       Minutes(minutes) + " min " +
           (setup.rapidFeed > 0.0
                ? "with rapids at " + Mm(setup.rapidFeed, 0) + " mm/min"
                : std::string("cutting (rapids not counted: no traverse "
                              "rate stated)")) +
           ", " + std::to_string(order.size()) + " tool" +
           (order.size() == 1 ? "" : "s") + ", " + std::to_string(changes) +
           " tool change" + (changes == 1 ? "" : "s")});

  // --- notes ------------------------------------------------------------
  // Each toleranced wall once, with every op that cuts it: a bore and its
  // spring pass are one wall.
  bool anyFit = false;
  std::vector<std::pair<const cam::Toolpath::Fit*, std::string>> walls;
  for (const cam::Toolpath& tp : paths)
    for (const cam::Toolpath::Fit& f : tp.fits) {
      bool same = false;
      for (auto& [w, ids] : walls)
        if (w->depth == f.depth && std::abs(w->nominal - f.nominal) < 1e-6 &&
            glm::distance(w->centre, f.centre) < 1e-3 &&
            std::abs(w->faceZ - f.faceZ) < 1e-6) {
          same = true;
          const std::string id = std::to_string(f.op);
          if (ids.find(id) == std::string::npos)
            ids += " and " + id;
        }
      if (!same)
        walls.push_back({&f, std::to_string(f.op)});
    }
  for (const auto& [wp, ids] : walls) {
    const cam::Toolpath::Fit& f = *wp;
    anyFit = true;
    {
      const std::string who =
          (ids.find(" and ") != std::string::npos ? "Ops " : "Op ") + ids +
          " (" + opName(f.op) + ")";
      if (f.depth)
        sheet.notes.push_back(
            who + ": the " + (f.internal ? "floor" : "shoulder") + " " +
            Mm(f.nominal, 3) + " from the face at Z" + Mm(f.refZ - o.z, 3) +
            " is held to " + f.label + " (" + Mm(f.Min(), 4) + " to " +
            Mm(f.Max(), 4) + "); the program aims at " + Mm(f.aim, 4) + " (Z" +
            Mm(f.ZAt(f.aim) - o.z, 4) + "). Measure it with a height gauge.");
      else
        sheet.notes.push_back(
            who + ": the " + (f.internal ? "bore" : "boss") + " Ø" +
            Mm(f.nominal, 3) + " " + f.label + " is held to " + Mm(f.Min(), 4) +
            " - " + Mm(f.Max(), 4) + "; the program aims at its middle, " +
            Mm(f.aim, 4) +
            ". A router's runout and the cutter's deflection "
            "move it: gauge it.");
    }
  }
  std::vector<uint64_t> compOps;
  int compTool = 0;
  std::string lines;
  for (size_t pi = 0; pi < paths.size(); pi++) {
    const cam::Toolpath& tp = paths[pi];
    if (!tp.error.empty() || tp.comps.empty())
      continue;
    if (std::find(compOps.begin(), compOps.end(), tp.operationId) ==
        compOps.end())
      compOps.push_back(tp.operationId);
    compTool = tp.toolNumber;
    if (pathLine[pi] != 0)
      lines += (lines.empty() ? "" : ", ") + std::to_string(pathLine[pi]);
  }
  if (!compOps.empty()) {
    std::string which;
    for (const uint64_t id : compOps)
      which += (which.empty() ? "" : " and ") + std::to_string(id);
    sheet.notes.push_back(
        "Cutter compensation (wear): op" +
        std::string(compOps.size() == 1 ? " " : "s ") + which +
        " run under G41/G42 with D" + std::to_string(compTool) +
        ". The program is already the tool centre, so tool " +
        std::to_string(compTool) +
        "'s DIAMETER in the controller's tool table holds the wear alone: 0 "
        "cuts what was verified, +0.02 leaves 0.01 more on each wall (a bore "
        "0.02 smaller), -0.02 takes 0.01 more. A wall cannot be put back: "
        "run first with a positive entry (+0.04 leaves a bore 0.04 small), "
        "gauge, take the shortfall off the entry and run " +
        (compOps.size() == 1 ? "that op" : "those ops") + " again (from line " +
        lines + "). Set the entry back to 0 before the next part.");
  }
  // Each thread milled here, against its class: a height map cannot see a
  // flank, so the gauge is the check (docs/cam/simulation.md#threads).
  for (const cam::Toolpath& tp : paths)
    for (const cam::Toolpath::Thread& t : tp.threads) {
      anyFit = true;
      sheet.notes.push_back(
          "Op " + std::to_string(t.op) + " (" + opName(t.op) +
          "): " + t.designation + "-" + t.threadClass +
          (t.internal ? " internal" : " external") +
          ", pitch diameter aimed at " + Mm(t.pdAim, 3) + " in " +
          Mm(t.pdMin, 3) + " - " + Mm(t.pdMax, 3) +
          ". Check it with a go / no-go " + (t.internal ? "plug" : "ring") +
          " gauge; a thread that does not go takes another pass with a "
          "deeper last aim, before the part comes out.");
    }
  // What the job leaves to be done by hand: the threads of the parts no
  // thread mill of the job targets
  // (docs/cam/simulation.md#what-the-job-does-not-cut).
  if (job != nullptr) {
    std::vector<uint64_t> milled;
    for (const cam::Setup& any : job->GetSetups())
      for (const cam::Operation& op : any.operations)
        if (op.enabled && op.type == cam::OpType::ThreadMill)
          milled.push_back(op.id);
    cam::Verdict uncut;
    cam::AddUncutThreads(job->GetSetups(), bodies, milled, uncut);
    std::map<std::string, int> count;
    for (const cam::UncutFeature& u : uncut.uncut)
      count[u.what]++;
    std::string list;
    for (const auto& [what, n] : count)
      list += (list.empty() ? "" : ", ") + std::to_string(n) + " x " + what;
    if (!list.empty())
      sheet.notes.push_back("Not cut by this job, in any setup: " + list +
                            " -- by hand (a tap), after the last setup.");
  }
  if (anyFit)
    sheet.notes.push_back(
        "Gauge every toleranced wall and thread above BEFORE the setup is "
        "broken: once the part is out of the vise it cannot be put back "
        "where it was cut.");
  for (const cam::Toolpath& tp : paths)
    if (!tp.error.empty())
      sheet.notes.push_back("Op " + std::to_string(tp.operationId) + " " +
                            opName(tp.operationId) +
                            " is NOT in the program: " + tp.error);
  sheet.notes.push_back(
      "At the machine: touch off on the zero above; the first rapid clears "
      "only the fixtures modelled here; after each tool change (M0) set Z "
      "zero for the new tool, then cycle start (docs/cam/post.md, At the "
      "machine).");
  return true;
}

bool SetupSheetPicture(AppState& state, const cam::Setup& setup,
                       cam::SetupSheet& sheet) {
  if (!state.captureRaster || state.camera == nullptr) {
    sheet.imageCaption = "No picture: this instance has no renderer "
                         "(--headless).";
    return false;
  }
  // The job tab showing THIS setup, from the iso view, framed for the
  // picture's aspect -- and everything put back after.
  const uint64_t shown = state.camActiveSetup;
  const bool switched = shown != setup.id;
  const Camera camera = *state.camera;
  // Another setup's playback draws its own stock over this one's part.
  if (switched) {
    if (state.camPlaybackOpen)
      EndCamPlayback(state);
    state.camActiveSetup = setup.id;
  }
  constexpr uint32_t kW = 720, kH = 540;
  FitCameraToDocument(state, static_cast<float>(kW) / static_cast<float>(kH));
  if (const conv::NamedView* v = conv::FindView("iso"))
    state.camera->SnapToView(glm::vec3(v->toCamera), glm::vec3(v->up));
  SyncRenderersToDocument(state);
  AppState::RasterStill req;
  req.width = kW;
  req.height = kH;
  req.supersample = 2;
  RasterFrame frame;
  std::string error, warning;
  const bool ok = state.captureRaster(req, &frame, &error, &warning);
  *state.camera = camera;
  if (switched) {
    state.camActiveSetup = shown;
    SyncRenderersToDocument(state);
  }
  if (!ok || frame.factor == 0 || frame.width < frame.factor ||
      frame.rgb.size() < size_t{frame.width} * frame.height * 3) {
    sheet.imageCaption = "No picture: the capture failed" +
                         (error.empty() ? std::string() : ": " + error);
    return false;
  }
  // Averaged down by the supersample, as the quick still is.
  const uint32_t k = frame.factor;
  const uint32_t w = frame.width / k, h = frame.height / k;
  sheet.imageWidth = w;
  sheet.imageHeight = h;
  sheet.rgb.assign(size_t{w} * h * 3, 0);
  for (uint32_t y = 0; y < h; y++)
    for (uint32_t x = 0; x < w; x++)
      for (int c = 0; c < 3; c++) {
        uint32_t sum = 0;
        for (uint32_t dy = 0; dy < k; dy++)
          for (uint32_t dx = 0; dx < k; dx++)
            sum += frame.rgb[((size_t{y} * k + dy) * frame.width +
                              (size_t{x} * k + dx)) *
                                 3 +
                             static_cast<size_t>(c)];
        sheet.rgb[(size_t{y} * w + x) * 3 + static_cast<size_t>(c)] =
            static_cast<uint8_t>(sum / (k * k));
      }
  sheet.imageCaption = "The setup as the job models it: the finished part "
                       "where it sits in the stock, the fixtures ghosted.";
  return true;
}

bool ExportSetupSheet(AppState& state, uint64_t setupId,
                      const std::vector<cam::Toolpath>& paths,
                      const std::string& path, const std::string& program,
                      bool picture, SetupSheetResult& result,
                      std::string* error) {
  cam::JobDocument* job = ActiveJob(state);
  const cam::Setup* setup = job != nullptr ? job->FindSetup(setupId) : nullptr;
  if (setup == nullptr) {
    if (error != nullptr)
      *error = "no setup " + std::to_string(setupId) + " in this job";
    return false;
  }
  for (const cam::Toolpath& tp : paths)
    if (tp.pending) {
      if (error != nullptr)
        *error = "a toolpath of this setup is still being cut: the sheet "
                 "would describe the previous one";
      return false;
    }
  result = {};
  result.program = program.empty() ? SetupProgramName(state, *setup) : program;
  cam::SetupSheet sheet;
  if (!SetupSheetOf(state, *setup, paths, result.program, sheet, error))
    return false;
  if (picture) {
    result.picture = SetupSheetPicture(state, *setup, sheet);
    if (!result.picture)
      result.warnings.push_back(sheet.imageCaption);
  } else {
    sheet.imageCaption = "No picture: left out on request.";
  }
  if (!cam::WriteSetupSheetPdf(sheet, path, error))
    return false;
  result.cycleMinutes = cam::CuttingMinutes(*setup, paths);
  result.tools = sheet.tables.empty() ? 0 : sheet.tables[0].rows.size();
  result.operations = sheet.tables.size() < 2 ? 0 : sheet.tables[1].rows.size();
  LOG_INFO("CAM: wrote the setup sheet {}", path);
  return true;
}

void PumpSetupSheet(AppState& state) {
  if (!state.camSheetPending)
    return;
  state.camSheetPending = false;
  const cam::Setup* setup = ActiveCamSetup(state);
  if (setup == nullptr || !state.job)
    return;
  if (const std::string why = CamToolpathsPendingWhy(state); !why.empty()) {
    state.camLastPost = why;
    return;
  }
  if (state.projectPath.empty()) {
    state.camLastPost = "save the project first -- the setup sheet goes "
                        "beside it, named after the setup";
    return;
  }
  const std::filesystem::path project(state.projectPath);
  std::filesystem::path dir = project.parent_path();
  if (dir.empty())
    dir = ".";
  const std::string path = (dir / (project.stem().string() + " - " +
                                   setup->name + " setup sheet.pdf"))
                               .string();
  SetupSheetResult result;
  std::string error;
  if (ExportSetupSheet(state, setup->id, state.camToolpaths, path, {}, true,
                       result, &error))
    state.camLastPost = path;
  else
    state.camLastPost = "could not write the setup sheet: " + error;
}

} // namespace ui

#endif // HAVE_CAM
