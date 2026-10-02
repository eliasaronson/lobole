#include "core/ElementOrigin.h"

#include <cctype>
#include <optional>
#include <utility>

namespace topo {

namespace {

struct RoleSpelling {
  ElementRole role;
  const char* name;
};

constexpr RoleSpelling kRoles[] = {
    {ElementRole::StartCap, "start_cap"},
    {ElementRole::EndCap, "end_cap"},
    {ElementRole::Side, "side"},
    {ElementRole::Generated, "generated"},
    {ElementRole::Pole, "pole"},
    {ElementRole::Seam, "seam"},
    {ElementRole::Positional, "positional"},
    {ElementRole::Boundary, "boundary"},
    {ElementRole::Bore, "bore"},
    {ElementRole::Bottom, "bottom"},
    {ElementRole::Counterbore, "counterbore"},
    {ElementRole::CounterboreFloor, "counterbore_floor"},
    {ElementRole::Countersink, "countersink"},
    {ElementRole::DrillPoint, "point"},
    {ElementRole::Outside, "outside"},
    {ElementRole::InnerSide, "inner_side"},
    {ElementRole::OuterSide, "outer_side"},
    {ElementRole::Shield, "shield"},
    {ElementRole::RingSide, "ring_side"},
    {ElementRole::GrooveFloor, "groove_floor"},
    {ElementRole::GrooveFlank, "groove_flank"},
};

bool AllDigits(const std::string& s) {
  if (s.empty())
    return false;
  for (const char c : s)
    if (!std::isdigit(static_cast<unsigned char>(c)))
      return false;
  return true;
}

std::optional<uint64_t> LeadingId(const std::string& s, size_t& pos) {
  uint64_t v = 0;
  size_t i = pos;
  bool any = false;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
    v = v * 10 + static_cast<uint64_t>(s[i] - '0');
    any = true;
    ++i;
  }
  if (!any)
    return std::nullopt;
  pos = i;
  return v;
}

std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    const size_t at = s.find(sep, start);
    out.push_back(s.substr(start, at == std::string::npos ? std::string::npos
                                                          : at - start));
    if (at == std::string::npos)
      return out;
    start = at + 1;
  }
}

// "cap1#2" -> "cap1", instances += "2". A pattern suffix can sit on any
// token, not only the last.
std::string StripInstances(const std::string& tok,
                           std::vector<std::string>& instances) {
  const size_t hash = tok.find('#');
  if (hash == std::string::npos)
    return tok;
  for (const std::string& inst : Split(tok.substr(hash + 1), '#'))
    instances.push_back(inst);
  return tok.substr(0, hash);
}

// A word that is `prefix` followed by digits: its number.
std::optional<uint32_t> Numbered(const std::string& word, const char* prefix) {
  const std::string p(prefix);
  if (word.size() <= p.size() || word.compare(0, p.size(), p) != 0)
    return std::nullopt;
  const std::string rest = word.substr(p.size());
  if (!AllDigits(rest))
    return std::nullopt;
  return static_cast<uint32_t>(std::stoul(rest));
}

// A text label's side-face word, "g<entity>.<glyph>.<contour>.<segment>":
// one label is ONE sketch entity, so the sub-index is what keeps its faces
// apart (docs/modeling/toponaming.md). The entity id, or nullopt.
std::optional<uint32_t> TextSideEntity(const std::string& word) {
  const std::vector<std::string> parts = Split(word, '.');
  if (parts.size() != 4)
    return std::nullopt;
  for (size_t i = 1; i < parts.size(); i++)
    if (!AllDigits(parts[i]))
      return std::nullopt;
  return Numbered(parts[0], "g");
}

