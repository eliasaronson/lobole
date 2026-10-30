// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The 3D files an Import reads, by extension (docs/interchange/README.md):
// one list, so the refusal, the file dialogs and the RPC say the same thing.

#include <filesystem>
#include <string>

namespace interchange {

enum class ImportFormat { None, Step, Iges, Brep, Stl, Obj, Ply, Gltf };

// What every refusal of another extension names.
inline constexpr const char* kImportExtensions =
    ".step/.stp, .igs/.iges, .brep, .stl, .obj, .ply, .glb/.gltf";

// Lower-cased: ".STL" -- how half the world's scanners and slicers write it
// -- reads as ".stl".
inline std::string LowerExtension(const std::string& path) {
  std::string ext = std::filesystem::path(path).extension().string();
  for (char& c : ext)
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  return ext;
}

inline ImportFormat FormatOf(const std::string& path) {
  const std::string ext = LowerExtension(path);
  if (ext == ".step" || ext == ".stp")
    return ImportFormat::Step;
  if (ext == ".igs" || ext == ".iges")
    return ImportFormat::Iges;
  if (ext == ".brep")
    return ImportFormat::Brep;
  if (ext == ".stl")
    return ImportFormat::Stl;
  if (ext == ".obj")
    return ImportFormat::Obj;
  if (ext == ".ply")
    return ImportFormat::Ply;
  if (ext == ".glb" || ext == ".gltf")
    return ImportFormat::Gltf;
  return ImportFormat::None;
}

// "STEP", "IGES", "BREP", "STL", "OBJ", "PLY", "glTF".
inline const char* FormatName(ImportFormat f) {
  switch (f) {
  case ImportFormat::Step:
    return "STEP";
  case ImportFormat::Iges:
    return "IGES";
  case ImportFormat::Brep:
    return "BREP";
  case ImportFormat::Stl:
    return "STL";
  case ImportFormat::Obj:
    return "OBJ";
  case ImportFormat::Ply:
    return "PLY";
  case ImportFormat::Gltf:
    return "glTF";
  case ImportFormat::None:
    break;
  }
  return "";
}

// A triangle mesh: what `representation` applies to.
inline bool IsMeshFormat(ImportFormat f) {
  return f == ImportFormat::Stl || f == ImportFormat::Obj ||
         f == ImportFormat::Ply || f == ImportFormat::Gltf;
}

// Carries no unit, so the import's `units` says what it was drawn in. STEP
// and IGES declare theirs, and glTF is metres by its specification.
inline bool TakesUnits(ImportFormat f) {
  return f == ImportFormat::Stl || f == ImportFormat::Obj ||
         f == ImportFormat::Ply || f == ImportFormat::Brep;
}

// Why `units` is refused on a file that is not TakesUnits.
inline std::string DeclaredUnit(ImportFormat f) {
  if (f == ImportFormat::Gltf)
    return "glTF, which is in metres by its specification (read as mm)";
  return std::string(FormatName(f)) + ", which declares its own unit";
}

} // namespace interchange
