#pragma once

#include <map>
#include <optional>
#include <string>

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
