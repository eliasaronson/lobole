#pragma once

#include "core/Feature.h"
#include "core/cam/JobDocument.h"
#include "ui/AppState.h"

#include <cstdint>
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
std::vector<Body> JobPlacedBodies(AppState& state);

// The active setup of the active job, or null. "Active" means the one the
// panel is showing; with none chosen it is the first.
const cam::Setup* ActiveJobSetup(AppState& state);

// A change counter for everything a job TAB draws: the job itself plus every
// source part it references. The renderer resyncs on it, so a model edit in
// one of the parts reaches the viewport.
uint64_t JobRenderVersion(AppState& state);

// The machining selection for a picked element: the ref, plus which job part
// it landed on, read off the body it resolves to. Returns jobPartId 0 when
// nothing in the job owns it -- a target the job cannot machine, which the
// caller refuses rather than guessing a part for.
cam::Target TargetFor(AppState& state, const ElementRef& ref);

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

} // namespace ui
