// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// STEP export (docs/interchange/README.md#exporting-step): what another CAD
// system finds when it opens our file. Every case here writes a real file
// and reads it back through XCAF — the same reader another OCC-based system
// uses — so what is asserted is the FILE, not the model we meant to write.

#include "cad/CADKernel.h"
#include "cad/XcafDocument.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Occurrence.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h" // BuildVisualExportItems
#include "ui/BodyAppearance.h"
#include "ui/DocumentSlots.h"
#include "ui/FileDialogs.h" // ExportActiveDocument
#include "ui/StepExport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Quantity_Color.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <TDF_Label.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDF_Tool.hxx>
#include <TDataStd_Name.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

namespace {

ui::AppState MakeHeadlessState() {
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);
  return state;
}

// A fresh part tab called `name`, holding one box per entry of `boxes`
// (min corner, size), each a body named after its feature.
struct BoxSpec {
  std::string name;
  glm::vec3 at;
  glm::vec3 size;
};
uint64_t AddBoxPart(ui::AppState& state, const std::string& name,
                    const std::vector<BoxSpec>& boxes) {
  ui::AddPart(state, name);
  Document& doc = *state.document;
  for (const BoxSpec& b : boxes)
    doc.AddFeature(std::make_unique<BoxFeature>(b.name, b.at, b.size.x,
                                                b.size.y, b.size.z));
  doc.Regenerate();
  REQUIRE(doc.GetBodies().size() == boxes.size());
  return state.parts[state.activePart].id;
}

Occurrence Place(const std::string& name, uint64_t refId, OccurrenceRef kind,
                 glm::vec3 at, glm::quat turn = glm::quat(1, 0, 0, 0)) {
  Occurrence o;
  o.name = name;
  o.partId = refId;
  o.refKind = kind;
  o.position = at;
  o.orientation = turn;
  return o;
}

std::string LabelName(const TDF_Label& label) {
  Handle(TDataStd_Name) n;
  if (!label.FindAttribute(TDataStd_Name::GetID(), n))
    return {};
  TCollection_AsciiString s(n->Get());
  return s.ToCString();
}

std::string Entry(const TDF_Label& label) {
  TCollection_AsciiString e;
  TDF_Tool::Entry(label, e);
  return e.ToCString();
}

// The file as a tree: what a receiving system's structure browser shows.
struct Node {
  std::string name;       // the definition's (product's) name
  std::string definition; // its label entry: equal entries = one product
  bool isAssembly = false;
  bool hasColor = false;
  glm::vec3 color{0.0f};
  struct Child {
    std::string name; // the instance's name
    gp_Trsf local;
    std::shared_ptr<Node> node;
  };
  std::vector<Child> children;
};

std::shared_ptr<Node> ReadNode(const Handle(XCAFDoc_ColorTool) & colors,
                               const TDF_Label& def) {
  auto node = std::make_shared<Node>();
  node->name = LabelName(def);
  node->definition = Entry(def);
  Quantity_Color qc;
  if (colors->GetColor(def, XCAFDoc_ColorSurf, qc) ||
      colors->GetColor(def, XCAFDoc_ColorGen, qc)) {
    node->hasColor = true;
    node->color = glm::vec3(qc.Red(), qc.Green(), qc.Blue());
  }
  node->isAssembly = XCAFDoc_ShapeTool::IsAssembly(def);
  if (node->isAssembly) {
    TDF_LabelSequence comps;
    XCAFDoc_ShapeTool::GetComponents(def, comps);
    for (Standard_Integer i = 1; i <= comps.Length(); i++) {
      TDF_Label childDef;
      XCAFDoc_ShapeTool::GetReferredShape(comps.Value(i), childDef);
      node->children.push_back(
          {LabelName(comps.Value(i)),
           XCAFDoc_ShapeTool::GetLocation(comps.Value(i)).Transformation(),
           ReadNode(colors, childDef)});
    }
  }
  return node;
}

// The single root of a file we wrote.
std::shared_ptr<Node> ReadStepTree(const std::string& path) {
  std::scoped_lock exchange(DataExchangeMutex());
  XcafDocument doc;
  STEPCAFControl_Reader reader;
  reader.SetNameMode(Standard_True);
  reader.SetColorMode(Standard_True);
  REQUIRE(reader.ReadFile(path.c_str()) == IFSelect_RetDone);
  REQUIRE(reader.Transfer(doc));
  Handle(XCAFDoc_ShapeTool) shapes =
      XCAFDoc_DocumentTool::ShapeTool(doc->Main());
  TDF_LabelSequence roots;
  shapes->GetFreeShapes(roots);
  REQUIRE(roots.Length() == 1);
  return ReadNode(XCAFDoc_DocumentTool::ColorTool(doc->Main()), roots.Value(1));
}

