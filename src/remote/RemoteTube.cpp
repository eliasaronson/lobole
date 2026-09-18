#include "remote/RemoteTube.h"

#include "core/Document.h"
#include "core/features/CurveFeature.h"
#include "core/tube/Check.h"
#include "core/tube/CutList.h"
#include "core/tube/Rules.h"
#include "ui/AppState.h"

#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using nlohmann::json;

namespace remote {

// describe: one row per `m ==` branch below — extend together.
static constexpr MethodDoc TubeMethods[] = {
    {"set_tube_rules",
     "Declare the stock this part is BENT from: material + size (OD x wall in "
     "mm, '25.4x2.4'), which fix the diameter and the wall. Everything else "
     "is derived from those unless given: the minimum straight between bends "
     "= 2 x OD (what a clamp die needs), the tail past the last bend = 1.5 x "
     "OD (what the bender grips), the thinning limit = 25% (what draw bending "
     "is accepted at). `former_radii_mm` is the die set — the CENTRELINE radii "
     "this bender owns, in mm; omit it and any radius is allowed, which is "
     "the honest reading of 'no bender declared'. `max_thinning` is a "
     "FRACTION of the wall, for a rulebook that demands better than the "
     "default. `clear: true` makes it not a tube part. Unlike the sheet's "
     "rules this drives no geometry, so setting it rebuilds nothing.",
     "clear, former_radii_mm, material, max_thinning, min_straight_mm, size, "
     "tail_length_mm",
     remote::NeedsPart},
    {"get_tube_rules",
     "This part's tube rules, with every derived value RESOLVED (so a caller "
     "sees the minimum straight it will actually be checked against, not a "
     "0 meaning 'derived'), plus the minimum former this stock takes and "
     "where that number came from. With no rules set it answers null and "
     "lists what IS stocked, so a picker built from it cannot offer a size "
     "set_tube_rules would refuse.",
     "part", remote::Deferrable},
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
     "Manufacturability of every bent path in the part against its tube "
     "rules: a former tighter than the wall can take (the limit is set by "
     "D/t, not D), a radius no declared die matches, two bends too close "
     "together to clamp between, nothing left to grip past the last one, an "
     "outer wall pulled thinner than `max_thinning`. Each violation names the "
     "member, the bend and the required-versus-actual number. `not_checked` "
     "says what this build cannot answer — ovality and springback belong to "
     "the tooling and the machine, which it has no model of — so an empty "
     "`violations` cannot be read as 'all clear' on those.",
     "part", remote::Deferrable},
};

namespace {

// The bends of every BentPath curve in the document, as the checker's plain
// records. A curve that is not a bent path is not a bendable member: a spline
// has no former to check.
std::vector<tube::Member> MembersOf(const Document& doc) {
  std::vector<tube::Member> members;
  for (size_t i = 0; i < doc.FeatureCount(); i++) {
    const auto* curve = dynamic_cast<const CurveFeature*>(doc.GetFeature(i));
    if (curve == nullptr || curve->kind != Curve3DKind::BentPath)
      continue;
    tube::Member m;
    m.name = curve->GetName();
    m.length = curve->Length();
    for (const CurveFeature::Bend& b : curve->Bends())
      m.bends.push_back({b.angleDeg, b.radius, b.straightBefore, b.rollDeg});
    members.push_back(std::move(m));
  }
  return members;
}

// A CSV field, quoted only where it has to be — the same rule export_bom's
// writer uses, so two files from one project do not disagree about quoting.
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
            {"size", r.size},
            {"outer_diameter_mm", r.outerDiameter},
            {"wall_mm", r.wallThickness},
            {"min_straight_mm", r.EffectiveMinStraight()},
            {"tail_length_mm", r.EffectiveTailLength()},
            {"max_thinning", r.EffectiveMaxThinning()},
            {"min_former_radius_mm",
             tube::MinBendRadius(r.material, r.outerDiameter, r.wallThickness)},
            {"min_former_source",
             tube::MinBendRadiusSource(r.material, r.outerDiameter,
                                       r.wallThickness)}};
  j["former_radii_mm"] = r.formerRadii; // empty = any radius
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
    if (p.contains("size"))
      r.size = p.at("size").get<std::string>();
    if (r.material.empty() || r.size.empty()) {
      throw std::runtime_error(
          "set_tube_rules needs a material and a size - a bend cannot guess "
          "the stock it is pulling. get_tube_rules lists what is stocked.");
    }
    // The size IS the diameter and the wall: resolving here rather than
    // storing numbers the caller supplies is what stops a document claiming
    // 25.4x2.4 at a 1.6 mm wall.
    std::string why;
    const std::optional<tube::Size> s =
        tube::LookupSize(r.material, r.size, &why);
    if (!s)
      throw std::runtime_error(why);
    r.outerDiameter = s->outerDiameter;
    r.wallThickness = s->wallThickness;
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
    // rpc-params: part
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

  if (m == "check_tube") {
    // rpc-params: part
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

  if (m == "export_cut_list" || m == "export_bend_schedule") {
    // rpc-params: part
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
    const std::string size =
        subject.Tube() ? subject.Tube()->size : std::string{};

    std::string csv;
    json out_extra = json::object();
    if (m == "export_cut_list") {
      csv = "member,quantity,material,size,cut_length_mm,bends,bodies\n";
      for (const tube::MemberRow& r : rows) {
        csv += Field(r.name) + "," + std::to_string(r.quantity) + "," +
               Field(material) + "," + Field(size) + "," + Num(r.cutLength) +
               "," + std::to_string(r.bends.size()) + "," +
               Field(Join(r.bodies)) + "\n";
      }
      if (p.contains("stock_length_mm")) {
        const double stock = p.at("stock_length_mm").get<double>();
        const double kerf = p.value("kerf_mm", 0.0);
        std::vector<double> pieces;
        for (const tube::MemberRow& r : rows) {
          for (int i = 0; i < r.quantity; i++) {
            if (r.cutLength > stock + 1e-9) {
              throw std::runtime_error(
                  "'" + r.name + "' is " + Num(r.cutLength) +
                  " mm and the stock is " + Num(stock) +
                  " mm - it cannot be cut from one length, and joining it is "
                  "a decision this cannot make for you");
            }
            pieces.push_back(r.cutLength);
          }
        }
        const std::vector<tube::Stick> sticks =
            tube::PackIntoStock(pieces, stock, kerf);
        csv += "\n";
        csv += "stick,pieces,used_mm,offcut_mm\n";
        int n = 0;
        json sticksJson = json::array();
        double offcut = 0.0;
        for (const tube::Stick& s : sticks) {
          csv += std::to_string(++n) + "," + std::to_string(s.pieces.size()) +
                 "," + Num(s.used) + "," + Num(s.offcut) + "\n";
          sticksJson.push_back({{"pieces", s.pieces},
                                {"used_mm", s.used},
                                {"offcut_mm", s.offcut}});
          offcut += s.offcut;
        }
        out_extra["sticks"] = std::move(sticksJson);
        out_extra["stock_length_mm"] = stock;
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
