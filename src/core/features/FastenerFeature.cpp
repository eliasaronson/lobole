#include "core/features/FastenerFeature.h"

#include "cad/CADKernel.h"
#include "common/Logger.h"
#include "core/ElementHistory.h"
#include "core/parts/FastenerTable.h"

#include <cmath>
#include <numbers>
#include <sstream>

namespace {

// A regular hexagonal prism, `acrossFlats` wide, standing on z = base and
// `height` tall, centred on the z axis.
//
// Built as the intersection of three slabs at 0, 60 and 120 degrees rather
// than from a polygon wire, because the kernel's primitives are what exist and
// three booleans on boxes is less machinery than a wire builder used once. The
// slabs are made over-long in their free direction so the intersection is
// decided by the three widths and nothing else.
CADShape HexPrism(double acrossFlats, double base, double height) {
  const double half = acrossFlats * 0.5;
  const double reach = acrossFlats * 2.0; // comfortably past the corners
  CADShape hex;
  for (int i = 0; i < 3; i++) {
    CADShape slab = CADKernel::Instance().CreateBox(
        glm::vec3(-half, -reach, base), glm::vec3(half, reach, base + height));
    if (!slab.IsValid())
      return {};
    if (i > 0) {
      const double angle =
          static_cast<double>(i) * std::numbers::pi / 3.0; // 60 degrees
      glm::mat4 rot(1.0f);
      rot[0][0] = static_cast<float>(std::cos(angle));
      rot[0][1] = static_cast<float>(std::sin(angle));
      rot[1][0] = static_cast<float>(-std::sin(angle));
      rot[1][1] = static_cast<float>(std::cos(angle));
      slab = slab.Transformed(rot);
      if (!slab.IsValid())
        return {};
    }
    hex = i == 0 ? std::move(slab) : hex.Intersection(slab);
    if (!hex.IsValid())
      return {};
  }
  return hex;
}

} // namespace

FastenerFeature::FastenerFeature(const std::string& name, std::string standard,
                                 std::string size, double length)
    : standard(std::move(standard)), size(std::move(size)), length(length) {
  m_name = name;
}

std::string FastenerFeature::Designation() const {
  // "iso4762def" -> "ISO 4762". The stem is the vendored file's name, and the
  // designation is what goes in the BOM and on a purchase order, so it is
  // spelled the way a supplier spells it rather than the way the file is.
  std::string number;
  for (const char c : standard)
    if (std::isdigit(static_cast<unsigned char>(c)))
      number += c;
  std::ostringstream out;
  out << (number.empty() ? standard : "ISO " + number) << " " << size;
  if (length > 0.0) {
    out << "x";
    // Whole millimetres read as whole millimetres: "M6x20", not "M6x20.000000".
    if (std::abs(length - std::round(length)) < 1e-9)
      out << static_cast<long long>(std::llround(length));
    else
      out << length;
  }
  return out.str();
}

bool FastenerFeature::Execute(RegenContext& ctx) {
  std::string error;
  const auto dims = parts::Dimensions(standard, size, &error);
  if (!dims) {
    // The table's own refusal, verbatim: it already names what is stocked and
    // the nearest sizes, which is more than this feature knows.
    SetError(error);
    return false;
  }
  const std::optional<double> nominal = parts::NominalDiameter(size);
  if (!nominal) {
    SetError("\"" + size +
             "\" is not a metric size — a fastener size is a "
             "letter and a number, like M6");
    return false;
  }
  if (length <= 0.0) {
    SetError("a fastener needs a length under the head; " + Designation() +
             " was asked for " + std::to_string(length) + " mm");
    return false;
  }
  if (drive != "hex") {
    // Only the drive this phase generates. Refused by name rather than
    // silently left plain: a screw with no recess cannot be turned and looks
    // perfectly fine in the viewport.
    SetError("drive \"" + drive +
             "\" is not generated yet — this build makes hex (socket) drives; "
             "the cross and hexalobular tables are vendored and unused");
    return false;
  }

  const auto dim = [&](const char* key) -> std::optional<double> {
    const auto it = dims->find(key);
    return it == dims->end() ? std::nullopt : std::optional<double>(it->second);
  };
  const std::optional<double> headDia = dim("dk_max");
  const std::optional<double> headHeight = dim("k");
  const std::optional<double> socket = dim("s_mean");
  const std::optional<double> socketDepth = dim("t");
  if (!headDia || !headHeight || !socket || !socketDepth) {
    SetError(standard + " " + size +
             " has no dk_max/k/s_mean/t — this feature builds a socket-head "
             "screw and that standard does not describe one");
    return false;
  }

  CADKernel& kernel = CADKernel::Instance();
  // Built head-down at the origin: the head sits below z = 0 and the shank
  // runs up +Z. The rim of the head is therefore the z = 0 plane, which is the
  // face that lands on the picked rim when phase 3 places one.
  CADShape head = kernel.CreateCylinder(
      glm::vec3(0.0f, 0.0f, -static_cast<float>(*headHeight)),
      glm::vec3(0.0f, 0.0f, 1.0f), *headDia * 0.5, *headHeight);
  CADShape shank = kernel.CreateCylinder(glm::vec3(0.0f), glm::vec3(0, 0, 1),
                                         *nominal * 0.5, length);
  if (!head.IsValid() || !shank.IsValid()) {
    SetError("could not build " + Designation() + " from its dimensions");
    return false;
  }
  CADShape screw = head.Union(shank);
  if (!screw.IsValid()) {
    SetError("could not join the head and shank of " + Designation());
    return false;
  }
  // The recess, cut from the head's outer face upward. The socket is measured
  // across the flats, which is what `s_mean` is.
  const CADShape recess =
      HexPrism(*socket, -*headHeight - 0.001, *socketDepth + 0.001);
  if (!recess.IsValid()) {
    SetError("could not build the " + std::to_string(*socket) +
             " mm hex socket of " + Designation());
    return false;
  }
  screw = screw.Difference(recess);
  if (!screw.IsValid()) {
    SetError("could not cut the hex socket of " + Designation());
    return false;
  }

  ElementMap map;
  topo::NamePrimitiveElements(screw, std::to_string(GetId()), map);
  ctx.bodies.push_back({m_name.empty() ? Designation() : m_name,
                        std::move(screw), m_material, GetId()});
  ctx.bodies.back().elementMap = std::move(map);
  return true;
}
