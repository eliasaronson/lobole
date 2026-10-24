// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/io/ImportAssembly.h"

#include "common/Logger.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cctype>
#include <filesystem>
#include <set>

namespace interchange {

namespace {

// A product name as a filesystem-safe stem.
std::string SanitizeStem(const std::string& name) {
  std::string out;
  for (char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_';
    out += ok ? c : '_';
  }
  return out.empty() ? "part" : out;
}

} // namespace

bool LoadStructured(const std::string& path, CADKernel::ImportedModel& out,
                    CADKernel::ImportProgress* progress) {
  std::string ext = std::filesystem::path(path).extension().string();
  for (char& c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (ext == ".igs" || ext == ".iges")
    return CADKernel::Instance().LoadIGESStructured(path, out, progress);
  if (ext == ".step" || ext == ".stp")
    return CADKernel::Instance().LoadSTEPStructured(path, out, progress);
  return false;
}

AssemblyImportPlan BuildAssemblyImportPlan(const std::string& stepPath,
                                           const std::string& destDir,
                                           UpAxis up, double turnDeg) {
  CADKernel::ImportedModel model;
  if (!LoadStructured(stepPath, model)) {
    AssemblyImportPlan plan;
    plan.error = "could not read '" + stepPath + "' as STEP or IGES";
    return plan;
  }
  return BuildAssemblyImportPlan(model, stepPath, destDir, nullptr, up,
                                 turnDeg);
}

AssemblyImportPlan
BuildAssemblyImportPlan(const CADKernel::ImportedModel& model,
                        const std::string& stepPath, const std::string& destDir,
                        const ExtractionProgress& onProduct, UpAxis up,
                        double turnDeg) {
  AssemblyImportPlan plan;
  plan.up = DecideUpAxis(up, model.origin, turnDeg);
  const glm::dmat4 R = UpAxisRotation(plan.up.applied, turnDeg);
  const bool turned = R != glm::dmat4(1.0);

  if (model.instances.empty()) {
    plan.error = "no importable shapes in '" + stepPath + "'";
    return plan;
  }

  std::error_code ec;
  std::filesystem::create_directories(destDir, ec);
  if (ec) {
    plan.error = "could not create '" + destDir + "': " + ec.message();
    return plan;
  }

  // One part per product: geometry re-centered at the part origin (parts
  // model around their own origin; the offset moves into the occurrence).
  std::vector<glm::dvec3> centers(model.products.size(), glm::dvec3(0.0));
  std::vector<bool> productUsable(model.products.size(), false);
  std::set<std::string> usedNames;
  for (size_t i = 0; i < model.products.size(); i++) {
    const auto& product = model.products[i];
    if (!product.shape.IsValid())
      continue;

    glm::dvec3 min(0.0), max(0.0);
    product.shape.GetBoundingBox(min, max);
    centers[i] = (min + max) * 0.5;

    std::string base = !product.name.empty()
                           ? product.name
                           : "Part " + std::to_string(plan.parts.size() + 1);
    std::string name = base;
    for (int n = 2; usedNames.count(name); n++)
      name = base + " " + std::to_string(n);
    usedNames.insert(name);

    CADShape centered = product.shape.Transformed(glm::dmat4(1.0));
    centered.Translate(-centers[i]);
    const std::string brep =
        (std::filesystem::path(destDir) /
         (SanitizeStem(name) + "-" + std::to_string(i) + ".brep"))
            .string();
    if (!CADKernel::Instance().SaveBREP(brep, centered)) {
      plan.error = "could not write '" + brep + "'";
      return plan;
    }
    productUsable[i] = true;
    // The part tab turns itself (the .brep is in the file's axes) and keeps
    // the .brep's origin when it does, so the placement below is exact.
    plan.parts.push_back({name, brep, product.hasColor, product.color,
                          plan.up.applied, turnDeg, turned});
    if (onProduct && !onProduct(i + 1, model.products.size())) {
      plan.error = "import cancelled";
      return plan;
    }
  }
  if (plan.parts.empty()) {
    plan.error = "no valid product geometry in '" + stepPath + "'";
    return plan;
  }

  // Products vector indexes differ from parts once invalid ones are skipped.
  std::vector<size_t> partOf(model.products.size(), SIZE_MAX);
  for (size_t i = 0, next = 0; i < model.products.size(); i++)
    if (productUsable[i])
      partOf[i] = next++;

  for (const auto& inst : model.instances) {
    if (inst.product >= partOf.size() || partOf[inst.product] == SIZE_MAX)
      continue;
    // Part geometry is centered, so the placement composes the instance
    // transform with the centering offset: world = M * T(center). The part
    // tab holds R * (centred geometry), and the whole file is turned by R,
    // so the occurrence is R * M * T(center) * R^-1.
    const glm::dmat4 placed =
        R * inst.transform *
        glm::translate(glm::dmat4(1.0), centers[inst.product]) *
        glm::inverse(R);
    PlannedOccurrence occ;
    occ.part = partOf[inst.product];
    occ.name = !inst.name.empty() ? inst.name : plan.parts[occ.part].name;
    occ.position = glm::dvec3(placed[3]);
    occ.orientation = glm::normalize(glm::quat_cast(glm::dmat3(placed)));
    plan.occurrences.push_back(std::move(occ));
  }

  LOG_INFO("Assembly import plan: {} parts, {} occurrences from '{}'",
           plan.parts.size(), plan.occurrences.size(), stepPath);
  return plan;
}

} // namespace interchange
