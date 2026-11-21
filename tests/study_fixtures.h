// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Studies for a test that solves on a part's geometry through the core
// planner (fem::PlanStudy, fem::RunStudy) without a simulation document: a
// part keeps no studies (they live in simulations,
// docs/simulation/README.md#the-simulation-document-core), so the studies
// are the test's own.

#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// A study whose id is taken from the document's counter, so the loads' ids,
// taken the same way, never meet it.
template <class Doc>
fem::Study NewStudy(Doc& doc, const std::string& name = "Study 1") {
  fem::Study s;
  s.id = doc.AllocateId();
  s.name = name;
  return s;
}

// The studies a study reads (a thermal load's, a topology shape's), held as
// a simulation holds them and handed to the planner as its siblings. Every
// stored result reads current unless `stale` names it.
struct TestStudies {
  std::vector<fem::Study> studies;
  std::map<uint64_t, fem::StudyRuntime> results;
  std::vector<uint64_t> stale;

  fem::Study& Add(fem::Study s) {
    studies.push_back(std::move(s));
    return studies.back();
  }
  fem::Study* Find(uint64_t id) {
    for (fem::Study& s : studies)
      if (s.id == id)
        return &s;
    return nullptr;
  }
  fem::StudySiblings Siblings() const {
    fem::StudySiblings out;
    out.find = [this](uint64_t id) -> const fem::Study* {
      for (const fem::Study& s : studies)
        if (s.id == id)
          return &s;
      return nullptr;
    };
    out.results = [this](uint64_t id) -> const fem::StudyRuntime* {
      const auto it = results.find(id);
      return it == results.end() ? nullptr : &it->second;
    };
    out.stale = [this](uint64_t id) {
      for (uint64_t s : stale)
        if (s == id)
          return true;
      return !results.contains(id);
    };
    out.all = [this] {
      std::vector<const fem::Study*> all;
      for (const fem::Study& s : studies)
        all.push_back(&s);
      return all;
    };
    return out;
  }
};
