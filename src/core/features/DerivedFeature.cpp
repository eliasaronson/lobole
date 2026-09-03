#include "core/features/DerivedFeature.h"

#include "common/Logger.h"
#include "core/Document.h"

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

glm::mat4 DerivedFeature::Placement() const {
  return glm::translate(glm::mat4(1.0f), position) *
         glm::mat4_cast(orientation);
}

bool DerivedFeature::IdentityPlacement() const {
  const glm::quat q = glm::normalize(orientation);
  return glm::length(position) < 1e-9f &&
         std::abs(std::abs(q.w) - 1.0f) < 1e-9f;
}

std::string
DerivedFeature::DerivedName(uint64_t sourceCreatedBy,
                            const std::string& sourceElementName) const {
  return std::to_string(GetId()) + "/" + std::to_string(sourceCreatedBy) + "/" +
         sourceElementName;
}

std::string DerivedFeature::WhyIncomplete() const {
  if (sourcePart == 0)
    return "no source part chosen";
  if (snapshot.empty())
    return "no snapshot taken from '" + sourceName + "' yet";
  return {};
}

bool DerivedFeature::Execute(RegenContext& ctx) {
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
  const glm::mat4 xf = Placement();
  for (const DerivedBody& sb : snapshot) {
    auto cached = m_decoded.find(sb.asset);
    if (cached == m_decoded.end()) {
      const auto bytesIt = ctx.assets->find(sb.asset);
      if (bytesIt == ctx.assets->end()) {
        SetError("snapshot body '" + sb.name + "' is missing from the file (" +
                 sb.asset + "); update it from '" + sourceName + "'");
        return false;
      }
      TopoDS_Shape shape;
      try {
        std::istringstream in(
            std::string(bytesIt->second.begin(), bytesIt->second.end()),
            std::ios::binary);
        BinTools::Read(shape, in); // void: an unreadable stream leaves it null
        if (shape.IsNull()) {
          SetError("snapshot body '" + sb.name + "' (" + sb.asset +
                   ") is unreadable; update it from '" + sourceName + "'");
          return false;
        }
      } catch (const Standard_Failure& e) {
        SetError("snapshot body '" + sb.name + "' (" + sb.asset +
                 ") is unreadable: " +
                 (e.GetMessageString() ? e.GetMessageString() : "OCC error"));
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

std::string TakeSnapshot(DerivedFeature& feature, const Document& source,
                         Document& host) {
  const std::vector<Body>& all = source.GetBodies();
  if (all.empty())
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

  std::vector<DerivedBody> taken;
  for (const Body& b : all) {
    if (b.reference)
      continue; // the source's own ghosts are not its geometry
    if (!feature.bodies.empty() &&
        std::find(feature.bodies.begin(), feature.bodies.end(), b.createdBy) ==
            feature.bodies.end())
      continue;
    std::ostringstream out(std::ios::binary);
    try {
      BinTools::Write(b.shape.GetShape(), out);
    } catch (const Standard_Failure& e) {
      return "could not serialize body '" + b.name + "': " +
             (e.GetMessageString() ? e.GetMessageString() : "OCC error");
    }
    const std::string s = out.str();
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
    if (host.GetAsset(db.asset) == nullptr)
      host.SetAsset(db.asset, std::move(bytes));
    taken.push_back(std::move(db));
  }
  if (taken.empty())
    return "source part has only reference bodies; nothing to derive";
  feature.snapshot = std::move(taken);
  feature.sourceChainHash = source.TipChainHash();
  return {};
}

std::optional<bool> IsStale(const DerivedFeature& feature,
                            const PartResolver& partById) {
  const Document* source = partById(feature.sourcePart);
  if (source == nullptr)
    return std::nullopt;
  return source->TipChainHash() != feature.sourceChainHash;
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
