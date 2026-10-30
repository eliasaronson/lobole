// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/Feature.h"

#include "cad/MeshRebuild.h"
#include "core/ModelSpace.h"
#include "core/io/ImportFormats.h"
#include "core/io/MeshImportCost.h"
#include "core/mesh/MeshReaders.h"
#include <glm/gtc/matrix_transform.hpp>
#include <mutex>

#include <algorithm>

#include "cad/CADMaterials.h"
#include "core/AxisRef.h"
#include "core/Configuration.h"

#include <filesystem>
#include <fstream>
#include <iterator>

#include "common/Logger.h"
#include "core/ElementHistory.h"

void VisitRefs(const AxisRef& ref, const Feature::RefVisitor& visit) {
  // Only the id this KIND resolves. Moving an axis from a sketch line back to
  // X leaves sketchId set, and reporting it then would claim a dependency the
  // feature does not have — the one place over-reporting is not harmless,
  // since it would roll a sketch edit back past features that do not use it.
  // No default: a new AxisRef kind has to be answered here.
  switch (ref.kind) {
  case AxisRef::Kind::SketchLine:
    visit(ref.sketchId);
    break;
  case AxisRef::Kind::DatumAxis:
    visit(ref.datumAxisId);
    break;
  case AxisRef::Kind::X:
  case AxisRef::Kind::Y:
  case AxisRef::Kind::Z:
  case AxisRef::Kind::Custom: // written out in full: it depends on nothing
    break;
  }
}

void VisitRefs(const ElementRef& ref, const Feature::RefVisitor& visit) {
  visit(ref.featureId); // the feature whose body owns the named element
}

void VisitRefs(const std::vector<ElementRef>& refs,
               const Feature::RefVisitor& visit) {
  for (const ElementRef& r : refs)
    VisitRefs(r, visit);
}

const Feature* RegenContext::FindById(uint64_t id) const {
  if (id == 0)
    return nullptr;
  // A template's own features first: they are what its references name,
  // and none of them is ever suppressed on its own.
  if (inner != nullptr)
    for (size_t i = 0; i < innerIndex && i < inner->size(); i++)
      if ((*inner)[i]->GetId() == id)
        return (*inner)[i].get();
  for (size_t i = 0; i < currentIndex && i < features.size(); i++) {
    // A suppressed feature is invisible to references — its consumers fail
    // with their normal missing-reference errors, same rule as deletion (R1).
    if (features[i]->GetId() != id)
      continue;
    if (configuration != nullptr && configuration->Unsuppresses(id))
      return features[i].get();
    const bool suppressed =
        features[i]->IsSuppressed() ||
        (configuration != nullptr && configuration->Suppresses(id));
    return suppressed ? nullptr : features[i].get();
  }
  return nullptr;
}

BoxFeature::BoxFeature(const std::string& name, const glm::dvec3& position,
                       double width, double height, double depth)
    : position(position), width(width), height(height), depth(depth) {
  m_name = name;
}

namespace {
// A stored primitive outside the model space (core/ModelSpace.h) is refused
// by name, as a Hole or a Sketch is: a cylinder at 2^53 mm reached a
// thread's geometry and took seconds, where it should have taken nothing.
bool InSpace(const glm::dvec3& v) {
  return InModelSpace(v.x) && InModelSpace(v.y) && InModelSpace(v.z);
}
std::string OutsideSpace(const char* what) {
  return std::string(what) + " is outside the " +
         std::to_string(static_cast<long>(kModelSpaceMm)) +
         " mm model space (core/ModelSpace.h)";
}
} // namespace

bool BoxFeature::Execute(RegenContext& ctx) {
  if (!InSpace(position) || !InModelSpace(width) || !InModelSpace(height) ||
      !InModelSpace(depth)) {
    SetError(OutsideSpace("its position or a size"));
    return false;
  }
  auto& bodies = ctx.bodies;
  CADShape shape = CADKernel::Instance().CreateBox(width, height, depth);
  if (!shape.IsValid()) {
    LOG_ERROR("BoxFeature '{}': invalid dimensions {}x{}x{}", m_name, width,
              height, depth);
    return false;
  }
  shape.Translate(position);
  ElementMap map;
  topo::NamePrimitiveElements(shape, std::to_string(GetId()), map);
  bodies.push_back({m_name, std::move(shape), m_material, GetId()});
  bodies.back().elementMap = std::move(map);
  return true;
}

SphereFeature::SphereFeature(const std::string& name, const glm::dvec3& center,
                             double radius)
    : center(center), radius(radius) {
  m_name = name;
}