// The seed's words after its tag: "g7", "g7.2.0.5", "cap1@g3", "b1/face4",
// "p5/bore".
bool DecodeSeed(const std::vector<std::string>& words, ElementOrigin& o) {
  if (words.empty())
    return false;
  const std::string& last = words.back();
  if (words.size() == 1) {
    if (const auto e = Numbered(last, "g")) {
      o.role = ElementRole::Side;
      o.entity = *e;
      return true;
    }
    if (const auto e = TextSideEntity(last)) {
      o.role = ElementRole::Side;
      o.entity = *e;
      return true;
    }
    const std::string cap = last.substr(0, last.find('@'));
    if (cap == "cap0" || cap == "cap1") {
      o.role = cap == "cap0" ? ElementRole::StartCap : ElementRole::EndCap;
      return true;
    }
    if (Numbered(last, "pole")) {
      o.role = ElementRole::Pole;
      return true;
    }
    if (Numbered(last, "seam")) {
      o.role = ElementRole::Seam;
      return true;
    }
    // A generated bearing's faces by what they are, a side's two faces
    // told apart by _a/_b (docs/parts/README.md#bearings).
    const std::string side =
        last.size() > 2 && (last.ends_with("_a") || last.ends_with("_b"))
            ? last.substr(0, last.size() - 2)
            : std::string{};
    if (last == "bore" || last == "outside" || side == "inner_side" ||
        side == "outer_side" || side == "shield") {
      o.role = last == "bore"         ? ElementRole::Bore
               : last == "outside"    ? ElementRole::Outside
               : side == "inner_side" ? ElementRole::InnerSide
               : side == "outer_side" ? ElementRole::OuterSide
                                      : ElementRole::Shield;
      return true;
    }
    // A retaining ring's sides, and the groove one sits in, its two flanks
    // told apart the same way (docs/parts/README.md#retaining-rings).
    if (side == "ring_side" || side == "groove_flank" ||
        last == "groove_floor") {
      o.role = side == "ring_side"      ? ElementRole::RingSide
               : side == "groove_flank" ? ElementRole::GrooveFlank
                                        : ElementRole::GrooveFloor;
      return true;
    }
  }
  // A hole's faces by role, keyed by the sketch point they were drilled at;
  // ".n" keeps two pieces of one role apart, and a face with no role (the
  // entry disc of a hole started inside the material) is positional.
  if (words.size() == 2) {
    if (const auto e = Numbered(words[0], "p")) {
      const std::string word = last.substr(0, last.find('.'));
      if (Numbered(word, "face")) {
        o.role = ElementRole::Positional;
        o.entity = *e;
        return true;
      }
      for (const RoleSpelling& r : kRoles) {
        const bool holeRole = r.role == ElementRole::Bore ||
                              r.role == ElementRole::Bottom ||
                              r.role == ElementRole::Counterbore ||
                              r.role == ElementRole::CounterboreFloor ||
                              r.role == ElementRole::Countersink ||
                              r.role == ElementRole::DrillPoint;
        if (holeRole && word == r.name) {
          o.role = r.role;
          o.entity = *e;
          return true;
        }
      }
      return false;
    }
  }
  // Positional: "face<i>", maybe behind a sub-body segment (b<k>, j<k>,
  // bend<k>, f<k> -- a split, a jog, a bend, a flange).
  if (Numbered(last, "face")) {
    o.role = ElementRole::Positional;
    return true;
  }
  return false;
}

} // namespace

const char* ElementRoleName(ElementRole role) {
  for (const RoleSpelling& r : kRoles)
    if (r.role == role)
      return r.name;
  return "unknown";
}

bool ElementRoleFromString(const std::string& s, ElementRole& out) {
  for (const RoleSpelling& r : kRoles)
    if (s == r.name) {
      out = r.role;
      return true;
    }
  return false;
}

std::string ElementRoleList() {
  std::string out;
  for (const RoleSpelling& r : kRoles)
    out += (out.empty() ? "" : ", ") + std::string(r.name);
  return out;
}

