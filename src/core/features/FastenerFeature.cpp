#include "core/features/FastenerFeature.h"

#include "cad/CADKernel.h"
#include "common/Logger.h"
#include "core/ElementHistory.h"
#include "core/parts/FastenerProfile.h"
#include "core/parts/FastenerTable.h"

#include <cmath>
#include <numbers>
#include <sstream>

namespace {

using Dims = std::map<std::string, double>;

// The first of these columns the row actually has. Standards spell the same
// dimension differently — a head is `dk_max` in ISO 4762, `dk` in ISO 7380 and
// `csh_diam` in ISO 10642 — and the alternative to listing the spellings is
// normalising the vendored files, which would end the ability to diff them
// against upstream. Absent rather than zero, so a missing dimension refuses.
std::optional<double> First(const Dims& dims,
                            std::initializer_list<const char*> columns) {
  for (const char* column : columns) {
    const auto it = dims.find(column);
    if (it != dims.end())
      return it->second;
  }
  return std::nullopt;
}

std::string Join(std::initializer_list<const char*> columns) {
  std::string out;
  for (const char* column : columns)
    out += (out.empty() ? "" : "/") + std::string(column);
  return out;
}

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

// A hexalobular (torx) recess: ISO 10664's `A` across the lobe tips, `B`
// across the valleys, lobe tip radius `Re`.
//
// Built as the union of a central cylinder of diameter B with six lobe
// cylinders of radius Re, centred on a circle of radius A/2 - Re. That puts
// the tips exactly on diameter A with exactly the standard's tip radius, and
// the valleys exactly on diameter B — the two dimensions a driver is measured
// by. What it does NOT reproduce is the flank between them: the standard joins
// the lobes with a concave arc and this blends them convexly, so the recess is
// very slightly full at the flanks. A driver's contact is at the tips, the
// error is hundredths of a millimetre on every size in the table, and the
// alternative is a wire builder for one recess — but it is an approximation
// and not the standard's profile, which is worth knowing before measuring one.
CADShape TorxPrism(double A, double B, double Re, double base, double height) {
  CADKernel& kernel = CADKernel::Instance();
  const glm::vec3 up(0.0f, 0.0f, 1.0f);
  CADShape torx = kernel.CreateCylinder(
      glm::vec3(0.0f, 0.0f, static_cast<float>(base)), up, B * 0.5, height);
  if (!torx.IsValid())
    return {};
  const double ring = A * 0.5 - Re;
  for (int i = 0; i < 6; i++) {
    const double angle = static_cast<double>(i) * std::numbers::pi / 3.0;
    const CADShape lobe = kernel.CreateCylinder(
        glm::vec3(static_cast<float>(ring * std::cos(angle)),
                  static_cast<float>(ring * std::sin(angle)),
                  static_cast<float>(base)),
        up, Re, height);
    if (!lobe.IsValid())
      return {};
    torx = torx.Union(lobe);
    if (!torx.IsValid())
      return {};
  }
  return torx;
}

} // namespace

FastenerFeature::FastenerFeature(const std::string& name, std::string standard,
                                 std::string size, double length)
    : standard(std::move(standard)), size(std::move(size)), length(length) {
  m_name = name;
}