const Node::Child& ChildNamed(const Node& n, const std::string& name) {
  for (const Node::Child& c : n.children)
    if (c.name == name)
      return c;
  FAIL("no child named " << name << " in " << n.name);
  return n.children.front(); // unreachable
}

std::string Slurp(const std::string& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void CheckColor(const Node& n, glm::vec3 want) {
  REQUIRE(n.hasColor);
  // The file stores colour as text; within 1/255 is the colour on screen.
  CHECK_THAT(n.color.r, WithinAbs(want.r, 1.0 / 255));
  CHECK_THAT(n.color.g, WithinAbs(want.g, 1.0 / 255));
  CHECK_THAT(n.color.b, WithinAbs(want.b, 1.0 / 255));
}

// The rig the cases share:
//   Bolt   — one body, painted red by an appearance override
//   Plate  — two bodies, one steel, one unpainted
//   Bracket (assembly) — B1, B2 (Bolts), P (Plate)
//   Rig    (assembly, active) — Left, Right (Brackets; Right turned 90° about
//          Z), Loose (a Bolt), Ghost (a Bolt, hidden)
struct Rig {
  ui::AppState state = MakeHeadlessState();
  uint64_t bolt = 0, plate = 0, bracket = 0;
  glm::vec3 red{0.9f, 0.1f, 0.1f};

  Rig() {
    bolt = AddBoxPart(state, "Bolt", {{"Shank", {0, 0, 0}, {2, 2, 10}}});
    Appearance paint;
    paint.color = red;
    state.document->GetFeature(0)->SetAppearance(paint);
    state.document->Regenerate();

    plate = AddBoxPart(
        state, "Plate",
        {{"Top", {0, 0, 0}, {20, 10, 1}}, {"Bottom", {0, 0, -5}, {20, 10, 1}}});
    state.document->GetFeature(0)->SetMaterial("steel");
    state.document->Regenerate();

    ui::AddAssembly(state, "Bracket");
    bracket = state.parts[state.activePart].id;
    state.assembly->AddOccurrence(
        Place("B1", bolt, OccurrenceRef::Part, {0, 0, 0}));
    state.assembly->AddOccurrence(
        Place("B2", bolt, OccurrenceRef::Part, {5, 0, 0}));
    state.assembly->AddOccurrence(
        Place("P", plate, OccurrenceRef::Part, {0, 0, 20}));

    ui::AddAssembly(state, "Rig");
    const glm::quat quarter =
        glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1));
    state.assembly->AddOccurrence(
        Place("Left", bracket, OccurrenceRef::Assembly, {100, 0, 0}));
    state.assembly->AddOccurrence(Place(
        "Right", bracket, OccurrenceRef::Assembly, {-100, 0, 0}, quarter));
    state.assembly->AddOccurrence(
        Place("Loose", bolt, OccurrenceRef::Part, {0, 50, 0}));
    Occurrence ghost = Place("Ghost", bolt, OccurrenceRef::Part, {0, -50, 0});
    ghost.visible = false;
    state.assembly->AddOccurrence(ghost);
  }
};

std::string TempPath(const std::string& name) {
  return (std::filesystem::temp_directory_path() / name).string();
}

} // namespace

