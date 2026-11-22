// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "remote/RemoteScanCompare.h"

#include "core/Document.h"
#include "core/ScanCompare.h"
#include "remote/RemoteHelpers.h"
#include "ui/AppState.h"
#include "ui/BackgroundJobs.h"
#include "ui/ScanCompareTool.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <thread>

using nlohmann::json;

namespace remote {

namespace {

// A comparison packed by the handler for ParkScanCompareRequest instead of
// run: what the job computes, and what answers on the frame when it lands.
struct OffFrameCompare {
  std::shared_ptr<const scancompare::Input> input;
  std::function<json(ui::AppState&,
                     const std::shared_ptr<const scancompare::Outcome>&)>
      commit;
  std::string label;
};
thread_local OffFrameCompare* g_offFrameCompare = nullptr;

uint64_t FeatureId(const json& v, const char* key, const char* what) {
  if (!v.is_number_unsigned())
    throw std::runtime_error(std::string(key) +
                             " is the feature_id (an "
                             "integer) of " +
                             what + ", not " + v.dump() +
                             " -- get_state lists every body with the "
                             "feature that made it (created_by)");
  return v.get<uint64_t>();
}

} // namespace

namespace {

// Either method's comparison: the pair read and gathered here, the run
// packed for the job when one parks it, else run on this thread.
json Compare(ui::AppState& state, Document* doc, const std::string& m,
             const RpcParams& p, bool show) {
  if (doc == nullptr)
    throw std::runtime_error(m + ": no active part document; a scan is "
                                 "compared on the part tab that holds it");
  scancompare::Request rq;
  rq.scanId =
      FeatureId(p.at("scan"), "scan", "the scan's Import (a mesh body)");
  rq.bodyId = FeatureId(p.at("body"), "body", "the body to compare it with");
  if (p.contains("tolerance_mm")) {
    const json& t = p.at("tolerance_mm");
    if (!t.is_number())
      throw std::runtime_error("tolerance_mm is a number of mm more than 0, "
                               "not " +
                               t.dump());
    rq.toleranceMm = t.get<double>();
  }
  auto input = std::make_shared<scancompare::Input>();
  if (const std::string why = scancompare::Gather(*doc, rq, *input);
      !why.empty())
    throw std::runtime_error(m + ": " + why);
  const Document* target = doc;
  const uint64_t version = doc->Version();
  const auto commit =
      [m, show, target, version,
       input](ui::AppState& s,
              const std::shared_ptr<const scancompare::Outcome>& outcome) {
        if (outcome->deviation.cancelled)
          throw std::runtime_error(m + ": cancelled; nothing was shown");
        json reply = outcome->reply;
        AttachReceipt(reply, outcome->receipt);
        if (show) {
          if (s.document.get() != target || target->Version() != version)
            throw std::runtime_error(
                m + ": the part changed or its tab closed while it compared; "
                    "nothing is shown -- ask again");
          ui::ShowScanDeviation(s, input, outcome);
          reply["shown"] = true;
        }
        return reply;
      };
  if (g_offFrameCompare != nullptr) {
    g_offFrameCompare->input = input;
    g_offFrameCompare->commit = commit;
    g_offFrameCompare->label =
        "Compare '" + input->scanName + "' to '" + input->bodyName + "'";
    return json::object();
  }
  // In a batch, or wherever nothing parks: on this thread.
  auto outcome = std::make_shared<const scancompare::Outcome>(
      scancompare::Run(*input, nullptr, nullptr));
  return commit(state, outcome);
}

} // namespace

std::optional<json> HandleScanCompareRpc(ui::AppState& state, Document* doc,
                                         const std::string& m,
                                         const RpcParams& p) {
  if (m == "set_scan_deviation") {
    if (p.contains("clear")) {
      const json& c = p.at("clear");
      if (!c.is_boolean() || !c.get<bool>())
        throw std::runtime_error("clear is true, to remove the deviation "
                                 "map; to show one pass {scan, body}");
      if (p.contains("scan") || p.contains("body") ||
          p.contains("tolerance_mm"))
        throw std::runtime_error("clear takes nothing else: scan, body and "
                                 "tolerance_mm are for showing a map");
      const bool was = state.scanDeviation.outcome != nullptr;
      if (g_offFrameCompare == nullptr) // parked: answered when dispatched
        ui::ClearScanDeviation(state);
      return json{{"cleared", was}};
    }
    return Compare(state, doc, m, p, true);
  }
  if (m == "compare_scan") {
    return Compare(state, doc, m, p, false);
  }
  return std::nullopt;
}

namespace {

class CompareWait : public Parking {
public:
  struct Run {
    std::function<json(ui::AppState&,
                       const std::shared_ptr<const scancompare::Outcome>&)>
        commit;
    std::shared_ptr<const scancompare::Outcome> outcome;
    std::string error;
    std::atomic<bool> done{false};
  };
  explicit CompareWait(std::shared_ptr<Run> run) : m_run(std::move(run)) {}
  Next Poll(ui::AppState& state, const RpcParams&, json& answer) override {
    if (!m_run->done.load(std::memory_order_acquire))
      return Next::Wait;
    if (!m_run->error.empty())
      throw std::runtime_error(m_run->error);
    answer = m_run->commit(state, m_run->outcome);
    return Next::Answer;
  }

private:
  std::shared_ptr<Run> m_run;
};

} // namespace

std::unique_ptr<Parking> ParkScanCompareRequest(ui::AppState& state,
                                                const std::string& m,
                                                const RpcParams& p) {
  if (m != "compare_scan" && m != "set_scan_deviation")
    return nullptr;
  OffFrameCompare slot;
  g_offFrameCompare = &slot;
  try {
    (void)HandleScanCompareRpc(state, state.document.get(), m, p);
  } catch (...) {
    g_offFrameCompare = nullptr;
    return nullptr; // the handler refuses it again, in its own words
  }
  g_offFrameCompare = nullptr;
  // A clear (answered as it is dispatched), or a key no handler read: the
  // server refuses that before anything starts.
  if (!slot.commit || !p.UnknownKeys().empty())
    return nullptr;
  auto run = std::make_shared<CompareWait::Run>();
  run->commit = std::move(slot.commit);
  auto bg = std::make_unique<ui::BackgroundJob>();
  bg->kind = "deviation";
  bg->label = slot.label;
  bg->cancellable = true;
  bg->fraction.store(0.0);
  ui::BackgroundJob* raw = bg.get();
  // Holds the job before it computes (tests only), so a smoke can cancel it
  // while it runs.
  int holdMs = 0;
  if (const char* ms = std::getenv("LOBOLE_TEST_DEVIATION_MS"))
    holdMs = std::max(0, std::atoi(ms));
  bg->thread = std::thread([run, raw, holdMs, input = slot.input]() {
    for (int waited = 0; waited < holdMs && !raw->cancel.load(); waited += 10)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    try {
      run->outcome = std::make_shared<const scancompare::Outcome>(
          scancompare::Run(*input, &raw->cancel, &raw->fraction));
    } catch (const std::exception& e) {
      run->error = e.what();
    }
    run->done.store(true, std::memory_order_release);
    raw->done.store(true); // last
  });
  bg->finish = [](ui::AppState&) { return true; };
  ui::StartBackgroundJob(state, std::move(bg));
  return std::make_unique<CompareWait>(std::move(run));
}

} // namespace remote
