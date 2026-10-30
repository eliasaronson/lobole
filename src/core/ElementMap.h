// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// A persistent reference to a named face/edge of a feature's output body
// (topological naming, T3). Stored in features that attach to geometry (e.g. a
// sketch on a body face); resolved each regeneration by finding the body the
// feature produced and looking `name` up in its ElementMap. Unlike a raw
// faceIndex, this survives upstream edits that renumber faces.
struct ElementRef {
  uint64_t featureId = 0; // the feature whose body owns the name
  std::string name;       // a MappedName in that body's ElementMap
  [[nodiscard]] bool IsSet() const { return featureId != 0; }
};

// Stable element names ↔ volatile face indices for one body (topological
// naming, T2). A MappedName encodes *why* a face exists (e.g. an extrude's
// side face from sketch entity 7 is "4/g7", its caps "4/cap0"/"4/cap1"), so a
// reference keyed on the name survives upstream edits even though the raw
// faceIndex (the TopExp_Explorer / picking order) renumbers. Transient: rebuilt
// every regeneration alongside the body's shape, never serialized.
struct ElementMap {
  std::map<int, std::string> names; // faceIndex -> MappedName
  // edgeIndex -> MappedName (T5). Edge indices follow the deduplicated
  // first-appearance order of TopExp::MapShapes(shape, TopAbs_EDGE) — unlike
  // faces, a raw explorer walk visits shared edges once per adjacent face.
  std::map<int, std::string> edgeNames;
  // Other names an edge ANSWERS to, never hands out: an edge two named
  // faces share takes its U name from one of them by a fixed rule
  // (docs/modeling/toponaming.md#how-each-operation-names-its-output), and
  // the other face's U name for it -- the one a document saved before v216
  // may hold, when the naming face followed the result's face order -- is
  // kept here so that reference still resolves. Only on the body the U pass
  // ran on; an edge renamed by a later operation leaves its aliases behind.
  std::map<std::string, int> edgeAliases;

  [[nodiscard]] bool Empty() const {
    return names.empty() && edgeNames.empty();
  }

  // The name of the face at `faceIndex`, or nullptr if none.
  [[nodiscard]] const std::string* NameOf(int faceIndex) const {
    const auto it = names.find(faceIndex);
    return it == names.end() ? nullptr : &it->second;
  }

  // The name of the edge at `edgeIndex`, or nullptr if none.
  [[nodiscard]] const std::string* EdgeNameOf(int edgeIndex) const {
    const auto it = edgeNames.find(edgeIndex);
    return it == edgeNames.end() ? nullptr : &it->second;
  }

  // The current faceIndex a name resolves to, or nullopt if the name is gone
  // (its generating element was deleted). This is the toponaming lookup:
  // resolution failure is a first-class signal, not a silent renumber.
  // Three steps (docs/modeling/toponaming.md#resolving-a-name): the exact
  // name; else, for a split piece "<stem>[a|b]", the one current piece of
  // <stem> bounded by a and b (or <stem> itself, when it is whole again);
  // else, for a positional piece "<stem>.<i>" saved before pieces were
  // named by their neighbours, the i-th piece of <stem> in result order --
  // noted on any PositionalLookups alive on this thread, so the
  // regeneration warns. Anything else does not resolve:
  // topo::UnresolvedReason says why.
  [[nodiscard]] std::optional<int> Resolve(const std::string& name) const;

  // The current edgeIndex a name resolves to, or nullopt if the name is gone
  // (e.g. the edge was consumed by a fillet or boolean). The same three
  // steps as Resolve.
  [[nodiscard]] std::optional<int> ResolveEdge(const std::string& name) const;
};

// Collects the names resolved BY POSITION (ElementMap::Resolve's third step)
// on this thread while it is alive: what a regeneration turns into a warning
// on the feature that holds the reference. Scopes nest; the innermost hears.
class PositionalLookups {
public:
  PositionalLookups();
  ~PositionalLookups();
  PositionalLookups(const PositionalLookups&) = delete;
  PositionalLookups& operator=(const PositionalLookups&) = delete;
  [[nodiscard]] const std::vector<std::string>& Names() const {
    return m_names;
  }
  // Records `name` on the innermost live scope of this thread, if any.
  static void Note(const std::string& name);

private:
  std::vector<std::string> m_names;
  PositionalLookups* m_outer = nullptr;
};
