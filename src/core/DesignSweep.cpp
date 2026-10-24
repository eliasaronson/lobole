// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/DesignSweep.h"

#include "cad/CADKernel.h" // ArmOccSignalHandlers
#include "core/Parallel.h"
#include "core/PartMeasures.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include "core/sketch/SketchSolver.h"

#include <Standard_Failure.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <thread>

using json = nlohmann::json;

namespace sweep {

std::string ValueText(double v) { return std::format("{:.17g}", v); }

std::vector<Chunk> PlanChunks(const Spec& spec) {
  std::vector<Chunk> chunks;
  const size_t n = spec.variables.size();
  std::vector<double> base(n);
  for (size_t i = 0; i < n; i++)
    base[i] = spec.variables[i].value;
  const int samples = std::max(2, spec.samples);
  for (size_t i = 0; i < n; i++) {
    Chunk c;
    c.variable = static_cast<int>(i);
    const Variable& v = spec.variables[i];
    for (int k = 0; k < samples; k++) {
      std::vector<double> p = base;
      // Ends exact: the last sample is `max`, not min + (samples-1)*step.
      p[i] = k == samples - 1 ? v.max
                              : v.min + (v.max - v.min) * k / (samples - 1);
      c.points.push_back(std::move(p));
    }
    chunks.push_back(std::move(c));
  }
  if (spec.corners && n > 0) {
    Chunk c;
    for (size_t mask = 0; mask < (size_t{1} << n); mask++) {
      std::vector<double> p(n);
      for (size_t i = 0; i < n; i++)
        p[i] = (mask >> i) & 1 ? spec.variables[i].max : spec.variables[i].min;
      c.points.push_back(std::move(p));
    }
    std::vector<double> centre(n);
    for (size_t i = 0; i < n; i++)
      centre[i] = (spec.variables[i].min + spec.variables[i].max) / 2.0;
    c.points.push_back(std::move(centre));
    chunks.push_back(std::move(c));
  }
  return chunks;
}

size_t PlannedCount(const Spec& spec) {
  size_t count = 0;
  for (const Chunk& c : PlanChunks(spec))
    count += c.points.size();
  return count + spec.variables.size() * 2 * static_cast<size_t>(spec.refine);
}

namespace {

std::string Num(double v) { return std::format("{:.6g}", v); }

// One worker's copies: the project variables and tables as the app had
// them, every rebuilt part read back from its saved form against them, and
// what the base looked like (each sketch's DOF), which a point's sketches
// are compared with.
struct Worker {
  std::vector<Parameter> params;
  SymbolTable cells;
  SymbolTable symbols; // what every copy's project scope points at
  std::vector<std::unique_ptr<Document>> docs; // aligned with Inputs::parts
  std::map<std::pair<size_t, uint64_t>, int> baseDof; // {part, sketch} -> dof
  json base;

  Worker() = default;
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