ElementOrigin DecodeElementName(const std::string& name) {
  ElementOrigin o;
  const std::vector<std::string> tokens = Split(name, ';');
  // The seed is the first token's first comma part (an L name joins the
  // names of its edges with commas).
  const std::string seed = Split(tokens[0], ',')[0];
  std::vector<std::string> words;
  for (const std::string& w : Split(seed, '/'))
    words.push_back(StripInstances(w, o.instances));
  // Leading ids: Derived pairs (derived id, source created_by), then the
  // seed's tag.
  size_t n = 0;
  while (n < words.size() && AllDigits(words[n]))
    n++;
  if (n == 0 || n % 2 == 0 || n == words.size())
    return o;
  for (size_t i = 0; i + 1 < n; i += 2)
    o.derived.push_back({std::stoull(words[i]), std::stoull(words[i + 1])});
  o.seedTag = std::stoull(words[n - 1]);
  o.madeBy = o.seedTag;
  if (!DecodeSeed({words.begin() + static_cast<long>(n), words.end()}, o))
    o.role = ElementRole::Unknown;

  for (size_t t = 1; t < tokens.size(); t++) {
    const std::string tok = StripInstances(tokens[t], o.instances);
    // An L name's inner edge names ("U3,4/g2") belong to the edges it was
    // named from, not to it.
    const std::string step = Split(tok, ',')[0];
    if (step.empty())
      return o;
    size_t pos = 1;
    switch (step[0]) {
    case 'S': // carried to the far side of a jog: moved, so modified
    case 'M':
      if (const auto id = LeadingId(step, pos))
        o.modifiedBy.push_back(*id);
      else
        return o;
      break;
    case 'G':
      if (const auto id = LeadingId(step, pos)) {
        o.madeBy = *id;
        o.role = ElementRole::Generated;
        o.entity = 0;
      } else {
        return o;
      }
      break;
    case 'U':
      break; // an edge of the face named so far: its origin is the face's
    case 'L':
      if (step != "L")
        return o;
      // Named from the edges round it: nothing says which feature made it.
      o.role = ElementRole::Boundary;
      o.madeBy = 0;
      o.entity = 0;
      o.modifiedBy.clear();
      break;
    default:
      return o;
    }
  }
  o.ok = true;
  return o;
}

std::vector<uint64_t> FeatureIdsMentioned(const std::string& name) {
  std::vector<uint64_t> ids;
  for (const std::string& token : Split(name, ';')) {
    for (const std::string& part : Split(token, ',')) {
      if (part.empty())
        continue;
      size_t pos = 0;
      if (part[0] == 'M' || part[0] == 'G') {
        pos = 1;
        if (const auto id = LeadingId(part, pos))
          ids.push_back(*id);
      } else if (std::isdigit(static_cast<unsigned char>(part[0]))) {
        if (const auto id = LeadingId(part, pos))
          if (pos < part.size() && part[pos] == '/')
            ids.push_back(*id);
      }
    }
  }
  return ids;
}

std::string RewriteIds(const std::string& name,
                       const std::function<uint64_t(uint64_t)>& map) {
  std::string out;
  out.reserve(name.size());
  size_t i = 0;
  bool partStart = true; // after the start, a ';' or a ','
  const auto digits = [&](size_t from) {
    size_t e = from;
    while (e < name.size() && std::isdigit(static_cast<unsigned char>(name[e])))
      ++e;
    return e;
  };
  while (i < name.size()) {
    const char c = name[i];
    if (partStart) {
      partStart = false;
      if (std::isdigit(static_cast<unsigned char>(c))) {
        const size_t e = digits(i);
        if (e < name.size() && name[e] == '/') {
          out += std::to_string(map(std::stoull(name.substr(i, e - i))));
          i = e;
          continue;
        }
      } else if ((c == 'M' || c == 'G' || c == 'S') && i + 1 < name.size() &&
                 std::isdigit(static_cast<unsigned char>(name[i + 1]))) {
        const size_t e = digits(i + 1);
        out += c;
        out += std::to_string(map(std::stoull(name.substr(i + 1, e - i - 1))));
        i = e;
        continue;
      }
    }
    if (c == ';' || c == ',')
      partStart = true;
    out += c;
    ++i;
  }
  return out;
}

} // namespace topo
