// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/DerivedFeature.h"

#include "cad/BoundedRead.h"

#include "common/Logger.h"
#include "core/Document.h"
#include "core/InputHash.h"

#include <BinTools.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <set>
#include <sstream>

DerivedFeature::DerivedFeature(const std::string& name, uint64_t sourcePart)
    : sourcePart(sourcePart) {
  m_name = name;
}

glm::dmat4 DerivedFeature::Placement() const {
  return glm::translate(glm::dmat4(1.0), position) *
         glm::mat4_cast(orientation);
}

bool DerivedFeature::IdentityPlacement() const {
  const glm::dquat q = glm::normalize(orientation);
  return glm::length(position) < 1e-9 && std::abs(std::abs(q.w) - 1.0) < 1e-9;
}

std::string
DerivedFeature::DerivedName(uint64_t sourceCreatedBy,
                            const std::string& sourceElementName) const {
  return std::to_string(GetId()) + "/" + std::to_string(sourceCreatedBy) + "/" +
         sourceElementName;
}

std::string DerivedFeature::SketchEntityName(uint64_t sourceSketchId,
                                             uint32_t entityId) const {
  return std::to_string(GetId()) + "/" + std::to_string(sourceSketchId) + "/" +
         std::to_string(entityId);
}

const DerivedSketch* DerivedFeature::FindSketch(uint64_t sourceSketchId) const {
  if (sourceSketchId == 0)
    return sketchSnapshot.size() == 1 ? &sketchSnapshot.front() : nullptr;
  for (const DerivedSketch& s : sketchSnapshot)
    if (s.sourceSketch == sourceSketchId)
      return &s;
  return nullptr;
}

SketchPlane DerivedFeature::PlacedPlane(const DerivedSketch& s) const {
  if (IdentityPlacement())
    return s.plane;
  const glm::dmat4 xf = Placement();
  const glm::dmat3 r(xf);
  return SketchPlane(glm::dvec3(xf * glm::dvec4(s.plane.GetOrigin(), 1.0)),
                     glm::normalize(r * s.plane.GetNormal()),
                     glm::normalize(r * s.plane.GetUAxis()));
}

bool DerivedFeature::ResolveSketchEntity(const std::string& name,
                                         const DerivedSketch*& sk,
                                         const sketch::Entity*& entity) const {
  // "<this id>/<source sketch id>/<entity id>", three numbers: a body's
  // element name under this feature has a source name after the second
  // slash, which is never a bare number of a sketch this feature copies.
  const size_t a = name.find('/');
  const size_t b = a == std::string::npos ? a : name.find('/', a + 1);
  if (b == std::string::npos || name.find('/', b + 1) != std::string::npos)
    return false;
  const auto number = [](std::string_view t, uint64_t& out) {
    if (t.empty() || t.size() > 19)
      return false;
    out = 0;
    for (char c : t) {
      if (c < '0' || c > '9')
        return false;
      out = out * 10 + static_cast<uint64_t>(c - '0');
    }
    return true;
  };
  uint64_t self = 0, sketchId = 0, entityId = 0;
  if (!number(std::string_view(name).substr(0, a), self) || self != GetId() ||
      !number(std::string_view(name).substr(a + 1, b - a - 1), sketchId) ||
      !number(std::string_view(name).substr(b + 1), entityId) ||
      entityId > UINT32_MAX)
    return false;
  for (const DerivedSketch& s : sketchSnapshot) {
    if (s.sourceSketch != sketchId)
      continue;
    for (const sketch::Entity& e : s.entities)
      if (sketch::EntityId(e) == entityId) {
        sk = &s;
        entity = &e;
        return true;
      }
  }
  return false;
}

std::string DerivedFeature::WhyIncomplete() const {
  if (sourcePart == 0)
    return "no source part chosen";
  if (snapshot.empty() && sketchSnapshot.empty())
    return "no snapshot taken from '" + sourceName + "' yet";
  return {};
}

