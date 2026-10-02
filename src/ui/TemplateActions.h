#pragma once

// Feature templates' project-level verbs (docs/modeling/templates.md). The
// template itself (core/Template.h) and its instance
// (core/features/TemplateFeature.h) know one document; the LIBRARY is the
// project's (AppState::projectTemplates), so saving, placing, updating,
// unlinking, exporting and importing live here, and the RPCs and the GUI go
// through the same functions -- the two surfaces cannot disagree about a
// refusal.

#include "core/Template.h"
#include "core/features/TemplateFeature.h"
#include "ui/AppState.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ui {

// One instance of a template somewhere in the project.
struct TemplateInstanceRef {
  uint64_t partId = 0;
  std::string part;
  uint64_t featureId = 0;
  std::string feature;
  uint64_t revision = 0;       // the revision it expands
  std::optional<bool> changed; // TemplateChanged's answer
};

// Saves `spec`'s features of the ACTIVE part as a template in the library:
// a new one, or -- with `replace` -- the next revision of the one of that
// name (refused without it, naming the revision it would replace). One
// project-scope undo step. `marked` receives every instance in the project
// that now reads "template changed". Throws std::runtime_error with the
// refusal (tmpl::FromFeatures's, or the name clash); changes nothing then.
const tmpl::Template& SaveTemplate(AppState& state, const tmpl::SaveSpec& spec,
                                   bool replace,
                                   std::vector<TemplateInstanceRef>* marked);

// Removes a template from the library (one undo step). Its instances keep
// the definitions they carry; they read as no longer in the library.
[[nodiscard]] std::string DeleteTemplate(AppState& state,
                                         const std::string& name);

// Whether the library's template of this name differs from what `instance`
// expands: nullopt when the library has none by that name (unknown, never
// "unchanged").
[[nodiscard]] std::optional<bool>
TemplateChanged(const AppState& state, const TemplateFeature& instance);

// Every instance in the project (of `name`, or of any template when empty).
std::vector<TemplateInstanceRef> TemplateInstances(AppState& state,
                                                   const std::string& name);

// A Template instance for the ACTIVE part, from the library's template
// `name`, the request's `params` and `on`: values parsed and refused as
// tmpl::ValuesFromRequest refuses, number expressions evaluated against the
// part's variables, references checked against the part as it stands (a
// face that is there, and planar where a sketch stands on it). Ids minted
// from the part. Throws the refusal, naming what is accepted.
std::unique_ptr<TemplateFeature>
MakeTemplateInstance(AppState& state, const std::string& name,
                     const nlohmann::json& params, const nlohmann::json* on,
                     const std::string& featureName);

// Adds `instance` to the active part as one undo step and regenerates. An
// instance that does not build -- a parameter the features it expands
// refuse, a size that makes no solid -- is taken back out and its error
// thrown: a bad parameter is refused, not committed. Returns its index.
size_t PlaceTemplateInstance(AppState& state,
                             std::unique_ptr<TemplateFeature> instance);

// Moves an instance of the active part to the library's current revision
// (one undo step), keeping every value the new revision still declares;
// `extra` gives ones it added. Returns the refusal or empty.
[[nodiscard]] std::string UpdateTemplateInstance(AppState& state,
                                                 uint64_t featureId,
                                                 const nlohmann::json& extra);

// Replaces an instance of the active part with its expanded features, as
// plain features with the same ids (so every face keeps its name and every
// reference to one still resolves), each expression reading the document's
// variables the instance read. One undo step. Returns the refusal or empty;
// `ids` receives the features' ids, in order.
[[nodiscard]] std::string UnlinkTemplate(AppState& state, uint64_t featureId,
                                         std::vector<uint64_t>* ids = nullptr);

// Writes the library's templates (all, or those named) to `path` as
// {"format": "designer-templates", "version": 1, "templates": [...]}.
// Refuses an unknown name (naming the library) or an unwritable path.
[[nodiscard]] std::string ExportTemplates(AppState& state,
                                          const std::string& path,
                                          const std::vector<std::string>& names,
                                          std::vector<std::string>* written);

// Reads such a file into the library (one undo step). A template whose name
// the library already has is refused naming it, unless `replace`, which
// makes it that template's next revision. Nothing is imported on a refusal.
[[nodiscard]] std::string ImportTemplates(AppState& state,
                                          const std::string& path, bool replace,
                                          std::vector<std::string>* imported);

// The Insert template tool (docs/modeling/templates.md#in-the-gui):
// refuses visibly with an empty library; otherwise arms the plane pick, and
// a picked face or plane opens a preview of the first template (or the one
// last inserted) standing on it, with its saved example values.
void CreateTemplateInstance(AppState& state);
// The plane pick landed: a preview instance standing on `on`.
void BeginTemplatePreview(AppState& state, const nlohmann::json& on);

} // namespace ui
