#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

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

  [[nodiscard]] bool Empty() const { return names.empty(); }

  // The name of the face at `faceIndex`, or nullptr if none.
  [[nodiscard]] const std::string* NameOf(int faceIndex) const {
    const auto it = names.find(faceIndex);
    return it == names.end() ? nullptr : &it->second;
  }

  // The current faceIndex a name resolves to, or nullopt if the name is gone
  // (its generating element was deleted). This is the toponaming lookup:
  // resolution failure is a first-class signal, not a silent renumber.
  [[nodiscard]] std::optional<int> Resolve(const std::string& name) const {
    for (const auto& [index, n] : names) {
      if (n == name) {
        return index;
      }
    }
    return std::nullopt;
  }
};