  void Evaluate() {
    SymbolTable fresh;
    EvaluateParameterTable(params, fresh, &cells, {});
    symbols = std::move(fresh);
  }
};

const Document* DocById(const Inputs& in, const Worker& w, uint64_t id,
                        size_t* index = nullptr) {
  for (size_t k = 0; k < in.parts.size(); k++)
    if (in.parts[k].id == id) {
      if (index)
        *index = k;
      return w.docs[k].get();
    }
  return nullptr;
}

struct SketchState {
  uint64_t id = 0;
  std::string name;
  bool ok = true;
  int dof = -1;
  std::vector<size_t> conflicting;
  std::string why;
};

std::vector<SketchState> Sketches(const Document& doc) {
  std::vector<SketchState> out;
  for (size_t i = 0; i < doc.FeatureCount(); i++) {
    const auto* sk = dynamic_cast<const SketchFeature*>(doc.GetFeature(i));
    if (sk == nullptr || sk->constraints.empty() || doc.EffectiveSuppressed(i))
      continue;
    sketch::Solver solver;
    solver.Load(*sk);
    const sketch::SolveResult r = solver.Solve();
    SketchState s;
    s.id = sk->GetId();
    s.name = sk->GetName();
    s.dof = r.dof;
    s.conflicting = r.conflictingConstraints;
    // A value edit the regeneration refused keeps the previous geometry
    // and says so: the sketch did not take this point's values.
    const std::string& warning = sk->GetWarning();
    const bool kept =
        warning.find("keeping the previous geometry") != std::string::npos;
    s.ok = r.ok && !kept && sk->GetError().empty();
    if (kept)
      s.why = warning;
    else if (!r.ok)
      s.why = "does not solve";
    out.push_back(std::move(s));
  }
  return out;
}

// The scope an `expression` requirement reads on the copies, or why it
// cannot.
const SymbolTable* ScopeOn(const Inputs& in, const Worker& w,
                           const req::Requirement& r, SymbolTable& storage,
                           std::string& why) {
  if (!r.measure.contains("document"))
    return &w.symbols;
  const Document* doc = DocById(in, w, r.Document());
  if (doc == nullptr) {
    why = "its part is not rebuilt";
    return nullptr;
  }
  if (!r.measure.contains("configuration"))
    return &doc->Symbols();
  const uint64_t cfg = r.measure.at("configuration").get<uint64_t>();
  if (cfg != 0 && doc->FindConfiguration(cfg) == nullptr) {
    why = "configuration " + std::to_string(cfg) + " is gone";
    return nullptr;
  }
  storage = doc->SymbolsIn(cfg);
  return &storage;
}

// One requirement measured on the copies: {id, name, state, value?, unit?,
// note?}, its bound read in the scope it reads.
json MeasureOn(const Inputs& in, const Worker& w, const req::Requirement& r) {
  json row{{"id", r.id}, {"name", r.name}};
  const std::string kind = r.Kind();
  std::optional<double> value;
  std::string unit, why;
  SymbolTable storage;
  const SymbolTable* boundScope = &w.symbols;
  try {
    if (kind == "expression") {
      const SymbolTable* scope = ScopeOn(in, w, r, storage, why);
      if (scope != nullptr) {
        boundScope = scope;
        const req::ExpressionReading reading = req::EvaluateIn(
            r.measure.at("expression").get<std::string>(), *scope);
        if (reading.value)
          value = reading.value;
        else
          why = reading.error;
      }
      unit = r.measure.value("unit", std::string("mm"));
    } else {
      const Document* doc = DocById(in, w, r.Document());
      if (doc == nullptr) {
        why = "its part is not rebuilt";
      } else if (kind == "mass" || kind == "volume") {
        const Measure::DocumentMass m = partmeasure::MassOf(doc->GetBodies());
        if (kind == "volume") {
          value = m.volume;
          unit = "mm3";
        } else if (m.massKnown) {
          value = m.massG;
          unit = "g";
        } else {
          why = "no mass: a body has no material density";
        }
      } else if (kind == "section_area") {
        value = partmeasure::SectionAreaMeasure(*doc, r.measure).value;
        unit = "mm2";
      }
    }
  } catch (const Standard_Failure& e) {
    why = std::string("the measure failed: ") +
          (e.GetMessageString() ? e.GetMessageString() : "OpenCASCADE failure");
  } catch (const std::exception& e) {
    why = e.what();
  }
  if (!unit.empty())
    row["unit"] = unit;
  if (!value) {
    row["state"] = "unresolved";
    row["note"] = why.empty() ? std::string("could not be measured") : why;
    return row;
  }
  row["value"] = *value;
  const req::Bound& b = r.bound;
  double lim = b.value, lo = b.min, hi = b.max;
  const auto side = [&](const std::string& text, double& out) {
    if (text.empty())
      return true;
    const req::ExpressionReading v = req::EvaluateIn(text, *boundScope);
    if (!v.value) {
      why = "bound '" + text + "': " + v.error;
      return false;
    }
    out = *v.value;
    return true;
  };
  if (!side(b.valueExpr, lim) || !side(b.minExpr, lo) || !side(b.maxExpr, hi)) {
    row["state"] = "unresolved";
    row["note"] = why;
    return row;
  }
  const bool pass = req::Holds(b, *value, lim, lo, hi);
  row["state"] = pass ? "pass" : "fail";
  if (!pass) {
    const std::string u = unit.empty() ? "" : " " + unit;
    row["note"] = "measured " + Num(*value) + u + ", must be " +
                  (b.kind == "at_most"    ? "<= " + Num(lim)
                   : b.kind == "at_least" ? ">= " + Num(lim)
                                          : Num(lo) + " .. " + Num(hi)) +
                  u;
  }
  return row;
}

// What the copies are now: each part's regeneration, broken features and
// sketches, its volume and mass, every requirement, and whether the point
// holds -- with why not, one reason a line.
json Record(const Inputs& in, Worker& w, bool isBase) {
  json parts = json::array();
  json reasons = json::array();
  for (size_t k = 0; k < in.parts.size(); k++) {
    const Document& doc = *w.docs[k];
    json p{{"document", in.parts[k].id}, {"name", in.parts[k].name}};
    json broken = json::array();
    for (size_t i = 0; i < doc.FeatureCount(); i++) {
      const Feature* f = doc.GetFeature(i);
      if (f->GetError().empty())
        continue;
      broken.push_back({{"feature_id", f->GetId()},
                        {"name", f->GetName()},
                        {"error", f->GetError()}});
      reasons.push_back("'" + in.parts[k].name + "': '" + f->GetName() + "' " +
                        f->GetError());
    }
    p["regen"] = broken.empty() ? "ok" : "broken";
    if (!broken.empty())
      p["broken"] = broken;
    json sketches = json::array();
    for (const SketchState& s : Sketches(doc)) {
      const auto key = std::make_pair(k, s.id);
      if (isBase) {
        w.baseDof[key] = s.dof;
      }
      const auto it = w.baseDof.find(key);
      const bool moved = it != w.baseDof.end() && it->second != s.dof;
      if (s.ok && !moved)
        continue;
      json sj{{"feature_id", s.id},
              {"name", s.name},
              {"ok", s.ok},
              {"dof", s.dof},
              {"conflicting", s.conflicting}};
      if (!s.why.empty())
        sj["why"] = s.why;
      sketches.push_back(std::move(sj));
      if (!s.ok)
        reasons.push_back("'" + in.parts[k].name + "': sketch '" + s.name +
                          "' " + (s.why.empty() ? "does not solve" : s.why));
    }
    if (!sketches.empty())
      p["sketches"] = sketches;
    try {
      const Measure::DocumentMass m = partmeasure::MassOf(doc.GetBodies());
      p["volume_mm3"] = m.volume;
      p["mass_g"] = m.massKnown ? json(m.massG) : json(nullptr);
    } catch (const std::exception& e) {
      p["volume_mm3"] = nullptr;
      p["mass_g"] = nullptr;
      p["mass_note"] = e.what();
    }
    parts.push_back(std::move(p));
  }
  json reqs = json::array();
  for (const req::Requirement& r : in.requirements) {
    json row = MeasureOn(in, w, r);
    const std::string st = row.at("state").get<std::string>();
    if (st != "pass")
      reasons.push_back("requirement '" + r.name + "' " + st +
                        (row.contains("note")
                             ? ": " + row.at("note").get<std::string>()
                             : std::string()));
    reqs.push_back(std::move(row));
  }
  json out{{"parts", parts}, {"requirements", reqs}, {"ok", reasons.empty()}};
  if (!reasons.empty())
    out["why"] = reasons;
  return out;
}

// The copies at `values`: each swept variable's expression overridden by
// its value, as a configuration overrides one, so its dependents follow.
void Apply(const Inputs& in, Worker& w, const std::vector<double>& values) {
  const auto& vars = in.spec.variables;
  for (size_t j = 0; j < vars.size(); j++) {
    if (vars[j].part != 0)
      continue;
    for (Parameter& p : w.params)
      if (p.name == vars[j].name)
        p.expression = ValueText(values[j]);
  }
  w.Evaluate();
  for (size_t k = 0; k < in.parts.size(); k++) {
    Document& doc = *w.docs[k];
    Document::BulkEdit bulk(doc); // one regeneration, whatever changed
    for (size_t j = 0; j < vars.size(); j++) {
      if (vars[j].part != in.parts[k].id)
        continue;
      const Parameter* had = doc.FindParameter(vars[j].name);
      doc.SetParameter(vars[j].name, ValueText(values[j]),
                       had ? had->comment : std::string());
    }
  }
}

void Load(const Inputs& in, Worker& w) {
  w.params = in.projectParameters;
  lookuptable::AddCellSymbols(in.tables, w.cells);
  w.Evaluate();
  for (const PartCopy& part : in.parts) {
    auto doc = DocumentIO::Load(
        part.file.string(), DocumentIO::ProjectScope{&w.symbols, &w.params});
    if (!doc)
      throw std::runtime_error("the copy of '" + part.name +
                               "' could not be read back: " + doc.error());
    w.docs.push_back(std::move(*doc));
  }
  w.base = Record(in, w, /*isBase=*/true);
}

bool Hold(const Inputs& in, const Progress& progress) {
  const auto until =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(in.holdMs);
  while (std::chrono::steady_clock::now() < until) {
    if (progress.cancel.load())
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return !progress.cancel.load();
}

json PointJson(const Inputs& in, const std::vector<double>& values,
               json record) {
  json v = json::object();
  for (size_t j = 0; j < in.spec.variables.size(); j++)
    v[in.spec.variables[j].name] = values[j];
  record["values"] = v;
  return record;
}

std::string Why(const json& record) {
  std::string out;
  for (const json& r : record.value("why", json::array()))
    out += (out.empty() ? "" : "; ") + r.get<std::string>();
  return out;
}

// One side of a variable's walk from its own value: the last good value
// before the first failing one, nearest the base first.
struct Bracket {
  std::optional<double> lastOk;
  std::optional<double> failed;
  std::string why;
};

Bracket Side(double base, bool baseOk, const std::vector<double>& at,
             const std::vector<const json*>& records, bool below) {
  std::vector<size_t> order;
  for (size_t k = 0; k < at.size(); k++)
    if (below ? at[k] < base : at[k] > base)
      order.push_back(k);
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return below ? at[a] > at[b] : at[a] < at[b];
  });
  Bracket out;
  if (baseOk)
    out.lastOk = base;
  for (size_t k : order) {
    if (records[k]->at("ok").get<bool>()) {
      out.lastOk = at[k];
      continue;
    }
    out.failed = at[k];
    out.why = Why(*records[k]);
    return out;
  }
  out.lastOk.reset();
  return out;
}

struct ChunkResult {
  json points = json::array();
  json firstFailure; // variable chunks only: {below?, above?}
};

void RunChunk(const Inputs& in, Worker& w, const Chunk& chunk, ChunkResult& out,
              Progress& progress, size_t planned) {
  const auto& vars = in.spec.variables;
  std::vector<double> at;
  std::vector<json> records;
  const auto label = [&](const std::vector<double>& values) {
    std::string text;
    if (chunk.variable >= 0) {
      text = vars[chunk.variable].name + " = " + Num(values[chunk.variable]);
    } else {
      for (size_t j = 0; j < vars.size(); j++)
        text += (j ? ", " : "") + vars[j].name + " = " + Num(values[j]);
    }
    std::lock_guard lock(progress.mutex);
    progress.label = text + " (" + std::to_string(progress.done.load() + 1) +
                     " of " + std::to_string(planned) + ")";
  };
  const auto runPoint = [&](const std::vector<double>& values) -> bool {
    if (!Hold(in, progress))
      return false;
    label(values);
    Apply(in, w, values);
    json rec = Record(in, w, false);
    out.points.push_back(PointJson(in, values, rec));
    if (chunk.variable >= 0) {
      at.push_back(values[chunk.variable]);
      records.push_back(std::move(rec));
    }
    progress.done.fetch_add(1);
    return true;
  };
  for (const std::vector<double>& values : chunk.points)
    if (!runPoint(values))
      return;
  if (chunk.variable < 0)
    return;
  const Variable& v = vars[chunk.variable];
  const bool baseOk = w.base.at("ok").get<bool>();
  const auto bracket = [&](bool below) {
    std::vector<const json*> refs;
    for (const json& r : records)
      refs.push_back(&r);
    return Side(v.value, baseOk, at, refs, below);
  };
  json ff = json::object();
  for (bool below : {true, false}) {
    Bracket b = bracket(below);
    if (!b.failed)
      continue;
    // Bisect the bracket: each midpoint either moves the last good value
    // toward the failure or the failure toward it.
    for (int k = 0; k < in.spec.refine && b.lastOk; k++) {
      std::vector<double> values(vars.size());
      for (size_t j = 0; j < vars.size(); j++)
        values[j] = vars[j].value;
      values[chunk.variable] = (*b.lastOk + *b.failed) / 2.0;
      if (!runPoint(values))
        break;
      b = bracket(below);
    }
    json side{{"failed", *b.failed}, {"why", b.why}};
    side["last_ok"] = b.lastOk ? json(*b.lastOk) : json(nullptr);
    ff[below ? "below" : "above"] = std::move(side);
  }
  out.firstFailure = std::move(ff);
}

} // namespace