bool DerivedFeature::Execute(RegenContext& ctx) {
  // A copy of sketches only adds no body: its sketches are read by the
  // sketches of this part that stand on them or project them.
  if (snapshot.empty() && !sketchSnapshot.empty())
    return true;
  if (snapshot.empty()) {
    SetError(sourceName.empty()
                 ? "no snapshot taken yet"
                 : "no snapshot taken from '" + sourceName + "' yet");
    return false;
  }
  if (ctx.assets == nullptr) {
    SetError("regenerated without an asset store, so the snapshot cannot be "
             "read");
    return false;
  }
  const bool identity = IdentityPlacement();
  const glm::dmat4 xf = Placement();
  for (const DerivedBody& sb : snapshot) {
    auto cached = m_decoded.find(sb.asset);
    if (cached == m_decoded.end()) {
      const auto bytesIt = ctx.assets->find(sb.asset);
      if (bytesIt == ctx.assets->end()) {
        SetError("snapshot body '" + sb.name + "' is missing from the file (" +
                 sb.asset + "); update it from '" + sourceName + "'");
        return false;
      }
      // Bounded: a damaged asset overran OCC 7.6's stack (cad/BoundedRead.h).
      std::string why;
      const TopoDS_Shape shape =
          cad::ReadBinaryBrep(std::string_view(reinterpret_cast<const char*>(
                                                   bytesIt->second.data()),
                                               bytesIt->second.size()),
                              &why);
      if (shape.IsNull()) {
        SetError("snapshot body '" + sb.name + "' (" + sb.asset +
                 ") is unreadable" + (why.empty() ? "" : ": " + why) +
                 "; update it from '" + sourceName + "'");
        return false;
      }
      cached = m_decoded.emplace(sb.asset, CADShape(shape)).first;
    }
    // Transforming shares the TShapes and moves the Location, so the
    // explorer order — and with it every face/edge index in the map — is the
    // source's. (test_derived pins it: a placed face keeps its name.)
    CADShape placed =
        identity ? cached->second : cached->second.Transformed(xf);
    if (!placed.IsValid()) {
      SetError("snapshot body '" + sb.name + "' could not be placed");
      return false;
    }
    Body body;
    body.name = sb.name;
    body.shape = std::move(placed);
    body.material = m_material.empty() ? sb.material : m_material;
    body.createdBy = GetId();
    body.derivedFrom = sb.sourceCreatedBy;
    body.reference = reference;
    for (const auto& [index, name] : sb.elementMap.names)
      body.elementMap.names[index] = DerivedName(sb.sourceCreatedBy, name);
    for (const auto& [index, name] : sb.elementMap.edgeNames)
      body.elementMap.edgeNames[index] = DerivedName(sb.sourceCreatedBy, name);
    if (m_material.empty() && sb.hasDisplayColor) {
      body.hasDisplayColor = true;
      body.displayColor = sb.displayColor;
    }
    ctx.bodies.push_back(std::move(body));
  }
  return true;
}

