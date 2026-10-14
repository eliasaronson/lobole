// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "remote/RemoteTube.h"

#include "cad/CADKernel.h"
#include "core/Document.h"
#include "core/features/CurveFeature.h"
#include "core/features/PipeFeature.h"
#include "core/tube/Check.h"
#include "core/tube/CutList.h"
#include "core/tube/Frame.h"
#include "core/tube/Rules.h"
#include "ui/AppState.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using nlohmann::json;

namespace remote {

// describe: one row per `m ==` branch below — extend together.
static constexpr MethodDoc TubeMethods[] = {
    {"set_tube_rules",
     "Declare the SHOP this part is bent in: the `material` every member is "
     "made of, and the bender. There is deliberately no SIZE here — unlike a "
     "sheet part, which is cut from one blank, a tube frame is several sizes "
     "at once (a Formula Student chassis is 25.4x2.4 in its hoops and 19.05 "
     "in its tertiary members), and each member's Pipe carries its own. "
     "`former_radii_mm` is the die set, the CENTRELINE radii this bender "
     "owns; omit it and any radius is allowed, which is the honest reading "
     "of 'no bender declared'. `min_straight_mm` (what a clamp die needs "
     "between bends) and `tail_length_mm` (what the bender grips past the "
     "last one) default to 2x and 1.5x THAT MEMBER's outer diameter. "
     "`max_thinning` is a FRACTION of the wall, for a rulebook that demands "
     "better than the 25% draw bending is accepted at. `clear: true` makes "
     "it not a tube part. It drives no geometry, so setting it rebuilds "
     "nothing.",
     "clear, former_radii_mm, material, max_thinning, min_straight_mm, "
     "tail_length_mm",
     remote::NeedsPart},
    {"get_tube_rules",
     "This part's tube rules (`min_straight` and `tail_length` in mm), with "
     "every derived value RESOLVED (so a caller "
     "sees the minimum straight it will actually be checked against, not a "
     "0 meaning 'derived'), plus the minimum former this stock takes and "
     "where that number came from. With no rules set it answers null and "
     "lists what IS stocked, so a picker built from it cannot offer a size "
     "set_tube_rules would refuse.",
     "part", remote::Deferrable,
     "rules:{material:string, max_thinning:number, "
     "former_radii_mm:number[], min_straight:number|string, "
     "tail_length:number|string}|null, stocked?:object"},
    {"check_frame",
     "Is this frame a TRUSS? Three questions on the member graph: a member "
     "welded to nothing at one end (a lever, and a space frame has none), a "
     "joint landing on a neighbour's BEND (a torus, thinned by the bending, "
     "with no straight to fit against), and a four-member bay with no "
     "diagonal across it — which carries its load by BENDING its welds. "
     "`not_checked` names the two things it cannot answer: how STIFF the "
     "frame is, which is a beam model's question, and whether a PANEL braces "
     "a bay, which a floor or a firewall does as surely as a diagonal. So an "
     "empty `violations` is not a claim that the frame is stiff.",
     "part", remote::Deferrable,
     "violations[]{kind,message,member,bend,actual,required}, "
     "members_checked, bends_checked, not_checked"},
    {"export_members",
     "Write one STEP per tube MEMBER into {dir}, each file named for its "
     "member and holding one product of the same name. That is what a tube "
     "laser takes: a solid with its ends "
     "already coped and mitred, one file per tube. Returns a row per file so "
     "a manifest can be built from the same call that wrote them. "
     "`overwrite` (default false) refuses rather than replacing a file that "
     "is already there, because a directory half from one run and half from "
     "another is an order nobody can check.",
     "dir, overwrite, part", NotInBatch},
    {"export_cut_list",
     "Write the cut list to a CSV {path}: one row per distinct member with "
     "the quantity, the material and size the part declares, the CUT length "
     "(the metal, with any cope taken off) and the bend count. Columns follow "
     "export_bom's where they carry over, so a shop reading both files meets "
     "one spelling of `material`. Pass `stock_length_mm` (and `kerf_mm`, the "
     "saw's) and it also answers the question a cut list exists for - how "
     "many lengths to BUY - by packing the pieces first-fit decreasing and "
     "reporting the sticks and their offcuts. There is no default stock "
     "length: a six-metre stick is a supplier's fact, not something derivable "
     "from the part, so it is asked for rather than guessed.",
     "kerf_mm, part, path, stock_length_mm", NotInBatch},
    {"export_bend_schedule",
     "Write the bend schedule to a CSV {path}: one row per BEND — member, "
     "bend number, the distance from the last one, the angle, the roll and "
     "the former. That row order is what a bender is programmed from, and it "
     "is not the cut list's shape, which is why it is a second file.",
     "part, path", NotInBatch},
    {"check_tube",
     "Manufacturability of every tube member in the part, each against ITS "
     "OWN stock: a size nobody rolls (so nobody can order it), a former "
     "tighter than that member's wall can take (the limit is set by D/t, not "
     "D), a radius no declared die matches, two bends too close together to "
     "clamp between, nothing left to grip past the last one, an outer wall "
     "pulled thinner than `max_thinning`. Each violation names the member, "
     "the bend and the required-versus-actual number. `not_checked` says "
     "what this build cannot answer — ovality and springback belong to the "
     "tooling and the machine, which it has no model of — so an empty "
     "`violations` cannot be read as 'all clear' on those.",
     "part", remote::Deferrable,
     "violations[]{kind,message,member,bend,actual,required}, "
     "members_checked, bends_checked, not_checked"},
};

namespace {

// The bends of every BentPath curve in the document, as the checker's plain
// records. A curve that is not a bent path is not a bendable member: a spline
// has no former to check.
std::vector<tube::Member> MembersOf(const Document& doc) {
  // The cut list's own derivation, so the check and the order agree about
  // what the members are and what each one is made of.
  std::vector<tube::Member> members;
  for (const tube::MemberRow& row : tube::Members(doc)) {
    tube::Member m;
    m.name = row.name;
    m.length = row.pathLength;
    m.outerDiameter = row.outerDiameter;
    m.wallThickness = row.wallThickness;
    m.bends = row.bends;
    // A row stands for `quantity` identical members, and each of them is
    // bent: a check that looked at one of four would report a quarter of the
    // trouble.
    for (int i = 0; i < row.quantity; i++)
      members.push_back(m);
  }
  return members;
}

// A CSV field, quoted only where it has to be — the rule export_bom's writer
// uses, so two files from one project do not disagree about quoting.
std::string Field(const std::string& v) {
  if (v.find_first_of(",\"\n") == std::string::npos)
    return v;
  std::string quoted = "\"";
  for (const char c : v) {
    if (c == '"')
      quoted += '"';
    quoted += c;
  }
  return quoted + "\"";
}

std::string Num(double v) {
  char buf[64];
  snprintf(buf, sizeof buf, "%.6g", v);
  return std::string(buf);
}

std::string Join(const std::vector<std::string>& v) {
  std::string out;
  for (const std::string& s : v)
    out += out.empty() ? s : "; " + s;
  return out;
}

json SizesJson(const std::string& material) {
  json arr = json::array();
  for (const std::string& s : tube::SizesFor(material))
    arr.push_back(s);
  return arr;
}

json RulesJson(const tube::TubeRules& r) {
  json j = {{"material", r.material},
            {"max_thinning", r.EffectiveMaxThinning()}};
  j["former_radii_mm"] = r.formerRadii; // empty = any radius
  // The straight a clamp needs and the tail a bender grips are multiples of
  // the MEMBER's diameter, so they are reported as the rule rather than as a
  // number that would be wrong for every member but one.
  j["min_straight"] = r.minStraight > 0.0
                          ? json(r.minStraight)
                          : json("2 x the member's outer diameter");
  j["tail_length"] = r.tailLength > 0.0
                         ? json(r.tailLength)
                         : json("1.5 x the member's outer diameter");
  return j;
}

} // namespace

bool HandleTubeRpc(ui::AppState& state, Document* doc, const std::string& m,
                   const RpcParams& p, json& out) {
  if (m == "set_tube_rules") {
    Document& active = *doc;
    if (p.value("clear", false)) {
      active.SetTube(std::nullopt);
      out = {{"rules", nullptr}};
      return true;
    }
    // Start from what is already set, so declaring a die set does not
    // silently clear the size.
    tube::TubeRules r;
    if (active.Tube())
      r = *active.Tube();
    if (p.contains("material"))
      r.material = p.at("material").get<std::string>();
    if (r.material.empty()) {
      throw std::runtime_error(
          "set_tube_rules needs a material - a bend cannot guess what it is "
          "pulling. There is deliberately no SIZE here: unlike a sheet part, "
          "a tube frame is several sizes at once and each member's Pipe "
          "carries its own.");
    }
    if (p.contains("former_radii_mm")) {
      r.formerRadii.clear();
      for (const auto& v : p.at("former_radii_mm"))
        r.formerRadii.push_back(v.get<double>());
    }
    if (p.contains("min_straight_mm"))
      r.minStraight = p.at("min_straight_mm").get<double>();
    if (p.contains("tail_length_mm"))
      r.tailLength = p.at("tail_length_mm").get<double>();
    if (p.contains("max_thinning"))
      r.maxThinning = p.at("max_thinning").get<double>();
    if (const std::string bad = tube::Validate(r); !bad.empty())
      throw std::runtime_error(bad);
    active.SetTube(r);
    out = {{"rules", RulesJson(r)}};
    return true;
  }

  if (m == "get_tube_rules") {
    const Document& subject = SubjectPart(state, doc, p);
    if (!subject.Tube()) {
      json materials = json::object();
      for (const std::string& mat : tube::Materials())
        materials[mat] = SizesJson(mat);
      out = {{"rules", nullptr}, {"stocked", std::move(materials)}};
      return true;
    }
    out = {{"rules", RulesJson(*subject.Tube())}};
    return true;
  }

  if (m == "check_frame") {
    const Document& subject = SubjectPart(state, doc, p);
    std::vector<tube::FrameMember> frame;
    for (size_t i = 0; i < subject.FeatureCount(); i++) {
      const auto* pipe =
          dynamic_cast<const PipeFeature*>(subject.GetFeature(i));
      if (pipe == nullptr)
        continue;
      const auto* curve = dynamic_cast<const CurveFeature*>(
          subject.FindFeatureById(pipe->pathId));
      if (curve == nullptr)
        continue;
      tube::FrameMember fm;
      fm.name = pipe->GetName();
      fm.outerDiameter = pipe->outerDiameter;
      // The METAL: the path clipped to what the member still occupies after
      // its joints were cut. Members are swept LONG — past their nodes, so a
      // mitre has material to cut — so the raw path runs a diameter beyond
      // the frame at each end, and reading it would report every one of them
      // as a member welded to nothing.
      double from = 0.0, to = std::numeric_limits<double>::max();
      for (const Body& body : subject.GetBodies()) {
        if (body.createdBy != pipe->GetId())
          continue;
        if (const auto span = tube::MetalAlongPath(body, curve->EdgeShape())) {
          from = span->first;
          to = span->second;
        }
      }
      // CLIPPED, not filtered: a straight edge samples to its two endpoints
      // and nothing between, and both of those are in the overrun — filtering
      // would drop the member entirely rather than shorten it.
      const auto samples = tube::SampleCurve(curve->EdgeShape(), 1.0);
      for (size_t k = 0; k < samples.size(); k++) {
        const auto& [pt, at] = samples[k];
        if (k + 1 < samples.size()) {
          const auto& [nextPt, nextAt] = samples[k + 1];
          // Where the metal STARTS, between this sample and the next.
          if (at < from && nextAt > from && nextAt > at) {
            const double t = (from - at) / (nextAt - at);
            fm.centreline.push_back(pt + (nextPt - pt) * t);
          }
        }
        if (at + 1e-9 >= from && at <= to + 1e-9)
          fm.centreline.push_back(pt);
        if (k + 1 < samples.size()) {
          const auto& [nextPt, nextAt] = samples[k + 1];
          if (at < to && nextAt > to && nextAt > at) {
            const double t = (to - at) / (nextAt - at);
            fm.centreline.push_back(pt + (nextPt - pt) * t);
          }
        }
      }
      if (fm.centreline.size() < 2)
        continue;
      for (const CurveFeature::Bend& b : curve->Bends()) {
        fm.bends.push_back({b.angleDeg, b.radius, b.straightBefore, b.rollDeg});
        fm.corners.push_back(b.corner);
      }
      frame.push_back(std::move(fm));
    }
    if (frame.empty()) {
      throw std::runtime_error(
          "this part has no tube members, so there is no frame to check - a "
          "member is a Pipe swept along a Curve3D");
    }
    const tube::CheckResult res = tube::CheckFrame(frame);
    json violations = json::array();
    for (const tube::Violation& v : res.violations) {
      violations.push_back({{"kind", v.kind},
                            {"message", v.message},
                            {"member", v.member},
                            {"bend", v.bend},
                            {"actual", v.actual},
                            {"required", v.required}});
    }
    out = {{"violations", std::move(violations)},
           {"members_checked", res.membersChecked},
           {"bends_checked", res.bendsChecked},
           {"not_checked", res.notChecked}};
    return true;
  }

  if (m == "check_tube") {
    const Document& subject = SubjectPart(state, doc, p);
    if (!subject.Tube()) {
      throw std::runtime_error(
          "this part has no tube rules, so there is nothing to check a bend "
          "against - set_tube_rules declares the stock and the bender");
    }
    const std::vector<tube::Member> members = MembersOf(subject);
    const tube::CheckResult res = tube::Check(members, *subject.Tube());
    json violations = json::array();
    for (const tube::Violation& v : res.violations) {
      violations.push_back({{"kind", v.kind},
                            {"message", v.message},
                            {"member", v.member},
                            {"bend", v.bend},
                            {"actual", v.actual},
                            {"required", v.required}});
    }
    out = {{"violations", std::move(violations)},
           {"members_checked", res.membersChecked},
           {"bends_checked", res.bendsChecked},
           {"not_checked", res.notChecked}};
    return true;
  }

  if (m == "export_members") {
    const Document& subject = SubjectPart(state, doc, p);
    const std::string dir = p.at("dir").get<std::string>();
    const bool overwrite = p.value("overwrite", false);

    // Planned whole, then written: every refusal -- two members with one
    // name, a file already there, nothing to export -- comes before the
    // directory is made or the first file lands. Checked one member at a
    // time between the writes, a refusal left exactly the directory "half
    // from one run and half from another" the overwrite rule exists to stop.
    struct Planned {
      const Body* body;
      std::string safe, path;
    };
    std::vector<Planned> plan;
    std::set<std::string> used;
    for (size_t i = 0; i < subject.GetBodies().size(); i++) {
      const Body& body = subject.GetBodies()[i];
      // A member is a body swept by a Pipe: the same rule the cut list uses,
      // so the files and the list cannot disagree about what a member is.
      bool isMember = false;
      for (const tube::MemberRow& row : tube::Members(subject)) {
        if (std::find(row.bodies.begin(), row.bodies.end(), body.name) !=
            row.bodies.end())
          isMember = true;
      }
      if (!isMember)
        continue;

      std::string safe;
      for (const char c : body.name)
        safe += (c == '/' || c == '\\' || c == ' ') ? '_' : c;
      if (!used.insert(safe).second) {
        throw std::runtime_error(
            "two members are both called '" + body.name +
            "', and each member's file is named after it, so they would "
            "overwrite each other");
      }
      const std::string path = dir + "/" + safe + ".step";
      if (!overwrite && std::filesystem::exists(path)) {
        throw std::runtime_error(
            "'" + path +
            "' is already there. Pass overwrite: true to replace it - a "
            "directory half from one run and half from another is an order "
            "nobody can check");
      }
      plan.push_back({&body, safe, path});
    }
    if (plan.empty()) {
      throw std::runtime_error(
          "this part has no tube members to export - a member is a Pipe "
          "swept along a Curve3D of kind bent_path");
    }

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    json files = json::array();
    for (const Planned& one : plan) {
      // The product inside is named after the member too, so the file still
      // says what it is once it has been renamed or opened in another system.
      CADKernel::StepExportModel model;
      model.parts.push_back(
          {one.body->name, {{one.body->shape, one.body->name, std::nullopt}}});
      std::string why;
      if (!CADKernel::Instance().SaveSTEPModel(one.path, model, &why))
        throw std::runtime_error("could not write '" + one.path + "': " + why);
      files.push_back({{"member", one.body->name},
                       {"file", one.safe + ".step"},
                       {"volume_mm3", one.body->shape.Volume()}});
    }
    const int count = static_cast<int>(files.size());
    out = {{"dir", dir}, {"files", std::move(files)}, {"count", count}};
    return true;
  }

  if (m == "export_cut_list" || m == "export_bend_schedule") {
    const Document& subject = SubjectPart(state, doc, p);
    const std::string path = p.at("path").get<std::string>();
    const std::vector<tube::MemberRow> rows = tube::Members(subject);
    if (rows.empty()) {
      throw std::runtime_error(
          "this part has no bent-path tube members, so there is nothing to "
          "cut or bend - a member is a Pipe swept along a Curve3D of kind "
          "bent_path");
    }
    const std::string material =
        subject.Tube() ? subject.Tube()->material : std::string{};
    std::string csv;
    json out_extra = json::object();
    if (m == "export_cut_list") {
      // Refused here, before the file is written: unread, it used to be
      // refused as unknown only after the CSV had landed.
      if (p.contains("kerf_mm") && !p.contains("stock_length_mm"))
        throw std::runtime_error(
            "kerf_mm is the saw cut between pieces packed into "
            "stock_length_mm; it means nothing without the stock length");
      csv = "member,quantity,material,size,cut_length_mm,bends,bodies\n";
      for (const tube::MemberRow& r : rows) {
        // The size is the MEMBER's, off its own Pipe: a frame is several
        // sizes at once, and a cut list that gave them all one would order
        // the wrong tube for most of it.
        csv += Field(r.name) + "," + std::to_string(r.quantity) + "," +
               Field(material) + "," +
               Field(tube::Designation(r.outerDiameter, r.wallThickness)) +
               "," + Num(r.cutLength) + "," + std::to_string(r.bends.size()) +
               "," + Field(Join(r.bodies)) + "\n";
      }
      if (p.contains("stock_length_mm")) {
        const double stock = p.at("stock_length_mm").get<double>();
        const double kerf = p.value("kerf_mm", 0.0);
        // Packed PER SIZE. A 19.05 member cannot be cut from a 25.4 stick,
        // so one pile of pieces across every size answers a question nobody
        // asked and buys the wrong tube.
        std::map<std::string, std::vector<double>> bySize;
        for (const tube::MemberRow& r : rows) {
          const std::string size =
              tube::Designation(r.outerDiameter, r.wallThickness);
          for (int i = 0; i < r.quantity; i++) {
            if (r.cutLength > stock + 1e-9) {
              throw std::runtime_error(
                  "'" + r.name + "' is " + Num(r.cutLength) +
                  " mm and the stock is " + Num(stock) +
                  " mm - it cannot be cut from one length, and joining it is "
                  "a decision this cannot make for you");
            }
            bySize[size].push_back(r.cutLength);
          }
        }
        csv += "\n";
        csv += "size,stick,pieces,used_mm,offcut_mm\n";
        json sticksJson = json::array();
        double offcut = 0.0;
        int total = 0;
        for (auto& [size, pieces] : bySize) {
          const std::vector<tube::Stick> sticks =
              tube::PackIntoStock(pieces, stock, kerf);
          int n = 0;
          for (const tube::Stick& s : sticks) {
            csv += Field(size) + "," + std::to_string(++n) + "," +
                   std::to_string(s.pieces.size()) + "," + Num(s.used) + "," +
                   Num(s.offcut) + "\n";
            sticksJson.push_back({{"size", size},
                                  {"stick", n},
                                  {"pieces", s.pieces},
                                  {"used_mm", s.used},
                                  {"offcut_mm", s.offcut}});
            offcut += s.offcut;
            total++;
          }
        }
        out_extra["sticks"] = std::move(sticksJson);
        out_extra["stock_length_mm"] = stock;
        out_extra["sticks_needed"] = total;
        out_extra["offcut_mm"] = offcut;
      }
    } else {
      csv = "member,bend,distance_mm,angle_deg,roll_deg,former_radius_mm\n";
      for (const tube::MemberRow& r : rows) {
        int n = 0;
        for (const tube::Bend& b : r.bends) {
          csv += Field(r.name) + "," + std::to_string(++n) + "," +
                 Num(b.straightBefore) + "," + Num(b.angleDeg) + "," +
                 Num(b.rollDeg) + "," + Num(b.radius) + "\n";
        }
      }
    }

    std::ofstream file(path, std::ios::binary);
    if (!file)
      throw std::runtime_error("cannot write '" + path + "'");
    file << csv;
    if (!file)
      throw std::runtime_error("failed while writing '" + path + "'");
    int members = 0, bends = 0;
    for (const tube::MemberRow& r : rows) {
      members += r.quantity;
      bends += static_cast<int>(r.bends.size()) * r.quantity;
    }
    out = {{"path", path},
           {"rows", static_cast<int>(rows.size())},
           {"members", members},
           {"bends", bends}};
    out.update(out_extra);
    return true;
  }

  return false;
}

std::span<const MethodDoc> TubeMethodDocs() { return TubeMethods; }

} // namespace remote