bool SphereFeature::Execute(RegenContext& ctx) {
  if (!InSpace(center) || !InModelSpace(radius)) {
    SetError(OutsideSpace("its centre or its radius"));
    return false;
  }
  auto& bodies = ctx.bodies;
  CADShape shape = CADKernel::Instance().CreateSphere(center, radius);
  if (!shape.IsValid()) {
    LOG_ERROR("SphereFeature '{}': invalid radius {}", m_name, radius);
    return false;
  }
  ElementMap map;
  topo::NamePrimitiveElements(shape, std::to_string(GetId()), map);
  bodies.push_back({m_name, std::move(shape), m_material, GetId()});
  bodies.back().elementMap = std::move(map);
  return true;
}

CylinderFeature::CylinderFeature(const std::string& name,
                                 const glm::dvec3& center,
                                 const glm::dvec3& direction, double radius,
                                 double height)
    : center(center), direction(direction), radius(radius), height(height) {
  m_name = name;
}

bool CylinderFeature::Execute(RegenContext& ctx) {
  if (!InSpace(center) || !InSpace(direction) || !InModelSpace(radius) ||
      !InModelSpace(height)) {
    SetError(OutsideSpace("its centre, axis, radius or height"));
    return false;
  }
  auto& bodies = ctx.bodies;
  CADShape shape =
      CADKernel::Instance().CreateCylinder(center, direction, radius, height);
  if (!shape.IsValid()) {
    LOG_ERROR("CylinderFeature '{}': invalid radius {} / height {}", m_name,
              radius, height);
    return false;
  }
  ElementMap map;
  topo::NamePrimitiveElements(shape, std::to_string(GetId()), map);
  bodies.push_back({m_name, std::move(shape), m_material, GetId()});
  bodies.back().elementMap = std::move(map);
  return true;
}

ImportFeature::ImportFeature(const std::string& name, const std::string& path)
    : path(path) {
  m_name = name;
}

bool ImportFeature::IsMesh(const std::string& path) {
  return interchange::IsMeshFormat(interchange::FormatOf(path));
}

std::string MeshBodyRefusal(const Body& body, const std::string& what) {
  if (!body.IsMesh())
    return "";
  const std::string head = what + " needs a B-Rep, and body '" + body.name +
                           "' is a mesh (" +
                           std::to_string(body.mesh->triangles.size()) +
                           " triangles, no faces or edges)";
  // Made one by a feature of this part: the B-Rep work belongs before it
  // (docs/modeling/README.md#after-a-cradle).
  if (!body.meshSince.empty())
    return head + " since " + body.meshSince +
           ": move this before it -- do the B-Rep work (fillets, chamfers, "
           "bosses) first, and the cradle and mesh-tool booleans last";
  return head + ": measure, section and export it as it is, or re-import a "
                "mechanical part with representation `recognize` to rebuild "
                "its faces";
}

bool BodyBounds(const Body& body, glm::dvec3& lo, glm::dvec3& hi, bool tight) {
  lo = hi = glm::dvec3(0.0);
  if (body.IsMesh()) {
    if (body.mesh->positions.empty())
      return false;
    body.mesh->Bounds(lo, hi);
    return true;
  }
  if (!body.shape.IsValid())
    return false;
  body.shape.GetBoundingBox(lo, hi, tight);
  return true;
}

Body PlacedBody(const Body& body, const glm::dmat4& xf) {
  Body out = body;
  if (body.IsMesh())
    out.mesh = std::make_shared<const mesh::TriMesh>(
        mesh::Transformed(*body.mesh, xf));
  else
    out.shape = body.shape.Transformed(xf);
  return out;
}

namespace {

bool ReadTriMesh(const std::string& path, mesh::TriMesh& m, std::string& error,
                 const mesh::Cancelled& cancelled, mesh::ReadExtras& extras) {
  extras = {};
  switch (interchange::FormatOf(path)) {
  case interchange::ImportFormat::Obj:
    return mesh::ReadObj(path, m, error, cancelled);
  case interchange::ImportFormat::Ply:
    return mesh::ReadPly(path, m, error, cancelled, &extras);
  case interchange::ImportFormat::Gltf:
    return mesh::ReadGltf(path, m, error, cancelled, &extras);
  default:
    return mesh::ReadStl(path, m, error, cancelled);
  }
}

// What a PLY or glTF said besides its triangles, onto the model: its notes,
// its header, and a uniform colour onto every product.
void ApplyExtras(CADKernel::ImportedModel& model,
                 const mesh::ReadExtras& extras) {
  for (const std::string& note : extras.notes)
    model.notes.push_back(note);
  for (const auto& field : extras.header)
    model.origin.fields.push_back(field);
  if (extras.color)
    for (auto& product : model.products) {
      product.hasColor = true;
      product.color = *extras.color;
    }
}

// A mesh kept as a mesh body: scaled to mm, one product, no shape.
void PushMeshProduct(CADKernel::ImportedModel& model, mesh::TriMesh m,
                     double scale, const std::string& format) {
  if (scale != 1.0)
    m = mesh::Transformed(m, glm::dmat4(glm::dmat3(scale)));
  CADKernel::ImportedShape product;
  product.mesh = std::make_shared<const mesh::TriMesh>(std::move(m));
  model.products.push_back(std::move(product));
  model.instances.push_back({0, glm::dmat4(1.0), ""});
  model.origin.format = format;
}

std::string Fixed1(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.1f", v);
  return buf;
}

} // namespace

