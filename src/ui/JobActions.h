#pragma once

#include "core/Feature.h"
#include "core/cam/CamVerify.h" // FixtureSolid
#include "core/cam/JobDocument.h"
#include "ui/AppState.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class Document;

// The CAM job tab (docs/cam/job-document.md).
//
// A job references parts by their stable project-document id and owns no
// geometry, so everything CAM used to reach through `state.document` now goes
// through here. `JobBodies` is deliberately the ONE place a job's geometry is
// assembled: the placement transform composes there and nowhere else, which
// is the lesson setup-frame.md had to learn when a fourth copy of the setup
// transform turned up in CamPlayback.
namespace ui {

// The active tab's job, or null when the active tab is not a job.
cam::JobDocument* ActiveJob(AppState& state);
const cam::JobDocument* ActiveJob(const AppState& state);

// The part Document behind a job part's `sourceDocId`, active or stashed.
// Null when the source is missing (deleted) or is not a part — a job cuts
// parts, and an assembly has no bodies of its own. Mirrors
// DrawingSourceDocument, deliberately: a cross-document reference that goes
// stale is a trap this repo has already paid for twice (A4, A5).
Document* JobSourceDocument(AppState& state, uint64_t sourceDocId);
// That document's Version(), or 0 when it is gone. What a staleness report
// compares against.
uint64_t JobSourceVersion(AppState& state, uint64_t sourceDocId);

// Every body the ACTIVE job cuts, in MODEL space. The setup's own framing is
// applied downstream by cam::BodiesInSetupFrame, which is unchanged: this
// answers "which bodies", not "which way up". Empty when the active tab is
// not a job, or when every source document is missing.
//
// Returns a reference into a cache on AppState rather than a fresh vector,
// because this stands exactly where `document->GetBodies()` used to and some
// of those call sites run every frame. The cache key is the job's version
// plus every source part's, so a model edit reaches the toolpath the same
// frame it happens.
const std::vector<Body>& JobBodies(AppState& state);

// The bodies a job TAB draws: every part, in the ACTIVE setup's frame and
// where its placement puts it. This is the "laid out on the stock" picture,
// and it is the same list the toolpath is generated from -- what you see is
// what is cut. Returns model-space bodies when no setup exists yet.
// Cached, like JobBodies and for the same reason: the OCC transform per body
// is not free, and this is read by the render sync, the camera fit and every
// viewport pick.
const std::vector<Body>& JobPlacedBodies(AppState& state);

// The active setup of the active job, or null. "Active" means the one the
// panel is showing; with none chosen it is the first.
const cam::Setup* ActiveJobSetup(AppState& state);

// A setup's FIXTURES as solids in setup (bed) space, each named for the
// verdict (docs/cam/workholding-plan.md). `enabledOnly` is the verifier's
// list; the viewport draws every fixture. A fixture whose part is gone is
// left out and its name appended to `missing` -- the caller says so rather
// than checking less than it was asked to.
std::vector<cam::FixtureSolid>
JobFixtureSolids(AppState& state, const cam::Setup& setup,
                 bool enabledOnly = true,
                 std::vector<std::string>* missing = nullptr);

// A fixture's bodies in its OWN coordinates, before FixtureTransform: its
// part's, reference bodies left out, or its keep-out box. Empty when the part
// is gone.
std::vector<Body> FixtureModelBodies(AppState& state, const cam::Fixture& f);

// Where a fixture IS, bed = T * model: its seat solved against the stock as
// it is now, or its stored pose when it has no seat. A seat that cannot solve
// falls back to the stored pose and says why in `why` -- the row shows it,
// rather than drawing a clamp somewhere plausible in silence.
glm::dmat4 FixtureBedTransform(AppState& state, const cam::Setup& setup,
                               const cam::Fixture& f,
                               std::string* why = nullptr);
// Re-solves every seated fixture of `setup` and KEEPS the pose on the
// fixture, so the per-frame readers (fit, the section bounds) can read `pos`
// without solving. Why a seat could not solve goes to
// `state.camSeatRefusals` for the row to say. Run where the job is
// re-synced, which the stock's definition now keys.
void RefreshFixtureSeats(AppState& state, cam::Setup& setup);

// When safe Z was raised to clear a fixture and the file then asks for more
// Z than the machine has, the finding that says WHICH fixture did it --
// otherwise it reads as a travel problem with no cause. Empty otherwise.
std::optional<cam::VerdictLine> FixtureTravelLine(AppState& state,
                                                  const cam::Setup& setup,
                                                  const cam::Verdict& v);

// Safe Z for a setup of the active job: clear of the stock AND of the
// setup's fixtures. Every place that derives safe Z goes through here, so a
// clamp taller than the stock cannot be forgotten by one of them.
double DeriveJobSafeZ(AppState& state, const cam::Setup& setup);

// A change counter for everything a job TAB draws: the job itself plus every
// source part it references. The renderer resyncs on it, so a model edit in
// one of the parts reaches the viewport.
uint64_t JobRenderVersion(AppState& state);

// The machining selection for a picked element: the ref, plus which job part
// it is on.
//
// `partHint` is where the pick actually LANDED — a viewport click knows the
// body it hit, and an RPC caller can pass the `job_part` list_elements gave
// it. Without one this searches, and a search is only an answer while the
// name is unique: two parts in a job can own the same feature id and the same
// element name (a part machined twice is the easy case), and picking the
// first match is the wrong bracket, cut for real.
//
// Returns jobPartId 0 when nothing in the job owns the ref, or when more than
// one part does and no hint said which. `why`, when given, receives the
// reason — callers REFUSE on it rather than machining a guess.
cam::Target TargetFor(AppState& state, const ElementRef& ref,
                      uint64_t partHint = 0, std::string* why = nullptr);

// Create a job tab for one part document and make it active. Returns the new
// tab's index. `name` empty derives one from the part.
size_t CreateJobForPart(AppState& state, uint64_t sourcePartId,
                        const std::string& name = "");

// "Machine this part": the job for `sourcePartId`, made if there is not one
// already, and made ACTIVE either way. Reuse rather than a second job is the
// point -- pressing the CAM button twice on a part should return you to the
// job you were cutting, not start a rival one with the same part in it.
size_t OpenJobForPart(AppState& state, uint64_t sourcePartId);

// The tab index of a job that already cuts this part, or -1.
int FindJobForPart(AppState& state, uint64_t sourcePartId);

// The setup the panel, the tool session and the overlays all act on (the one
// selected in the combo, else the first). Null when there is none.
cam::Setup* ActiveCamSetup(AppState& state);

} // namespace ui