TEST_CASE("an assembly exports as its tree: each part once, placed by "
          "reference",
          "[step][export][assembly]") {
  Rig rig;
  const std::string path = TempPath("designer_step_rig.step");
  const auto counts = ui::ExportStep(rig.state, nullptr, -1, path);
  REQUIRE(counts);
  // Two parts; Left and Right place three parts each, plus Loose; the hidden
  // Ghost is not written. Bodies: (1 + 1 + 2) per bracket, twice, plus 1.
  CHECK(counts->products == 2);
  CHECK(counts->instances == 7);
  CHECK(counts->bodies == 9);

  const std::shared_ptr<Node> root = ReadStepTree(path);
  CHECK(root->name == "Rig");
  REQUIRE(root->isAssembly);
  REQUIRE(root->children.size() == 3);
  const Node::Child& left = ChildNamed(*root, "Left");
  const Node::Child& right = ChildNamed(*root, "Right");
  ChildNamed(*root, "Loose");

  // ONE Bracket definition, referenced twice — not two copies.
  CHECK(left.node->definition == right.node->definition);
  CHECK(left.node->name == "Bracket");
  REQUIRE(left.node->children.size() == 3);
  const Node::Child& b1 = ChildNamed(*left.node, "B1");
  const Node::Child& b2 = ChildNamed(*left.node, "B2");
  const Node::Child& p = ChildNamed(*left.node, "P");
  // ...and ONE Bolt, shared by B1, B2 and Loose.
  CHECK(b1.node->definition == b2.node->definition);
  CHECK(ChildNamed(*root, "Loose").node->definition == b1.node->definition);
  CHECK(b1.node->name == "Bolt");

  // Placements are local to their parent, and compose to the solved world.
  CHECK_THAT(left.local.TranslationPart().X(), WithinAbs(100.0, 1e-6));
  CHECK_THAT(b2.local.TranslationPart().X(), WithinAbs(5.0, 1e-6));
  const gp_Pnt b2InRight = gp_Pnt(0, 0, 0).Transformed(right.local * b2.local);
  CHECK_THAT(b2InRight.X(), WithinAbs(-100.0, 1e-4)); // (5,0,0) turned 90°
  CHECK_THAT(b2InRight.Y(), WithinAbs(5.0, 1e-4));

  // A multi-body part is a product of named body products, each coloured
  // as the viewport draws it.
  CHECK(p.node->name == "Plate");
  REQUIRE(p.node->isAssembly);
  REQUIRE(p.node->children.size() == 2);
  const Node::Child& top = ChildNamed(*p.node, "Top");
  ChildNamed(*p.node, "Bottom");

  // Colours are what the screen shows: resolved by the one resolver the
  // renderers use, the palette seeded from the part as the assembly tab
  // seeds it.
  CheckColor(*b1.node, rig.red);
  size_t palette = static_cast<size_t>(rig.plate);
  const Document* plateDoc = ui::PartDocById(rig.state, rig.plate);
  REQUIRE(plateDoc != nullptr);
  const glm::vec3 steel =
      ui::ResolveBodyLook(plateDoc->GetBodies()[0], palette).Color();
  CheckColor(*top.node, steel);

  // Millimetres, said explicitly.
  CHECK_THAT(Slurp(path), ContainsSubstring("SI_UNIT(.MILLI.,.METRE.)"));

  // Our own leaf reader: one shape per placed body, at its placement.
  std::vector<CADShape> leaves;
  REQUIRE(CADKernel::Instance().LoadSTEP(path, leaves));
  CHECK(leaves.size() == 9);
  double volume = 0;
  for (const CADShape& s : leaves)
    volume += s.Volume();
  // Per bracket two bolts (40 each) and two plate bodies (200 each); twice,
  // plus Loose.
  CHECK_THAT(volume, WithinAbs(2 * (2 * 40.0 + 2 * 200.0) + 40.0, 1e-3));
  std::filesystem::remove(path);
}

TEST_CASE("a part exports as one named product of its bodies",
          "[step][export]") {
  ui::AppState state = MakeHeadlessState();
  AddBoxPart(
      state, "Plate",
      {{"Top", {0, 0, 0}, {20, 10, 1}}, {"Bottom", {0, 0, -5}, {20, 10, 1}}});
  const std::string path = TempPath("designer_step_plate.step");

  SECTION("every body") {
    const auto counts = ui::ExportStep(state, state.document.get(), -1, path);
    REQUIRE(counts);
    CHECK(counts->bodies == 2);
    const std::shared_ptr<Node> root = ReadStepTree(path);
    CHECK(root->name == "Plate");
    REQUIRE(root->children.size() == 2);
    CHECK(ChildNamed(*root, "Top").node->name == "Top");
    ChildNamed(*root, "Bottom");
    std::vector<CADShape> leaves;
    REQUIRE(CADKernel::Instance().LoadSTEP(path, leaves));
    CHECK(leaves.size() == 2); // a flat reader saw ONE compound here
  }
  SECTION("one body, by index") {
    const auto counts = ui::ExportStep(state, state.document.get(), 1, path);
    REQUIRE(counts);
    CHECK(counts->bodies == 1);
    const std::shared_ptr<Node> root = ReadStepTree(path);
    CHECK(root->name == "Plate");
    CHECK_FALSE(root->isAssembly);
  }
  SECTION("an index past the bodies is refused") {
    const auto counts = ui::ExportStep(state, state.document.get(), 2, path);
    REQUIRE_FALSE(counts);
    CHECK_THAT(counts.error(), ContainsSubstring("out of range"));
  }
  std::filesystem::remove(path);
}

