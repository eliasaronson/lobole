#include "core/io/ImportAssembly.h"

#include "Logger.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

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

AssemblyImportPlan BuildAssemblyImportPlan(const std::string& stepPath,
                                           const std::string& destDir) {
  AssemblyImportPlan plan;

  CADKernel::ImportedModel model;
  if (!CADKernel::Instance().LoadSTEPStructured(stepPath, model)) {
    plan.error = "could not read '" + stepPath + "' as STEP";
    return plan;
  }
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
  std::vector<glm::vec3> centers(model.products.size(), glm::vec3(0.0f));
  std::vector<bool> productUsable(model.products.size(), false);
  std::set<std::string> usedNames;
  for (size_t i = 0; i < model.products.size(); i++) {
    const auto& product = model.products[i];
    if (!product.shape.IsValid())
      continue;

    glm::vec3 min(0.0f), max(0.0f);
    product.shape.GetBoundingBox(min, max);
    centers[i] = (min + max) * 0.5f;

    std::string base =
        !product.name.empty() ? product.name
                              : "Part " + std::to_string(plan.parts.size() + 1);
    std::string name = base;
    for (int n = 2; usedNames.count(name); n++)
      name = base + " " + std::to_string(n);
    usedNames.insert(name);

    CADShape centered = product.shape.Transformed(glm::mat4(1.0f));
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
    plan.parts.push_back({name, brep, product.hasColor, product.color});
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
    // transform with the centering offset: world = M * T(center).
    const glm::mat4 placed =
        inst.transform * glm::translate(glm::mat4(1.0f), centers[inst.product]);
    PlannedOccurrence occ;
    occ.part = partOf[inst.product];
    occ.name = !inst.name.empty() ? inst.name : plan.parts[occ.part].name;
    occ.position = glm::vec3(placed[3]);
    occ.orientation = glm::normalize(glm::quat_cast(glm::mat3(placed)));
    plan.occurrences.push_back(std::move(occ));
  }

  LOG_INFO("Assembly import plan: {} parts, {} occurrences from '{}'",
           plan.parts.size(), plan.occurrences.size(), stepPath);
  return plan;
}

} // namespace interchange