std::string FastenerFeature::Designation() const {
  // What a supplier calls it, which is what goes in the BOM and on a purchase
  // order. The name comes from the profile table rather than from scraping
  // digits out of the file stem, because the stem is upstream's filename:
  // ISO 4017's is `iso4017head`, and "ISO 4017head" is not a thing to order.
  const parts::Profile* profile = parts::ProfileFor(standard);
  std::ostringstream out;
  out << (profile ? profile->designation : standard) << " " << size;
  // A nut and a washer have no length, so their designation has no "x20".
  const bool hasLength =
      profile == nullptr || profile->kind == parts::Kind::Screw;
  if (hasLength && length > 0.0) {
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
  const parts::Profile* profile = parts::ProfileFor(standard);
  if (profile == nullptr) {
    SetError("\"" + standard +
             "\" is not a standard this build makes — it makes " +
             parts::KnownStandards());
    return false;
  }

  std::string error;
  const auto dims = parts::Dimensions(profile->stem, size, &error);
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

  // The head dimensions, which for ISO 14579 are ISO 4762's: its vendored file
  // carries only the recess, because the standard IS ISO 4762 with a different
  // socket. Named in the profile table rather than inferred here.
  Dims headDims = *dims;
  if (!profile->headFrom.empty()) {
    const auto borrowed = parts::Dimensions(profile->headFrom, size, &error);
    if (!borrowed) {
      SetError(profile->designation + " takes its head from " +
               profile->headFrom + ", and " + error);
      return false;
    }
    headDims = *borrowed;
  }

  // The recess. Empty means "whatever this standard is made with", which is
  // the answer almost always wanted; anything else has to agree with it.
  parts::Drive wantDrive = profile->drive;
  if (!drive.empty()) {
    const std::optional<parts::Drive> named = parts::DriveFromName(drive);
    if (!named) {
      SetError("drive \"" + drive +
               "\" is not one this build cuts — it cuts hex and torx "
               "(cross and hexalobular-by-name are not the same thing); "
               "leave it empty for the one " +
               profile->designation + " is made with");
      return false;
    }
    if (*named != profile->drive) {
      SetError(profile->designation + " is made with a " +
               parts::DriveName(profile->drive) + " drive, not " +
               parts::DriveName(*named) +
               " — a different recess is a different standard, so ask for "
               "that one instead of reconfiguring this one (" +
               parts::KnownStandards() + ")");
      return false;
    }
    wantDrive = *named;
  }

  if (profile->kind != parts::Kind::Screw && length > 0.0) {
    SetError(profile->designation + " is a " + profile->what +
             ", which has no length — " + Designation() + " was asked for " +
             std::to_string(length) +
             " mm. Leave the length out, or ask for a screw.");
    return false;
  }
  if (profile->kind == parts::Kind::Screw && length <= 0.0) {
    SetError("a fastener needs a length under the head; " + Designation() +
             " was asked for " + std::to_string(length) + " mm");
    return false;
  }

  CADKernel& kernel = CADKernel::Instance();
  const glm::vec3 up(0.0f, 0.0f, 1.0f);
  const auto missing = [&](std::initializer_list<const char*> columns) {
    SetError(profile->stem + " " + size + " has no " + Join(columns) +
             ", which a " + profile->what + " is built from");
  };

  // Every fastener is built with its BEARING FACE on z = 0, its body below and
  // its reach above: a screw's head sits at -z and its shank runs +z, a nut
  // and a washer sit entirely at -z. That single convention is what lets the
  // placement layer point one along a hole's axis without knowing which of the
  // three it is holding (docs/parts/plan.md, phase 4).
  CADShape solid;
  double headHeight = 0.0;

  if (profile->kind == parts::Kind::Washer) {
    const std::optional<double> outer = First(*dims, {"d2_max", "d2"});
    const std::optional<double> bore = First(*dims, {"d1_min", "d1"});
    const std::optional<double> thickness = First(*dims, {"h", "h_max"});
    if (!outer || !bore || !thickness) {
      missing({"d2_max", "d1_min", "h"});
      return false;
    }
    CADShape disc = kernel.CreateCylinder(glm::vec3(0.0f, 0.0f, -*thickness),
                                          up, *outer * 0.5, *thickness);
    const CADShape hole =
        kernel.CreateCylinder(glm::vec3(0.0f, 0.0f, -*thickness - 0.001f), up,
                              *bore * 0.5, *thickness + 0.002);
    if (!disc.IsValid() || !hole.IsValid()) {
      SetError("could not build " + Designation() + " from its dimensions");
      return false;
    }
    solid = disc.Difference(hole);
  } else if (profile->kind == parts::Kind::Nut) {
    const std::optional<double> acrossFlats = First(*dims, {"s_nom", "s"});
    const std::optional<double> height = First(*dims, {"m"});
    if (!acrossFlats || !height) {
      missing({"s_nom", "m"});
      return false;
    }
    // The corner chamfers ISO 4032 specifies are omitted, like the thread:
    // both make the model heavier than the part, which is the safe direction
    // and is stated in the header rather than discovered from a mass report.
    CADShape body = HexPrism(*acrossFlats, -*height, *height);
    const CADShape thread =
        kernel.CreateCylinder(glm::vec3(0.0f, 0.0f, -*height - 0.001f), up,
                              *nominal * 0.5, *height + 0.002);
    if (!body.IsValid() || !thread.IsValid()) {
      SetError("could not build " + Designation() + " from its dimensions");
      return false;
    }
    solid = body.Difference(thread);
  } else {
    // A screw: a head, a shank, and a recess in whichever of the two the
    // standard puts one in.
    CADShape head;
    switch (profile->head) {
    case parts::Head::SocketCap: {
      const std::optional<double> dia = First(headDims, {"dk_max", "dk"});
      const std::optional<double> k = First(headDims, {"k"});
      if (!dia || !k) {
        missing({"dk_max", "k"});
        return false;
      }
      headHeight = *k;
      head =
          kernel.CreateCylinder(glm::vec3(0.0f, 0.0f, -*k), up, *dia * 0.5, *k);
      break;
    }
    case parts::Head::HexHead: {
      const std::optional<double> acrossFlats = First(headDims, {"s", "s_nom"});
      const std::optional<double> k = First(headDims, {"k"});
      if (!acrossFlats || !k) {
        missing({"s", "k"});
        return false;
      }
      headHeight = *k;
      head = HexPrism(*acrossFlats, -*k, *k);
      break;
    }
    case parts::Head::Button: {
      const std::optional<double> dia = First(headDims, {"dk", "dk_max"});
      const std::optional<double> k = First(headDims, {"k"});
      if (!dia || !k) {
        missing({"dk", "k"});
        return false;
      }
      headHeight = *k;
      // A spherical cap `k` tall whose flat face is `dk` across: the sphere
      // that does that has radius (r^2 + k^2) / 2k, with its apex at -k.
      const double r = *dia * 0.5;
      const double sphere = (r * r + *k * *k) / (2.0 * *k);
      const CADShape ball = kernel.CreateSphere(
          glm::vec3(0.0f, 0.0f, static_cast<float>(-*k + sphere)), sphere);
      // Clipped BELOW the apex rather than at it. The sphere reaches exactly
      // z = -k, so a box whose floor is -k is tangent to it, and a tangent
      // boolean is the one case OCC resolves badly here: it came back 0.36 mm
      // short, a dome measurably flatter than the standard's and perfectly
      // plausible in a viewport. The extra room costs nothing — there is no
      // sphere down there to keep.
      const CADShape below =
          kernel.CreateBox(glm::vec3(static_cast<float>(-sphere - 1.0),
                                     static_cast<float>(-sphere - 1.0),
                                     static_cast<float>(-*k - 1.0)),
                           glm::vec3(static_cast<float>(sphere + 1.0),
                                     static_cast<float>(sphere + 1.0), 0.0f));
      if (!ball.IsValid() || !below.IsValid()) {
        SetError("could not build the domed head of " + Designation());
        return false;
      }
      head = ball.Intersection(below);
      break;
    }
    case parts::Head::Countersunk: {
      const std::optional<double> dia = First(headDims, {"csh_diam", "dk_max"});
      const std::optional<double> k = First(headDims, {"csh_height", "k"});
      if (!dia || !k) {
        missing({"csh_diam", "csh_height"});
        return false;
      }
      headHeight = *k;
      // A countersunk head is sunk INTO the part, so it lives at +z with its
      // wide face flush on z = 0 — the one head of the four that does. The
      // nominal length is measured over it (profile->lengthOverHead), which is
      // why the shank below runs the whole length rather than starting under
      // the head.
      head = kernel.CreateCone(glm::vec3(0.0f), up, *dia * 0.5, *nominal * 0.5,
                               *k);
      break;
    }
    case parts::Head::None:
      SetError(profile->designation + " has no head to build");
      return false;
    }
    if (!head.IsValid()) {
      SetError("could not build the head of " + Designation() +
               " from its dimensions");
      return false;
    }

    const CADShape shank =
        kernel.CreateCylinder(glm::vec3(0.0f), up, *nominal * 0.5, length);
    if (!shank.IsValid()) {
      SetError("could not build the shank of " + Designation());
      return false;
    }
    solid = head.Union(shank);
    if (!solid.IsValid()) {
      SetError("could not join the head and shank of " + Designation());
      return false;
    }

    if (wantDrive != parts::Drive::None) {
      // The recess is cut from the head's OUTER face inward: that face is
      // z = -k for a head that stands proud and z = 0 for a countersunk one.
      const double face = profile->lengthOverHead ? 0.0 : -headHeight;
      CADShape recess;
      if (wantDrive == parts::Drive::Hex) {
        const std::optional<double> acrossFlats =
            First(*dims, {"s_mean", "s_nom"});
        const std::optional<double> depth =
            First(*dims, {"t", "t_min", "t_mean"});
        if (!acrossFlats || !depth) {
          missing({"s_mean", "t"});
          return false;
        }
        recess = HexPrism(*acrossFlats, face - 0.001, *depth + 0.001);
      } else {
        const std::optional<double> across = First(*dims, {"A"});
        const std::optional<double> depth = First(*dims, {"t_mean", "t"});
        const std::optional<std::string> driver =
            parts::TextField(profile->stem, size, "DriveSize", &error);
        if (!across || !depth || !driver) {
          SetError(profile->stem + " " + size +
                   " does not say which driver it takes (A/t_mean/DriveSize), "
                   "so its hexalobular recess cannot be cut");
          return false;
        }
        // The lobe geometry itself is ISO 10664's, keyed by the driver the
        // screw's own table names — two tables because that is how the
        // standards are written, and the join is the driver size.
        const auto lobe = parts::Dimensions("iso10664def", *driver, &error);
        if (!lobe) {
          SetError("ISO 10664 has no " + *driver + " — " + error);
          return false;
        }
        const std::optional<double> B = First(*lobe, {"B"});
        const std::optional<double> Re = First(*lobe, {"Re"});
        if (!B || !Re) {
          SetError("ISO 10664 " + *driver + " has no B/Re to shape a lobe");
          return false;
        }
        recess = TorxPrism(*across, *B, *Re, face - 0.001, *depth + 0.001);
      }
      if (!recess.IsValid()) {
        SetError("could not build the " + parts::DriveName(wantDrive) +
                 " recess of " + Designation());
        return false;
      }
      solid = solid.Difference(recess);
    }
  }

  if (!solid.IsValid()) {
    SetError("could not build " + Designation() + " from its dimensions");
    return false;
  }

  ElementMap map;
  topo::NamePrimitiveElements(solid, std::to_string(GetId()), map);
  // Steel unless told otherwise. An unmarked fastener IS steel, and half the
  // reason to generate these at all is that `bom` and `mass_properties` are
  // currently blind to the hardware — a screw with no material would leave
  // them exactly as wrong as they are today, in a more convincing way. The
  // BOM says so out loud ("has no material — mass unknown"), which is how
  // this was caught.
  // The SLUG, not the display name: MassProps resolves a body's material
  // through MaterialLibrary::FromSlug, so "Steel" reads as unknown and the BOM
  // says "has no density — mass unknown". Caught by looking at the BOM rather
  // than at the field.
  const std::string material = m_material.empty() ? "steel" : m_material;
  ctx.bodies.push_back({m_name.empty() ? Designation() : m_name,
                        std::move(solid), material, GetId()});
  ctx.bodies.back().elementMap = std::move(map);
  return true;
}