TEST_CASE("Export on an assembly tab writes STEP and refuses STL by name",
          "[step][export][assembly]") {
  Rig rig;
  const std::string step = TempPath("designer_step_export_active.step");
  std::string error;
  const bool exported = ui::ExportActiveDocument(rig.state, step, &error);
  INFO("export error: " << error);
  REQUIRE(exported);
  CHECK(ReadStepTree(step)->name == "Rig");
  std::filesystem::remove(step);

  CHECK_FALSE(ui::ExportActiveDocument(
      rig.state, TempPath("designer_step_export_active.stl"), &error));
  CHECK_THAT(error, ContainsSubstring("part tab"));
  CHECK_FALSE(ui::ExportActiveDocument(
      rig.state, TempPath("designer_step_export_active.iges"), &error));
  CHECK_THAT(error, ContainsSubstring(".step"));
}

TEST_CASE("visual export colours every occurrence of a part as the screen "
          "does",
          "[step][export][gltf]") {
  Rig rig;
  const std::vector<MeshExport::Item> items =
      ui::BuildVisualExportItems(rig.state);
  std::vector<glm::vec3> bolts, bottoms;
  for (const MeshExport::Item& item : items) {
    if (item.name.ends_with("B1") || item.name.ends_with("B2") ||
        item.name == "Loose")
      bolts.push_back(item.baseColor);
    if (item.name.ends_with("-Bottom"))
      bottoms.push_back(item.baseColor);
  }
  // The appearance override survives (glTF used to drop it)...
  REQUIRE(bolts.size() == 5);
  for (const glm::vec3& c : bolts)
    CHECK(c == rig.red);
  // ...and an unpainted body is one colour in both brackets.
  REQUIRE(bottoms.size() == 2);
  CHECK(bottoms[0] == bottoms[1]);
}

TEST_CASE("a body that is a compound of one solid keeps its name and colour",
          "[step][export]") {
  // Booleans often hand back a compound around a single solid. Pinned
  // because a LOCATED shape does arrive a level down, unnamed and uncoloured
  // (XCAF wraps it in a product of one placed component), and a compound is
  // the next thing a writer might treat the same way.
  auto& k = CADKernel::Instance();
  const CADShape box =
      k.CreateBox(glm::vec3(0.0f), glm::vec3(1.0f, 2.0f, 3.0f));
  CADKernel::StepExportModel model;
  model.parts.push_back(
      {"Lug",
       {{CADShape::CompoundOf({box}), "Lug", glm::vec3(0.2f, 0.4f, 0.6f)}}});
  const std::string path = TempPath("designer_step_compound.step");
  REQUIRE(k.SaveSTEPModel(path, model));
  const std::shared_ptr<Node> root = ReadStepTree(path);
  CHECK(root->name == "Lug");
  CHECK_FALSE(root->isAssembly);
  CheckColor(*root, glm::vec3(0.2f, 0.4f, 0.6f));
  std::filesystem::remove(path);
}

namespace {
// The element name and index of the face of `body` centred at `at`.
std::pair<std::string, int> FaceCentredAt(const Body& body, glm::dvec3 at) {
  int i = 0;
  for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), ++i) {
    GProp_GProps props;
    BRepGProp::SurfaceProperties(TopoDS::Face(ex.Current()), props);
    const gp_Pnt c = props.CentreOfMass();
    if (glm::length(glm::dvec3(c.X(), c.Y(), c.Z()) - at) < 1e-6) {
      const std::string* name = body.elementMap.NameOf(i);
      REQUIRE(name != nullptr);
      return {*name, i};
    }
  }
  FAIL("no face centred there");
  return {};
}
} // namespace

