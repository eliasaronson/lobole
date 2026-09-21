#include "imgui.h"
#include "ui/AppState.h"
#include "ui/CatalogActions.h"
#include "ui/ExprInput.h"
#include "ui/Widgets.h"
#include <array>
#include <cfloat>
#include <map>
#include <string>

namespace ui {
namespace {
void FieldLabel(const char* label) {
  ImGui::TextWrapped("%s", label);
  ImGui::SetNextItemWidth(-FLT_MIN);
}
bool TextField(const char* label, char* value, size_t capacity) {
  FieldLabel(label);
  return ImGui::InputText((std::string("##") + label).c_str(), value, capacity);
}
void MeasurementField(AppState& state, const char* label, double* value) {
  FieldLabel(label);
  ExprInputDouble(state, (std::string("##") + label).c_str(), value, "%.5f");
}
} // namespace
void DrawCatalogSection(AppState& state) {
  if (!SectionHeader("Engineering catalogs"))
    return;
  ImGui::PushID("engineering_catalogs");
  static char path[1024] = "", id[128] = "", revision[128] = "";
  static char source[1024] = "", license[256] = "";
  static int units = 0;
  static bool embedded = false;
  static catalog::Json preview;
  static std::string error;
  static uint64_t generation = 0;
  if (generation != state.projectGeneration) {
    preview = nullptr;
    error.clear();
    generation = state.projectGeneration;
  }
  bool edited = false;
  if (ImGui::TreeNode("Import bearing data")) {
    ImGui::TextWrapped(
        "CSV or JSON records. Importing supplies reference data; it does not "
        "resize parts or select a bearing fit.");
    edited |= TextField("File", path, sizeof(path));
    edited |= TextField("Catalog name", id, sizeof(id));
    edited |= TextField("Revision", revision, sizeof(revision));
    edited |= TextField("Source", source, sizeof(source));
    edited |= TextField("License / usage terms", license, sizeof(license));
    FieldLabel("Input units");
    edited |= ImGui::Combo("##Input units", &units, "mm\0in\0");
    edited |= ImGui::Checkbox("Embed records", &embedded);
    ImGui::TextWrapped(
        embedded ? "Include records in saved/shared projects. Choose this "
                   "only when your data license permits inclusion."
                 : "Reference only: saved projects omit records. "
                   "Reopen by importing the matching dataset.");
    static const char* names[] = {
        "designation",  "bore_diameter", "outer_diameter",    "width",
        "shaft_min",    "shaft_max",     "housing_min",       "housing_max",
        "shoulder_min", "shoulder_max",  "fillet_radius_max", "step_reference"};
    static std::array<std::array<char, 128>, 12> columns{};
    catalog::Json mapping = catalog::Json::object();
    if (ImGui::TreeNode("Column mapping")) {
      ImGui::TextWrapped(
          "Leave a field blank when the source already uses the shown name. "
          "Optional limits remain unverified when absent.");
      for (size_t i = 0; i < columns.size(); ++i)
        edited |= TextField(names[i], columns[i].data(), columns[i].size());
      ImGui::TreePop();
    }
    for (size_t i = 0; i < columns.size(); ++i)
      if (columns[i][0])
        mapping[names[i]] = columns[i].data();
    if (edited)
      preview = nullptr;
    if (ImGui::Button("Preview import")) {
      try {
        catalog::Json options = {
            {"id", id},
            {"revision", revision},
            {"source", source},
            {"license", license},
            {"units", units == 0 ? "mm" : "in"},
            {"storage", embedded ? "embedded" : "reference"},
            {"mapping", mapping}};
        preview = catalog::ReadFile(path, options);
        error.clear();
      } catch (const std::exception& e) {
        error = e.what();
        preview = nullptr;
      }
    }
    if (!preview.is_null()) {
      ImGui::TextWrapped("%zu bearing records; normalized to mm",
                         preview.at("record_count").get<size_t>());
      for (size_t i = 0;
           i < std::min<size_t>(5, preview.contains("records")
                                       ? preview.at("records").size()
                                       : 0);
           ++i) {
        const auto& r = preview["records"][i];
        ImGui::TextWrapped("%s: bore %.4f, OD %.4f, width %.4f mm",
                           r["designation"].get<std::string>().c_str(),
                           r["bore_diameter"].get<double>(),
                           r["outer_diameter"].get<double>(),
                           r["width"].get<double>());
        if (!r["unsupported_fields"].empty())
          ImGui::TextWrapped("Uninterpreted columns: %s",
                             r["unsupported_fields"].dump().c_str());
      }
      if (ImGui::Button("Install revision")) {
        try {
          InstallCatalog(state, preview);
          error.clear();
          preview = nullptr;
        } catch (const std::exception& e) {
          error = e.what();
        }
      }
    }
    if (!error.empty())
      ImGui::TextWrapped("%s", error.c_str());
    ImGui::TreePop();
  }
  for (const auto& e : state.catalogs) {
    const std::string label = e.at("id").get<std::string>() + " @ " +
                              e.at("revision").get<std::string>();
    if (!ImGui::TreeNode(label.c_str()))
      continue;
    ImGui::TextWrapped("Source: %s", e.at("source").get<std::string>().c_str());
    ImGui::TextWrapped("Terms: %s", e.at("license").get<std::string>().c_str());
    ImGui::Text("Storage: %s", e.at("storage").get<std::string>().c_str());
    if (!e.contains("records"))
      ImGui::TextWrapped("Unresolved reference. Import the same catalog name, "
                         "revision and content to restore access.");
    else {
      // Page records so opening a large catalog does not draw every row.
      const auto& records = e.at("records");
      ImGui::BeginChild("records", ImVec2(0, 240), ImGuiChildFlags_Borders);
      static std::map<std::string, int> pages;
      int& page = pages[label];
      FieldLabel("Page (50 records)");
      ExprInputInt(state, "##Page", &page);
      page = std::clamp(page, 0, static_cast<int>((records.size() - 1) / 50));
      for (size_t index = static_cast<size_t>(page) * 50;
           index < records.size() && index < static_cast<size_t>(page + 1) * 50;
           ++index) {
        const auto& r = records[index];
        if (!ImGui::TreeNode(r.at("designation").get<std::string>().c_str()))
          continue;
        ImGui::TextWrapped("Bore %.4f / OD %.4f / width %.4f mm",
                           r["bore_diameter"].get<double>(),
                           r["outer_diameter"].get<double>(),
                           r["width"].get<double>());
        static std::map<std::string, std::array<double, 4>> observations;
        auto& values = observations[e.at("hash").get<std::string>() + ":" +
                                    r.at("designation").get<std::string>()];
        auto& shaft = values[0];
        auto& housing = values[1];
        auto& shoulder = values[2];
        auto& fillet = values[3];
        ImGui::TextWrapped("Seat check: enter measured dimensions in mm. Zero "
                           "leaves an observation unspecified.");
        MeasurementField(state, "Shaft diameter", &shaft);
        MeasurementField(state, "Housing diameter", &housing);
        MeasurementField(state, "Shoulder diameter", &shoulder);
        MeasurementField(state, "Fillet radius", &fillet);
        catalog::Json measurements = catalog::Json::object();
        if (shaft > 0)
          measurements["shaft_diameter"] = shaft;
        if (housing > 0)
          measurements["housing_diameter"] = housing;
        if (shoulder > 0)
          measurements["shoulder_diameter"] = shoulder;
        if (fillet > 0)
          measurements["fillet_radius"] = fillet;
        const auto report = catalog::SeatRequirements(
            e, r["designation"].get<std::string>(), measurements);
        for (const auto& check : report["checks"]) {
          ImGui::TextWrapped("%s: %s",
                             check["characteristic"].get<std::string>().c_str(),
                             check["status"].get<std::string>().c_str());
          if (check.contains("maximum"))
            ImGui::TextWrapped("Limits: %.5f .. %.5f mm",
                               check.value("minimum", 0.0),
                               check["maximum"].get<double>());
          if (check.contains("reason"))
            ImGui::TextWrapped("  %s",
                               check["reason"].get<std::string>().c_str());
        }
        ImGui::TextWrapped("Caller-supplied measurements only. Load, speed, "
                           "temperature and fit selection are not evaluated.");
        if (!r["unsupported_fields"].empty())
          ImGui::TextWrapped("Uninterpreted columns: %s",
                             r["unsupported_fields"].dump().c_str());
        ImGui::TreePop();
      }
      ImGui::EndChild();
    }
    ImGui::TreePop();
  }
  ImGui::PopID();
}
} // namespace ui