namespace derived {

std::string AssetNameFor(const std::vector<unsigned char>& bytes) {
  const size_t h = std::hash<std::string_view>{}(std::string_view(
      reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  char buf[32];
  std::snprintf(buf, sizeof buf, "%016zx", h);
  return std::string("derived-") + buf + ".brep";
}

namespace {

// The key of one copied body: its exact B-Rep and every field the copy
// carries from it. FNV, so the same body keys the same on every platform and
// in every build. The B-Rep goes in as inputhash digests it -- the shape as
// a project reads it back, without the meshes and flags OCC hangs on it --
// so a source reopened from its baked geometry keys as the one regenerated.
void HashBody(inputhash::Hasher& h, const DerivedBody& b,
              const CADShape& shape) {
  h.Str("body");
  h.Shape(shape);
  h.Str(b.name);
  h.Str(b.material);
  h.U64(b.sourceCreatedBy);
  h.U64(b.elementMap.names.size());
  for (const auto& [index, name] : b.elementMap.names) {
    h.U64(static_cast<uint64_t>(index));
    h.Str(name);
  }
  h.U64(b.elementMap.edgeNames.size());
  for (const auto& [index, name] : b.elementMap.edgeNames) {
    h.U64(static_cast<uint64_t>(index));
    h.Str(name);
  }
  h.U64(b.hasDisplayColor ? 1 : 0);
  if (b.hasDisplayColor) {
    h.F64(b.displayColor.x);
    h.F64(b.displayColor.y);
    h.F64(b.displayColor.z);
  }
  h.U64(b.fitBands.size());
  for (const FitBand& f : b.fitBands) {
    h.Str(f.face.name); // the source's name; the copy renames it
    h.F64(f.lower);
    h.F64(f.upper);
    h.U64(static_cast<uint64_t>(f.side));
    h.F64(f.allowance);
    h.U64(f.printedPress ? 1 : 0);
    h.Str(f.label);
  }
}

void HashVec(inputhash::Hasher& h, const glm::dvec2& v) {
  h.F64(v.x);
  h.F64(v.y);
}
void HashVec(inputhash::Hasher& h, const glm::dvec3& v) {
  h.F64(v.x);
  h.F64(v.y);
  h.F64(v.z);
}

// The key of one copied sketch: its plane and each entity's fields, named
// one by one (a JSON dump would move with every key a build adds).
void HashSketch(inputhash::Hasher& h, const DerivedSketch& s) {
  h.Str("sketch");
  h.U64(s.sourceSketch);
  h.Str(s.name);
  HashVec(h, s.plane.GetOrigin());
  HashVec(h, s.plane.GetNormal());
  HashVec(h, s.plane.GetUAxis());
  h.U64(s.entities.size());
  for (const sketch::Entity& e : s.entities) {
    h.U64(e.index());
    h.U64(sketch::EntityId(e));
    h.U64(sketch::IsConstruction(e) ? 1 : 0);
    std::visit(
        [&](const auto& x) {
          using T = std::decay_t<decltype(x)>;
          if constexpr (std::is_same_v<T, sketch::Point>) {
            HashVec(h, x.p);
          } else if constexpr (std::is_same_v<T, sketch::Line>) {
            HashVec(h, x.a);
            HashVec(h, x.b);
          } else if constexpr (std::is_same_v<T, sketch::Circle>) {
            HashVec(h, x.center);
            h.F64(x.radius);
          } else if constexpr (std::is_same_v<T, sketch::Arc>) {
            HashVec(h, x.center);
            h.F64(x.radius);
            h.F64(x.startAngle);
            h.F64(x.endAngle);
          } else if constexpr (std::is_same_v<T, sketch::Ellipse>) {
            HashVec(h, x.center);
            h.F64(x.rMajor);
            h.F64(x.rMinor);
            h.F64(x.rotation);
          } else if constexpr (std::is_same_v<T, sketch::EllipseArc>) {
            HashVec(h, x.center);
            h.F64(x.rMajor);
            h.F64(x.rMinor);
            h.F64(x.rotation);
            h.F64(x.startParam);
            h.F64(x.endParam);
          } else if constexpr (std::is_same_v<T, sketch::BSpline>) {
            h.U64(static_cast<uint64_t>(x.degree));
            h.U64(x.poles.size());
            for (const glm::dvec2& p : x.poles)
              HashVec(h, p);
          } else if constexpr (std::is_same_v<T, sketch::Text>) {
            HashVec(h, x.anchor);
            h.Str(x.text);
            h.Str(x.font);
            h.F64(x.height);
            h.F64(x.angle);
            h.U64(static_cast<uint64_t>(x.halign));
            h.U64(static_cast<uint64_t>(x.valign));
            h.F64(x.spacing);
          }
        },
        e);
  }
}

uint64_t KeyOf(const std::vector<DerivedBody>& bodies,
               const std::function<CADShape(size_t)>& shapeOf,
               const std::vector<DerivedSketch>& sketches) {
  inputhash::Hasher h;
  h.Str("derived-snapshot");
  h.U64(bodies.size());
  for (size_t i = 0; i < bodies.size(); i++)
    HashBody(h, bodies[i], shapeOf(i));
  h.U64(sketches.size());
  for (const DerivedSketch& s : sketches)
    HashSketch(h, s);
  // 0 means "not recorded"; a real key is never it.
  return h.Digest() == 0 ? 1 : h.Digest();
}

} // namespace

std::string Take(const DerivedFeature& feature, const Document& source,
                 Taken& out) {
  out = Taken{};
  const std::vector<Body>& all = source.GetBodies();
  // A copy that names sketches copies only the bodies it names as well.
  const bool wantsSketches =
      !feature.sketches.empty() || !feature.sketchNames.empty();
  const bool wantsBodies =
      !wantsSketches || !feature.bodies.empty() || !feature.bodyNames.empty();
  if (wantsBodies && all.empty())
    return "source part has no bodies to derive from";

  // The filter must name bodies the source actually has, by their producer.
  std::set<uint64_t> have;
  for (const Body& b : all)
    if (!b.reference)
      have.insert(b.createdBy);
  for (uint64_t id : feature.bodies) {
    if (!have.contains(id)) {
      std::string list;
      for (uint64_t h : have)
        list += (list.empty() ? "" : ", ") + std::to_string(h);
      return "source part has no body created by feature " +
             std::to_string(id) + " (it has: " + list + ")";
    }
  }

  // The name filter likewise (v85): a source's body names are what
  // list_elements and get_state show, and are the only handle on the 197
  // bodies one Import creates.
  for (const std::string& want : feature.bodyNames) {
    bool found = false;
    for (const Body& b : all)
      if (!b.reference && b.name == want)
        found = true;
    if (found)
      continue;
    std::string list;
    size_t shown = 0;
    for (const Body& b : all) {
      if (b.reference || shown >= 12)
        continue;
      list += (list.empty() ? "" : ", ") + b.name;
      shown++;
    }
    return "source part has no body named '" + want + "' (it has " +
           std::to_string(have.size()) +
           " creators and bodies including: " + list + ")";
  }

  // The sketches, by id and/or by name; each one the source has and that
  // built. The ones it has are listed when one is not there.
  if (wantsSketches) {
    std::vector<const SketchFeature*> sources;
    std::map<const SketchFeature*, size_t> indexOf;
    for (size_t i = 0; i < source.GetFeatures().size(); i++)
      if (const auto* sk = dynamic_cast<const SketchFeature*>(
              source.GetFeatures()[i].get())) {
        sources.push_back(sk);
        indexOf[sk] = i;
      }
    const auto known = [&] {
      std::string list;
      for (const SketchFeature* sk : sources)
        list += (list.empty() ? "" : ", ") + sk->GetName() + " (" +
                std::to_string(sk->GetId()) + ")";
      return list.empty() ? std::string("it has none") : "it has: " + list;
    };
    for (uint64_t id : feature.sketches)
      if (std::none_of(
              sources.begin(), sources.end(),
              [&](const SketchFeature* sk) { return sk->GetId() == id; }))
        return "source part has no sketch with id " + std::to_string(id) +
               " (" + known() + ")";
    for (const std::string& want : feature.sketchNames)
      if (std::none_of(
              sources.begin(), sources.end(),
              [&](const SketchFeature* sk) { return sk->GetName() == want; }))
        return "source part has no sketch named '" + want + "' (" + known() +
               ")";
    for (const SketchFeature* sk : sources) {
      const bool byId =
          std::find(feature.sketches.begin(), feature.sketches.end(),
                    sk->GetId()) != feature.sketches.end();
      const bool byName =
          std::find(feature.sketchNames.begin(), feature.sketchNames.end(),
                    sk->GetName()) != feature.sketchNames.end();
      // Both filters given narrow together, as bodies and body_names do.
      if (!feature.sketches.empty() && !feature.sketchNames.empty()
              ? !(byId && byName)
              : !(byId || byName))
        continue;
      if (!sk->GetError().empty())
        return "source sketch '" + sk->GetName() +
               "' failed: " + sk->GetError();
      if (source.EffectiveSuppressed(indexOf.at(sk)))
        return "source sketch '" + sk->GetName() +
               "' is suppressed in the source";
      DerivedSketch ds;
      ds.sourceSketch = sk->GetId();
      ds.name = sk->GetName();
      ds.plane = sk->plane;
      ds.entities = sk->entities;
      out.sketches.push_back(std::move(ds));
    }
    if (out.sketches.empty())
      return "that filter selects none of the source's sketches (sketches "
             "and sketch_names narrow TOGETHER — a sketch must match both "
             "when both are given)";
  }

  std::vector<const CADShape*> shapes;
  for (const Body& b : all) {
    if (!wantsBodies)
      break;
    if (b.reference)
      continue; // the source's own ghosts are not its geometry
    if (!feature.bodies.empty() &&
        std::find(feature.bodies.begin(), feature.bodies.end(), b.createdBy) ==
            feature.bodies.end())
      continue;
    if (!feature.bodyNames.empty() &&
        std::find(feature.bodyNames.begin(), feature.bodyNames.end(), b.name) ==
            feature.bodyNames.end())
      continue;
    std::ostringstream stream(std::ios::binary);
    try {
      BinTools::Write(b.shape.GetShape(), stream);
    } catch (const Standard_Failure& e) {
      return "could not serialize body '" + b.name + "': " +
             (e.GetMessageString() ? e.GetMessageString() : "OCC error");
    }
    const std::string s = stream.str();
    if (s.empty())
      return "could not serialize body '" + b.name + "'";
    std::vector<unsigned char> bytes(s.begin(), s.end());
    DerivedBody db;
    db.asset = AssetNameFor(bytes);
    db.name = b.name;
    db.material = b.material;
    db.sourceCreatedBy = b.createdBy;
    db.elementMap = b.elementMap;
    db.hasDisplayColor = b.hasDisplayColor;
    db.displayColor = b.displayColor;
    db.fitBands = b.fitBands;
    for (FitBand& band : db.fitBands)
      band.face.featureId = 0; // the source's; the copy names its own
    out.bodies.push_back(std::move(db));
    out.bytes.push_back(std::move(bytes));
    shapes.push_back(&b.shape);
  }
  if (wantsBodies && out.bodies.empty()) {
    // Name the cause: a filter that matches nothing is a different mistake
    // from a source that has nothing to give, and both used to say the
    // second (bodies + body_names can narrow to the empty set together).
    if (!feature.bodies.empty() || !feature.bodyNames.empty())
      return "that filter selects none of the source's bodies (bodies and "
             "body_names narrow TOGETHER — a body must match both when both "
             "are given)";
    return "source part has only reference bodies; nothing to derive";
  }
  out.key =
      KeyOf(out.bodies, [&](size_t i) { return *shapes[i]; }, out.sketches);
  return {};
}

std::string TakeSnapshot(DerivedFeature& feature, const Document& source,
                         Document& host) {
  Taken taken;
  if (std::string why = Take(feature, source, taken); !why.empty())
    return why;
  for (size_t i = 0; i < taken.bodies.size(); i++)
    if (host.GetAsset(taken.bodies[i].asset) == nullptr)
      host.SetAsset(taken.bodies[i].asset, std::move(taken.bytes[i]));
  feature.snapshot = std::move(taken.bodies);
  feature.sketchSnapshot = std::move(taken.sketches);
  feature.sourceKey = taken.key;
  return {};
}

uint64_t StoredKey(const DerivedFeature& feature, const Document& host) {
  if (feature.sourceKey != 0)
    return feature.sourceKey;
  return KeyOf(
      feature.snapshot,
      [&](size_t i) {
        // The stored B-Rep, read as Execute reads it; an unreadable one keys
        // as nothing, which no source gives.
        TopoDS_Shape shape;
        if (const auto* bytes = host.GetAsset(feature.snapshot[i].asset))
          shape = cad::ReadBinaryBrep(std::string_view(
              reinterpret_cast<const char*>(bytes->data()), bytes->size()));
        return shape.IsNull() ? CADShape() : CADShape(shape);
      },
      feature.sketchSnapshot);
}

bool IsStale(const DerivedFeature& feature, const Document& source,
             const Document& host) {
  Taken taken;
  if (!Take(feature, source, taken).empty())
    return true; // the source lost what this copies: behind, and says why
  return taken.key != StoredKey(feature, host);
}

bool WouldCycle(uint64_t hostId, uint64_t sourceId,
                const PartResolver& partById, std::string* why) {
  if (hostId == sourceId) {
    if (why)
      *why = "a part cannot derive from itself";
    return true;
  }
  // Depth-first over derived edges from the source; the chain is kept for
  // the message. A source that is not loaded has no edges we can see.
  std::set<uint64_t> visited;
  std::vector<uint64_t> chain{sourceId};
  std::function<bool(uint64_t)> walk = [&](uint64_t id) -> bool {
    if (!visited.insert(id).second)
      return false;
    const Document* doc = partById(id);
    if (doc == nullptr)
      return false;
    for (const auto& f : doc->GetFeatures()) {
      const auto* d = dynamic_cast<const DerivedFeature*>(f.get());
      if (d == nullptr)
        continue;
      chain.push_back(d->sourcePart);
      if (d->sourcePart == hostId)
        return true;
      if (walk(d->sourcePart))
        return true;
      chain.pop_back();
    }
    return false;
  };
  if (!walk(sourceId))
    return false;
  if (why) {
    std::string s = "part " + std::to_string(hostId) + " would derive from";
    for (uint64_t id : chain)
      s += " " + std::to_string(id) + " ->";
    *why = s + " itself";
  }
  return true;
}

} // namespace derived