bool ImportFeature::TakesUnits(const std::string& path) {
  return interchange::TakesUnits(interchange::FormatOf(path));
}

size_t ImportFeature::ContentHash() const {
  std::error_code ec;
  const std::filesystem::path fp(path);
  const uintmax_t size = std::filesystem::file_size(fp, ec);
  if (ec)
    return 0;
  const auto mtime = std::filesystem::last_write_time(fp, ec);
  if (ec)
    return 0;
  const long long stamp = mtime.time_since_epoch().count();
  if (m_hashValue != 0 && m_hashPath == path && m_hashSize == size &&
      m_hashMtime == stamp)
    return m_hashValue;
  std::ifstream in(fp, std::ios::binary);
  if (!in)
    return 0;
  std::string bytes((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
  size_t h = std::hash<std::string>{}(bytes);
  if (h == 0)
    h = 1; // 0 means "unreadable"
  m_hashPath = path;
  m_hashSize = size;
  m_hashMtime = stamp;
  m_hashValue = h;
  return h;
}

bool ImportFeature::ReadModel(const std::string& path,
                              const ReadOptions& options,
                              CADKernel::ImportedModel& model,
                              std::string& error,
                              CADKernel::ImportProgress* progress) {
  model = {};
  const units::Length unit = options.unit;
  using interchange::ImportFormat;
  const ImportFormat fileFormat = interchange::FormatOf(path);
  const auto hasExt = [&](const char* e) {
    return interchange::LowerExtension(path) == e;
  };
  const bool isIges = fileFormat == ImportFormat::Iges;
  // Refused, naming what is read: a file with another extension was read as
  // STEP, and failed as one, which named the wrong problem.
  if (fileFormat == ImportFormat::None) {
    error = "'" + path + "' is not a file the import reads (accepted: " +
            interchange::kImportExtensions + ")";
    return false;
  }
  // STEP and IGES say what unit they are in, and the reader converts, as
  // the glTF reader converts its metres; a unit given on top would be
  // applied twice or contradict the file.
  if (!TakesUnits(path) && unit != units::Length::Millimetre) {
    error = "'" + path + "' is " + interchange::DeclaredUnit(fileFormat) +
            " — `units` is for STL, OBJ, PLY and BREP, which carry none";
    return false;
  }
  // A representation is what a mesh becomes; anything else is refused
  // rather than read as if it had not been asked.
  if (options.representation != mesh::Representation::Facets && !IsMesh(path)) {
    error = "'" + path +
            "' is not a mesh: `representation` is for STL, OBJ, PLY and glTF";
    return false;
  }
  if (options.minSides < mesh::kMinSidesLowest ||
      options.minSides > mesh::kMinSidesHighest) {
    error = "min_sides " + std::to_string(options.minSides) + " is outside " +
            std::to_string(mesh::kMinSidesLowest) + ".." +
            std::to_string(mesh::kMinSidesHighest);
    return false;
  }
  const double scale = units::MmPerLength(unit);
  const auto cancelled = [&] {
    return progress != nullptr && progress->Cancelled();
  };
  using Rep = mesh::Representation;
  const Rep rep = options.representation;
  const std::string format = interchange::FormatName(fileFormat);
  // PLY and glTF are read only into a TriMesh: every representation starts
  // there, facets included (docs/interchange/README.md#ply-and-gltf).
  const bool scanFormat =
      fileFormat == ImportFormat::Ply || fileFormat == ImportFormat::Gltf;
  if (IsMesh(path)) {
    model.representation = mesh::ToString(rep);
    model.representationWhy = "the import names it";
  }
  // A mesh body, or `auto` deciding between one and a recognised solid
  // (docs/interchange/README.md#mesh-bodies). Counted first: a mesh past
  // what a mesh body can hold is refused before it is read, and `auto`
  // past the recognition limit never segments.
  if (IsMesh(path) &&
      (scanFormat || rep == Rep::Mesh || rep == Rep::Auto ||
       (rep == Rep::Recognize && fileFormat == ImportFormat::Obj))) {
    const std::optional<uint64_t> counted =
        interchange::CountMeshTriangles(path);
    if (counted && *counted > interchange::MeshBodyTriangleLimit() &&
        (rep == Rep::Mesh || rep == Rep::Auto)) {
      error = interchange::MeshBodyTooLarge(path, *counted);
      return false;
    }
    if (progress != nullptr)
      progress->OnPhase(CADKernel::ImportProgress::Phase::ReadingFile, -1.0);
    const uint64_t limit = interchange::RecognitionTriangleLimit();
    if (rep == Rep::Auto && counted && *counted >= limit) {
      model.representation = "mesh";
      model.representationWhy = interchange::DescribeTriangles(*counted) +
                                ", past the recognition limit of " +
                                interchange::DescribeTriangles(limit);
    }
    mesh::TriMesh m;
    std::string unread;
    mesh::ReadExtras extras;
    if (!ReadTriMesh(path, m, unread, cancelled, extras)) {
      error = cancelled() ? "cancelled"
                          : "failed to read '" + path + "' as " + format +
                                ": " + unread;
      return false;
    }
    if (m.triangles.size() > interchange::MeshBodyTriangleLimit() &&
        (rep == Rep::Mesh || rep == Rep::Auto)) {
      error = interchange::MeshBodyTooLarge(path, m.triangles.size());
      return false;
    }
    // Whatever it becomes below, what the file said besides its triangles
    // goes with it.
    const auto landed = [&] {
      ApplyExtras(model, extras);
      return true;
    };
    // Facets of a PLY or glTF: its triangles as planar faces, sewn
    // (CADKernel::FacetsFromMesh), scaled as an STL's are.
    const auto pushFacets = [&](const mesh::TriMesh& mm) {
      if (progress != nullptr)
        progress->OnPhase(CADKernel::ImportProgress::Phase::Building, 0.0);
      CADShape shape;
      std::string open;
      if (!CADKernel::Instance().FacetsFromMesh(mm, shape, open, progress)) {
        error = cancelled() ? "cancelled"
                            : "failed to build facets of '" + path + "'";
        return false;
      }
      if (!open.empty())
        model.notes.push_back(open);
      if (scale != 1.0)
        shape.Scale(scale);
      model.products.push_back({std::move(shape), "", false, glm::vec3(0.0f)});
      model.instances.push_back({0, glm::dmat4(1.0), ""});
      model.origin.format = format;
      return true;
    };
    if (rep == Rep::Facets)
      return pushFacets(m) && landed();
    if (rep == Rep::Mesh ||
        (rep == Rep::Auto && model.representation == "mesh")) {
      PushMeshProduct(model, std::move(m), scale, format);
      return landed();
    }
    // Segmented, then rebuilt when its regions say it is mechanical.
    if (progress != nullptr)
      progress->OnPhase(CADKernel::ImportProgress::Phase::Building, 0.0);
    mesh::SegmentOptions so;
    so.minSides = options.minSides;
    so.cancelled = cancelled;
    mesh::Segmentation seg = mesh::SegmentAll(m, so);
    if (seg.cancelled || cancelled()) {
      error = "cancelled";
      return false;
    }
    // Mechanical or organic: how much of the surface the large regions
    // hold (interchange::kMechanicalAreaShare).
    double total = 0.0, large = 0.0;
    for (const mesh::Region& r : seg.regions)
      total += r.area;
    for (const mesh::Region& r : seg.regions)
      if (r.area >= interchange::kLargeRegionShare * total)
        large += r.area;
    const double share = total > 0.0 ? large / total : 0.0;
    const std::string shareText =
        Fixed1(100.0 * share) + "% of the surface in regions of at least " +
        Fixed1(100.0 * interchange::kLargeRegionShare) + "% of it";
    if (rep == Rep::Auto && share < interchange::kMechanicalAreaShare) {
      model.representation = "mesh";
      model.representationWhy =
          shareText + ": organic (a mechanical part has " +
          Fixed1(100.0 * interchange::kMechanicalAreaShare) + "% or more)";
      PushMeshProduct(model, std::move(m), scale, format);
      return landed();
    }
    if (progress != nullptr)
      progress->OnPhase(CADKernel::ImportProgress::Phase::Building, 0.5);
    CADShape shape;
    mesh::Recognition report;
    if (!cad::RebuildRecognized(m, seg, options.minSides, shape, report,
                                cancelled)) {
      if (cancelled()) {
        error = "cancelled";
        return false;
      }
      if (rep == Rep::Auto) {
        // A mesh `auto` cannot rebuild stays a mesh, as it is.
        model.representation = "mesh";
        model.representationWhy =
            "recognition failed a check (" + report.fellBack + ")";
        PushMeshProduct(model, std::move(m), scale, format);
        return landed();
      }
      // An OBJ, PLY or glTF asked to be recognised: its facets, as before,
      // and why.
      model.notes.push_back("recognition kept it as facets: " +
                            report.fellBack);
      if (scanFormat) {
        // Priced by the sewing law, as an STL's fallback is.
        if (!options.facetsPastLimit)
          if (const auto cost = interchange::MeshImportCost(path);
              cost && cost->overLimit) {
            error = "recognition kept it as facets (" + report.fellBack +
                    "); as facets " +
                    interchange::LargeMeshRefusal(path, *cost,
                                                  options.largeMeshOverride);
            return false;
          }
        if (!pushFacets(m))
          return false;
        model.representation = "facets";
        model.representationWhy = "recognition failed a check";
        return landed();
      }
      if (!CADKernel::Instance().LoadOBJStructured(path, model, progress)) {
        error =
            cancelled() ? "cancelled" : "failed to read '" + path + "' as OBJ";
        return false;
      }
      model.representation = "facets";
      model.representationWhy = "recognition failed a check";
      for (auto& product : model.products)
        if (product.shape.IsValid() && scale != 1.0)
          product.shape.Scale(scale);
      return landed();
    }
    if (rep == Rep::Auto) {
      model.representation = "recognize";
      model.representationWhy = shareText + ": mechanical";
    }
    if (scale != 1.0) {
      shape.Scale(scale);
      report.Transform(glm::dmat4(1.0), scale);
    }
    model.products.push_back({std::move(shape), "", false, glm::vec3(0.0f)});
    model.products.back().recognition = std::move(report);
    model.instances.push_back({0, glm::dmat4(1.0), ""});
    model.origin.format = format;
    return landed();
  }
  if (hasExt(".stl") &&
      options.representation == mesh::Representation::Recognize) {
    // The surfaces the triangles were tessellated from, rebuilt; a mesh
    // that fails a check is sewn as facets and the report says why
    // (cad::LoadStlRecognized).
    CADShape shape;
    mesh::Recognition report;
    cad::RecognizeOptions ro;
    ro.minSides = options.minSides;
    std::optional<interchange::MeshCost> cost;
    if (!options.facetsPastLimit) {
      cost = interchange::MeshImportCost(path);
      ro.facetsAllowed = !(cost && cost->overLimit);
    }
    bool facetsRefused = false;
    if (!cad::LoadStlRecognized(path, ro, shape, report, error, progress,
                                &facetsRefused)) {
      if (cancelled())
        error = "cancelled";
      else if (facetsRefused && cost)
        error += "; as facets " + interchange::LargeMeshRefusal(
                                      path, *cost, options.largeMeshOverride);
      return false;
    }
    if (scale != 1.0) {
      shape.Scale(scale);
      report.Transform(glm::dmat4(1.0), scale);
    }
    if (!report.fellBack.empty()) {
      model.notes.push_back("recognition kept it as facets: " +
                            report.fellBack);
      model.representation = "facets";
      model.representationWhy = "recognition failed a check";
    }
    model.products.push_back({std::move(shape), "", false, glm::vec3(0.0f)});
    model.products.back().recognition = std::move(report);
    model.instances.push_back({0, glm::dmat4(1.0), ""});
    model.origin.format = "STL";
  } else if (hasExt(".stl")) {
    // Triangle mesh → B-Rep: one planar face per triangle, sewn; a closed
    // mesh arrives as a solid (LoadSTL), an open one as a surface shell.
    // Fine for print round-trips and reference geometry; organic meshes get
    // heavy — every triangle is a face (interchange::MeshImportCost).
    CADShape shape;
    if (!CADKernel::Instance().LoadSTL(path, shape, progress)) {
      error =
          cancelled() ? "cancelled" : "failed to read '" + path + "' as STL";
      return false;
    }
    if (scale != 1.0)
      shape.Scale(scale);
    model.products.push_back({std::move(shape), "", false, glm::vec3(0.0f)});
    model.instances.push_back({0, glm::dmat4(1.0), ""});
    model.origin.format = "STL";
  } else if (hasExt(".brep")) {
    // Native BREP sidecar (assembly import extracts per-part geometry as
    // .brep, I2): one anonymous product at identity.
    CADShape shape;
    if (!CADKernel::Instance().LoadBREP(path, shape)) {
      error = "failed to read '" + path + "' as BREP";
      return false;
    }
    if (scale != 1.0)
      shape.Scale(scale);
    model.products.push_back({std::move(shape), "", false, glm::vec3(0.0f)});
    model.instances.push_back({0, glm::dmat4(1.0), ""});
    model.origin.format = "BREP";
  } else if (hasExt(".obj")) {
    if (!CADKernel::Instance().LoadOBJStructured(path, model, progress)) {
      error =
          cancelled() ? "cancelled" : "failed to read '" + path + "' as OBJ";
      return false;
    }
    // The reader starts the model afresh: say again what it became.
    model.representation = "facets";
    model.representationWhy = "the import names it";
    if (scale != 1.0)
      for (auto& product : model.products)
        if (product.shape.IsValid())
          product.shape.Scale(scale);
  } else if (isIges) {
    if (!CADKernel::Instance().LoadIGESStructured(path, model, progress)) {
      error =
          cancelled() ? "cancelled" : "failed to read '" + path + "' as IGES";
      return false;
    }
  } else if (!CADKernel::Instance().LoadSTEPStructured(path, model, progress)) {
    error = cancelled() ? "cancelled" : "failed to read '" + path + "' as STEP";
    return false;
  }
  return true;
}

namespace {

// The last mesh-only model an import read, keyed by the file's bytes and the
// read options. A placement edit re-executes the import -- every preview of
// align_import's -- and a 2M-triangle scan takes seconds to read again. Meshes
// only, which Execute copies and never writes: a B-Rep product shares
// OpenCASCADE topology that a later boolean writes onto, so it is never
// handed out twice. One entry: it holds a scan's triangles once more.
struct MeshReadCache {
  std::mutex mutex;
  std::string key;
  CADKernel::ImportedModel model;
};
MeshReadCache& ReadCache() {
  static MeshReadCache cache;
  return cache;
}
bool MeshOnly(const CADKernel::ImportedModel& model) {
  if (model.products.empty())
    return false;
  for (const auto& p : model.products)
    if (p.mesh == nullptr || p.shape.IsValid())
      return false;
  return true;
}

} // namespace

bool ImportFeature::Execute(RegenContext& ctx) {
  auto& bodies = ctx.bodies;
  m_recognized.clear();
  m_chosen.clear();
  m_chosenWhy.clear();
  // Structured (XCAF) read: geometry plus the names/colors STEP carries.
  // Each instance is a body — a flat part file is identity instances; an
  // assembly file imported into a PART places every occurrence (the
  // assembly-document import is a separate app-level flow, I2).
  CADKernel::ImportedModel model;
  const bool takesUnits = TakesUnits(path);
  // Consumed, not kept: one Execute uses what the importer already parsed,
  // every later one re-reads `path` as it always did (see SeedGeometry).
  std::unique_ptr<CADKernel::ImportedModel> seed = std::move(m_seed);
  const size_t content = ContentHash();
  const std::string cacheKey = content == 0
                                   ? std::string()
                                   : path + "|" + std::to_string(content) +
                                         "|" + units::ToString(lengthUnit) +
                                         "|" + mesh::ToString(representation) +
                                         "|" + std::to_string(minSides);
  bool cached = false;
  if (seed == nullptr && !cacheKey.empty()) {
    std::lock_guard<std::mutex> lock(ReadCache().mutex);
    if (ReadCache().key == cacheKey) {
      model = ReadCache().model;
      cached = true;
    }
  }
  if (cached) {
  } else if (seed != nullptr) {
    model = std::move(*seed);
  } else if (std::string why; !ReadModel(path, Options(), model, why)) {
    SetError(why);
    LOG_ERROR("ImportFeature '{}': {}", m_name, why);
    return false;
  }
  if (!cached && !cacheKey.empty() && MeshOnly(model)) {
    std::lock_guard<std::mutex> lock(ReadCache().mutex);
    ReadCache().key = cacheKey;
    ReadCache().model = model;
  }
  m_chosen = model.representation;
  m_chosenWhy = model.representationWhy;

  // Which axis of the file is up, decided from its header (or as asked) and
  // applied as a turn about the FILE origin, before the centring: the shift
  // below then centres the part as it stands. A placement, so it changes no
  // topology and a sketch on an imported face follows an axis change.
  m_up = interchange::DecideUpAxis(upAxis, model.origin, turnDeg);
  const glm::dmat4 turn = interchange::UpAxisRotation(m_up.applied, turnDeg);

  // Placed shapes first, so centering covers the assembled positions. What
  // is skipped is NAMED: a surface IGES (or an open mesh) used to vanish
  // here without a word, and the import looked complete.
  struct Placed {
    CADShape shape;
    size_t product;
    std::string name;
    glm::dmat4 placement{1.0}; // file to part, before the centring
    std::shared_ptr<mesh::TriMesh> mesh; // a mesh body's, placed
  };
  std::vector<Placed> placed;
  std::vector<std::string> skipped;
  std::vector<std::string> openNotes;
  for (const auto& inst : model.instances) {
    // A mesh body: kept whether or not it closes. An open scan is the
    // normal case (the table it stood on), so it is reported, not skipped.
    if (const auto& m = model.products[inst.product].mesh) {
      auto moved = std::make_shared<mesh::TriMesh>(
          mesh::Transformed(*m, turn * inst.transform));
      if (!moved->Closed()) {
        const std::vector<double> loops = mesh::BoundaryLoops(*moved);
        char buf[160];
        if (!loops.empty())
          std::snprintf(buf, sizeof buf,
                        "the mesh is open: %zu boundary loop%s, the longest "
                        "%.1f mm",
                        loops.size(), loops.size() == 1 ? "" : "s",
                        loops.front());
        else
          std::snprintf(buf, sizeof buf,
                        "the mesh is not a closed solid: %zu edges on three "
                        "or more triangles, %zu joining triangles facing "
                        "opposite ways",
                        moved->nonManifoldEdges, moved->flippedEdges);
        openNotes.push_back(buf);
      }
      placed.push_back({CADShape(), inst.product, inst.name,
                        turn * inst.transform, std::move(moved)});
      continue;
    }
    const CADShape& src = model.products[inst.product].shape;
    const std::string& label = !model.products[inst.product].name.empty()
                                   ? model.products[inst.product].name
                                   : inst.name;
    const std::string who =
        label.empty()
            ? "shape " + std::to_string(skipped.size() + placed.size() + 1)
            : "\"" + label + "\"";
    if (!src.IsValid()) {
      skipped.push_back(who + " (no geometry)");
      continue;
    }
    CADShape moved = src.Transformed(turn * inst.transform);
    if (!moved.IsValid()) {
      skipped.push_back(who + " (could not be placed)");
      continue;
    }
    // Measured by the import job's worker when it read the file: a rigid
    // placement keeps a volume, and a 100k-face mesh is seconds to measure.
    const std::optional<double>& known = model.products[inst.product].volume;
    if ((known ? *known : moved.Volume()) <= 1e-6) {
      skipped.push_back(who + " (a surface, not a solid)");
      continue;
    }
    placed.push_back(
        {std::move(moved), inst.product, inst.name, turn * inst.transform});
  }
  std::string warning;
  for (const std::string& note : model.notes)
    warning += (warning.empty() ? "" : "; ") + note;
  for (const std::string& note : openNotes)
    warning += (warning.empty() ? "" : "; ") + note;
  if (!skipped.empty()) {
    std::string list;
    for (const std::string& s : skipped)
      list += (list.empty() ? "" : ", ") + s;
    warning += (warning.empty() ? "" : "; ") + std::string("skipped ") +
               std::to_string(skipped.size()) + " of " +
               std::to_string(model.instances.size()) + ": " + list;
  }
  SetWarning(warning);

  // Combined bounding box over valid shapes, so the whole import is centered
  // as one unit and shapes keep their relative placement.
  glm::dvec3 combinedMin(0.0), combinedMax(0.0);
  bool haveBounds = false;
  for (const auto& p : placed) {
    glm::dvec3 min, max;
    if (p.mesh)
      p.mesh->Bounds(min, max);
    else
      p.shape.GetBoundingBox(min, max);
    combinedMin = haveBounds ? glm::min(combinedMin, min) : min;
    combinedMax = haveBounds ? glm::max(combinedMax, max) : max;
    haveBounds = true;
  }
  if (!haveBounds) {
    SetError("no solid in '" + path + "'" +
             (warning.empty() ? std::string() : ": " + warning));
    LOG_ERROR("ImportFeature '{}': no valid shapes in '{}'", m_name, path);
    return false;
  }
  // The commonest unit mistake an unitless file allows: a mesh drawn in
  // metres read as millimetres, so a 20 cm part arrives 0.2 mm across. A
  // warning and not a refusal — a genuinely tiny part is legitimate — but it
  // names the fix, so the part is not quietly 1000x too small.
  if (takesUnits) {
    const glm::dvec3 extent = combinedMax - combinedMin;
    double largest = extent.x;
    if (extent.y > largest)
      largest = extent.y;
    if (extent.z > largest)
      largest = extent.z;
    // Measured as placed: a scan scaled up by its placement is not small.
    largest *= placement.scale;
    if (largest < 1.0) {
      char across[32];
      std::snprintf(across, sizeof across, "%.3g", largest);
      SetWarning((warning.empty() ? std::string() : warning + "; ") + "only " +
                 across + " mm across, read as " + units::ToString(lengthUnit) +
                 " — an STL, OBJ, PLY or BREP carries no unit; if it was "
                 "drawn in metres or inches, set the import's units (m, "
                 "in, cm)");
    }
  }
  // Centre on the combined box, unless the caller asked to keep the file's
  // own coordinates. Recorded either way: an import that MOVED and did not
  // say so is how a fit check against a neighbour part compares two things
  // that are not where it thinks they are.
  const glm::dvec3 center =
      keepOrigin ? glm::dvec3(0.0) : (combinedMin + combinedMax) * 0.5;
  m_shift = -center;

  // Real-world assembly STEPs run to hundreds of instances (the Waveshare
  // adapter test file has 143); the cap only guards against pathological
  // files, so it must sit above real ones.
  constexpr size_t kMaxShapes = 500;
  if (placed.size() > kMaxShapes) {
    LOG_WARN("ImportFeature '{}': '{}' has {} shapes, importing first {}",
             m_name, path, placed.size(), kMaxShapes);
  }

  size_t added = 0;
  for (auto& p : placed) {
    if (added >= kMaxShapes)
      break;
    added++;
    // All import bodies share createdBy, so the prefix carries the body
    // ordinal — otherwise two bodies' identical face names would resolve
    // against whichever body is found first.
    const std::string prefix =
        std::to_string(GetId()) + "/b" + std::to_string(added);
    ElementMap map;
    if (p.mesh) {
      // A mesh body has one element, the mesh; the whole body picks and
      // highlights as it (docs/interchange/README.md#mesh-bodies). The
      // placement is a turn, a move and a positive scale: the triangles keep
      // their adjacency and their winding, so it is applied in place.
      const glm::dmat4 m =
          placement.Matrix() * glm::translate(glm::dmat4(1.0), -center);
      for (glm::dvec3& v : p.mesh->positions)
        v = glm::dvec3(m * glm::dvec4(v, 1.0));
      p.mesh->quantum *= placement.scale;
      map.names[0] = prefix + "/mesh";
    } else {
      p.shape.Translate(-center);
      // Placed after the centring; a placement changes no topology, so the
      // face names below are the file's.
      if (placement.scale != 1.0)
        p.shape.Scale(placement.scale);
      if (placement.translateMm != glm::dvec3(0.0) ||
          placement.rotateDeg != glm::dvec3(0.0))
        p.shape = p.shape.Transformed(placement.Rigid());
      topo::NamePrimitiveElements(p.shape, prefix, map);
    }
    // The PRODUCT name labels the body ("Widget", not the writer's
    // auto-generated instance name "Widget:1"); instance names matter for
    // assembly occurrences (I2), not part bodies.
    const auto& product = model.products[p.product];
    const std::string bodyName =
        !product.name.empty()
            ? product.name
            : (!p.name.empty() ? p.name : m_name + " " + std::to_string(added));
    bodies.push_back({bodyName, std::move(p.shape), m_material, GetId()});
    bodies.back().elementMap = std::move(map);
    if (p.mesh)
      bodies.back().mesh = std::move(p.mesh);
    if (product.recognition) {
      Recognized r{bodyName, *product.recognition};
      r.recognition.Transform(
          glm::translate(glm::dmat4(1.0), -center) * p.placement, 1.0);
      if (!placement.IsIdentity())
        r.recognition.Transform(placement.Rigid(), placement.scale);
      m_recognized.push_back(std::move(r));
    }
    if (m_material.empty()) {
      if (product.hasColor) {
        bodies.back().hasDisplayColor = true;
        bodies.back().displayColor = product.color;
      } else if (hasColor) { // feature-level color (assembly-import sidecars)
        bodies.back().hasDisplayColor = true;
        bodies.back().displayColor = color;
      }
    }
  }
  return true;
}

bool Feature::SetFaceAppearance(const std::string& element,
                                const Appearance& look) {
  const auto it = std::find_if(
      m_faceAppearances.begin(), m_faceAppearances.end(),
      [&](const FaceAppearance& f) { return f.element == element; });
  const bool had = it != m_faceAppearances.end();
  if (!look.Any()) {
    if (had)
      m_faceAppearances.erase(it);
  } else if (had) {
    it->look = look;
  } else {
    m_faceAppearances.push_back({element, look});
  }
  return had;
}

void Feature::SetMaterial(const std::string& material) {
  // The ONE funnel every material ingress runs through: the set_material RPC,
  // and FeatureFromJson, which is what add_feature, modify_feature and
  // document load all use. Refusing here is what closes the hole where
  // add_feature(feature={..., "material":"unobtanium"}) was accepted and the
  // bad string sat on the feature until a renderer or a mass query tripped
  // over it — the add_mate(type:"revolut") shape, RPC-reachable.
  //
  // Throws, naming what IS accepted. The empty slug stays legal and means
  // default. The GUI only ever passes MaterialLibrary::Slug() output, so it
  // cannot reach this.
  (void)CADMaterials::MaterialLibrary::MaterialTypeFromString(material);
  m_material = material;
}
