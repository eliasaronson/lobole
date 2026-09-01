#include "ui/BackgroundJobs.h"

#include "core/Document.h"
#include "ui/AppActions.h"
#include "ui/AppState.h"
#include "ui/ImportJob.h"
#include "ui/SimulationActions.h"
#include "ui/SyncJob.h"

namespace ui {

void PollBackgroundJobs(AppState& state) {
  // Order is not load-bearing — no job's completion feeds another's — but it
  // is kept stable so a log read across frames is comparable.

  // Advance an in-flight offline render by one sample batch. Not a worker
  // thread at all: it slices on the main thread with its own one-shot submits
  // (docs/ui/long-operations.md says why that is the one candidate for a real
  // background render thread later).
  PumpRenderJob(state);

  // Join a finished STEP import worker, then create part tabs a slice per
  // frame so the progress modal keeps painting.
  PollImport(state);

  // A git fetch/push/clone. Was polled from a collapsed-able TreeNode.
  PollSync(state);

  // The FEM jobs need a document; an assembly tab has none. Guarding here
  // rather than inside each is deliberate: `PollTopologyRun` takes a
  // `Document&`, and inventing a null-document meaning for it would push the
  // question into the domain that has no answer for it.
  //
  // A solve started on a part tab and then left for an assembly tab is
  // therefore still not reaped until you return — which is the same shape as
  // the bug above and is NOT fixed here. It needs the job to stop being
  // addressed by "the active document", which is the registry's business
  // (docs/ui/long-operations.md, phase 3). Named rather than left to be
  // rediscovered.
  if (Document* doc = state.document.get()) {
    PollTopologyRun(state, *doc);
    PollStudyRun(state, *doc);
  }
}

} // namespace ui