json Run(const Inputs& in, Progress& progress) {
  const std::vector<Chunk> chunks = PlanChunks(in.spec);
  const size_t planned = PlannedCount(in.spec);
  std::vector<ChunkResult> results(chunks.size());
  std::atomic<size_t> next{0};
  std::mutex baseMutex;
  json base;
  std::string error;
  std::mutex errorMutex;
  const size_t workers =
      std::max<size_t>(1, std::min(parallel::Workers(), chunks.size()));
  parallel::For(workers, [&](size_t) {
    // OCC's signal handler is per thread; a pool thread has none yet.
    ArmOccSignalHandlers();
    try {
      if (progress.cancel.load())
        return;
      Worker w;
      Load(in, w);
      {
        std::lock_guard lock(baseMutex);
        if (base.is_null())
          base = w.base;
      }
      for (size_t c = next.fetch_add(1); c < chunks.size();
           c = next.fetch_add(1)) {
        if (progress.cancel.load())
          return;
        RunChunk(in, w, chunks[c], results[c], progress, planned);
      }
    } catch (const Standard_Failure& e) {
      std::lock_guard lock(errorMutex);
      error = std::string("OpenCASCADE: ") +
              (e.GetMessageString() ? e.GetMessageString() : "failure");
      progress.cancel.store(true);
    } catch (const std::exception& e) {
      std::lock_guard lock(errorMutex);
      error = e.what();
      progress.cancel.store(true);
    }
  });
  if (!error.empty())
    throw std::runtime_error(error);
  json points = json::array();
  json firstFailure = json::object();
  for (size_t c = 0; c < chunks.size(); c++) {
    for (json& p : results[c].points)
      points.push_back(std::move(p));
    if (chunks[c].variable >= 0 && results[c].firstFailure.is_object() &&
        !results[c].firstFailure.empty())
      firstFailure[in.spec.variables[chunks[c].variable].name] =
          results[c].firstFailure;
  }
  json baseValues = json::object();
  for (const Variable& v : in.spec.variables)
    baseValues[v.name] = v.value;
  if (base.is_object())
    base["values"] = baseValues;
  return json{{"points", points},
              {"base", base},
              {"first_failure", firstFailure},
              {"cancelled", progress.cancel.load()}};
}

} // namespace sweep