TEST_CASE("a face painted apart exports as a colour on that face",
          "[step][export][gltf][appearance]") {
  ui::AppState state = MakeHeadlessState();
  AddBoxPart(state, "Wall", {{"Wall", {0, 0, 0}, {4, 4, 2}}});
  Document& doc = *state.document;
  const auto [top, topIndex] = FaceCentredAt(doc.GetBodies()[0], {2, 2, 2});
  const glm::vec3 red(0.8f, 0.02f, 0.02f);
  Appearance paint;
  paint.color = red;
  doc.GetFeature(0)->SetFaceAppearance(top, paint);
  doc.Regenerate();

  SECTION("STEP: an XCAF colour on the face's sub-shape") {
    const auto model = ui::BuildPartStepModel(doc, "Wall", -1);
    REQUIRE(model.has_value());
    REQUIRE(model->parts.at(0).bodies.at(0).faceColors.size() == 1);
    const std::string path = TempPath("designer_step_face_colour.step");
    REQUIRE(CADKernel::Instance().SaveSTEPModel(path, *model));

    // Read back the way another OCC-based system would: the face that
    // carries a colour of its own is the top, and it is red. The body keeps
    // its own colour, which is not.
    std::scoped_lock exchange(DataExchangeMutex());
    XcafDocument xdoc;
    STEPCAFControl_Reader reader;
    reader.SetColorMode(Standard_True);
    REQUIRE(reader.ReadFile(path.c_str()) == IFSelect_RetDone);
    REQUIRE(reader.Transfer(xdoc));
    Handle(XCAFDoc_ShapeTool) shapes =
        XCAFDoc_DocumentTool::ShapeTool(xdoc->Main());
    Handle(XCAFDoc_ColorTool) colors =
        XCAFDoc_DocumentTool::ColorTool(xdoc->Main());
    TDF_LabelSequence roots;
    shapes->GetFreeShapes(roots);
    REQUIRE(roots.Length() == 1);
    Quantity_Color bodyColour;
    REQUIRE(colors->GetColor(roots.Value(1), XCAFDoc_ColorSurf, bodyColour));
    CHECK(std::abs(bodyColour.Green() - red.g) > 0.1); // the palette's
    int coloured = 0;
    for (TopExp_Explorer ex(shapes->GetShape(roots.Value(1)), TopAbs_FACE);
         ex.More(); ex.Next()) {
      Quantity_Color qc;
      if (!colors->GetColor(ex.Current(), XCAFDoc_ColorSurf, qc))
        continue;
      coloured++;
      GProp_GProps props;
      BRepGProp::SurfaceProperties(TopoDS::Face(ex.Current()), props);
      CHECK_THAT(props.CentreOfMass().Z(), WithinAbs(2.0, 1e-6));
      CHECK_THAT(qc.Red(), WithinAbs(red.r, 1.0 / 255));
      CHECK_THAT(qc.Green(), WithinAbs(red.g, 1.0 / 255));
      CHECK_THAT(qc.Blue(), WithinAbs(red.b, 1.0 / 255));
    }
    CHECK(coloured == 1);
    std::filesystem::remove(path);
  }

  SECTION("glTF: the face is its own primitive, in its own material") {
    const std::vector<MeshExport::Item> items =
        ui::BuildVisualExportItems(state);
    REQUIRE(items.size() == 1);
    REQUIRE(items[0].faces.size() == 1);
    CHECK(items[0].faces[0].face == topIndex);
    const std::string path = TempPath("designer_face_colour.gltf");
    std::string error;
    REQUIRE(MeshExport::SaveVisual(path, items, 0.1, &error));
    const nlohmann::json gltf = nlohmann::json::parse(Slurp(path));
    // Which material is red, and how many primitives draw with it.
    std::set<size_t> redMaterials;
    const auto& mats = gltf.at("materials");
    for (size_t i = 0; i < mats.size(); i++) {
      const auto& f = mats[i].at("pbrMetallicRoughness").at("baseColorFactor");
      if (std::abs(f[0].get<double>() - red.r) < 1e-3 &&
          std::abs(f[1].get<double>() - red.g) < 1e-3)
        redMaterials.insert(i);
    }
    REQUIRE(redMaterials.size() == 1);
    size_t redPrims = 0, otherPrims = 0;
    for (const auto& mesh : gltf.at("meshes"))
      for (const auto& prim : mesh.at("primitives"))
        (redMaterials.count(prim.value("material", size_t(-1))) ? redPrims
                                                                : otherPrims)++;
    CHECK(redPrims == 1);
    CHECK(otherPrims >= 1);
    std::filesystem::remove(path);
    std::filesystem::remove(TempPath("designer_face_colour.bin"));
  }
}
