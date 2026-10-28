// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/HeavyBuild.h"

#include "cad/CADKernel.h"
#include "common/Logger.h"

#include <chrono>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace heavy {

namespace {

struct Interrupt {
  const std::atomic<bool>* cancel = nullptr;
  std::atomic<double>* fraction = nullptr;
};
thread_local Interrupt t_interrupt;

std::mutex g_storeMutex;
std::deque<Result> g_store; // oldest first
// A finished build is a few bodies (shared shapes): a handful of the
// session's heavy features, each with a couple of alternatives to undo to.
constexpr size_t kStoreCap = 32;

std::mutex g_testMutex;
// Read from the environment on first use, not at static initialisation:
// the logger it warns through does not exist yet then.
TestOptions& TestOptionsLocked() {
  static TestOptions t = [] {
    TestOptions o;
    if (const char* e = std::getenv("LOBOLE_TEST_HEAVY_FORCE");
        e != nullptr && e[0] != '\0' && std::string(e) != "0")
      o.force = true;
    if (const char* e = std::getenv("LOBOLE_TEST_HEAVY_DELAY_MS");
        e != nullptr && e[0] != '\0')
      o.delayMs = std::atoi(e);
    if (o.force || o.delayMs > 0)
      LOG_WARN("LOBOLE_TEST_HEAVY_*: every heavy-capable feature builds off "
               "the frame, slowed by {} ms (a test knob)",
               o.delayMs);
    return o;
  }();
  return t;
}

} // namespace

bool BuildsOffFrame(const Feature& f, const std::vector<Body>& input) {
  if (!f.MayBuildOffFrame())
    return false;
  if (GetTestOptions().force)
    return true;
  return f.BuildCostS(input) >= kOffFrameS;
}

void Detach(Request& request) {
  if (request.detached)
    return;
  request.original.clear();
  request.original.reserve(request.input.size());
  for (Body& b : request.input) {
    request.original.push_back(b.shape);
    if (b.shape.GeometryId() != nullptr)
      b.shape = b.shape.DetachedUnlocated().LocatedLike(b.shape);
  }
  request.detached = true;
}

std::optional<Result> Run(Request& request, const std::atomic<bool>* cancel,
                          std::atomic<double>* fraction) {
  struct Scope {
    Interrupt saved = t_interrupt;
    ~Scope() { t_interrupt = saved; }
  } scope;
  t_interrupt = {cancel, fraction};
  const auto cancelled = [&] { return cancel != nullptr && cancel->load(); };
  if (const int ms = GetTestOptions().delayMs; ms > 0) {
    const auto end =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
      if (cancelled())
        return std::nullopt;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  if (cancelled())
    return std::nullopt;

  std::vector<Body> bodies = request.input;
  std::vector<std::unique_ptr<Feature>> features;
  features.reserve(request.refs.size() + 1);
  for (const auto& r : request.refs)
    features.push_back(r->Clone());
  std::unique_ptr<Feature> f = request.feature->Clone();
  Feature* built = f.get();
  features.push_back(std::move(f));
  RegenContext ctx{bodies,
                   features,
                   features.size() - 1,
                   &request.symbols,
                   nullptr,
                   request.sheetMetal ? &*request.sheetMetal : nullptr};
  ctx.configuration = request.configuration ? &*request.configuration : nullptr;
  ctx.print = request.print ? &*request.print : nullptr;
  ctx.printUp = request.printUp;
  built->ClearError();
  built->ClearWarning();
  bool ok = false;
  try {
    ok = built->Execute(ctx);
  } catch (const std::exception& e) {
    built->SetError(e.what());
  }
  // A cancel the builder saw ends in a refusal that is not the feature's:
  // nothing of it is kept.
  if (cancelled())
    return std::nullopt;
  if (!ok && built->GetError().empty())
    built->SetError("regeneration failed");
  FollowModifiedFaces(bodies);
  // What the build did not touch is the document's own shape again.
  if (request.detached)
    for (size_t i = 0; i < bodies.size() && i < request.original.size(); i++)
      if (i < request.input.size() &&
          bodies[i].shape.SameGeometryAs(request.input[i].shape))
        bodies[i].shape = request.original[i];
  Result out;
  out.hash = request.hash;
  out.bodiesAfter = std::move(bodies);
  out.executed = std::shared_ptr<const Feature>(built->Clone());
  return out;
}

bool Cancelled() {
  return t_interrupt.cancel != nullptr && t_interrupt.cancel->load();
}

void ReportFraction(double f) {
  if (t_interrupt.fraction != nullptr)
    t_interrupt.fraction->store(f);
}

std::optional<Result> Find(size_t hash) {
  if (hash == 0)
    return std::nullopt;
  std::lock_guard lock(g_storeMutex);
  for (const Result& r : g_store)
    if (r.hash == hash)
      return r;
  return std::nullopt;
}

void Put(Result result) {
  if (result.hash == 0 || result.executed == nullptr)
    return;
  std::lock_guard lock(g_storeMutex);
  for (auto it = g_store.begin(); it != g_store.end(); ++it)
    if (it->hash == result.hash) {
      g_store.erase(it);
      break;
    }
  g_store.push_back(std::move(result));
  while (g_store.size() > kStoreCap)
    g_store.pop_front();
}

void ForgetAll() {
  std::lock_guard lock(g_storeMutex);
  g_store.clear();
}

size_t StoreSize() {
  std::lock_guard lock(g_storeMutex);
  return g_store.size();
}

void SetTestOptions(const TestOptions& options) {
  std::lock_guard lock(g_testMutex);
  TestOptionsLocked() = options;
}

TestOptions GetTestOptions() {
  std::lock_guard lock(g_testMutex);
  return TestOptionsLocked();
}

} // namespace heavy
