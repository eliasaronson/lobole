// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/ElementOrigin.h"

#include "core/ElementMap.h"

#include <algorithm>
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
    {ElementRole::Roof, "roof"},
    {ElementRole::Outside, "outside"},
    {ElementRole::InnerSide, "inner_side"},
    {ElementRole::OuterSide, "outer_side"},
    {ElementRole::Shield, "shield"},
    {ElementRole::RingSide, "ring_side"},
    {ElementRole::GrooveFloor, "groove_floor"},
    {ElementRole::GrooveFlank, "groove_flank"},
    {ElementRole::Mesh, "mesh"},
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

// The seed's words after its tag: "g7", "g7.2.0.5", "g7@h2", "cap1@g3",
// "b1/face4", "p5/bore".
bool DecodeSeed(const std::vector<std::string>& words, ElementOrigin& o) {
  if (words.empty())
    return false;
  std::string last = words.back();
  // A herringbone's upper half names its sides as the lower's, plus "@h2"
  // (docs/modeling/README.md#twisted-extrudes): a side all the same.
  if (last.size() > 3 && last.compare(last.size() - 3, 3, "@h2") == 0 &&
      last.rfind("cap", 0) != 0)
    last.resize(last.size() - 3);
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
        const bool holeRole =
            r.role == ElementRole::Bore || r.role == ElementRole::Bottom ||
            r.role == ElementRole::Counterbore ||
            r.role == ElementRole::CounterboreFloor ||
            r.role == ElementRole::Countersink ||
            r.role == ElementRole::DrillPoint || r.role == ElementRole::Roof;
        if (holeRole && word == r.name) {
          o.role = r.role;
          o.entity = *e;
          return true;
        }
      }
      return false;
    }
  }
  // A mesh body's one element, maybe behind an import's body segment.
  if (last == "mesh" &&
      (words.size() == 1 || (words.size() == 2 && Numbered(words[0], "b")))) {
    o.role = ElementRole::Mesh;
    return true;
  }
  // Positional: "face<i>", maybe behind a sub-body segment (b<k>, j<k>,
  // bend<k>, f<k> -- a split, a jog, a bend, a flange).
  if (Numbered(last, "face")) {
    o.role = ElementRole::Positional;
    return true;
  }
  return false;
}

// One step of a name, read as a piece of a split: "M<tag>", then a bracketed
// qualifier and/or an ordinal ".<i>", then nothing, or a pattern suffix
// ("#2") or the next comma part of an L name. `stripped` is the step with
// the qualifier and ordinal taken out.
struct PieceStep {
  bool piece = false;
  bool qualified = false;
  std::string content; // the bracket's
  int index = -1;
  uint64_t tag = 0;
  std::string stripped;
};

PieceStep ReadPieceStep(const std::string& st) {
  PieceStep out;
  out.stripped = st;
  if (st.size() < 2 || (st[0] != 'M' && st[0] != 'G') ||
      !std::isdigit(static_cast<unsigned char>(st[1])))
    return out;
  size_t j = 1;
  uint64_t tag = 0;
  while (j < st.size() && std::isdigit(static_cast<unsigned char>(st[j])))
    tag = tag * 10 + static_cast<uint64_t>(st[j++] - '0');
  const size_t head = j;
  PieceStep p;
  p.tag = tag;
  if (j < st.size() && st[j] == '[') {
    int depth = 0;
    size_t k = j;
    for (; k < st.size(); k++) {
      if (st[k] == '[')
        depth++;
      else if (st[k] == ']' && --depth == 0)
        break;
    }
    if (k >= st.size())
      return out; // unbalanced: not a qualifier
    p.qualified = true;
    p.content = st.substr(j + 1, k - j - 1);
    j = k + 1;
  }
  if (j + 1 < st.size() && st[j] == '.' &&
      std::isdigit(static_cast<unsigned char>(st[j + 1]))) {
    size_t k = j + 1;
    int v = 0;
    while (k < st.size() && std::isdigit(static_cast<unsigned char>(st[k])))
      v = v * 10 + (st[k++] - '0');
    p.index = v;
    j = k;
  }
  if (!p.qualified && p.index < 0)
    return out;
  if (j < st.size() && st[j] != '#' && st[j] != ',')
    return out;
  p.piece = true;
  p.stripped = st.substr(0, head) + st.substr(j);
  return p;
}

std::string JoinSteps(const std::vector<std::string>& steps) {
  std::string out;
  for (size_t i = 0; i < steps.size(); i++)
    out += (i == 0 ? "" : ";") + steps[i];
  return out;
}

// Splits a qualifier's content on '|' outside any bracket.
std::vector<std::string> SplitBounds(const std::string& content) {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  for (const char c : content) {
    if (c == '[')
      depth++;
    else if (c == ']')
      depth--;
    if (c == '|' && depth == 0) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty() || !out.empty())
    out.push_back(cur);
  return out;
}

// The lookup behind ElementMap::Resolve and ResolveEdge (see ElementMap.h).
std::optional<int> LookUp(const std::map<int, std::string>& names,
                          const std::string& name) {
  for (const auto& [index, n] : names)
    if (n == name)
      return index;
  // Cheap test before any parsing: a piece name has a '[' or a '.'.
  if (name.find_first_of("[.") == std::string::npos)
    return std::nullopt;
  const PieceName want = ParsePiece(name);
  if (!want.piece)
    return std::nullopt;
  const std::string prefix = want.of.substr(0, want.of.find(';'));
  std::vector<int> pieces, bounded;
  std::optional<int> whole;
  for (const auto& [index, n] : names) {
    if (n.compare(0, prefix.size(), prefix) != 0)
      continue;
    if (n == want.of) {
      whole = index;
      continue;
    }
    const PieceName have = ParsePiece(n);
    if (!have.piece || have.of != want.of)
      continue;
    pieces.push_back(index);
    if (want.qualified &&
        std::all_of(want.boundedBy.begin(), want.boundedBy.end(),
                    [&](const std::string& b) {
                      return std::find(have.boundedBy.begin(),
                                       have.boundedBy.end(),
                                       b) != have.boundedBy.end();
                    }))
      bounded.push_back(index);
  }
  if (want.qualified) {
    // 2. the same piece: the one bounded by everything it was bounded by;
    // or the whole face, when the split is gone.
    if (bounded.size() == 1)
      return bounded.front();
    if (bounded.size() > 1 && want.ordinal && want.index >= 0 &&
        static_cast<size_t>(want.index) < bounded.size()) {
      PositionalLookups::Note(name);
      return bounded[static_cast<size_t>(want.index)];
    }
    if (bounded.empty() && pieces.empty() && whole)
      return whole;
    return std::nullopt;
  }
  // 3. a positional name from before pieces were qualified: the i-th piece
  // in result order, the rule the build that saved it used.
  if (want.index >= 0 && static_cast<size_t>(want.index) < pieces.size()) {
    PositionalLookups::Note(name);
    return pieces[static_cast<size_t>(want.index)];
  }
  return std::nullopt;
}

thread_local PositionalLookups* t_positional = nullptr;
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

ElementOrigin DecodeElementName(const std::string& qualified) {
  ElementOrigin o;
  o.piece = ParsePiece(qualified);
  // A qualifier's neighbours are not what the element is: read without.
  const std::string name = WithoutQualifiers(qualified);
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

std::vector<uint64_t> FeatureIdsMentioned(const std::string& qualified) {
  // A piece's neighbours are not features that made or changed it.
  const std::string name = WithoutQualifiers(qualified);
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
  bool partStart = true; // after the start, a ';', a ',', a '[' or a '|'
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
    // A qualifier's neighbours are names too: their ids move with the rest.
    if (c == ';' || c == ',' || c == '[' || c == '|')
      partStart = true;
    out += c;
    ++i;
  }
  return out;
}

std::vector<std::string> NameSteps(const std::string& name) {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  for (const char c : name) {
    if (c == '[')
      depth++;
    else if (c == ']' && depth > 0)
      depth--;
    if (c == ';' && depth == 0) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

std::string WithoutQualifiers(const std::string& name) {
  if (name.find('[') == std::string::npos)
    return name;
  std::string out;
  int depth = 0;
  for (const char c : name) {
    if (c == '[') {
      depth++;
      continue;
    }
    if (c == ']' && depth > 0) {
      depth--;
      continue;
    }
    if (depth == 0)
      out += c;
  }
  return out;
}

std::string PieceStem(const std::string& name) {
  if (name.find_first_of("[.") == std::string::npos)
    return name;
  std::vector<std::string> steps = NameSteps(name);
  for (size_t i = 1; i < steps.size(); i++) {
    const PieceStep p = ReadPieceStep(steps[i]);
    if (p.piece)
      steps[i] = p.stripped;
  }
  return WithoutQualifiers(JoinSteps(steps));
}

PieceName ParsePiece(const std::string& name) {
  PieceName out;
  if (name.find_first_of("[.") == std::string::npos)
    return out;
  std::vector<std::string> steps = NameSteps(name);
  for (size_t i = steps.size(); i-- > 1;) {
    const PieceStep p = ReadPieceStep(steps[i]);
    if (!p.piece)
      continue;
    out.piece = true;
    out.qualified = p.qualified;
    if (p.qualified)
      out.boundedBy = SplitBounds(p.content);
    out.ordinal = p.index >= 0;
    out.index = p.index;
    out.splitBy = p.tag;
    steps[i] = p.stripped;
    out.of = JoinSteps(steps);
    return out;
  }
  return out;
}

Descent DescentOf(const std::string& name, const std::string& later,
                  uint64_t* splitBy) {
  if (later.size() <= name.size() || later.compare(0, name.size(), name) != 0 ||
      later[name.size()] != ';')
    return Descent::None;
  bool split = false;
  const std::vector<std::string> steps =
      NameSteps(later.substr(name.size() + 1));
  for (const std::string& st : steps) {
    if (st.size() < 2 || st[0] != 'M' ||
        !std::isdigit(static_cast<unsigned char>(st[1])))
      return Descent::Other;
    const PieceStep p = ReadPieceStep(st);
    const std::string& bare = p.piece ? p.stripped : st;
    for (size_t k = 1; k < bare.size(); k++)
      if (!std::isdigit(static_cast<unsigned char>(bare[k])))
        return Descent::Other; // a pattern copy's, an L name's, ...
    if (p.piece) {
      if (!split && splitBy != nullptr)
        *splitBy = p.tag;
      split = true;
    }
  }
  return split ? Descent::Split : Descent::Modified;
}

} // namespace topo

std::optional<int> ElementMap::Resolve(const std::string& name) const {
  return topo::LookUp(names, name);
}

std::optional<int> ElementMap::ResolveEdge(const std::string& name) const {
  if (std::optional<int> found = topo::LookUp(edgeNames, name))
    return found;
  // The other face's U name for an edge (edgeAliases).
  if (const auto it = edgeAliases.find(name); it != edgeAliases.end())
    return it->second;
  return std::nullopt;
}

PositionalLookups::PositionalLookups() : m_outer(topo::t_positional) {
  topo::t_positional = this;
}

PositionalLookups::~PositionalLookups() { topo::t_positional = m_outer; }

void PositionalLookups::Note(const std::string& name) {
  if (topo::t_positional != nullptr)
    topo::t_positional->m_names.push_back(name);
}
