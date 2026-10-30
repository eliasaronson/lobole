// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/fem/FemStudy.h"
#include "core/ElementHistory.h"
#include "core/PrintedMass.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_State.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Sphere.hxx>

#include "cad/CADMaterials.h"
#include "core/Document.h"
#include "core/Measure.h"
#include "core/fem/Calculix.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemThermal.h"
#include "core/fem/FemTopo.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <utility>

namespace fem {

const char* LoadTypeToString(LoadType type) {
  switch (type) {
  case LoadType::Fixed:
    return "fixed";
  case LoadType::Force:
    return "force";
  case LoadType::Pressure:
    return "pressure";
  case LoadType::Torque:
    return "torque";
  case LoadType::Remote:
    return "remote";
  case LoadType::Pin:
    return "pin";
  case LoadType::Contact:
    return "contact";
  case LoadType::Temperature:
    return "temperature";
  case LoadType::HeatFlux:
    return "heat_flux";
  case LoadType::Convection:
    return "convection";
  case LoadType::HeatGeneration:
    return "heat_generation";
  }
  return "fixed";
}

bool LoadTypeFromString(const std::string& s, LoadType& out) {
  if (s == "fixed") {
    out = LoadType::Fixed;
    return true;
  }
  if (s == "force") {
    out = LoadType::Force;
    return true;
  }
  if (s == "pressure") {
    out = LoadType::Pressure;
    return true;
  }
  if (s == "torque") {
    out = LoadType::Torque;
    return true;
  }
  if (s == "remote") {
    out = LoadType::Remote;
    return true;
  }
  if (s == "pin") {
    out = LoadType::Pin;
    return true;
  }
  if (s == "contact") {
    out = LoadType::Contact;
    return true;
  }
  if (s == "temperature") {
    out = LoadType::Temperature;
    return true;
  }
  if (s == "heat_flux") {
    out = LoadType::HeatFlux;
    return true;
  }
  if (s == "convection") {
    out = LoadType::Convection;
    return true;
  }
  if (s == "heat_generation") {
    out = LoadType::HeatGeneration;
    return true;
  }
  return false;
}

const char* AnalysisTypeToString(AnalysisType type) {
  switch (type) {
  case AnalysisType::Modal:
    return "modal";
  case AnalysisType::Topology:
    return "topology";
  case AnalysisType::Buckling:
    return "buckling";
  case AnalysisType::Thermal:
    return "thermal";
  case AnalysisType::Static:
    break;
  }
  return "static";
}

const char* SolverToString(Solver solver) {
  switch (solver) {
  case Solver::InHouse:
    return "in_house";
  case Solver::Calculix:
    return "calculix";
  case Solver::Auto:
    break;
  }
  return "auto";
}

bool SolverFromString(const std::string& s, Solver& out) {
  if (s == "auto") {
    out = Solver::Auto;
    return true;
  }
  if (s == "in_house") {
    out = Solver::InHouse;
    return true;
  }
  if (s == "calculix") {
    out = Solver::Calculix;
    return true;
  }
  return false;
}

std::string SolverRefusal(Solver solver, AnalysisType type) {
  if (solver == Solver::Calculix && type != AnalysisType::Static &&
      type != AnalysisType::Buckling && type != AnalysisType::Thermal)
    return std::string("CalculiX solves static, buckling and thermal studies "
                       "here; a ") +
           AnalysisTypeToString(type) +
           " study is solved in-house (solver auto or in_house)";
  return {};
}

bool HasContact(const Study& study) {
  for (const Load& l : study.loads)
    if (l.type == LoadType::Contact)
      return true;
  for (const Participant& p : study.participants)
    if (p.role == ParticipantRole::Contact)
      return true;
  return false;
}

bool IsNonlinear(const Study& study) {
  return study.nonlinear.On() || HasContact(study);
}

Solver EffectiveSolver(const Study& study) {
  return IsNonlinear(study) && study.solver == Solver::Auto ? Solver::Calculix
                                                            : study.solver;
}

std::string NonlinearRefusal(Solver solver, AnalysisType type,
                             const Nonlinear& nl, size_t loadCases,
                             ConvergeMode converge, bool contact) {
  const bool any = nl.On() || nl.snapThrough || nl.increments != 0 ||
                   nl.hasImperfection || nl.hasDeflectionLimit;
  if (!any && !contact)
    return {};
  if (any && !nl.largeDeformation && !nl.plasticity && !contact)
    return "nonlinear needs large_deformation: true or plasticity: true -- "
           "the analyses every other nonlinear option belongs to (a study "
           "with a contact is nonlinear without them)";
  if (type != AnalysisType::Static)
    return contact ? std::string("a contact is a static study's (solved "
                                 "nonlinear, on CalculiX); this one is ") +
                         AnalysisTypeToString(type)
                   : std::string("nonlinear is a static study's; this one "
                                 "is ") +
                         AnalysisTypeToString(type);
  if (solver == Solver::InHouse)
    return contact ? "the in-house solver is linear: a study with a contact "
                     "solves on CalculiX (solver auto or calculix)"
           : nl.plasticity
               ? "the in-house solver is linear and elastic: a plastic study "
                 "solves on CalculiX (solver auto or calculix)"
               : "the in-house solver is linear: a nonlinear study "
                 "solves on CalculiX (solver auto or calculix)";
  if (nl.increments < 0)
    return "nonlinear increments is at least 1 (0 or absent: the default)";
  if (nl.snapThrough && !nl.largeDeformation)
    return "snap_through needs large_deformation: true";
  if (!nl.snapThrough && (nl.hasImperfection || nl.hasDeflectionLimit))
    return "imperfection_mm and deflection_limit_mm are a snap-through run's: "
           "set snap_through: true";
  if (nl.hasImperfection && !(nl.imperfectionMm >= 0.0))
    return "imperfection_mm is not negative (0: the perfect shape)";
  if (nl.hasDeflectionLimit && !(nl.deflectionLimitMm > 0.0))
    return "deflection_limit_mm is positive";
  if (loadCases > 0)
    return contact ? "a contact's answer is not a sum of cases -- where it "
                     "closes depends on the whole load -- so a study with a "
                     "contact takes no load cases: make a study for each "
                     "way the part is loaded"
                   : "a nonlinear answer is not a sum of cases, and a "
                     "snap-through run scales ONE load set to its peak, so a "
                     "nonlinear study takes no load cases: make a study for "
                     "each way the part is loaded";
  if (nl.snapThrough && contact)
    return "a snap-through run seeds its shape with the first buckling mode, "
           "solved in-house, and the in-house solver cannot hold a contact: "
           "a study with a contact steps its load up with snap_through false";
  if (nl.snapThrough && converge == ConvergeMode::Always)
    return "converge: always refines round a stress peak, and a snap-through "
           "run's answer is its peak load factor, which that does not "
           "settle: use converge auto or never";
  if (nl.plasticity && converge == ConvergeMode::Always)
    return "converge: always refines round a stress peak, and a plastic "
           "run's stress is held down by the yield where it concentrates -- "
           "what concentrates is the plastic strain, which that does not "
           "settle: use converge auto (which reports it not checked) or "
           "never";
  if (contact && converge == ConvergeMode::Always)
    return "converge: always refines the faces a stress peak lies on, and a "
           "contact's peak lies on a contact face whose partner is meshed "
           "apart from it, so refining one side of the contact does not "
           "settle it: use converge auto (which reports it not checked) or "
           "never";
  return {};
}

std::string HardeningRefusal(const Material& m, const std::string& label,
                             const std::string& withCurves) {
  const std::string names = withCurves.empty()
                                ? std::string("none has one yet")
                                : "the library has one for: " + withCurves;
  if (m.hardening.empty())
    return label +
           " has no hardening curve -- its stress past yield against its "
           "plastic strain -- so a plastic study cannot run on it, and none "
           "is assumed (perfect plasticity would carry no load past yield "
           "that the part does carry, a guessed slope any it does not); " +
           names;
  if (!(m.yieldMPa > 0.0))
    return label + " has no yield strength for its hardening curve to "
                   "start at";
  if (std::abs(m.hardening.front().first - m.yieldMPa) > 1e-9 * m.yieldMPa ||
      m.hardening.front().second != 0.0)
    return label + "'s hardening curve does not start at its yield, " +
           std::to_string(m.yieldMPa) + " MPa at zero plastic strain";
  for (size_t i = 1; i < m.hardening.size(); ++i)
    if (!(m.hardening[i].second > m.hardening[i - 1].second) ||
        !(m.hardening[i].first >= m.hardening[i - 1].first))
      return label + "'s hardening curve is not a rising plastic strain "
                     "under a stress that does not fall";
  return {};
}

std::string ContactRefusal(const Study& study) {
  const bool contact = HasContact(study);
  if (const std::string why =
          NonlinearRefusal(study.solver, study.type, study.nonlinear,
                           study.loadCases.size(), study.converge, contact);
      !why.empty())
    return why;
  if (!contact)
    return {};
  const auto friction = [](double mu) {
    return std::isfinite(mu) && mu >= 0.0;
  };
  for (const Load& l : study.loads) {
    if (l.type == LoadType::Contact) {
      if (!friction(l.friction))
        return "contact support " + std::to_string(l.id) +
               ": friction is a coefficient, 0 or more (0: frictionless)";
      if (!l.region.IsWhole())
        return "contact support " + std::to_string(l.id) +
               ": a contact takes its whole face, so it has no region";
    }
    if (l.type == LoadType::Pin)
      return "pin " + std::to_string(l.id) +
             ": a pin's contact is found by the in-house solver, which cannot "
             "hold a contact as well; hold the part with fixed supports and "
             "contacts";
  }
  for (const Participant& p : study.participants)
    if (p.role == ParticipantRole::Contact && !friction(p.friction))
      return "contact participant " + std::to_string(p.occurrenceId) +
             ": friction is a coefficient, 0 or more (0: frictionless)";
  if (study.shapeFrom.study != 0)
    return "a static study of a topology result solves the result's own "
           "tets, and a contact's partner is meshed from a shape: a study "
           "with a contact meshes its body";
  return {};
}

Material MaterialFromLibrary(const CADMaterials::MaterialProperties& mp) {
  // GPa -> MPa; density kg/m^3 -> tonne/mm^3 for the consistent mm-N-t-s
  // system; a conductivity in W/(m K) is the same number in mW/(mm K).
  Material m{mp.youngGPa * 1000.0, mp.poisson,       mp.density * 1e-12,
             mp.yieldMPa,          mp.interlayerMPa, mp.hardening};
  m.conductivityWmK = mp.conductivityWmK;
  m.expansionPerK = mp.expansionPerK;
  m.serviceMaxC = mp.serviceMaxC;
  m.serviceBasis = mp.serviceBasis;
  m.bulkConductivity = mp.bulkConductivity;
  return m;
}

std::string ThermalMaterialRefusal(const Material& m, const std::string& label,
                                   bool needConductivity, bool needExpansion,
                                   const std::string& with) {
  const std::string names = with.empty() ? std::string("none has one yet")
                                         : "the library has one for: " + with;
  if (needConductivity && !(m.conductivityWmK > 0.0))
    return label +
           " has no thermal conductivity in the library (none is entered "
           "without a cited source), so a thermal study cannot solve it; " +
           names;
  if (needExpansion && !(m.expansionPerK > 0.0))
    return label +
           " has no thermal expansion coefficient in the library (none is "
           "entered without a cited source), so a thermal load cannot "
           "expand it; " +
           names;
  return {};
}

namespace {
bool Given(double v) { return !std::isnan(v); }
} // namespace

std::string ThermalLoadRefusal(const Load& l) {
  const std::string what =
      std::string(LoadTypeToString(l.type)) + " " + std::to_string(l.id);
  const auto temperature = [&](double c, const char* key) -> std::string {
    if (!std::isfinite(c))
      return what + ": " + key + " is a number, degrees C";
    if (c < kAbsoluteZeroC)
      return what + ": " + key + " " + std::to_string(c) +
             " is below absolute zero (" + std::to_string(kAbsoluteZeroC) +
             " C)";
    return {};
  };
  // A quantity another type takes is refused, not carried: the file writes
  // what is given, and its reader refuses it on the wrong type.
  const auto stray = [&](const char* key, double v, bool mine) {
    return !mine && Given(v) ? what + " does not take " + key : std::string();
  };
  using LT = LoadType;
  for (const std::string& why :
       {stray("temperature_c", l.temperatureC, l.type == LT::Temperature),
        stray("power_w", l.powerW,
              l.type == LT::HeatFlux || l.type == LT::HeatGeneration),
        stray("flux_w_m2", l.fluxWm2, l.type == LT::HeatFlux),
        stray("film_w_m2k", l.filmWm2K, l.type == LT::Convection),
        stray("ambient_c", l.ambientC, l.type == LT::Convection),
        std::string(!l.body.empty() && l.type != LT::HeatGeneration
                        ? what + " does not take body"
                        : "")})
    if (!why.empty())
      return why;
  switch (l.type) {
  case LoadType::Temperature:
    if (!Given(l.temperatureC))
      return what + " holds its face at temperature_c, which it needs: a "
                    "held temperature is not derivable from the part";
    return temperature(l.temperatureC, "temperature_c");
  case LoadType::HeatFlux:
    if (Given(l.powerW) == Given(l.fluxWm2))
      return what + " takes exactly one of power_w (the total into the face, "
                    "W) and flux_w_m2 (per area, W/m^2)";
    if (!std::isfinite(Given(l.powerW) ? l.powerW : l.fluxWm2))
      return what + ": its heat is a number";
    return {};
  case LoadType::Convection:
    if (!Given(l.filmWm2K))
      return what +
             " needs film_w_m2k, the film coefficient: it is not derivable "
             "from the part. Incropera's Table 1.1 gives free convection in "
             "gases 2-25 W/(m^2 K), forced convection in gases 25-250, and "
             "liquids higher (free 50-1000, forced 100-20000)";
    if (!(std::isfinite(l.filmWm2K) && l.filmWm2K > 0.0))
      return what + ": film_w_m2k is positive, W/(m^2 K)";
    if (!Given(l.ambientC))
      return what + " needs ambient_c, the temperature of the fluid it "
                    "convects to: it is not derivable from the part";
    return temperature(l.ambientC, "ambient_c");
  case LoadType::HeatGeneration:
    if (!Given(l.powerW))
      return what + " needs power_w, the heat the body makes in all (W)";
    if (!std::isfinite(l.powerW))
      return what + ": power_w is a number";
    if (l.body.empty() && l.occurrenceId == 0)
      return what + " names the body it heats (body, its name; occurrence "
                    "on an assembly)";
    if (!l.region.IsWhole())
      return what + " heats its whole body, so it has no region";
    return {};
  default:
    break;
  }
  return {};
}

std::optional<double> DerivedStressFree(const Study& thermal) {
  std::optional<double> ambient;
  for (const Load& l : thermal.loads)
    if (l.type == LoadType::Convection && Given(l.ambientC)) {
      if (ambient && *ambient != l.ambientC)
        return std::nullopt;
      ambient = l.ambientC;
    }
  return ambient;
}

std::string ThermalRefusal(const Study& study) {
  const bool thermal = study.type == AnalysisType::Thermal;
  for (const Load& l : study.loads) {
    const std::string what =
        "load " + std::to_string(l.id) + " (" + LoadTypeToString(l.type) + ")";
    if (thermal && !IsThermalLoad(l.type))
      return what + " is a structural load, and a thermal study takes " +
             kThermalLoadTypeNames +
             " loads: a thermal load on a static study expands the part "
             "(thermal_load)";
    if (!thermal && IsThermalLoad(l.type))
      return what + " is a thermal study's, and this is a " +
             AnalysisTypeToString(study.type) +
             " study: put it on a study of type thermal";
    if (IsThermalLoad(l.type))
      if (std::string why = ThermalLoadRefusal(l); !why.empty())
        return why;
  }
  if (thermal) {
    if (!study.loadCases.empty())
      return "a thermal study takes no load cases: its loads all act "
             "together; make a study for each way the part is heated";
    if (study.gravity != glm::dvec3(0.0))
      return "gravity is a static study's load; a thermal study takes "
             "temperature, heat_flux, convection and heat_generation loads";
    if (study.nonlinear.On() || study.nonlinear.snapThrough)
      return "nonlinear is a static study's; a thermal study solves steady "
             "conduction, which is linear";
    if (study.shapeFrom.study != 0)
      return "shape_from is a static study's; a thermal study meshes its "
             "body";
    if (study.thermalLoad.On())
      return "thermal_load is a static study's: a thermal study solves for "
             "the temperature it would take";
    for (const Participant& p : study.participants) {
      if (p.role == ParticipantRole::Contact)
        return "contact participant " + std::to_string(p.occurrenceId) +
               ": no heat crosses a contact here (it has no conductance); "
               "bond it to conduct across the joint, or leave it out";
      if (p.role == ParticipantRole::Rigid)
        return "rigid participant " + std::to_string(p.occurrenceId) +
               ": a rigid part is a support, and is not a heat sink unless "
               "a load says so -- hold the face it touches at a temperature "
               "or cool it by convection, and leave the part out";
    }
    for (const Load& l : study.loads)
      if (!l.region.IsWhole() && l.type == LoadType::HeatGeneration)
        return "heat_generation " + std::to_string(l.id) +
               " heats its whole body, so it has no region";
    return {};
  }
  const ThermalLoad& tl = study.thermalLoad;
  if (!tl.On())
    return {};
  if (study.type == AnalysisType::Buckling)
    return "thermal_load is a static study's: thermal buckling (a clamped "
           "part's critical temperature rise) is not solved yet -- CalculiX "
           "takes no temperature in a linear buckling step, so its "
           "cross-check needs a preloaded perturbation step this bridge does "
           "not write (docs/simulation/README.md#not-done)";
  if (study.type != AnalysisType::Static)
    return std::string("thermal_load is a static study's; this one is ") +
           AnalysisTypeToString(study.type);
  if (tl.kind == ThermalLoad::Kind::Uniform) {
    if (!tl.hasStressFree)
      return "a uniform thermal_load needs stress_free_c, the temperature "
             "the part is stress-free at: it is not derivable from the part";
    if (!std::isfinite(tl.uniformC) || tl.uniformC < kAbsoluteZeroC)
      return "thermal_load uniform_c is a temperature, not below absolute "
             "zero";
  }
  if (tl.kind == ThermalLoad::Kind::FromStudy && tl.fromStudy == 0)
    return "thermal_load from_study names the thermal study whose "
           "temperature it takes";
  if (tl.hasStressFree &&
      (!std::isfinite(tl.stressFreeC) || tl.stressFreeC < kAbsoluteZeroC))
    return "thermal_load stress_free_c is a temperature, not below absolute "
           "zero";
  if (study.nonlinear.snapThrough)
    return "a snap-through run seeds its shape with an in-house buckling mode, "
           "which does not take a thermal load: run it as a large-deformation "
           "study (snap_through false)";
  for (const Participant& p : study.participants)
    if (p.role == ParticipantRole::Contact)
      return "contact participant " + std::to_string(p.occurrenceId) +
             ": a thermal load is not solved beside a contact participant -- "
             "no heat crosses the contact, so the partner's temperature is "
             "not known";
  if (study.shapeFrom.study != 0 && tl.kind == ThermalLoad::Kind::FromStudy)
    return "a static study of a topology result has no faces to take a "
           "thermal study's loads; use a uniform thermal_load";
  return {};
}

std::string AssemblyThermalRefusal(const Study& study) {
  if (study.type == AnalysisType::Thermal)
    return "a thermal study is a part study's: an assembly study does not "
           "solve heat yet (docs/simulation/README.md#not-done); a static "
           "assembly study takes a uniform thermal_load";
  for (const Load& l : study.loads)
    if (IsThermalLoad(l.type))
      return "load " + std::to_string(l.id) + " (" + LoadTypeToString(l.type) +
             ") is a thermal study's, and an assembly study does not solve "
             "heat yet (docs/simulation/README.md#not-done)";
  if (study.thermalLoad.kind == ThermalLoad::Kind::FromStudy)
    return "thermal_load from_study re-solves a thermal study's heat, and an "
           "assembly study does not solve heat yet: on an assembly the "
           "thermal load is uniform, {uniform_c, stress_free_c}";
  return {};
}

const char* CpuSolverName(AnalysisType type) {
  switch (type) {
  case AnalysisType::Modal:
    return "cpu (Eigen SimplicialLDLT, shift-invert)";
  case AnalysisType::Topology:
    return "cpu (Eigen CG-IC)";
  case AnalysisType::Buckling:
    return "cpu (Eigen SimplicialLDLT; Spectra, Cholesky mode)";
  case AnalysisType::Thermal:
    return "cpu (Eigen SimplicialLDLT, scalar conduction)";
  case AnalysisType::Static:
    break;
  }
  return "cpu (Eigen SimplicialLDLT)";
}

bool AnalysisTypeFromString(const std::string& s, AnalysisType& out) {
  if (s == "static") {
    out = AnalysisType::Static;
    return true;
  }
  if (s == "modal") {
    out = AnalysisType::Modal;
    return true;
  }
  if (s == "topology") {
    out = AnalysisType::Topology;
    return true;
  }
  if (s == "buckling") {
    out = AnalysisType::Buckling;
    return true;
  }
  if (s == "thermal") {
    out = AnalysisType::Thermal;
    return true;
  }
  return false;
}

const char* ConvergeModeToString(ConvergeMode mode) {
  switch (mode) {
  case ConvergeMode::Always:
    return "always";
  case ConvergeMode::Never:
    return "never";
  case ConvergeMode::Auto:
    break;
  }
  return "auto";
}

bool ConvergeModeFromString(const std::string& s, ConvergeMode& out) {
  for (ConvergeMode m :
       {ConvergeMode::Auto, ConvergeMode::Always, ConvergeMode::Never})
    if (s == ConvergeModeToString(m)) {
      out = m;
      return true;
    }
  return false;
}

const char* ParticipantRoleToString(ParticipantRole role) {
  switch (role) {
  case ParticipantRole::Meshed:
    return "meshed";
  case ParticipantRole::Rigid:
    return "rigid";
  case ParticipantRole::Ignored:
    return "ignored";
  case ParticipantRole::Bonded:
    return "bonded";
  case ParticipantRole::Contact:
    return "contact";
  }
  return "rigid";
}

bool ParticipantRoleFromString(const std::string& s, ParticipantRole& out) {
  if (s == "meshed") {
    out = ParticipantRole::Meshed;
    return true;
  }
  if (s == "rigid") {
    out = ParticipantRole::Rigid;
    return true;
  }
  if (s == "ignored") {
    out = ParticipantRole::Ignored;
    return true;
  }
  if (s == "bonded") {
    out = ParticipantRole::Bonded;
    return true;
  }
  if (s == "contact") {
    out = ParticipantRole::Contact;
    return true;
  }
  return false;
}

bool EvaluateStudyExprs(Study& study, const SymbolTable& symbols,
                        std::string* error) {
  bool ok = true;
  // Evaluate one value+expr pair; on failure records the first error but keeps
  // going so every bad field is left with its last good value.
  auto eval = [&](const std::string& text, double& value,
                  const std::string& what) {
    if (text.empty())
      return;
    const auto v = ExpressionEngine::Default().EvaluateText(text, symbols);
    if (v.has_value()) {
      value = *v;
    } else if (ok) {
      ok = false;
      if (error)
        *error = what + " expression '" + text + "': " + v.error().message;
    }
  };

  eval(study.meshSizeExpr, study.meshSize, "mesh size");
  for (auto& r : study.refinements)
    eval(r.sizeExpr, r.size, "refinement size");
  for (auto& l : study.loads)
    eval(l.magnitudeExpr, l.magnitude, "load magnitude");
  for (auto& pt : study.participants)
    eval(pt.meshSizeExpr, pt.meshSize, "participant mesh size");
  for (auto& c : study.loadCases)
    eval(c.weightExpr, c.weight, "load case '" + c.name + "' weight");
  if (study.type == AnalysisType::Topology) {
    eval(study.volumeFractionExpr, study.volumeFraction, "volume fraction");
    eval(study.filterRadiusExpr, study.filterRadius, "filter radius");
  }
  return ok;
}

bool LoadRegion::Contains(const glm::dvec3& p) const {
  switch (kind) {
  case Kind::Whole:
    return true;
  case Kind::Band: {
    const double t = glm::dot(p, glm::normalize(normal));
    return t >= fromMm && t <= toMm;
  }
  case Kind::Sphere:
    return glm::length(p - center) <= radiusMm;
  }
  return true;
}

std::string LoadRegion::Why() const {
  if (kind == Kind::Band) {
    if (!(glm::length(normal) > 1e-12) || !std::isfinite(glm::length(normal)))
      return "a band region's normal is a non-zero [x, y, z]";
    if (!std::isfinite(fromMm) || !std::isfinite(toMm) || !(toMm > fromMm))
      return "a band region runs from from_mm to a larger to_mm (mm along "
             "its normal)";
  }
  if (kind == Kind::Sphere && (!(radiusMm > 0.0) || !std::isfinite(radiusMm) ||
                               !std::isfinite(glm::length(center))))
    return "a radius region needs center_mm [x, y, z] and a positive "
           "radius_mm";
  return {};
}

bool ValidateLoadCases(const Study& study, std::string* error) {
  auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  // A load on part of a face
  // (docs/simulation/README.md#loads-on-part-of-a-face): a static or modal
  // study of a part. A topology study's frozen skin is whole faces.
  for (const Load& l : study.loads) {
    if (l.region.IsWhole())
      continue;
    if (const std::string why = l.region.Why(); !why.empty())
      return fail("load " + std::to_string(l.id) + ": " + why);
    if (study.type == AnalysisType::Topology)
      return fail("load " + std::to_string(l.id) +
                  " acts on part of its face (region), and a topology "
                  "study freezes whole faces: load a face of its own");
    if (study.IsAssemblyStudy())
      return fail("load " + std::to_string(l.id) +
                  " acts on part of its face (region), which is a part "
                  "study's; on an assembly study load a whole face");
  }
  const auto known = [&] {
    std::string k;
    for (const LoadCase& c : study.loadCases)
      k += (k.empty() ? "" : ", ") + ("'" + c.name + "'");
    return k;
  };
  if (study.loadCases.empty()) {
    for (const Load& l : study.loads)
      if (!l.loadCase.empty())
        return fail("load " + std::to_string(l.id) + " names load case '" +
                    l.loadCase +
                    "', and the study has none (set_study load_cases)");
    return true;
  }
  if (study.type == AnalysisType::Modal)
    return fail("a modal study has no loads to put in load cases; load cases "
                "belong to static and topology studies");
  if (study.type == AnalysisType::Buckling)
    return fail("a buckling factor scales ONE load set, so a buckling study "
                "takes no load cases: make a buckling study for each way the "
                "part is loaded");
  for (size_t i = 0; i < study.loadCases.size(); ++i) {
    const LoadCase& c = study.loadCases[i];
    if (c.name.empty())
      return fail("load case " + std::to_string(i + 1) + " has no name");
    for (size_t j = 0; j < i; ++j)
      if (study.loadCases[j].name == c.name)
        return fail("load case '" + c.name + "' is named twice");
    if (!(c.weight > 0.0))
      return fail("load case '" + c.name + "': weight " +
                  std::to_string(c.weight) + " is not positive");
  }
  std::vector<int> count(study.loadCases.size(), 0);
  for (const Load& l : study.loads) {
    if (IsSupport(l.type)) {
      if (!l.loadCase.empty())
        return fail("load " + std::to_string(l.id) +
                    " is a support in load case '" + l.loadCase +
                    "': supports hold in every case, so a support names none");
      continue;
    }
    const int k = study.FindLoadCase(l.loadCase);
    if (k < 0)
      return fail(l.loadCase.empty()
                      ? "load " + std::to_string(l.id) +
                            " is in no load case; the study's cases are " +
                            known()
                      : "load " + std::to_string(l.id) + " names load case '" +
                            l.loadCase + "'; the study's cases are " + known());
    ++count[k];
  }
  for (size_t i = 0; i < count.size(); ++i)
    if (count[i] == 0)
      return fail("load case '" + study.loadCases[i].name +
                  "' has no force or pressure in it");
  return true;
}

// featureId 0 = "whichever body has this element". An element name looks
// like it carries its feature ("3/face7"), but that prefix is the feature
// that CREATED the element, not the body's `created_by` — after one boolean
// the two differ, so a caller who reads the id off the name picks a body
// that does not exist. Searching is the honest resolution, and ambiguity is
// refused rather than guessed.
bool ResolveFace(const Document& doc, const ElementRef& ref,
                 const std::string& what, ResolvedFace& out,
                 std::string* error) {
  const auto& bodies = doc.GetBodies();
  std::vector<int> hits;
  for (int i = 0; i < static_cast<int>(bodies.size()); ++i) {
    if (ref.featureId != 0 && bodies[i].createdBy != ref.featureId)
      continue;
    if (const auto idx = bodies[i].elementMap.Resolve(ref.name)) {
      // A mesh body has no faces to load or mesh: a study refuses it
      // (docs/interchange/README.md#mesh-bodies).
      if (bodies[i].IsMesh()) {
        if (error)
          *error = MeshBodyRefusal(bodies[i], what + " of a FEM study");
        return false;
      }
      if (ref.featureId != 0) {
        out.bodyIndex = i;
        out.faceIndex = *idx;
        return true;
      }
      hits.push_back(i);
      out.bodyIndex = i;
      out.faceIndex = *idx;
    }
  }
  if (hits.size() == 1)
    return true;
  if (error) {
    if (hits.size() > 1) {
      *error = what + ": '" + ref.name + "' is on " +
               std::to_string(hits.size()) +
               " bodies — say which with feature_id (its created_by):";
      for (int i : hits)
        *error += " " + std::to_string(bodies[i].createdBy) + " (" +
                  bodies[i].name + ")";
    } else {
      *error = topo::UnresolvedReason(bodies, ref, topo::ElementKind::Face,
                                      what + ": face");
      if (ref.featureId != 0) {
        bool onAnother = false;
        for (const auto& b : bodies)
          if (b.elementMap.Resolve(ref.name))
            onAnother = true;
        if (onAnother)
          *error += " on the body created by feature " +
                    std::to_string(ref.featureId) +
                    " — it is on another body (drop feature_id and it will be "
                    "found, or read created_by from get_state)";
      }
    }
  }
  return false;
}

std::optional<glm::dvec3> DefaultForceDirection(const Document& doc,
                                                const ElementRef& face) {
  ResolvedFace resolved;
  if (!ResolveFace(doc, face, "force", resolved, nullptr))
    return std::nullopt;
  const auto info = Measure::DescribeFaceAt(doc.GetBodies()[resolved.bodyIndex],
                                            resolved.faceIndex);
  if (!info)
    return std::nullopt;
  const glm::dvec3 n = info->normal;
  if (glm::dot(n, n) < 1e-12)
    return std::nullopt;
  // Into the face: pushing on a face is what placing a force on it means.
  return -glm::normalize(n);
}

namespace {
// Face `index` of `shape` in TopExp_Explorer order, which is what a
// ResolvedFace counts (Measure's FaceAt).
TopoDS_Shape FaceByIndex(const TopoDS_Shape& shape, int index) {
  int i = 0;
  for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next(), ++i)
    if (i == index)
      return ex.Current();
  return {};
}

// Every cylindrical, conical and spherical face of `shape`, by its index:
// what a buckling mode's knockdown reads (docs/simulation/README.md
// #buckling).
std::map<int, StudyJobBody::CurvedFace>
CurvedFacesOf(const TopoDS_Shape& shape) {
  std::map<int, StudyJobBody::CurvedFace> out;
  if (shape.IsNull())
    return out;
  const auto v = [](const gp_Pnt& p) {
    return glm::dvec3(p.X(), p.Y(), p.Z());
  };
  const auto d = [](const gp_Dir& p) {
    return glm::dvec3(p.X(), p.Y(), p.Z());
  };
  int i = 0;
  for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next(), ++i) {
    BRepAdaptor_Surface surf(TopoDS::Face(ex.Current()));
    StudyJobBody::CurvedFace f;
    switch (surf.GetType()) {
    case GeomAbs_Cylinder: {
      const gp_Cylinder c = surf.Cylinder();
      f.kind = "cylinder";
      f.origin = v(c.Location());
      f.axis = d(c.Axis().Direction());
      f.radius = c.Radius();
      break;
    }
    case GeomAbs_Cone: {
      const gp_Cone c = surf.Cone();
      f.kind = "cone";
      f.origin = v(c.Location());
      f.axis = d(c.Axis().Direction());
      f.radius = c.RefRadius();
      f.semiAngle = c.SemiAngle();
      break;
    }
    case GeomAbs_Sphere: {
      const gp_Sphere c = surf.Sphere();
      f.kind = "sphere";
      f.origin = v(c.Location());
      f.radius = c.Radius();
      break;
    }
    default:
      continue;
    }
    out[i] = f;
  }
  return out;
}

// The ground under a flat face (docs/simulation/README.md#contact): a box on
// the face's outer side, its face against the part on the face's plane,
// reaching past the face's extent by this fraction of its larger side on
// every side, and as deep. Rigid, so its depth and mesh size (the same) only
// have to hold the face's plane, which any mesh of a flat face does.
constexpr double kGroundMargin = 0.25;

} // namespace

bool GroundUnder(const TopoDS_Shape& face, StudyBodyPlan::ContactPartner& out,
                 std::string* error) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  if (face.IsNull() || face.ShapeType() != TopAbs_FACE)
    return fail("its face is not on the body");
  BRepAdaptor_Surface surf(TopoDS::Face(face));
  if (surf.GetType() != GeomAbs_Plane)
    return fail("on a part the ground is the plane a flat face lies on, and "
                "this face is not flat; a curved face rests on a rigid "
                "occurrence in an assembly study (contact_with)");
  const gp_Pln pl = surf.Plane();
  gp_Dir n = pl.Axis().Direction();
  if (face.Orientation() == TopAbs_REVERSED)
    n.Reverse();
  const gp_Dir u = pl.XAxis().Direction();
  const gp_Dir v = n.Crossed(u);
  const gp_Pnt o = pl.Location();
  Bnd_Box box;
  BRepBndLib::Add(face, box);
  if (box.IsVoid())
    return fail("its face has no extent");
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  double umin = 1e300, umax = -1e300, vmin = 1e300, vmax = -1e300;
  for (int c = 0; c < 8; ++c) {
    const gp_Vec d(o,
                   gp_Pnt(c & 1 ? x1 : x0, c & 2 ? y1 : y0, c & 4 ? z1 : z0));
    umin = std::min(umin, d.Dot(gp_Vec(u)));
    umax = std::max(umax, d.Dot(gp_Vec(u)));
    vmin = std::min(vmin, d.Dot(gp_Vec(v)));
    vmax = std::max(vmax, d.Dot(gp_Vec(v)));
  }
  const double extent = std::max(umax - umin, vmax - vmin);
  if (!(extent > 0.0))
    return fail("its face has no extent");
  const double margin = kGroundMargin * extent;
  const gp_Pnt corner =
      o.Translated(gp_Vec(u) * (umin - margin) + gp_Vec(v) * (vmin - margin));
  BRepPrimAPI_MakeBox mk(gp_Ax2(corner, n, u), umax - umin + 2 * margin,
                         vmax - vmin + 2 * margin, margin);
  mk.Build();
  if (!mk.IsDone())
    return fail("the ground under it could not be made");
  out = StudyBodyPlan::ContactPartner{};
  out.rigid = true;
  out.name = "ground";
  out.shape = mk.Shape();
  out.settings.maxSize = margin;
  int i = 0;
  for (TopExp_Explorer ex(out.shape, TopAbs_FACE); ex.More(); ex.Next(), ++i) {
    GProp_GProps props;
    BRepGProp::SurfaceProperties(ex.Current(), props);
    const gp_Vec d(o, props.CentreOfMass());
    if (std::abs(d.Dot(gp_Vec(n))) < 1e-9 * extent)
      out.faces.push_back(i);
  }
  if (out.faces.size() != 1)
    return fail("the ground under it has no face on its plane");
  return true;
}

std::optional<LoadAxis> CylindricalFaceAxis(const TopoDS_Shape& face) {
  if (face.IsNull() || face.ShapeType() != TopAbs_FACE)
    return std::nullopt;
  BRepAdaptor_Surface surf(TopoDS::Face(face));
  if (surf.GetType() != GeomAbs_Cylinder)
    return std::nullopt;
  const gp_Ax1 ax = surf.Cylinder().Axis();
  glm::dvec3 d(ax.Direction().X(), ax.Direction().Y(), ax.Direction().Z());
  // The surface's own sign is arbitrary; the largest component positive
  // makes a shaft along Z turn about +Z whichever way it was modelled.
  int big = 0;
  for (int k = 1; k < 3; ++k)
    if (std::abs(d[k]) > std::abs(d[big]) + 1e-9)
      big = k;
  if (d[big] < 0)
    d = -d;
  const glm::dvec3 o(ax.Location().X(), ax.Location().Y(), ax.Location().Z());
  GProp_GProps props;
  BRepGProp::SurfaceProperties(face, props);
  const gp_Pnt c = props.CentreOfMass();
  const glm::dvec3 cc(c.X(), c.Y(), c.Z());
  LoadAxis out;
  out.direction = glm::normalize(d);
  out.origin = o + glm::dot(cc - o, out.direction) * out.direction;
  return out;
}

std::optional<LoadAxis> CylindricalFaceAxis(const Document& doc,
                                            const ElementRef& face) {
  ResolvedFace rf;
  if (!ResolveFace(doc, face, "torque", rf, nullptr))
    return std::nullopt;
  return CylindricalFaceAxis(FaceByIndex(
      doc.GetBodies()[rf.bodyIndex].shape.GetShape(), rf.faceIndex));
}

std::optional<LoadAxis> TorqueAxis(const Document& doc, const Load& load) {
  if (load.HasAxis())
    return LoadAxis{load.axisOrigin, glm::normalize(load.axisDirection)};
  return CylindricalFaceAxis(doc, load.face);
}

bool CompleteLoadGeometry(const TopoDS_Shape& face, Load& load,
                          std::string* error) {
  if (load.type == LoadType::Torque && !load.HasAxis()) {
    const auto axis = CylindricalFaceAxis(face);
    if (!axis) {
      if (error)
        *error = "load " + std::to_string(load.id) +
                 ": a torque on a face that is not cylindrical needs an axis "
                 "(axis_origin_mm and axis_dir); a cylindrical face lends its "
                 "own";
      return false;
    }
    load.axisOrigin = axis->origin;
    load.axisDirection = axis->direction;
  }
  if (load.type == LoadType::Pin) {
    const auto axis = CylindricalFaceAxis(face);
    if (!axis) {
      if (error)
        *error = "load " + std::to_string(load.id) +
                 ": a pin support goes on a cylindrical face (a bore or a "
                 "shaft); this face is not one";
      return false;
    }
    load.axisOrigin = axis->origin;
    load.axisDirection = axis->direction;
  }
  if (load.type == LoadType::Remote && !load.hasPoint) {
    if (error)
      *error = "load " + std::to_string(load.id) +
               ": a remote load needs the point it acts at (point_mm)";
    return false;
  }
  return true;
}

FaceLoad MakeFaceLoad(const Load& load) {
  FaceLoad fl;
  fl.type = load.type;
  fl.magnitude = load.magnitude;
  fl.direction = load.direction;
  if (load.type == LoadType::Torque) {
    fl.origin = load.axisOrigin;
    fl.axis = load.axisDirection;
  } else if (load.type == LoadType::Remote) {
    fl.origin = load.point;
  }
  return fl;
}

bool PlanStudyMesh(const Document& doc, Study& study, MeshPlan& out,
                   std::string* error) {
  out = MeshPlan{};
  study.error.clear();
  auto fail = [&](const std::string& m) {
    study.error = m;
    if (error)
      *error = m;
    return false;
  };

  std::string err;
  if (!EvaluateStudyExprs(study, doc.Symbols(), &err))
    return fail(err);

  // Resolve every ref, grouping per body. Per-face refinement sizes are keyed
  // by resolved face index (what fem::Mesh's MeshSettings.faceSizes expects).
  std::map<int, MeshSettings> perBody; // bodyIndex -> settings
  auto touch = [&](int bodyIndex) -> MeshSettings& {
    MeshSettings& s = perBody[bodyIndex];
    s.maxSize = study.meshSize; // 0 => auto, per body
    return s;
  };

  for (size_t i = 0; i < study.loads.size(); ++i) {
    // A heat source names its body.
    if (study.loads[i].type == LoadType::HeatGeneration) {
      const auto& bodies = doc.GetBodies();
      int found = -1;
      for (size_t b = 0; b < bodies.size(); ++b)
        if (!bodies[b].reference && bodies[b].name == study.loads[i].body)
          found = static_cast<int>(b);
      if (found < 0)
        return fail("heat_generation " + std::to_string(study.loads[i].id) +
                    ": no body is named '" + study.loads[i].body + "'");
      touch(found);
      continue;
    }
    ResolvedFace rf;
    if (!ResolveFace(doc, study.loads[i].face, "load " + std::to_string(i + 1),
                     rf, &err))
      return fail(err);
    touch(rf.bodyIndex); // ensure the loaded body is meshed
  }
  for (size_t i = 0; i < study.refinements.size(); ++i) {
    ResolvedFace rf;
    if (!ResolveFace(doc, study.refinements[i].face,
                     "refinement " + std::to_string(i + 1), rf, &err))
      return fail(err);
    touch(rf.bodyIndex)
        .faceSizes.push_back({rf.faceIndex, study.refinements[i].size});
  }

  if (perBody.empty())
    return fail("study has no loads or refinements to mesh");

  const auto& bodies = doc.GetBodies();
  for (const auto& [bodyIndex, settings] : perBody) {
    if (bodies[bodyIndex].reference)
      return fail("body '" + bodies[bodyIndex].name +
                  "' is a derived reference (another part's geometry); set "
                  "reference: false on its Derived feature to analyse it");
    MeshBodyPlan b;
    b.bodyIndex = bodyIndex;
    b.bodyName = bodies[bodyIndex].name;
    b.shape = BRepBuilderAPI_Copy(bodies[bodyIndex].shape.GetShape()).Shape();
    b.settings = settings;
    out.bodies.push_back(std::move(b));
  }
  return true;
}

bool MeshPlanned(const MeshPlan& plan, std::vector<StudyBodyMesh>& out,
                 std::string* error) {
  out.clear();
  std::string err;
  for (const MeshBodyPlan& b : plan.bodies) {
    auto mesh = Mesh(b.shape, b.settings, &err);
    if (!mesh) {
      if (error)
        *error = "body '" + b.bodyName + "': " + err;
      return false;
    }
    StudyBodyMesh r;
    r.bodyIndex = b.bodyIndex;
    r.bodyName = b.bodyName;
    r.mesh = std::move(*mesh);
    out.push_back(std::move(r));
  }
  return true;
}

bool MeshStudy(const Document& doc, Study& study,
               std::vector<StudyBodyMesh>& out, std::string* error) {
  out.clear();
  MeshPlan plan;
  if (!PlanStudyMesh(doc, study, plan, error))
    return false;
  std::string err;
  if (!MeshPlanned(plan, out, &err)) {
    study.error = err;
    if (error)
      *error = err;
    return false;
  }
  return true;
}

namespace {

// The mesh of a static study of a topology result (ShapeSource): the result's
// tets at or above the threshold, or all of them when `whole`, less what is
// not face-connected to a support. Fills bp.premeshed and the plan's report.
bool PlanShapeMesh(const Document& doc, const Study& study, int bodyIndex,
                   StudyBodyPlan& bp, StudyPlan& out, std::string* error) {
  auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  const ShapeSource& src = study.shapeFrom;
  if (study.type != AnalysisType::Static)
    return fail("only a static study takes its shape from a topology result");
  const Study* topo = doc.FindStudy(src.study);
  if (topo == nullptr || topo->type != AnalysisType::Topology)
    return fail("shape_from names study " + std::to_string(src.study) +
                ", which is not a topology study of this part");
  const StudyRuntime* rt = doc.StudyResults(src.study);
  if (rt == nullptr || rt->bodies.empty())
    return fail("topology study '" + topo->name +
                "' has no result to take a shape from: run it first");
  if (doc.StudyResultsStale(src.study))
    return fail("topology study '" + topo->name +
                "' is stale: the part changed after it ran, so its shape is "
                "not this part's -- run it again first");
  const StudyBodyResult& tb = rt->bodies.front();
  if (tb.bodyIndex != bodyIndex)
    return fail("the loads are on body '" + doc.GetBodies()[bodyIndex].name +
                "', and topology study '" + topo->name + "' optimised '" +
                tb.bodyName + "'");
  if (!study.refinements.empty())
    return fail("a study of a topology result meshes nothing, so it takes no "
                "refinements: the mesh is the result's");
  const FemMesh& m = tb.mesh;
  const size_t n = m.tets.size();
  if (tb.density.size() != n)
    return fail("topology study '" + topo->name + "' has no density field");
  if (!src.whole && (src.threshold < 0.0 || src.threshold >= 1.0))
    return fail("shape threshold " + std::to_string(src.threshold) +
                " is not in [0, 1) (0 = the volume-matched level)");
  // The field thresholded is the density averaged onto the corners and back
  // onto each tet -- the field the iso-surface (the Shape view, the STL) is
  // drawn from. Per tet, a member one or two tets wide at its threshold is a
  // chain that meets itself at edges: a hinge to a static solve, which then
  // reports the swing of a loose sliver as the part's deflection.
  std::vector<double> nodeD(m.nodes.size(), 0.0);
  std::vector<int> nodeN(m.nodes.size(), 0);
  for (size_t e = 0; e < n; ++e)
    for (int i = 0; i < 4; ++i) {
      nodeD[m.tets[e][i]] += tb.density[e];
      ++nodeN[m.tets[e][i]];
    }
  std::vector<double> smooth(n, 0.0);
  for (size_t e = 0; e < n; ++e) {
    for (int i = 0; i < 4; ++i) {
      const int v = m.tets[e][i];
      smooth[e] += 0.25 * nodeD[v] / std::max(1, nodeN[v]);
    }
    // A frozen tet (a support, a load, a keep face) stays, whatever its
    // neighbours.
    if (tb.density[e] >= 1.0 - 1e-9)
      smooth[e] = 1.0;
  }
  const double thr =
      src.whole ? 0.0
      : src.threshold > 0.0
          ? src.threshold
          : AutoIsoThreshold(m, smooth, rt->volumeFractionAchieved);

  // The supports' tets, which the solid must reach, and the loaded faces,
  // which must reach a support through it.
  std::set<int> fixedFaces;
  std::map<int, uint64_t> loadedFaces; // face index -> a load on it
  for (const auto& [faceIndex, load] : bp.loads)
    if (IsSupport(load.type))
      fixedFaces.insert(faceIndex);
    else
      loadedFaces.emplace(faceIndex, load.id);
  std::vector<char> fixedNode(m.nodes.size(), 0);
  for (const FemMesh::BoundaryTri& t : m.boundary)
    if (fixedFaces.count(t.faceIndex))
      for (int v : t.nodes)
        fixedNode[v] = 1;
  std::vector<char> anchored(n, 0);
  for (size_t e = 0; e < n; ++e)
    for (int v : m.tets[e])
      anchored[e] |= fixedNode[v];
  // A loaded face's triangles, by the tet that owns each (the one that has
  // all three of its corners).
  std::vector<std::vector<int>> tetsOfNode(m.nodes.size());
  for (size_t e = 0; e < n; ++e)
    for (int i = 0; i < 4; ++i)
      tetsOfNode[m.tets[e][i]].push_back(static_cast<int>(e));
  std::map<int, std::vector<int>> loadedTets; // face index -> owner tets
  for (const FemMesh::BoundaryTri& t : m.boundary) {
    if (!loadedFaces.count(t.faceIndex))
      continue;
    for (int e : tetsOfNode[t.nodes[0]]) {
      int shared = 0;
      for (int i = 0; i < 4; ++i)
        shared += m.tets[e][i] == t.nodes[0] || m.tets[e][i] == t.nodes[1] ||
                  m.tets[e][i] == t.nodes[2];
      if (shared == 3)
        loadedTets[t.faceIndex].push_back(e);
    }
  }

  // The solid at `level`: what is at or above it and face-connected to a
  // support (a tet that meets the rest at an edge or a corner is a hinge,
  // and a solve hands back its free swing as the answer). Returns the first
  // loaded face it leaves out, or -1.
  std::vector<char> solid(n, 0);
  int dropped = 0;
  const auto cut = [&](double level) {
    for (size_t e = 0; e < n; ++e)
      solid[e] = src.whole || smooth[e] >= level;
    int count = 0;
    const std::vector<int> comp = TetFaceComponents(m, solid, &count);
    std::vector<char> reaches(count, 0);
    for (size_t e = 0; e < n; ++e)
      if (comp[e] >= 0 && anchored[e])
        reaches[comp[e]] = 1;
    dropped = 0;
    for (size_t e = 0; e < n; ++e)
      if (solid[e] && !reaches[comp[e]]) {
        solid[e] = 0;
        ++dropped;
      }
    for (const auto& [fi, tets] : loadedTets) {
      bool any = false;
      for (int e : tets)
        any = any || solid[e];
      if (!any)
        return fi;
    }
    return -1;
  };
  double level = thr;
  int lost = cut(level);
  if (lost >= 0 && !src.whole && src.threshold > 0.0)
    return fail("at threshold " + std::to_string(thr) + " the face of load " +
                std::to_string(loadedFaces[lost]) +
                " is not connected to a support through the shape (only "
                "through tets meeting at edges or corners); a lower "
                "threshold keeps more of it");
  if (lost >= 0 && !src.whole) {
    // The volume-matched shape does not carry every load to a support: the
    // highest level that does, found by bisection, and said
    // (StudyRuntime::shapeThresholdLowered).
    double lo = 0.0, hi = thr;
    if (cut(lo) >= 0)
      return fail("the face of load " + std::to_string(loadedFaces[lost]) +
                  " is not connected to a support through any of topology "
                  "study '" +
                  topo->name + "'s result");
    for (int it = 0; it < 24; ++it) {
      const double mid = 0.5 * (lo + hi);
      (cut(mid) < 0 ? lo : hi) = mid;
    }
    level = lo;
    cut(level);
    out.shapeThresholdLoweredFrom = thr;
  }
  auto mesh = std::make_shared<FemMesh>(ExtractSubMesh(m, solid));
  if (mesh->tets.empty())
    return fail("no tet of topology study '" + topo->name + "' at threshold " +
                std::to_string(thr) + " is connected to a support");
  out.sourceDigest = rt->densityDigest;
  out.shapeThreshold = level;
  out.shapeIslandTets = dropped;
  out.shapeVolume = TetSetVolume(m, solid);
  bp.premeshed = std::move(mesh);
  return true;
}

} // namespace

bool PlanStudy(const Document& doc, Study& study, StudyPlan& out,
               std::string* error) {
  out = StudyPlan{};
  study.error.clear();
  auto fail = [&](const std::string& m) {
    study.error = m;
    if (error)
      *error = m;
    return false;
  };

  std::string err;
  if (!EvaluateStudyExprs(study, doc.Symbols(), &err))
    return fail(err);
  if (!ValidateLoadCases(study, &err))
    return fail(err);
  if (const std::string why = ContactRefusal(study); !why.empty())
    return fail(why);
  if (const std::string why = ThermalRefusal(study); !why.empty())
    return fail(why);
  out.type = study.type;
  out.solver = EffectiveSolver(study);
  out.nonlinear = study.nonlinear;
  out.thermalLoad = study.thermalLoad;
  out.numModes = study.numModes;
  out.gravity = study.gravity;
  for (const LoadCase& c : study.loadCases)
    out.caseNames.push_back(c.name);

  const auto& bodies = doc.GetBodies();

  // Group every load by the body it resolves to, and collect per-face
  // refinement sizes. A refinement alone does not make a body "loaded".
  struct BodyPlan {
    std::vector<std::pair<int, size_t>> loads; // (faceIndex, load index)
    MeshSettings settings;
  };
  std::map<int, BodyPlan> perBody;

  // A heat source names its body, not a face.
  const auto bodyNamed = [&](const Load& l, int& index) {
    std::string names;
    index = -1;
    for (size_t b = 0; b < bodies.size(); ++b) {
      if (bodies[b].reference)
        continue;
      names += (names.empty() ? "" : ", ") + ("'" + bodies[b].name + "'");
      if (bodies[b].name == l.body) {
        if (index >= 0) {
          err = "heat_generation " + std::to_string(l.id) +
                ": two bodies are named '" + l.body +
                "'; rename one so the source names one body";
          return false;
        }
        index = static_cast<int>(b);
      }
    }
    if (index < 0)
      err = "heat_generation " + std::to_string(l.id) + ": no body is named '" +
            l.body + "' (the part has: " + names + ")";
    return index >= 0;
  };
  for (size_t i = 0; i < study.loads.size(); ++i) {
    ResolvedFace rf;
    if (study.loads[i].type == LoadType::HeatGeneration) {
      if (!bodyNamed(study.loads[i], rf.bodyIndex))
        return fail(err);
    } else if (!ResolveFace(doc, study.loads[i].face,
                            "load " + std::to_string(i + 1), rf, &err))
      return fail(err);
    BodyPlan& bp = perBody[rf.bodyIndex];
    bp.settings.maxSize = study.meshSize;
    bp.loads.push_back({rf.faceIndex, i});
  }
  if (perBody.empty())
    return fail("study has no loads to solve");

  // A static study under a from_study thermal load: the thermal study's
  // loads, on the same bodies, re-solved on this study's mesh.
  const bool thermalStudy = study.type == AnalysisType::Thermal;
  std::map<int, std::vector<std::pair<int, Load>>> heatOf;
  if (study.thermalLoad.kind == ThermalLoad::Kind::FromStudy) {
    const Study* src = doc.FindStudy(study.thermalLoad.fromStudy);
    if (!src || src->type != AnalysisType::Thermal) {
      std::string names;
      for (const Study& st : doc.GetStudies())
        if (st.type == AnalysisType::Thermal)
          names += (names.empty() ? "" : ", ") + std::to_string(st.id);
      return fail(
          "thermal_load from_study " +
          std::to_string(study.thermalLoad.fromStudy) +
          (src ? " is a " + std::string(AnalysisTypeToString(src->type)) +
                     " study, not a thermal one"
               : " names no study") +
          " (thermal studies: " + (names.empty() ? "none" : names) + ")");
    }
    Study heat = *src;
    if (!EvaluateStudyExprs(heat, doc.Symbols(), &err))
      return fail("thermal_load's study: " + err);
    if (const std::string why = ThermalRefusal(heat); !why.empty())
      return fail("thermal_load's study: " + why);
    if (!study.thermalLoad.hasStressFree) {
      const std::optional<double> ambient = DerivedStressFree(heat);
      if (!ambient)
        return fail("thermal_load needs stress_free_c: the thermal study's "
                    "convections do not share one ambient for the part to "
                    "have been at before it was heated");
      out.thermalLoad.stressFreeC = *ambient;
      out.thermalLoad.hasStressFree = true;
    }
    std::set<int> theirs, ours;
    for (const auto& [b, plan] : perBody)
      ours.insert(b);
    for (size_t i = 0; i < heat.loads.size(); ++i) {
      ResolvedFace rf;
      if (heat.loads[i].type == LoadType::HeatGeneration) {
        if (!bodyNamed(heat.loads[i], rf.bodyIndex))
          return fail("thermal_load's study: " + err);
      } else if (!ResolveFace(doc, heat.loads[i].face,
                              "the thermal study's load " +
                                  std::to_string(i + 1),
                              rf, &err))
        return fail(err);
      theirs.insert(rf.bodyIndex);
      heatOf[rf.bodyIndex].push_back({rf.faceIndex, heat.loads[i]});
    }
    if (theirs != ours) {
      const auto list = [&](const std::set<int>& set) {
        std::string s;
        for (int b : set)
          s += (s.empty() ? "'" : ", '") + bodies[b].name + "'";
        return s.empty() ? std::string("none") : s;
      };
      return fail("thermal_load from_study " +
                  std::to_string(study.thermalLoad.fromStudy) +
                  " heats the bodies " + list(theirs) +
                  ", and this study loads " + list(ours) +
                  ": a thermal load takes the temperature of the bodies it "
                  "solves, so both studies must load the same ones");
    }
  }

  for (size_t i = 0; i < study.refinements.size(); ++i) {
    ResolvedFace rf;
    if (!ResolveFace(doc, study.refinements[i].face,
                     "refinement " + std::to_string(i + 1), rf, &err))
      return fail(err);
    // Only refine bodies that are actually solved.
    auto it = perBody.find(rf.bodyIndex);
    if (it != perBody.end())
      it->second.settings.faceSizes.push_back(
          {rf.faceIndex, study.refinements[i].size});
  }

  for (auto& [bodyIndex, plan] : perBody) {
    const Body& body = bodies[bodyIndex];

    // Material must have stiffness data (0 = unknown, like density); a
    // thermal study's, a conductivity instead.
    const auto matType =
        CADMaterials::MaterialLibrary::MaterialTypeFromString(body.material);
    const auto& mp = CADMaterials::MaterialLibrary::GetMaterial(matType);
    if (!thermalStudy)
      if (const std::string why = CADMaterials::ElasticDataRefusal(mp);
          !why.empty())
        return fail("body '" + body.name + "': material '" +
                    (body.material.empty() ? "default" : body.material) + "' " +
                    why);

    // Each loaded body needs at least one support; a thermal body somewhere
    // for its heat to go (a held temperature or a convection), without
    // which its temperature is not determined.
    if (thermalStudy) {
      bool sink = false;
      for (const auto& [faceIndex, loadIdx] : plan.loads)
        sink = sink || study.loads[loadIdx].type == LoadType::Temperature ||
               study.loads[loadIdx].type == LoadType::Convection;
      if (!sink)
        return fail("body '" + body.name +
                    "': its heat has nowhere to go -- no face of it is held "
                    "at a temperature or cooled by convection, so its "
                    "temperature is not determined; add a temperature or a "
                    "convection load");
    }
    bool hasFixed = thermalStudy;
    for (const auto& [faceIndex, loadIdx] : plan.loads) {
      const Load& l = study.loads[loadIdx];
      if (IsSupport(l.type))
        hasFixed = true;
      // A pin holds only where it is pressed, and a mode has no load to
      // press it (docs/simulation/README.md#pin-supports).
      if (l.type == LoadType::Pin && study.type == AnalysisType::Modal)
        return fail("load " + std::to_string(l.id) +
                    ": a modal study's supports are fixed; a pin holds where "
                    "a load presses the part onto it, and a mode has none");
    }
    if (!hasFixed)
      return fail("body '" + body.name + "' has loads but no fixed support");

    // Density-dependent analyses must fail HERE (prepare has the material
    // library and study), not on the worker.
    if (study.type == AnalysisType::Modal && mp.density <= 0.0f)
      return fail("body '" + body.name +
                  "': material has no density (needed "
                  "for modal analysis)");
    if (study.type != AnalysisType::Modal &&
        (study.gravity.x != 0 || study.gravity.y != 0 ||
         study.gravity.z != 0) &&
        mp.density <= 0.0f)
      return fail("body '" + body.name +
                  "': material has no density (needed "
                  "for gravity)");

    // GPa -> MPa; density kg/m^3 -> tonne/mm^3 for the consistent mm-N-t-s
    // system (so gravity loads and modal frequencies come out right).
    StudyBodyPlan bp;
    bp.bodyIndex = bodyIndex;
    bp.bodyName = body.name;
    bp.mat = MaterialFromLibrary(mp);
    if (thermalStudy || study.thermalLoad.kind == ThermalLoad::Kind::FromStudy)
      if (const std::string why = ThermalMaterialRefusal(
              bp.mat, "body '" + body.name + "': material " + mp.name, true,
              false, CADMaterials::MaterialLibrary::ConductivitySlugList());
          !why.empty())
        return fail(why);
    if (study.thermalLoad.On())
      if (const std::string why = ThermalMaterialRefusal(
              bp.mat, "body '" + body.name + "': material " + mp.name, false,
              true, CADMaterials::MaterialLibrary::ExpansionSlugList());
          !why.empty())
        return fail(why);
    // A plastic study runs the material along its own hardening curve, or
    // not at all (docs/simulation/README.md#plasticity).
    if (study.nonlinear.plasticity)
      if (const std::string why = HardeningRefusal(
              bp.mat, "body '" + body.name + "': material " + mp.name,
              CADMaterials::MaterialLibrary::HardeningSlugList());
          !why.empty())
        return fail(why);
    bp.interlayerMPa = mp.interlayerMPa;
    // A PRINTED part weighs what its walls, shells and infill weigh
    // (docs/tolerances/README.md#printed-mass): its self-weight and modal
    // mass take the printed estimate's share of the solid, spread evenly.
    // The stiffness stays the solid material's.
    if (doc.PrintProcess()) {
      std::string why;
      if (const auto pm = printmass::EstimatePrinted(
              &*doc.PrintProcess(), doc.PrintSettings().For(body.createdBy),
              body, &why);
          pm && pm->solidMm3 > 0.0)
        bp.mat.densityTPerMM3 *= pm->effectiveMm3 / pm->solidMm3;
    }
    // A deep copy: the mesher writes triangulation into the faces it
    // meshes, and this shape may be meshed on a worker while the document's
    // own is being drawn.
    bp.settings = plan.settings;
    if (auto it = heatOf.find(bodyIndex); it != heatOf.end())
      bp.heatLoads = it->second;
    for (const auto& [faceIndex, loadIdx] : plan.loads) {
      Load l = study.loads[loadIdx];
      if (IsThermalLoad(l.type)) {
        bp.loads.push_back({faceIndex, std::move(l)});
        continue;
      }
      if (l.type == LoadType::Contact) {
        // A part rests on the ground: the plane its flat face lies on
        // (docs/simulation/README.md#contact).
        if (l.contactWith != 0)
          return fail("contact support " + std::to_string(l.id) +
                      ": contact_with names an occurrence, and a part study "
                      "has none -- on a part the contact is with the ground");
        StudyBodyPlan::ContactPartner ground;
        if (!GroundUnder(FaceByIndex(body.shape.GetShape(), faceIndex), ground,
                         &err))
          return fail("contact support " + std::to_string(l.id) + ": " + err);
        StudyBodyPlan::Contact c;
        c.id = l.id;
        c.faces = {faceIndex};
        c.partner = static_cast<int>(bp.partners.size());
        c.friction = l.friction;
        bp.partners.push_back(std::move(ground));
        bp.contacts.push_back(std::move(c));
        continue;
      }
      if (!CompleteLoadGeometry(FaceByIndex(body.shape.GetShape(), faceIndex),
                                l, &err))
        return fail(err);
      bp.loads.push_back({faceIndex, std::move(l)});
    }
    if (study.shapeFrom.study != 0) {
      // A static study of a topology result: its mesh is cut from the
      // result, nothing is meshed
      // (docs/topopt/README.md#a-static-study-of-the-optimised-shape).
      if (!PlanShapeMesh(doc, study, bodyIndex, bp, out, &err))
        return fail(err);
    } else {
      bp.shape = BRepBuilderAPI_Copy(body.shape.GetShape()).Shape();
    }
    out.bodies.push_back(std::move(bp));
  }
  return true;
}

bool MeshPlannedStudy(const StudyPlan& plan, StudyJobInput& out,
                      std::string* error) {
  out = StudyJobInput{};
  out.type = plan.type;
  out.solver = plan.solver;
  out.nonlinear = plan.nonlinear;
  out.thermalLoad = plan.thermalLoad;
  out.numModes = plan.numModes;
  out.gravity = plan.gravity;
  out.caseNames = plan.caseNames;
  out.sourceDigest = plan.sourceDigest;
  out.shapeThreshold = plan.shapeThreshold;
  out.shapeIslandTets = plan.shapeIslandTets;
  out.shapeVolume = plan.shapeVolume;
  out.shapeThresholdLoweredFrom = plan.shapeThresholdLoweredFrom;
  std::string err;
  auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  for (const StudyBodyPlan& bp : plan.bodies) {
    auto mesh = bp.premeshed ? std::optional<FemMesh>(*bp.premeshed)
                             : Mesh(bp.shape, bp.settings, &err);
    if (!mesh)
      return fail("body '" + bp.bodyName + "': " + err);

    // A bonded group (S2): the mesh spans several solids, each its own
    // domain, each its own material. Which domain is which member is
    // answered by geometry — a tet's centroid is inside exactly one — not
    // by trusting the mesher's numbering.
    std::vector<Material> materials;
    std::vector<int> memberOfDomain; // a bonded group's: domain -> member
    if (!bp.members.empty()) {
      int domains = 0;
      for (int d : mesh->tetDomain)
        domains = std::max(domains, d + 1);
      materials.assign(std::max(domains, 1), bp.mat);
      std::vector<bool> known(materials.size(), false);
      for (size_t t = 0; t < mesh->tets.size(); ++t) {
        const int d = mesh->tetDomain.empty() ? 0 : mesh->tetDomain[t];
        if (known[d])
          continue;
        const auto& tet = mesh->tets[t];
        const glm::dvec3 c = 0.25 * (mesh->nodes[tet[0]] + mesh->nodes[tet[1]] +
                                     mesh->nodes[tet[2]] + mesh->nodes[tet[3]]);
        bool found = false;
        memberOfDomain.resize(materials.size(), -1);
        for (size_t mi = 0; mi < bp.members.size(); ++mi) {
          const StudyBodyPlan::Member& m = bp.members[mi];
          BRepClass3d_SolidClassifier cls(m.solid, gp_Pnt(c.x, c.y, c.z), 1e-6);
          if (cls.State() == TopAbs_IN) {
            materials[d] = m.mat;
            memberOfDomain[d] = static_cast<int>(mi);
            found = true;
            break;
          }
        }
        if (!found)
          return fail("body '" + bp.bodyName +
                      "': a mesh domain lies in none of the bonded members");
        known[d] = true;
      }
    }

    // A contact participant (docs/simulation/README.md#contact) is meshed
    // on its own and joins this mesh as domains of its own, not sharing a
    // node: its faces numbered from its offset, its loads already under
    // those numbers.
    const size_t mainNodes = mesh->nodes.size();
    std::vector<std::pair<size_t, size_t>> memberNodes(bp.partners.size(),
                                                       {0, 0});
    for (size_t k = 0; k < bp.partners.size(); ++k) {
      const StudyBodyPlan::ContactPartner& pt = bp.partners[k];
      if (pt.rigid)
        continue;
      auto m = Mesh(pt.shape, pt.settings, &err);
      if (!m)
        return fail("contact participant '" + pt.name + "': " + err);
      if (materials.empty()) {
        int domains = 0;
        for (int d : mesh->tetDomain)
          domains = std::max(domains, d + 1);
        materials.assign(std::max(domains, 1), bp.mat);
      }
      if (mesh->tetDomain.empty())
        mesh->tetDomain.assign(mesh->tets.size(), 0);
      const int base = static_cast<int>(mesh->nodes.size());
      const int domainBase = static_cast<int>(materials.size());
      int domains = 1;
      for (int d : m->tetDomain)
        domains = std::max(domains, d + 1);
      for (int d = 0; d < domains; ++d)
        materials.push_back(pt.mat);
      mesh->nodes.insert(mesh->nodes.end(), m->nodes.begin(), m->nodes.end());
      for (size_t t = 0; t < m->tets.size(); ++t) {
        std::array<int, 10> q = m->tets[t];
        for (int& v : q)
          v += base;
        mesh->tets.push_back(q);
        mesh->tetDomain.push_back(domainBase +
                                  (m->tetDomain.empty() ? 0 : m->tetDomain[t]));
      }
      for (FemMesh::BoundaryTri tri : m->boundary) {
        for (int& v : tri.nodes)
          v += base;
        tri.faceIndex += pt.faceOffset;
        mesh->boundary.push_back(tri);
      }
      mesh->stats.nodeCount = mesh->nodes.size();
      mesh->stats.tetCount = mesh->tets.size();
      mesh->stats.worstQuality =
          std::min(mesh->stats.worstQuality, m->stats.worstQuality);
      memberNodes[k] = {static_cast<size_t>(base), mesh->nodes.size()};
    }

    // Build fixed-node set and face tractions from the resolved faces. Fixed
    // faces contribute all their boundary-tri nodes; force/pressure faces
    // contribute their tri indices.
    std::set<int> fixedSet;
    std::vector<SupportFacets> supportFacets;
    std::vector<StudyJobBody::PinSupport> pins;
    std::vector<FaceLoad> faceLoads;
    std::vector<std::vector<FaceLoad>> caseLoads(plan.caseNames.size());
    // A torque or remote load on a face the imprint split arrives once per
    // piece: its pieces are one FaceLoad, so its field is the whole face's.
    std::map<uint64_t, std::pair<size_t, size_t>> fieldLoad; // id -> (case, i)
    // A facet is in a load's region when its centroid is (LoadRegion).
    const auto inRegion = [&](const FemMesh::BoundaryTri& tri,
                              const LoadRegion& region) {
      if (region.IsWhole())
        return true;
      glm::dvec3 c(0.0);
      for (int k = 0; k < 3; ++k)
        c += mesh->nodes[tri.nodes[k]];
      return region.Contains(c / 3.0);
    };
    // Thermal loads (docs/simulation/README.md#thermal), into the solver's
    // units here, once: W -> mW, per m^2 -> per mm^2.
    HeatLoads heat;
    double baseC = std::numeric_limits<double>::infinity();
    const auto addHeat = [&](int faceIndex, const Load& load) -> bool {
      std::vector<int> tris;
      if (load.type != LoadType::HeatGeneration) {
        for (int t = 0; t < static_cast<int>(mesh->boundary.size()); ++t)
          if (mesh->boundary[t].faceIndex == faceIndex &&
              inRegion(mesh->boundary[t], load.region))
            tris.push_back(t);
        if (tris.empty()) {
          err = "body '" + bp.bodyName + "': " + LoadTypeToString(load.type) +
                " " + std::to_string(load.id) +
                (load.region.IsWhole()
                     ? "'s face meshed to no facets"
                     : "'s region holds none of its face's mesh facets");
          return false;
        }
      }
      // A face an imprint split arrives once per piece: one load.
      const auto merge = [&](auto& list) {
        for (auto& have : list)
          if (load.id != 0 && have.id == load.id) {
            have.tris.insert(have.tris.end(), tris.begin(), tris.end());
            return true;
          }
        return false;
      };
      switch (load.type) {
      case LoadType::Temperature:
        baseC = std::min(baseC, load.temperatureC);
        if (!merge(heat.held))
          heat.held.push_back({load.id, tris, load.temperatureC});
        break;
      case LoadType::HeatFlux: {
        if (merge(heat.fluxes))
          break;
        HeatLoads::Flux f;
        f.id = load.id;
        f.tris = tris;
        f.total = !std::isnan(load.powerW);
        f.totalMW = f.total ? load.powerW * kMilliwattsPerWatt : 0.0;
        f.fluxMWmm2 = f.total ? 0.0 : load.fluxWm2 * kWm2ToMWmm2;
        heat.fluxes.push_back(std::move(f));
        break;
      }
      case LoadType::Convection:
        baseC = std::min(baseC, load.ambientC);
        if (!merge(heat.films))
          heat.films.push_back(
              {load.id, tris, load.filmWm2K * kWm2ToMWmm2, load.ambientC});
        break;
      case LoadType::HeatGeneration: {
        HeatLoads::Source src;
        src.id = load.id;
        src.powerMW = load.powerW * kMilliwattsPerWatt;
        if (!memberOfDomain.empty()) {
          // A bonded group: the member the source names.
          for (size_t t = 0; t < mesh->tets.size(); ++t) {
            const int d = mesh->tetDomain.empty() ? 0 : mesh->tetDomain[t];
            const int mi = d < static_cast<int>(memberOfDomain.size())
                               ? memberOfDomain[d]
                               : -1;
            if (mi >= 0 && bp.members[mi].name == load.body)
              src.tets.push_back(static_cast<int>(t));
          }
          if (src.tets.empty()) {
            err = "body '" + bp.bodyName + "': heat_generation " +
                  std::to_string(load.id) + " names '" + load.body +
                  "', which is none of its bonded members";
            return false;
          }
        }
        heat.sources.push_back(std::move(src));
        break;
      }
      default:
        break;
      }
      return true;
    };
    for (const auto& [faceIndex, load] : bp.heatLoads)
      if (!addHeat(faceIndex, load))
        return fail(err);
    for (const auto& [faceIndex, load] : bp.loads) {
      if (IsThermalLoad(load.type)) {
        if (!addHeat(faceIndex, load))
          return fail(err);
        continue;
      }
      if (load.type == LoadType::Fixed) {
        size_t before = fixedSet.size();
        SupportFacets sf{load.id, load.type, {}};
        for (int t = 0; t < static_cast<int>(mesh->boundary.size()); ++t) {
          const auto& tri = mesh->boundary[t];
          if (tri.faceIndex == faceIndex && inRegion(tri, load.region)) {
            sf.tris.push_back(t);
            for (int n : tri.nodes)
              fixedSet.insert(n);
          }
        }
        if (!load.region.IsWhole() && fixedSet.size() == before)
          return fail("body '" + bp.bodyName + "': load " +
                      std::to_string(load.id) +
                      "'s region holds none of its face's mesh facets");
        // A fixed support split by an imprint is one support.
        bool merged = false;
        for (SupportFacets& have : supportFacets)
          if (load.id != 0 && have.id == load.id) {
            have.tris.insert(have.tris.end(), sf.tris.begin(), sf.tris.end());
            merged = true;
          }
        if (!merged)
          supportFacets.push_back(std::move(sf));
        for (SupportFacets& have : supportFacets)
          if (have.id == load.id)
            have.awayMm = SupportAwayMm(*mesh, have.tris, LoadType::Fixed,
                                        glm::dvec3(0.0), glm::dvec3(0.0));
      } else if (load.type == LoadType::Pin) {
        StudyJobBody::PinSupport pin;
        pin.id = load.id;
        pin.axisOrigin = load.axisOrigin;
        pin.axisDirection = load.axisDirection;
        pin.fixRotation = load.fixRotation;
        for (int t = 0; t < static_cast<int>(mesh->boundary.size()); ++t)
          if (mesh->boundary[t].faceIndex == faceIndex &&
              inRegion(mesh->boundary[t], load.region))
            pin.tris.push_back(t);
        if (pin.tris.empty())
          return fail("body '" + bp.bodyName + "': pin " +
                      std::to_string(load.id) +
                      (load.region.IsWhole()
                           ? "'s face meshed to no facets"
                           : "'s region holds none of its face's mesh facets"));
        supportFacets.push_back(
            {load.id, load.type, pin.tris,
             SupportAwayMm(*mesh, pin.tris, LoadType::Pin, pin.axisOrigin,
                           pin.axisDirection)});
        pins.push_back(std::move(pin));
      } else {
        std::vector<int> tris;
        for (int t = 0; t < static_cast<int>(mesh->boundary.size()); ++t)
          if (mesh->boundary[t].faceIndex == faceIndex &&
              inRegion(mesh->boundary[t], load.region))
            tris.push_back(t);
        if (tris.empty() && !load.region.IsWhole())
          return fail("body '" + bp.bodyName + "': load " +
                      std::to_string(load.id) +
                      "'s region holds none of its face's mesh facets");
        if (tris.empty() && bp.premeshed)
          return fail("body '" + bp.bodyName + "': load " +
                      std::to_string(load.id) +
                      "'s face has no material left in the optimised shape");
        size_t k = 0;
        if (!plan.caseNames.empty()) {
          while (k < plan.caseNames.size() &&
                 plan.caseNames[k] != load.loadCase)
            ++k;
          if (k == plan.caseNames.size())
            return fail("load " + std::to_string(load.id) +
                        " names load case '" + load.loadCase +
                        "', which the study does not have");
        }
        std::vector<FaceLoad>& into =
            plan.caseNames.empty() ? faceLoads : caseLoads[k];
        const bool field =
            load.type == LoadType::Torque || load.type == LoadType::Remote;
        if (field && load.id != 0)
          if (auto it = fieldLoad.find(load.id); it != fieldLoad.end()) {
            std::vector<int>& more = into[it->second.second].tris;
            more.insert(more.end(), tris.begin(), tris.end());
            continue;
          }
        FaceLoad fl = MakeFaceLoad(load);
        fl.tris = std::move(tris);
        if (field && load.id != 0)
          fieldLoad[load.id] = {k, into.size()};
        into.push_back(std::move(fl));
      }
    }
    if (fixedSet.empty() && pins.empty() && bp.contacts.empty() &&
        plan.type != AnalysisType::Thermal)
      return fail("body '" + bp.bodyName + "': fixed face meshed to no nodes");

    // Each contact: the body's facets against its partner's, a rigid
    // partner meshed coarse here with every node fixed.
    std::vector<StudyJobBody::ContactPair> contacts;
    std::vector<FemMesh> rigidPartners;
    std::vector<int> rigidIndex(bp.partners.size(), -1);
    for (const StudyBodyPlan::Contact& c : bp.contacts) {
      if (c.partner < 0 || c.partner >= static_cast<int>(bp.partners.size()))
        return fail("body '" + bp.bodyName + "': a contact has no partner");
      const StudyBodyPlan::ContactPartner& pt = bp.partners[c.partner];
      StudyJobBody::ContactPair pair;
      pair.id = c.id;
      pair.participant = c.participant;
      pair.name = pt.name;
      pair.friction = c.friction;
      pair.partner = -1;
      const std::set<int> mine(c.faces.begin(), c.faces.end());
      for (int t = 0; t < static_cast<int>(mesh->boundary.size()); ++t)
        if (mine.count(mesh->boundary[t].faceIndex) &&
            static_cast<size_t>(mesh->boundary[t].nodes[0]) < mainNodes)
          pair.slaveTris.push_back(t);
      if (pt.rigid) {
        if (rigidIndex[c.partner] < 0) {
          auto m = Mesh(pt.shape, pt.settings, &err);
          if (!m)
            return fail("body '" + bp.bodyName + "': the rigid partner '" +
                        pt.name + "' of its contact: " + err);
          rigidIndex[c.partner] = static_cast<int>(rigidPartners.size());
          rigidPartners.push_back(std::move(*m));
        }
        pair.partner = rigidIndex[c.partner];
        const FemMesh& pm = rigidPartners[pair.partner];
        const std::set<int> theirs(pt.faces.begin(), pt.faces.end());
        for (int t = 0; t < static_cast<int>(pm.boundary.size()); ++t)
          if (theirs.count(pm.boundary[t].faceIndex))
            pair.masterTris.push_back(t);
      } else {
        std::set<int> theirs;
        for (int f : pt.faces)
          theirs.insert(f + pt.faceOffset);
        for (int t = 0; t < static_cast<int>(mesh->boundary.size()); ++t)
          if (theirs.count(mesh->boundary[t].faceIndex))
            pair.masterTris.push_back(t);
      }
      const std::string what =
          (c.participant
               ? "contact with '"
               : "contact support " + std::to_string(c.id) + " against '") +
          pt.name + "'";
      if (pair.slaveTris.empty())
        return fail("body '" + bp.bodyName + "': " + what +
                    ": its faces on the body meshed to no facets");
      if (pair.masterTris.empty())
        return fail("body '" + bp.bodyName + "': " + what +
                    ": its partner's faces meshed to no facets");
      // The penalty from the softer side and the body's facets there.
      double edge = 0.0;
      for (int t : pair.slaveTris) {
        const auto& tri = mesh->boundary[t];
        for (int a = 0; a < 3; ++a)
          edge += glm::length(mesh->nodes[tri.nodes[a]] -
                              mesh->nodes[tri.nodes[(a + 1) % 3]]);
      }
      edge /= 3.0 * pair.slaveTris.size();
      const double soft = pt.rigid ? bp.mat.youngMPa
                                   : std::min(bp.mat.youngMPa, pt.mat.youngMPa);
      pair.penalty = ContactPenalty(soft, edge);
      if (!c.participant)
        supportFacets.push_back(
            {c.id, LoadType::Contact, pair.slaveTris,
             SupportAwayMm(*mesh, pair.slaveTris, LoadType::Fixed,
                           glm::dvec3(0.0), glm::dvec3(0.0))});
      contacts.push_back(std::move(pair));
    }
    // A part no fixed support or pin holds -- held by a contact alone --
    // takes weak springs while its contact closes.
    std::vector<StudyJobBody::Stabilized> stabilized;
    if (!contacts.empty()) {
      const auto spring = [&](size_t from, size_t to, double youngMPa) {
        if (to <= from)
          return;
        glm::dvec3 lo(1e300), hi(-1e300);
        for (size_t n = from; n < to; ++n) {
          if (fixedSet.count(static_cast<int>(n)))
            return;
          lo = glm::min(lo, mesh->nodes[n]);
          hi = glm::max(hi, mesh->nodes[n]);
        }
        StudyJobBody::Stabilized st;
        for (size_t n = from; n < to; ++n)
          st.nodes.push_back(static_cast<int>(n));
        st.kPerNode = kStabilization * youngMPa * glm::length(hi - lo) /
                      static_cast<double>(to - from);
        stabilized.push_back(std::move(st));
      };
      if (pins.empty())
        spring(0, mainNodes, bp.mat.youngMPa);
      for (size_t k = 0; k < bp.partners.size(); ++k)
        if (!bp.partners[k].rigid)
          spring(memberNodes[k].first, memberNodes[k].second,
                 bp.partners[k].mat.youngMPa);
    }

    StudyJobBody jb;
    jb.contacts = std::move(contacts);
    jb.rigidPartners = std::move(rigidPartners);
    jb.stabilized = std::move(stabilized);
    jb.supports = std::move(supportFacets);
    jb.pins = std::move(pins);
    jb.bodyIndex = bp.bodyIndex;
    jb.bodyName = bp.bodyName;
    jb.mat = bp.mat;
    jb.materials = std::move(materials);
    jb.fixedNodes.assign(fixedSet.begin(), fixedSet.end());
    jb.faceLoads = std::move(faceLoads);
    jb.caseLoads = std::move(caseLoads);
    jb.heat = std::move(heat);
    jb.baseC = std::isfinite(baseC) ? baseC : 0.0;
    jb.thermalLoad = plan.thermalLoad;
    if (plan.type == AnalysisType::Buckling)
      jb.curvedFaces = CurvedFacesOf(bp.shape);
    jb.mesh = std::move(*mesh);
    out.bodies.push_back(std::move(jb));
  }
  return true;
}

bool PrepareStudy(const Document& doc, Study& study, StudyJobInput& out,
                  std::string* error) {
  out = StudyJobInput{};
  StudyPlan plan;
  if (!PlanStudy(doc, study, plan, error))
    return false;
  std::string err;
  if (!MeshPlannedStudy(plan, out, &err)) {
    study.error = err;
    if (error)
      *error = err;
    return false;
  }
  return true;
}

namespace {

// The facet's corner normal, outward as Netgen orients a boundary triangle.
glm::dvec3 FacetCross(const FemMesh& mesh, const FemMesh::BoundaryTri& t) {
  const glm::dvec3& a = mesh.nodes[t.nodes[0]];
  return glm::cross(mesh.nodes[t.nodes[1]] - a, mesh.nodes[t.nodes[2]] - a);
}

// One pin as the contact iteration sees it: its facets, its nodes and each
// node's frame (into the material, round the axis, along it).
struct PinState {
  const StudyJobBody::PinSupport* pin = nullptr;
  std::vector<int> nodes;                       // unique, ascending
  std::map<int, int> slot;                      // node -> index into nodes
  std::vector<std::array<glm::dvec3, 3>> frame; // per node
  std::vector<char> active;                     // per facet (pin->tris)
  std::vector<double> nodeWeight;               // area share x radius, per node
  double faceArea = 0.0;
};

// Solves one body with pins (docs/simulation/README.md#pin-supports): per
// case, the facets each pin presses on are found by iterating linear solves
// -- a facet whose nodes the pin pulls leaves the contact, one whose nodes
// move into the pin returns -- and each iteration's constraints are tested
// for a rigid motion they leave free before anything is factored.
std::optional<std::vector<FemResults>>
SolveWithPins(const StudyJobBody& jb, const std::vector<Material>& mats,
              const std::vector<std::vector<FaceLoad>>& rhs,
              const glm::dvec3& g, const SolveOptions& options,
              SolveReport& report, std::string* error,
              std::vector<Supports>* settledSupports = nullptr) {
  const FemMesh& mesh = jb.mesh;
  auto fail =
      [&](const std::string& m) -> std::optional<std::vector<FemResults>> {
    if (error)
      *error = m;
    return std::nullopt;
  };
  std::vector<PinState> pins(jb.pins.size());
  for (size_t p = 0; p < jb.pins.size(); ++p) {
    PinState& ps = pins[p];
    ps.pin = &jb.pins[p];
    const glm::dvec3 a = glm::normalize(ps.pin->axisDirection);
    const glm::dvec3 o = ps.pin->axisOrigin;
    std::set<int> nodes;
    double side = 0.0; // > 0: the face looks away from the axis (a shaft)
    for (int t : ps.pin->tris) {
      const auto& tri = mesh.boundary[t];
      for (int v : tri.nodes)
        nodes.insert(v);
      const glm::dvec3 cr = FacetCross(mesh, tri);
      ps.faceArea += 0.5 * glm::length(cr);
      glm::dvec3 c = (mesh.nodes[tri.nodes[0]] + mesh.nodes[tri.nodes[1]] +
                      mesh.nodes[tri.nodes[2]]) /
                     3.0;
      glm::dvec3 r = (c - o) - glm::dot(c - o, a) * a;
      side += glm::dot(cr, r);
    }
    ps.nodes.assign(nodes.begin(), nodes.end());
    for (size_t i = 0; i < ps.nodes.size(); ++i) {
      ps.slot[ps.nodes[i]] = static_cast<int>(i);
      const glm::dvec3 x = mesh.nodes[ps.nodes[i]];
      glm::dvec3 r = (x - o) - glm::dot(x - o, a) * a;
      if (glm::length(r) <= 0.0)
        return fail("pin " + std::to_string(ps.pin->id) +
                    ": a node of its face lies on its axis");
      r = glm::normalize(r);
      // Into the material: away from the axis in a bore, towards it on a
      // shaft.
      const glm::dvec3 m = side > 0.0 ? -r : r;
      ps.frame.push_back({m, glm::cross(a, m), a});
    }
    ps.active.assign(ps.pin->tris.size(), 1);
    // Each node's share of its facets' area (a third of each corner's, a
    // sixth of each node's; any positive share ties the turn), times its
    // distance from the axis: the tie is then the face's moment of slide.
    ps.nodeWeight.assign(ps.nodes.size(), 0.0);
    for (int t : ps.pin->tris) {
      const auto& tri = mesh.boundary[t];
      const double area = 0.5 * glm::length(FacetCross(mesh, tri));
      for (int v : tri.nodes)
        ps.nodeWeight[ps.slot.at(v)] += area / 6.0;
    }
    for (size_t i = 0; i < ps.nodes.size(); ++i) {
      const glm::dvec3 x = mesh.nodes[ps.nodes[i]];
      ps.nodeWeight[i] *= glm::length((x - o) - glm::dot(x - o, a) * a);
    }
  }
  const auto pinFreeToTurn = [&](const glm::dvec3& dir, const glm::dvec3& pt,
                                 double scale) -> const PinState* {
    for (const PinState& ps : pins) {
      if (ps.pin->fixRotation)
        continue;
      const glm::dvec3 a = glm::normalize(ps.pin->axisDirection);
      const glm::dvec3 d = pt - ps.pin->axisOrigin;
      if (std::abs(glm::dot(a, dir)) > 1.0 - 1e-6 &&
          glm::length(d - glm::dot(d, a) * a) < 1e-3 * scale)
        return &ps;
    }
    return nullptr;
  };
  glm::dvec3 lo(1e300), hi(-1e300);
  for (const glm::dvec3& p : mesh.nodes) {
    lo = glm::min(lo, p);
    hi = glm::max(hi, p);
  }
  const double scale = glm::length(hi - lo);

  // Each iteration changes the set, and a repeat ends it (below): the cap
  // is a guard, the undamped iterations and then one per pin facet.
  size_t pinFacets = 0;
  for (const PinState& ps : pins)
    pinFacets += ps.pin->tris.size();
  constexpr int kUndampedIterations = 30;
  const int kMaxContactIterations =
      kUndampedIterations + static_cast<int>(pinFacets);
  std::vector<FemResults> out;
  for (size_t k = 0; k < rhs.size(); ++k) {
    for (PinState& ps : pins)
      ps.active.assign(ps.pin->tris.size(), 1);
    const std::string inCase =
        rhs.size() > 1 ? " in load case " + std::to_string(k + 1) : "";
    bool settled = false, finalPass = false;
    // Whether the set moves one side at a time (below); from the first
    // repeat, or after kUndampedIterations.
    bool damped = false;
    std::vector<std::vector<char>> seen;
    {
      std::vector<char> state;
      for (const PinState& ps : pins)
        state.insert(state.end(), ps.active.begin(), ps.active.end());
      seen.push_back(std::move(state));
    }
    for (int it = 0; it < kMaxContactIterations && !settled; ++it) {
      Supports sup;
      sup.fixedNodes = jb.fixedNodes;
      sup.reactions = true;
      for (const PinState& ps : pins) {
        std::vector<char> held(ps.nodes.size(), 0);
        for (size_t f = 0; f < ps.pin->tris.size(); ++f)
          if (ps.active[f])
            for (int v : mesh.boundary[ps.pin->tris[f]].nodes)
              held[ps.slot.at(v)] = 1;
        // Radially and axially where the facets press on the pin. A pin
        // held from turning ties its face's slides round the axis, weighted
        // by the area each node stands for and its radius: the face as a
        // whole does not turn, while each node may still slide.
        Tie tie;
        for (size_t i = 0; i < ps.nodes.size(); ++i)
          if (held[i] || ps.pin->fixRotation) {
            HeldNode h;
            h.node = ps.nodes[i];
            h.frame = ps.frame[i];
            h.held = {bool(held[i]), false, bool(held[i])};
            sup.held.push_back(h);
            if (ps.pin->fixRotation)
              tie.entries.push_back({ps.nodes[i], 1, ps.nodeWeight[i]});
          }
        if (!tie.entries.empty())
          sup.ties.push_back(std::move(tie));
      }
      glm::dvec3 dir(0.0), pt(0.0);
      bool turns = false;
      const std::string free = FreeRigidMotion(mesh, sup, &dir, &pt, &turns);
      if (!free.empty()) {
        const PinState* about = turns ? pinFreeToTurn(dir, pt, scale) : nullptr;
        if (it == 0)
          return fail("its supports leave it free to " + free +
                      (about ? ": pin " + std::to_string(about->pin->id) +
                                   " turns freely, so the part needs a second "
                                   "support, or fix_rotation on the pin"
                             : ""));
        return fail("the loads" + inCase +
                    " pull it off its pins: held only where it presses on "
                    "them, it is free to " +
                    free);
      }
      SolveReport r;
      auto one = SolveCases(mesh, mats, sup, {rhs[k]}, error, g, options, &r);
      if (!one)
        return std::nullopt;
      report.solver = r.solver;
      report.iterations += r.iterations;
      report.worstResidual = std::max(report.worstResidual, r.worstResidual);
      if (!r.externalDeclined.empty())
        report.externalDeclined = r.externalDeclined;
      FemResults& res = one->front();
      // Thresholds: a pull smaller than a millionth of the largest reaction
      // on the pins, a move smaller than a billionth of the largest one.
      double maxR = 0.0;
      for (const PinState& ps : pins)
        for (int v : ps.nodes)
          maxR = std::max(maxR, glm::length(res.nodeReaction[v]));
      const double tolR = 1e-6 * maxR;
      const double tolU = 1e-9 * std::max(res.maxDisplacement, 1e-300);
      bool changed = false;
      double pushed = 0.0, pulled = 0.0;
      // Every facet that pulls lets go and every one the part moves into is
      // taken back, in one step: fast where the contact is one patch (a
      // lug's bore). Where that cycles, or has not settled in
      // kUndampedIterations, the set moves one side at a time
      // (docs/simulation/README.md#pin-supports): while any facet pulls,
      // those pulling at least half as hard as the worst let go and none is
      // taken back; once none pulls, every facet the part moves into is
      // taken back. A short bushing leaned on 22 mm off its middle, pressed
      // at its two ends on opposite sides, cycled undamped and never
      // settled.
      double maxPull = 0.0;
      for (const PinState& ps : pins)
        for (size_t f = 0; f < ps.pin->tris.size(); ++f) {
          if (!ps.active[f])
            continue;
          double push = 0.0;
          for (int v : mesh.boundary[ps.pin->tris[f]].nodes)
            push += glm::dot(res.nodeReaction[v], ps.frame[ps.slot.at(v)][0]);
          maxPull = std::max(maxPull, -push);
        }
      const bool anyPull = maxPull > tolR;
      const double letGo = damped ? std::max(tolR, 0.5 * maxPull) : tolR;
      for (PinState& ps : pins)
        for (size_t f = 0; f < ps.pin->tris.size(); ++f) {
          const auto& tri = mesh.boundary[ps.pin->tris[f]];
          double push = 0.0, into = 0.0;
          for (int v : tri.nodes) {
            const glm::dvec3& m = ps.frame[ps.slot.at(v)][0];
            push += glm::dot(res.nodeReaction[v], m);
            into += glm::dot(res.displacement[v], m);
          }
          if (ps.active[f]) {
            pushed += std::max(push, 0.0);
            pulled += std::max(-push, 0.0);
          }
          if (finalPass)
            continue;
          if (ps.active[f] && push < -letGo) {
            ps.active[f] = 0;
            changed = true;
          } else if ((!damped || !anyPull) && !ps.active[f] &&
                     into / 6.0 < -tolU) {
            ps.active[f] = 1;
            changed = true;
          }
        }
      // A facet straddling the contact's edge can flip each time: once the
      // set repeats, only the facets every state of the cycle had in
      // contact stay in it, and that is solved once more as the answer --
      // the edge facets let go rather than kept pulling.
      if (changed && !finalPass) {
        std::vector<char> state;
        for (const PinState& ps : pins)
          state.insert(state.end(), ps.active.begin(), ps.active.end());
        bool repeat = false;
        for (const auto& old : seen)
          repeat = repeat || old == state;
        if (!damped && (repeat || it + 1 >= kUndampedIterations)) {
          damped = true;
          seen.clear();
        }
        for (size_t h = 0; h < seen.size(); ++h)
          if (seen[h] == state) {
            for (size_t q = h; q < seen.size(); ++q)
              for (size_t i = 0; i < state.size(); ++i)
                state[i] &= seen[q][i];
            size_t at = 0;
            for (PinState& ps : pins)
              for (char& c : ps.active)
                c = state[at++];
            finalPass = true;
            break;
          }
        seen.push_back(std::move(state));
      } else if (finalPass) {
        changed = false;
      }
      // The answer holds no pull worth the name: a pin that pulls is not one.
      if (!changed && pulled > 1e-3 * pushed)
        return fail("the pins' contact" + inCase +
                    " did not settle: it still pulls on the part");
      if (!changed) {
        settled = true;
        if (settledSupports)
          settledSupports->push_back(sup);
        // What each pin pressed on.
        for (const PinState& ps : pins) {
          FemResults::SupportReaction sr;
          sr.support = ps.pin->id;
          sr.type = LoadType::Pin;
          sr.faceAreaMm2 = ps.faceArea;
          for (size_t f = 0; f < ps.pin->tris.size(); ++f)
            if (ps.active[f])
              sr.contactAreaMm2 +=
                  0.5 *
                  glm::length(FacetCross(mesh, mesh.boundary[ps.pin->tris[f]]));
          res.reactions.push_back(sr);
        }
        out.push_back(std::move(res));
      }
    }
    if (!settled)
      return fail("the pins' contact" + inCase + " did not settle in " +
                  std::to_string(kMaxContactIterations) + " iterations");
  }
  return out;
}

// Each support's total reaction, from the nodes' (every node counted once,
// for the first support that holds it), into the result's reaction list;
// then the support-edge flag against `zones` (SupportEdgeZones). Clears the
// per-node reactions.
void FinishSupports(const StudyJobBody& jb, const std::vector<int>& zones,
                    const std::vector<FemResults::SupportReaction>& owners,
                    FemResults& res) {
  std::vector<FemResults::SupportReaction> pinRows = std::move(res.reactions);
  res.reactions.clear();
  std::vector<char> counted(jb.mesh.nodes.size(), 0);
  for (const SupportFacets& sf : jb.supports) {
    FemResults::SupportReaction sr;
    for (const auto& pr : pinRows)
      if (pr.support == sf.id && sf.type == LoadType::Pin)
        sr = pr;
    sr.support = sf.id;
    sr.type = sf.type;
    if (res.nodeReaction.size() == jb.mesh.nodes.size()) {
      sr.hasMoment = true;
      for (int t : sf.tris)
        for (int v : jb.mesh.boundary[t].nodes)
          if (!counted[v]) {
            counted[v] = 1;
            sr.forceN += res.nodeReaction[v];
            sr.momentNmm += glm::cross(jb.mesh.nodes[v], res.nodeReaction[v]);
          }
    }
    res.reactions.push_back(sr);
  }
  res.nodeReaction.clear();
  res.nodeReaction.shrink_to_fit();
  res.supportEdgePeak =
      PeakAwayFromEdges(jb.mesh, res.vonMises, res.maxVmNode, zones, owners);
}

// ---- Buckling: what is read off the modes ----------------------------------
// (docs/simulation/README.md#buckling)

// The wall at boundary node `node`: the distance along its inward normal
// (its facets' area-weighted, outward as Netgen orients them, reversed) to
// the nearest other boundary facet. 0 when the ray meets none.
double WallAt(const FemMesh& mesh, int node,
              const std::vector<std::vector<int>>& facetsOf) {
  glm::dvec3 n(0.0);
  for (int t : facetsOf[node])
    n += FacetCross(mesh, mesh.boundary[t]);
  if (!(glm::length(n) > 0.0))
    return 0.0;
  const glm::dvec3 dir = -glm::normalize(n);
  const glm::dvec3 o = mesh.nodes[node];
  double best = 0.0;
  for (size_t t = 0; t < mesh.boundary.size(); ++t) {
    const auto& tri = mesh.boundary[t];
    if (std::find(tri.nodes.begin(), tri.nodes.end(), node) != tri.nodes.end())
      continue;
    // Moller-Trumbore on the facet's corners.
    const glm::dvec3 a = mesh.nodes[tri.nodes[0]];
    const glm::dvec3 e1 = mesh.nodes[tri.nodes[1]] - a;
    const glm::dvec3 e2 = mesh.nodes[tri.nodes[2]] - a;
    const glm::dvec3 pv = glm::cross(dir, e2);
    const double det = glm::dot(e1, pv);
    if (std::abs(det) < 1e-18)
      continue;
    const glm::dvec3 tv = o - a;
    const double u = glm::dot(tv, pv) / det;
    if (u < -1e-9 || u > 1 + 1e-9)
      continue;
    const glm::dvec3 qv = glm::cross(tv, e1);
    const double w = glm::dot(dir, qv) / det;
    if (w < -1e-9 || u + w > 1 + 1e-9)
      continue;
    const double dist = glm::dot(e2, qv) / det;
    if (dist > 1e-9 && (best == 0.0 || dist < best))
      best = dist;
  }
  return best;
}

// The mean corner-to-corner edge of the tets that use `node`.
double ElementSizeAt(const FemMesh& mesh, int node) {
  double sum = 0.0;
  int n = 0;
  for (const auto& tet : mesh.tets) {
    if (std::find(tet.begin(), tet.end(), node) == tet.end())
      continue;
    for (int a = 0; a < 4; ++a)
      for (int b = a + 1; b < 4; ++b) {
        sum += glm::length(mesh.nodes[tet[a]] - mesh.nodes[tet[b]]);
        ++n;
      }
  }
  return n > 0 ? sum / n : 0.0;
}

constexpr double kThinShellRatio = 10.0; // R/t from which a wall is a shell

// NASA SP-8007's knockdown for a curved wall of radius R and thickness t.
double Sp8007Knockdown(double radius, double wall) {
  const double phi = std::sqrt(radius / wall) / 16.0;
  return 1.0 - 0.901 * (1.0 - std::exp(-phi));
}

// Everything the result says of a buckling body beyond its factors: the
// knockdown, the yield and interlayer factors, the governing factor and
// the flags. `modes` ascend by |factor|.
StudyBodyResult::Buckling ReadBuckling(const StudyJobBody& jb,
                                       const std::vector<Material>& mats,
                                       const FemResults& base) {
  StudyBodyResult::Buckling out;
  out.solved = true;
  const FemMesh& mesh = jb.mesh;
  const FemResults::ModeShape* first = nullptr;
  for (const FemResults::ModeShape& m : base.modes)
    if (m.factor > 0.0 && (!first || m.factor < first->factor))
      first = &m;
  if (first) {
    out.buckles = true;
    out.factor = first->factor;
  } else {
    out.none = base.modes.empty()
                   ? "nothing in this load buckles: no part of it is in "
                     "compression"
                   : "nothing in this load buckles: it buckles only under "
                     "the reversed load";
  }

  // Yield and interlayer factors from the base state; a node's strength is
  // the weakest of the materials of the tets that use it.
  const size_t nNodes = mesh.nodes.size();
  std::vector<double> yieldAt(nNodes, 0.0), interAt(nNodes, 0.0);
  for (size_t t = 0; t < mesh.tets.size(); ++t) {
    const size_t d = mesh.tetDomain.empty() ? 0 : (size_t)mesh.tetDomain[t];
    const Material& m = mats[std::min(d, mats.size() - 1)];
    for (int v : mesh.tets[t]) {
      if (m.yieldMPa > 0 && (yieldAt[v] == 0.0 || m.yieldMPa < yieldAt[v]))
        yieldAt[v] = m.yieldMPa;
      if (m.interlayerMPa > 0 &&
          (interAt[v] == 0.0 || m.interlayerMPa < interAt[v]))
        interAt[v] = m.interlayerMPa;
    }
  }
  double worstYield = 0.0, worstInter = 0.0; // stress / strength
  const std::vector<double> tension = MaxPrincipalField(base);
  for (size_t n = 0; n < nNodes; ++n) {
    if (yieldAt[n] > 0 && n < base.vonMises.size())
      worstYield = std::max(worstYield, base.vonMises[n] / yieldAt[n]);
    if (interAt[n] > 0 && n < tension.size())
      worstInter = std::max(worstInter, tension[n] / interAt[n]);
  }
  if (worstYield > 0)
    out.yieldFactor = 1.0 / worstYield;
  if (worstInter > 0)
    out.interlayerFactor = 1.0 / worstInter;

  if (first) {
    // Where the first mode's motion peaks, on a node inside one face (an
    // edge node's normal is no wall's): its face carries the knockdown and
    // the wall the through-thickness check reads.
    std::vector<std::vector<int>> facetsOf(nNodes);
    for (size_t t = 0; t < mesh.boundary.size(); ++t)
      for (int v : mesh.boundary[t].nodes)
        facetsOf[v].push_back(static_cast<int>(t));
    int peak = -1;
    double peakU = -1.0;
    std::vector<std::pair<double, int>> inside; // (motion, node)
    for (size_t n = 0; n < nNodes; ++n) {
      const double u = glm::length(first->displacement[n]);
      if (u > peakU) {
        peakU = u;
        peak = static_cast<int>(n);
      }
      if (facetsOf[n].empty())
        continue;
      bool oneFace = true;
      for (int t : facetsOf[n])
        oneFace = oneFace && mesh.boundary[t].faceIndex ==
                                 mesh.boundary[facetsOf[n][0]].faceIndex;
      if (oneFace)
        inside.push_back({u, static_cast<int>(n)});
    }
    // The wall the mode bends is the thinnest one where it moves most: of
    // the nodes inside one face moving at least nine tenths of the peak,
    // the one with the thinnest wall (an even sample of them, when there
    // are many: each is a ray through every boundary facet).
    std::sort(inside.begin(), inside.end(), std::greater<>());
    size_t moving = 0;
    while (moving < inside.size() &&
           inside[moving].first >= 0.9 * inside.front().first)
      ++moving;
    constexpr size_t kWallSamples = 512;
    const size_t stride = std::max<size_t>(1, moving / kWallSamples);
    int at = peak;
    double wall = 0.0;
    for (size_t i = 0; i < moving; i += stride) {
      const double w = WallAt(mesh, inside[i].second, facetsOf);
      if (w > 0 && (wall == 0.0 || w < wall)) {
        wall = w;
        at = inside[i].second;
      }
    }
    if (at >= 0 && !facetsOf[at].empty()) {
      out.face = mesh.boundary[facetsOf[at][0]].faceIndex;
      out.wallMm = wall > 0 ? wall : WallAt(mesh, at, facetsOf);
      const double h = ElementSizeAt(mesh, at);
      if (out.wallMm > 0 && h > 0)
        out.wallLayers =
            std::max(1, static_cast<int>(std::lround(out.wallMm / h)));
    }
    out.shapeClass = "flat";
    if (auto it = jb.curvedFaces.find(out.face);
        it != jb.curvedFaces.end() && at >= 0) {
      const StudyJobBody::CurvedFace& f = it->second;
      const glm::dvec3 p = mesh.nodes[at];
      double r = f.radius;
      if (f.kind == "cone") {
        const glm::dvec3 a = glm::normalize(f.axis);
        const glm::dvec3 q = (p - f.origin) - glm::dot(p - f.origin, a) * a;
        r = glm::length(q) / std::max(std::cos(f.semiAngle), 1e-6);
      }
      out.radiusMm = r;
      // A thin shell, R/t at least ten (thin-shell theory's bound, and
      // where SP-8007's tests begin), is knocked down; anything thicker is
      // a member, which an imperfection does not knock down the way it does
      // a shell.
      if (out.wallMm > 0 && r >= kThinShellRatio * out.wallMm) {
        out.shapeClass = f.kind;
        out.knockdown = Sp8007Knockdown(r, out.wallMm);
      } else {
        out.shapeClass = "member";
      }
    }

    // Crippling where a load enters: the peak motion at a support's or
    // load's edge, and the mode local there -- every node moving half the
    // peak or more lies within twice that edge's reach of the peak, and
    // within a quarter of the part's size. A column's end moves most too,
    // but its whole length moves with it.
    std::vector<SupportFacets> edges = jb.supports;
    for (const FaceLoad& fl : jb.faceLoads) {
      if (fl.tris.empty())
        continue;
      SupportFacets sf;
      sf.type = fl.type;
      sf.tris = fl.tris;
      sf.awayMm = SupportAwayMm(mesh, fl.tris, LoadType::Fixed, glm::dvec3(0.0),
                                glm::dvec3(0.0));
      edges.push_back(std::move(sf));
    }
    const std::vector<int> zones = SupportEdgeZones(mesh, edges);
    if (peak >= 0 && static_cast<size_t>(peak) < zones.size() &&
        zones[peak] >= 0) {
      const SupportFacets& e = edges[zones[peak]];
      double extent = 0.0;
      for (size_t n = 0; n < nNodes; ++n)
        if (glm::length(first->displacement[n]) >= 0.5 * peakU)
          extent =
              std::max(extent, glm::length(mesh.nodes[n] - mesh.nodes[peak]));
      const double reach = e.awayMm > 0 ? e.awayMm : ElementSizeAt(mesh, peak);
      glm::dvec3 lo(1e300), hi(-1e300);
      for (const glm::dvec3& p : mesh.nodes) {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
      }
      if (extent <= std::min(2.0 * reach, 0.25 * glm::length(hi - lo))) {
        out.atLoadEdge = true;
        out.edgeLoad = e.id;
        out.edgeType = LoadTypeToString(e.type);
        out.edgeFace = mesh.boundary[e.tris.front()].faceIndex;
      }
    }
  }

  // 1/governing = 1/(knockdown x buckling) + 1/min(yield, interlayer).
  double plastic = out.yieldFactor;
  if (out.interlayerFactor > 0 &&
      (plastic == 0 || out.interlayerFactor < plastic))
    plastic = out.interlayerFactor;
  const double elastic = out.buckles ? out.knockdown * out.factor : 0.0;
  if (elastic > 0 && plastic > 0)
    out.governingFactor = 1.0 / (1.0 / elastic + 1.0 / plastic);
  else
    out.governingFactor = elastic > 0 ? elastic : plastic;
  return out;
}

} // namespace

bool StaticSupportsOf(
    const StudyJobBody& jb, const std::vector<Material>& mats,
    const std::vector<std::vector<FaceLoad>>& rhs,
    const glm::dvec3& gravityAccel, const SolveOptions& options,
    std::vector<Supports>& supports,
    std::vector<std::vector<FemResults::SupportReaction>>& pinRows,
    std::string* error) {
  supports.clear();
  pinRows.clear();
  if (jb.pins.empty()) {
    Supports sup;
    sup.fixedNodes = jb.fixedNodes;
    sup.reactions = true;
    supports.assign(rhs.size(), sup);
    pinRows.assign(rhs.size(), {});
    return true;
  }
  // The contact each case settles on, found in-house: a pin pushes, never
  // pulls, and which facets it pushes on is what the iteration decides.
  SolveReport report;
  auto all = SolveWithPins(jb, mats, rhs, gravityAccel, options, report, error,
                           &supports);
  if (!all)
    return false;
  for (const FemResults& r : *all)
    pinRows.push_back(r.reactions);
  return supports.size() == rhs.size();
}

namespace {

// One body of a nonlinear static study on CalculiX
// (docs/simulation/README.md#nonlinear-static): a pin's contact found by
// the in-house iteration and held; a snap-through run's shape seeded with
// the first linear buckling mode of the same load first. On a snap-through
// run `jb.mesh` becomes the seeded shape, which is what was solved.
bool SolveNonlinearBody(StudyJobBody& jb, const std::vector<Material>& mats,
                        const std::vector<FaceLoad>& loads, const glm::dvec3& g,
                        const Nonlinear& nl, const SolveOptions& options,
                        FemResults& out, StudyBodyResult::Nonlinear& summary,
                        std::vector<StudyBodyResult::ContactResult>& contacts,
                        std::string* error) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  SolveOptions own = options;
  own.calculix = nullptr;
  own.given = nullptr;
  std::vector<Supports> sups;
  std::vector<std::vector<FemResults::SupportReaction>> pinRows;
  std::string err;
  if (!StaticSupportsOf(jb, mats, {loads}, g, own, sups, pinRows, &err))
    return fail(err);
  NonlinearRequest req;
  // A contact alone is solved at small deformation.
  req.nlgeom = nl.largeDeformation;
  req.snapThrough = nl.snapThrough;
  req.plasticity = nl.plasticity;
  req.increments = nl.increments > 0 ? nl.increments
                                     : (nl.snapThrough ? kSnapIncrements
                                                       : kNonlinearIncrements);
  const FemMesh& mesh = jb.mesh;
  const size_t nNodes = mesh.nodes.size();
  glm::dvec3 lo(1e300), hi(-1e300);
  for (const glm::dvec3& p : mesh.nodes) {
    lo = glm::min(lo, p);
    hi = glm::max(hi, p);
  }
  const double diagonal = nNodes ? glm::length(hi - lo) : 0.0;
  double linear = 0.0, imperfection = 0.0, limit = 0.0;
  if (nl.snapThrough) {
    // The first mode of the same load, linearly: what the shape is seeded
    // with, and the factor the run's cap is twice.
    const size_t dofs = 3 * (nNodes - std::min(nNodes, jb.fixedNodes.size()));
    const uint64_t estimate = BucklingMemoryEstimate(dofs);
    const uint64_t available = options.availableBytes ? options.availableBytes
                                                      : AvailableMemoryBytes();
    if (available > 0 && estimate > available) {
      char buf[320];
      std::snprintf(buf, sizeof(buf),
                    "a snap-through run seeds its shape with the first "
                    "buckling mode, whose solve of %zu degrees of freedom "
                    "would need about %.2f GB, and %.2f GB is free. A coarser "
                    "mesh_size solves it",
                    dofs, estimate / 1e9, available / 1e9);
      return fail(buf);
    }
    Supports sup = sups.front();
    sup.reactions = false;
    auto base = SolveCases(mesh, mats, sup, {loads}, &err, g, own);
    if (!base)
      return fail("the linear base state: " + err);
    auto modes = SolveBuckling(mesh, mats, sup, base->front().displacement,
                               kBucklingFactorsDefault, &err);
    if (!modes)
      return fail("the first buckling mode: " + err);
    FemResults withModes = base->front();
    withModes.modes = std::move(*modes);
    const StudyBodyResult::Buckling b = ReadBuckling(jb, mats, withModes);
    if (!b.buckles)
      return fail(b.none + ", so a snap-through run has no mode to seed its "
                           "shape with: a large-deformation run "
                           "(snap_through false) steps this load up as it is");
    linear = b.factor;
    const FemResults::ModeShape* first = nullptr;
    for (const FemResults::ModeShape& m : withModes.modes)
      if (m.factor > 0.0 && (!first || m.factor < first->factor))
        first = &m;
    imperfection = nl.hasImperfection ? nl.imperfectionMm
                   : b.wallMm > 0     ? 0.1 * b.wallMm
                                      : 1e-3 * diagonal;
    limit = nl.hasDeflectionLimit ? nl.deflectionLimitMm : 0.1 * diagonal;
    for (size_t n = 0; n < nNodes; ++n)
      jb.mesh.nodes[n] += imperfection * first->displacement[n];
    req.maxFactor = 2.0 * linear;
    req.deflectionLimitMm = limit;
  }
  // The factor the path first yields at: a snap-through run's, and a
  // plastic run's, whose answer is past it.
  if (nl.snapThrough || nl.plasticity) {
    req.yieldAt.assign(nNodes, 0.0);
    for (size_t t = 0; t < mesh.tets.size(); ++t) {
      const size_t d = mesh.tetDomain.empty() ? 0 : (size_t)mesh.tetDomain[t];
      const Material& m = mats[std::min(d, mats.size() - 1)];
      for (int v : mesh.tets[t])
        if (m.yieldMPa > 0 &&
            (req.yieldAt[v] == 0.0 || m.yieldMPa < req.yieldAt[v]))
          req.yieldAt[v] = m.yieldMPa;
    }
  }
  if (!NonlinearOnCalculix(jb, mats, sups.front(), loads, g, req, options, out,
                           summary, &err, &contacts))
    return fail(err);
  summary.linearFactor = linear;
  summary.imperfectionMm = imperfection;
  summary.imperfectionGiven = nl.hasImperfection;
  summary.deflectionLimitMm = limit;
  out.reactions = pinRows.front();
  return true;
}

// The heat of one body (docs/simulation/README.md#thermal): solved
// in-house, or on CalculiX and held to our K_T.
bool SolveBodyHeat(const StudyJobBody& jb, const std::vector<Material>& mats,
                   bool onCalculix, const SolveOptions& options,
                   HeatSolution& out, std::string* error) {
  SolveOptions own = options;
  own.calculix = nullptr;
  own.given = nullptr;
  own.thermal = nullptr;
  if (!onCalculix)
    return SolveHeat(jb.mesh, mats, jb.heat, own, out, error);
  if (!options.calculix) {
    if (error)
      *error = "this study solves on CalculiX, and this run was given no ccx "
               "to run it with";
    return false;
  }
  std::vector<double> T;
  if (!HeatOnCalculix(jb.mesh, mats, jb.heat, jb.bodyName, options, T, error))
    return false;
  if (options.solving)
    options.solving("calculix: checking the answer");
  GivenTemperatures given;
  given.temperature = &T;
  given.precision = kCalculixDatPrecision;
  given.name = options.calculix->ccx.Name();
  own.solving = nullptr;
  return SolveHeat(jb.mesh, mats, jb.heat, own, out, error, &given);
}

// What a body's temperature field says: its range and flux peak, and
// against each material's service temperature, the margin of the most
// pressed (the hottest node of each domain against its own limit).
StudyBodyResult::Thermal ReadThermal(const StudyJobBody& jb,
                                     const std::vector<Material>& mats,
                                     const std::vector<double>& T,
                                     const std::vector<glm::dvec3>* flux) {
  StudyBodyResult::Thermal th;
  const FemMesh& mesh = jb.mesh;
  th.baseC = jb.baseC;
  bool first = true;
  for (size_t n = 0; n < T.size() && n < mesh.nodes.size(); ++n) {
    if (first || T[n] > th.maxC) {
      th.maxC = T[n];
      th.maxAt = mesh.nodes[n];
    }
    if (first || T[n] < th.minC) {
      th.minC = T[n];
      th.minAt = mesh.nodes[n];
    }
    first = false;
    if (flux && n < flux->size()) {
      const double f = glm::length((*flux)[n]);
      if (f > th.maxFluxWm2) {
        th.maxFluxWm2 = f;
        th.maxFluxAt = mesh.nodes[n];
      }
    }
  }
  // The hottest node of each domain.
  std::vector<double> hottest(mats.size(),
                              -std::numeric_limits<double>::infinity());
  for (size_t t = 0; t < mesh.tets.size(); ++t) {
    const size_t d =
        std::min(mesh.tetDomain.empty() ? size_t{0} : (size_t)mesh.tetDomain[t],
                 mats.size() - 1);
    for (int v : mesh.tets[t])
      if (static_cast<size_t>(v) < T.size())
        hottest[d] = std::max(hottest[d], T[v]);
  }
  for (size_t d = 0; d < mats.size(); ++d) {
    th.bulkConductivity = th.bulkConductivity || mats[d].bulkConductivity;
    if (std::isnan(mats[d].serviceMaxC) || !std::isfinite(hottest[d]))
      continue;
    const double margin = mats[d].serviceMaxC - hottest[d];
    if (!th.hasService || margin < th.marginK) {
      th.hasService = true;
      th.marginK = margin;
      th.serviceMaxC = mats[d].serviceMaxC;
      th.serviceBasis = mats[d].serviceBasis;
    }
  }
  return th;
}

} // namespace

bool SolvePreparedStudy(StudyJobInput& in, StudyRuntime& rt, std::string* error,
                        const SolveOptions& options) {
  rt = StudyRuntime{};
  rt.caseNames = in.caseNames;
  rt.sourceDigest = in.sourceDigest;
  rt.shapeThreshold = in.shapeThreshold;
  rt.shapeIslandTets = in.shapeIslandTets;
  rt.shapeVolume = in.shapeVolume;
  rt.shapeThresholdLoweredFrom = in.shapeThresholdLoweredFrom;
  std::string err;
  // A failed (or cancelled) solve leaves no half result: a caller reads a
  // runtime with bodies as solved whole.
  auto fail = [&](const std::string& m) {
    rt = StudyRuntime{};
    if (error)
      *error = m;
    return false;
  };
  // CalculiX solves static studies only, and a study set to it is never
  // solved in-house instead (docs/simulation/README.md#calculix).
  if (const std::string why = SolverRefusal(in.solver, in.type); !why.empty())
    return fail(why);
  if (in.solver == Solver::Calculix && !options.calculix)
    return fail("this study solves on CalculiX, and this run was given no "
                "ccx to run it with");
  bool anyContact = false;
  for (const StudyJobBody& jb : in.bodies)
    anyContact = anyContact || !jb.contacts.empty();
  if (const std::string why =
          NonlinearRefusal(in.solver, in.type, in.nonlinear,
                           in.caseNames.size(), ConvergeMode::Auto, anyContact);
      !why.empty())
    return fail(why);
  if (anyContact && !options.calculix)
    return fail("a study with a contact solves on CalculiX, and this run was "
                "given no ccx to run it with");
  for (StudyJobBody& jb : in.bodies) {
    StudyBodyResult sbr;
    sbr.bodyIndex = jb.bodyIndex;
    sbr.bodyName = jb.bodyName;

    if (in.type == AnalysisType::Thermal) {
      const std::vector<Material> mats =
          jb.materials.empty() ? std::vector<Material>{jb.mat} : jb.materials;
      HeatSolution hs;
      if (!SolveBodyHeat(jb, mats, in.solver == Solver::Calculix, options, hs,
                         &err))
        return fail("body '" + jb.bodyName + "': " + err);
      sbr.thermal = ReadThermal(jb, mats, hs.temperature, &hs.heatFlux);
      sbr.thermal.solved = true;
      sbr.thermal.flows = hs.flows;
      sbr.thermal.inW = hs.inW;
      sbr.thermal.outW = hs.outW;
      sbr.thermal.imbalanceW = hs.imbalanceW;
      sbr.results.temperature = std::move(hs.temperature);
      sbr.results.heatFlux = std::move(hs.heatFlux);
      if (rt.solver.empty() || rt.solver == kDirectSolverName)
        rt.solver = hs.solver;
      rt.residual = std::max(rt.residual, hs.residual);
      rt.residualFloor = std::max(rt.residualFloor, hs.residualFloor);
      rt.energyResidual = std::max(rt.energyResidual, hs.energyResidual);
    } else if (in.type == AnalysisType::Modal) {
      auto modes =
          jb.materials.empty()
              ? SolveModal(jb.mesh, jb.mat, jb.fixedNodes, in.numModes, &err)
              : SolveModal(jb.mesh, jb.materials, jb.fixedNodes, in.numModes,
                           &err);
      if (!modes)
        return fail("body '" + jb.bodyName + "': " + err);
      sbr.results.modes = std::move(*modes);
    } else {
      const glm::dvec3 g = in.gravity * 1000.0; // m/s^2 -> mm/s^2
      // Every case on one factorisation; without cases, the one set of loads.
      const std::vector<std::vector<FaceLoad>> rhs =
          jb.caseLoads.empty()
              ? std::vector<std::vector<FaceLoad>>{jb.faceLoads}
              : jb.caseLoads;
      const std::vector<Material> mats =
          jb.materials.empty() ? std::vector<Material>{jb.mat} : jb.materials;
      // A thermal load (docs/simulation/README.md
      // #the-thermal-load-on-a-static-study): the temperature this mesh
      // takes, every case carrying it.
      SolveOptions opts = options;
      std::shared_ptr<NodalTemperature> field;
      if (jb.thermalLoad.On()) {
        field = std::make_shared<NodalTemperature>();
        field->stressFreeC = jb.thermalLoad.stressFreeC;
        if (jb.thermalLoad.kind == ThermalLoad::Kind::Uniform) {
          field->temperature.assign(jb.mesh.nodes.size(),
                                    jb.thermalLoad.uniformC);
        } else {
          HeatSolution hs;
          if (opts.solving)
            opts.solving("the thermal load's heat");
          if (!SolveBodyHeat(jb, mats, in.solver == Solver::Calculix, opts, hs,
                             &err))
            return fail("body '" + jb.bodyName + "': the thermal load: " + err);
          field->temperature = std::move(hs.temperature);
          rt.residual = std::max(rt.residual, hs.residual);
          rt.residualFloor = std::max(rt.residualFloor, hs.residualFloor);
          rt.energyResidual = std::max(rt.energyResidual, hs.energyResidual);
        }
        opts.thermal = field;
      }
      const bool buckling = in.type == AnalysisType::Buckling;
      if (buckling) {
        // The direct factor of K is what a buckling solve cannot do
        // without, and it fills in steeply: a run that will not fit is
        // refused before anything is solved.
        const size_t n = jb.mesh.nodes.size();
        const size_t dofs = 3 * (n - std::min(n, jb.fixedNodes.size()));
        const uint64_t estimate = BucklingMemoryEstimate(dofs);
        const uint64_t available =
            opts.availableBytes ? opts.availableBytes : AvailableMemoryBytes();
        if (available > 0 && estimate > available) {
          char buf[320];
          std::snprintf(
              buf, sizeof(buf),
              "body '%s': a buckling solve of %zu degrees of freedom would "
              "need about %.2f GB, and %.2f GB is free -- its eigensolve "
              "factors the stiffness directly, so it was not started. A "
              "coarser mesh_size solves it",
              jb.bodyName.c_str(), dofs, estimate / 1e9, available / 1e9);
          return fail(buf);
        }
      }
      // A buckling study's base state: the supports it was solved under,
      // a pin's contact set held as it settled.
      std::vector<Supports> settled;
      SolveReport report;
      std::optional<std::vector<FemResults>> all;
      if (in.nonlinear.On() || !jb.contacts.empty()) {
        FemResults res;
        if (SolveNonlinearBody(jb, mats, jb.faceLoads, g, in.nonlinear, opts,
                               res, sbr.nonlinear, sbr.contacts, &err))
          all = std::vector<FemResults>{std::move(res)};
        report.solver = opts.calculix ? opts.calculix->ccx.Name() : "";
      } else if (in.solver == Solver::Calculix) {
        std::vector<FemResults> res;
        if (SolveBodyOnCalculix(jb, mats, rhs, g, opts, res, report, &err,
                                buckling ? &settled : nullptr))
          all = std::move(res);
        rt.residual = std::max(rt.residual, report.worstResidual);
        rt.residualFloor =
            std::max(rt.residualFloor, report.worstResidualFloor);
        rt.energyResidual =
            std::max(rt.energyResidual, report.worstEnergyResidual);
      } else if (jb.pins.empty()) {
        Supports sup;
        sup.fixedNodes = jb.fixedNodes;
        sup.reactions = true;
        all = SolveCases(jb.mesh, mats, sup, rhs, &err, g, opts, &report);
        settled.assign(rhs.size(), sup);
      } else {
        report.solver = kDirectSolverName;
        all = SolveWithPins(jb, mats, rhs, g, opts, report, &err,
                            buckling ? &settled : nullptr);
      }
      if (!all)
        return fail("body '" + jb.bodyName + "': " + err);
      sbr.supportEdgeZone = SupportEdgeZones(jb.mesh, jb.supports);
      for (const SupportFacets& e : jb.supports) {
        FemResults::SupportReaction o;
        o.support = e.id;
        o.type = e.type;
        sbr.edgeOwners.push_back(o);
      }
      if (field) {
        for (FemResults& c : *all)
          c.temperature = field->temperature;
        sbr.thermal = ReadThermal(jb, mats, field->temperature, nullptr);
        sbr.thermal.load = true;
        sbr.thermal.stressFreeC = field->stressFreeC;
        sbr.thermal.fromStudy = jb.thermalLoad.fromStudy;
      }
      for (FemResults& c : *all) {
        FinishSupports(jb, sbr.supportEdgeZone, sbr.edgeOwners, c);
        // A contact support carried what its pair says it pressed with
        // (docs/simulation/README.md#contact).
        for (FemResults::SupportReaction& r : c.reactions)
          if (r.type == LoadType::Contact)
            for (const StudyBodyResult::ContactResult& cr : sbr.contacts)
              if (!cr.participant && cr.id == r.support) {
                r.forceN = cr.forceN;
                r.momentNmm = cr.momentNmm;
                r.hasMoment = true;
                r.contactAreaMm2 = cr.closedMm2;
                r.faceAreaMm2 = cr.areaMm2;
              }
      }
      if (buckling) {
        // Held as the base state was: linear theory scales the load
        // without turning it, so a pin's contact set holds at every factor.
        if (settled.empty())
          return fail("body '" + jb.bodyName +
                      "': the base state left no supports to buckle about");
        if (opts.cancelled && opts.cancelled())
          return fail("cancelled");
        if (opts.solving)
          opts.solving(in.solver == Solver::Calculix ? "calculix: buckling"
                                                     : "buckling eigensolve");
        std::optional<std::vector<FemResults::ModeShape>> modes;
        if (in.solver == Solver::Calculix) {
          std::vector<FemResults::ModeShape> theirs;
          if (BucklingOnCalculix(jb, mats, settled.front(), jb.faceLoads, g,
                                 in.numModes, opts, theirs, &err))
            modes = std::move(theirs);
        } else {
          modes = SolveBuckling(jb.mesh, mats, settled.front(),
                                all->front().displacement, in.numModes, &err);
        }
        if (!modes)
          return fail("body '" + jb.bodyName + "': " + err);
        all->front().modes = std::move(*modes);
        sbr.buckling = ReadBuckling(jb, mats, all->front());
      }
      // Several bodies may solve differently; the study names the
      // heaviest solver any body needed, which is the one a reader wants to
      // know about, and why an offered GPU was not it.
      std::string name = report.solver;
      if (!report.externalDeclined.empty())
        name += "; GPU declined: " + report.externalDeclined;
      if (rt.solver.empty() || rt.solver == kDirectSolverName ||
          report.solver == kGpuSolverName)
        rt.solver = name;
      // The maxima a summary reads are the worst over the cases.
      for (const FemResults& c : *all) {
        if (c.maxVonMises > rt.maxVonMises) {
          rt.maxVonMises = c.maxVonMises;
          rt.maxVmAt = c.maxVmAt;
          rt.maxVmBody = jb.bodyName;
        }
        if (c.maxDisplacement > rt.maxDisplacement) {
          rt.maxDisplacement = c.maxDisplacement;
          rt.maxDispAt = c.maxDispAt;
          rt.maxDispBody = jb.bodyName;
        }
      }
      sbr.results = all->front();
      if (!jb.caseLoads.empty())
        sbr.cases = std::move(*all);
    }
    sbr.mesh = std::move(jb.mesh);
    rt.bodies.push_back(std::move(sbr));
  }
  return true;
}

bool RunStudy(const Document& doc, Study& study, StudyRuntime& rt,
              std::string* error) {
  ConvergeOptions converge;
  converge.mode = study.converge;
  return RunStudy(doc, study, rt, converge, error);
}

bool RunStudy(const Document& doc, Study& study, StudyRuntime& rt,
              const ConvergeOptions& converge, std::string* error) {
  StudyPlan plan;
  if (!PlanStudy(doc, study, plan, error))
    return false;
  StudyJobInput input;
  std::string err;
  if (!SolveConverged(plan, input, rt, converge, &err)) {
    // Mesh and solve failures set study.error too, as they always have.
    study.error = err.empty() ? "solve failed" : err;
    if (error)
      *error = study.error;
    return false;
  }
  return true;
}

// ---- A peak that has settled ------------------------------------------------
// (docs/simulation/README.md#a-peak-that-has-settled)

namespace {

constexpr int kMaxRefinements = 4;
constexpr double kSettledChange = 0.02; // every peak moves under 2 %

// One governing peak of one body: the worst over its load cases, read away
// from a support's edge when it sits on one, and the node it is at.
struct GoverningPeak {
  int body = 0; // index into the runtime's (and the plan's) bodies
  std::string quantity;
  bool away = false;
  double mpa = 0.0;
  int node = -1;
  // Every node (off the edges, for an away value) within kHotShare of the
  // peak: a part with two fillets, or a hole, peaks on each of them alike,
  // and refining one only moves the peak to its twin, which reads as
  // settled when it is not.
  std::vector<int> hot;
};
constexpr double kHotShare = 0.9;

// The field's peak, or, when that node is on a support's edge, its largest
// value off every edge.
GoverningPeak PeakOfField(const std::vector<double>& field,
                          const std::vector<int>& zones) {
  const auto hot = [&](GoverningPeak& g, bool offEdges) {
    for (size_t i = 0; i < field.size(); ++i)
      if (field[i] >= kHotShare * g.mpa && (!offEdges || zones[i] < 0))
        g.hot.push_back(static_cast<int>(i));
  };
  GoverningPeak p;
  for (size_t i = 0; i < field.size(); ++i)
    if (p.node < 0 || field[i] > p.mpa) {
      p.mpa = field[i];
      p.node = static_cast<int>(i);
    }
  if (p.node < 0 || zones.size() != field.size() || zones[p.node] < 0) {
    if (p.node >= 0)
      hot(p, false);
    return p;
  }
  GoverningPeak away;
  away.away = true;
  for (size_t i = 0; i < field.size(); ++i)
    if (zones[i] < 0 && (away.node < 0 || field[i] > away.mpa)) {
      away.mpa = field[i];
      away.node = static_cast<int>(i);
    }
  if (away.node < 0) {
    hot(p, false);
    return p;
  }
  hot(away, true);
  return away;
}

std::vector<GoverningPeak> GoverningPeaks(const StudyRuntime& rt,
                                          const StudyPlan& plan,
                                          const ConvergeOptions& converge) {
  std::vector<GoverningPeak> out;
  for (size_t b = 0; b < rt.bodies.size(); ++b) {
    const StudyBodyResult& body = rt.bodies[b];
    if (body.thermal.solved) {
      // A thermal body's peak temperature RISE over its lowest held or
      // ambient temperature: a change in degrees C is an offset's artefact.
      std::vector<double> rise = body.results.temperature;
      for (double& t : rise)
        t -= body.thermal.baseC;
      GoverningPeak p = PeakOfField(rise, {});
      if (p.node >= 0 && p.mpa > 0.0) {
        p.body = static_cast<int>(b);
        p.quantity = "temperature_rise";
        out.push_back(p);
      }
      continue;
    }
    const bool printed =
        b < plan.bodies.size() && plan.bodies[b].interlayerMPa > 0.0;
    const char* layers =
        converge.hasLayerNormal ? "across_layers" : "max_principal";
    GoverningPeak vm, il;
    vm.node = il.node = -1;
    for (size_t k = 0; k < std::max<size_t>(1, body.cases.size()); ++k) {
      const FemResults& r = body.Case(static_cast<int>(k));
      GoverningPeak p = PeakOfField(r.vonMises, body.supportEdgeZone);
      if (p.node >= 0 && (vm.node < 0 || p.mpa > vm.mpa))
        vm = p;
      if (!printed)
        continue;
      const std::vector<double> f =
          converge.hasLayerNormal ? AcrossLayersField(r, converge.layerNormal)
                                  : MaxPrincipalField(r);
      p = PeakOfField(f, body.supportEdgeZone);
      if (p.node >= 0 && (il.node < 0 || p.mpa > il.mpa))
        il = p;
    }
    if (vm.node >= 0 && vm.mpa > 0.0) {
      vm.body = static_cast<int>(b);
      vm.quantity = "von_mises";
      out.push_back(vm);
    }
    if (il.node >= 0 && il.mpa > 0.0) {
      il.body = static_cast<int>(b);
      il.quantity = layers;
      out.push_back(il);
    }
  }
  return out;
}

// The B-Rep face a peak node lies on: of the faces whose facets use it, the
// smallest (a bore or a fillet rather than the flat it meets); an interior
// node takes the nearest boundary node's. Built once a mesh.
struct FaceLookup {
  const FemMesh& mesh;
  std::vector<int> faceOf; // per node: its smallest face, -1 if interior
  explicit FaceLookup(const FemMesh& m) : mesh(m), faceOf(m.nodes.size(), -1) {
    std::map<int, double> area;
    for (const FemMesh::BoundaryTri& t : m.boundary) {
      const glm::dvec3 a = m.nodes[t.nodes[0]];
      area[t.faceIndex] +=
          0.5 * glm::length(glm::cross(m.nodes[t.nodes[1]] - a,
                                       m.nodes[t.nodes[2]] - a));
    }
    for (const FemMesh::BoundaryTri& t : m.boundary)
      for (int v : t.nodes)
        if (faceOf[v] < 0 || area[t.faceIndex] < area[faceOf[v]])
          faceOf[v] = t.faceIndex;
  }
  bool OnBoundary(int node) const {
    return node >= 0 && static_cast<size_t>(node) < faceOf.size() &&
           faceOf[node] >= 0;
  }
  int Face(int node) const {
    if (node < 0 || static_cast<size_t>(node) >= faceOf.size())
      return -1;
    if (faceOf[node] >= 0)
      return faceOf[node];
    double best = 1e300;
    int face = -1;
    for (size_t v = 0; v < faceOf.size(); ++v)
      if (faceOf[v] >= 0) {
        const double d = glm::length(mesh.nodes[v] - mesh.nodes[node]);
        if (d < best) {
          best = d;
          face = faceOf[v];
        }
      }
    return face;
  }
};

// The size the face's elements have now: the mean edge length of its
// facets (corner to corner).
double FaceElementSize(const FemMesh& mesh, int face) {
  double sum = 0.0;
  int n = 0;
  for (const FemMesh::BoundaryTri& t : mesh.boundary) {
    if (t.faceIndex != face)
      continue;
    for (int k = 0; k < 3; ++k) {
      sum += glm::length(mesh.nodes[t.nodes[k]] -
                         mesh.nodes[t.nodes[(k + 1) % 3]]);
      ++n;
    }
  }
  return n > 0 ? sum / n : 0.0;
}

std::size_t NodeCount(const StudyRuntime& rt) {
  std::size_t n = 0;
  for (const StudyBodyResult& b : rt.bodies)
    n += b.mesh.nodes.size();
  return n;
}

std::vector<ConvergencePeak> Record(const StudyRuntime& rt,
                                    const std::vector<GoverningPeak>& peaks) {
  std::vector<ConvergencePeak> out;
  for (const GoverningPeak& p : peaks)
    out.push_back({rt.bodies[p.body].bodyName, p.quantity, p.away, p.mpa});
  return out;
}

double Seconds(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - since)
      .count();
}

} // namespace

bool SolveConverged(const StudyPlan& planIn, StudyJobInput& in,
                    StudyRuntime& rt, const ConvergeOptions& converge,
                    std::string* error, const SolveOptions& options) {
  auto started = std::chrono::steady_clock::now();
  bool contact = false;
  for (const StudyBodyPlan& bp : planIn.bodies)
    contact = contact || !bp.contacts.empty();
  if (const std::string why =
          NonlinearRefusal(planIn.solver, planIn.type, planIn.nonlinear,
                           planIn.caseNames.size(), converge.mode, contact);
      !why.empty()) {
    if (error)
      *error = why;
    return false;
  }
  if (!MeshPlannedStudy(planIn, in, error) ||
      (options.cancelled && options.cancelled()))
    return false;
  if (!SolvePreparedStudy(in, rt, error, options))
    return false;
  Convergence conv;
  conv.hasLayerNormal = converge.hasLayerNormal;
  conv.layerNormal = converge.layerNormal;
  const double first = Seconds(started);
  const auto notChecked = [&](const std::string& why) {
    conv.why = why;
    rt.convergence = conv;
    return true;
  };
  if (planIn.type != AnalysisType::Static &&
      planIn.type != AnalysisType::Buckling &&
      planIn.type != AnalysisType::Thermal)
    return notChecked("a " + std::string(AnalysisTypeToString(planIn.type)) +
                      " study has no peak stress to settle");
  if (converge.mode == ConvergeMode::Never)
    return notChecked("converge: never");
  if (planIn.nonlinear.snapThrough)
    return notChecked("a snap-through run's answer is its peak load factor, "
                      "which refining round a stress peak does not settle");
  if (planIn.nonlinear.plasticity)
    return notChecked("a plastic run's stress is held down by the yield "
                      "where it concentrates, and its plastic strain, which "
                      "is what concentrates there, is not refined round");
  if (contact)
    return notChecked("a contact's peak lies on a contact face whose partner "
                      "is meshed apart from it, and refining one side of a "
                      "contact does not settle it");
  for (const StudyBodyPlan& bp : planIn.bodies)
    if (bp.premeshed)
      return notChecked("a static study of a topology result solves the "
                        "result's own tets, which cannot be refined");
  std::vector<GoverningPeak> peaks = GoverningPeaks(rt, planIn, converge);
  if (peaks.empty())
    return notChecked(planIn.type == AnalysisType::Thermal
                          ? "no body rises above its lowest held or ambient "
                            "temperature"
                          : "no body carries a stress to settle");
  conv.checked = true;
  conv.steps.push_back({{}, NodeCount(rt), first, Record(rt, peaks)});
  StudyPlan plan = planIn;
  double last = first;
  for (int step = 1; step <= kMaxRefinements; ++step) {
    if (options.cancelled && options.cancelled())
      break;
    // On auto a step costs at least what the last one did: one that would
    // run past the limit is not started.
    if (converge.mode == ConvergeMode::Auto && last >= converge.autoSeconds) {
      conv.stopped = "time";
      conv.stoppedSeconds = last;
      break;
    }
    // Halve the element size on each face a governing peak lies on.
    StudyPlan next = plan;
    ConvergenceStep rec;
    std::vector<std::pair<int, int>> where; // (body, face), each once
    for (const GoverningPeak& p : peaks) {
      const FaceLookup faces(rt.bodies[p.body].mesh);
      std::vector<int> nodes{p.node};
      for (int n : p.hot)
        if (n != p.node && faces.OnBoundary(n))
          nodes.push_back(n);
      for (int n : nodes) {
        const std::pair<int, int> bf{p.body, faces.Face(n)};
        if (bf.second >= 0 &&
            std::find(where.begin(), where.end(), bf) == where.end())
          where.push_back(bf);
      }
    }
    for (const auto& [body, face] : where) {
      const FemMesh& mesh = rt.bodies[body].mesh;
      const std::string& bodyName = rt.bodies[body].bodyName;
      const double size = 0.5 * FaceElementSize(mesh, face);
      if (size <= 0.0)
        continue;
      bool dup = false;
      for (const ConvergenceRefinement& r : rec.refined)
        dup = dup || (r.body == bodyName && r.face == face);
      if (dup)
        continue;
      auto& sizes = next.bodies[body].settings.faceSizes;
      bool set = false;
      for (auto& [f, s] : sizes)
        if (f == face) {
          s = std::min(s, size);
          set = true;
        }
      if (!set)
        sizes.emplace_back(face, size);
      rec.refined.push_back({bodyName, face, size});
    }
    if (rec.refined.empty()) {
      conv.stopped = "failed";
      conv.why = "no face to refine under the peak";
      break;
    }
    started = std::chrono::steady_clock::now();
    StudyJobInput nextIn;
    StudyRuntime nextRt;
    std::string err;
    if (!MeshPlannedStudy(next, nextIn, &err) ||
        (options.cancelled && options.cancelled()) ||
        !SolvePreparedStudy(nextIn, nextRt, &err, options)) {
      if (options.cancelled && options.cancelled())
        break;
      conv.stopped = "failed";
      conv.why = "refinement " + std::to_string(step) + " failed: " + err;
      break;
    }
    last = Seconds(started);
    std::vector<GoverningPeak> now = GoverningPeaks(nextRt, next, converge);
    // How far each peak moved: matched by body and quantity.
    double change = 0.0;
    for (const GoverningPeak& p : now)
      for (const GoverningPeak& q : peaks)
        if (p.body == q.body && p.quantity == q.quantity && q.mpa > 0.0)
          change = std::max(change, std::abs(p.mpa - q.mpa) / q.mpa);
    rec.nodes = NodeCount(nextRt);
    rec.seconds = last;
    rec.peaks = Record(nextRt, now);
    conv.steps.push_back(std::move(rec));
    conv.lastChangePct = 100.0 * change;
    rt = std::move(nextRt);
    in = std::move(nextIn);
    plan = std::move(next);
    peaks = std::move(now);
    if (change < kSettledChange) {
      conv.converged = true;
      break;
    }
  }
  if (!conv.converged && conv.stopped.empty() &&
      !(options.cancelled && options.cancelled()))
    conv.stopped = "steps";
  rt.convergence = std::move(conv);
  return true;
}

namespace {

// What a keep-out names, for its report and its refusals.
std::string KeepOutLabel(const KeepOut& k) {
  if (!k.body.empty())
    return "body '" + k.body + "'";
  char buf[160];
  std::snprintf(buf, sizeof(buf), "box [%g, %g, %g]-[%g, %g, %g]", k.boxMin.x,
                k.boxMin.y, k.boxMin.z, k.boxMax.x, k.boxMax.y, k.boxMax.z);
  return buf;
}

} // namespace

bool PrepareTopology(const Document& doc, Study& study, TopoInput& out,
                     std::string* error) {
  out = TopoInput{};
  study.error.clear();
  auto fail = [&](const std::string& m) {
    study.error = m;
    if (error)
      *error = m;
    return false;
  };

  std::string err;
  if (!EvaluateStudyExprs(study, doc.Symbols(), &err))
    return fail(err);
  if (study.volumeFraction <= 0.0 || study.volumeFraction >= 1.0)
    return fail("volume fraction must be in (0, 1)");
  for (const Load& l : study.loads)
    if (l.type == LoadType::Pin)
      return fail("load " + std::to_string(l.id) +
                  ": a topology study's supports are fixed (it freezes the "
                  "faces they hold); a pin is a static study's");
  // A contact (or a nonlinear option) is a static study's, never read as
  // a fixed support here.
  if (const std::string why = ContactRefusal(study); !why.empty())
    return fail(why);
  if (!ValidateLoadCases(study, &err))
    return fail(err);

  const auto& bodies = doc.GetBodies();

  // Group loads by body (v1 optimizes exactly one loaded body).
  std::map<int, std::vector<std::pair<int, size_t>>>
      bodyLoads; // (faceIndex, loadIdx)
  for (size_t i = 0; i < study.loads.size(); ++i) {
    ResolvedFace rf;
    if (!ResolveFace(doc, study.loads[i].face, "load " + std::to_string(i + 1),
                     rf, &err))
      return fail(err);
    bodyLoads[rf.bodyIndex].push_back({rf.faceIndex, i});
  }
  if (bodyLoads.empty())
    return fail("study has no loads");
  if (bodyLoads.size() > 1)
    return fail("topology optimization supports a single loaded body (v1)");
  const int bodyIndex = bodyLoads.begin()->first;
  const Body& body = bodies[bodyIndex];

  MeshSettings settings;
  settings.maxSize = study.meshSize;
  for (size_t i = 0; i < study.refinements.size(); ++i) {
    ResolvedFace rf;
    if (!ResolveFace(doc, study.refinements[i].face,
                     "refinement " + std::to_string(i + 1), rf, &err))
      return fail(err);
    if (rf.bodyIndex == bodyIndex)
      settings.faceSizes.push_back({rf.faceIndex, study.refinements[i].size});
  }

  const auto matType =
      CADMaterials::MaterialLibrary::MaterialTypeFromString(body.material);
  const auto& mp = CADMaterials::MaterialLibrary::GetMaterial(matType);
  if (const std::string why = CADMaterials::ElasticDataRefusal(mp);
      !why.empty())
    return fail("body '" + body.name + "': material '" +
                (body.material.empty() ? "default" : body.material) + "' " +
                why);

  // Keep faces (frozen solid beyond the auto-frozen support/load skin), by
  // keep-face index so an island can say which it took.
  std::vector<int> keepFaceIdx(study.keepFaces.size(), -1);
  for (size_t i = 0; i < study.keepFaces.size(); ++i) {
    ResolvedFace rf;
    if (!ResolveFace(doc, study.keepFaces[i],
                     "keep face " + std::to_string(i + 1), rf, &err))
      return fail(err);
    if (rf.bodyIndex == bodyIndex)
      keepFaceIdx[i] = rf.faceIndex;
  }

  // Keep-outs, resolved before anything is meshed: a body the part does not
  // have, the design body itself and an empty box are refused by name.
  struct KeepOutShape {
    const TopoDS_Shape* shape = nullptr; // a body's; null = the box
    glm::dvec3 lo{0.0}, hi{0.0};         // the box, or the body's bounds
  };
  std::vector<KeepOutShape> keepOuts;
  for (size_t i = 0; i < study.keepOuts.size(); ++i) {
    const KeepOut& k = study.keepOuts[i];
    KeepOutShape ks;
    if (!k.body.empty()) {
      const Body* hit = nullptr;
      std::string names;
      for (const Body& b : bodies) {
        names += (names.empty() ? "" : ", ") + ("'" + b.name + "'");
        if (b.name == k.body)
          hit = &b;
      }
      if (hit == nullptr)
        return fail("keep-out " + std::to_string(i + 1) +
                    ": the part has no "
                    "body '" +
                    k.body + "' (it has " + names +
                    "); a neighbour's geometry arrives as a Derived "
                    "reference body");
      if (hit == &body)
        return fail("keep-out " + std::to_string(i + 1) + ": '" + k.body +
                    "' is the body being optimised");
      ks.shape = &hit->shape.GetShape();
      if (!Measure::WorldBounds({*hit}, glm::dmat4(1.0), ks.lo, ks.hi))
        return fail("keep-out " + std::to_string(i + 1) + ": body '" + k.body +
                    "' has no shape");
    } else {
      if (!(k.boxMax.x > k.boxMin.x && k.boxMax.y > k.boxMin.y &&
            k.boxMax.z > k.boxMin.z))
        return fail("keep-out " + std::to_string(i + 1) + ": " +
                    KeepOutLabel(k) +
                    " is empty: its max must exceed its min on every axis");
      ks.lo = k.boxMin;
      ks.hi = k.boxMax;
    }
    keepOuts.push_back(ks);
  }

  // A deep copy, as a static plan meshes: Netgen writes triangulation into
  // the faces it meshes and clears their flags, and the document's shape is
  // what the renderer draws and the project saves.
  auto meshed =
      Mesh(BRepBuilderAPI_Copy(body.shape.GetShape()).Shape(), settings, &err);
  if (!meshed)
    return fail("body '" + body.name + "': " + err);

  // The faces that carry a support, a load or a keep, which no keep-out may
  // take a tet of.
  std::map<int, std::string> anchorFace; // face index -> what holds it
  for (const auto& [faceIndex, loadIdx] : bodyLoads[bodyIndex])
    anchorFace.emplace(
        faceIndex,
        (IsSupport(study.loads[loadIdx].type) ? "support " : "load ") +
            std::to_string(study.loads[loadIdx].id));
  for (size_t i = 0; i < keepFaceIdx.size(); ++i)
    if (keepFaceIdx[i] >= 0)
      anchorFace.emplace(keepFaceIdx[i], "keep face " + std::to_string(i + 1));

  // A tet whose centroid lies inside a keep-out leaves the mesh: void, and
  // not design space (docs/topopt/README.md#keep-outs).
  if (!keepOuts.empty()) {
    const FemMesh& full = *meshed;
    // The triangles of those faces, by their sorted corners: a keep-out may
    // not take a tet one of them is a face of. (A tet that only touches such
    // a face at an edge or a corner carries none of it, and goes.)
    const auto triKey = [](int a, int b, int c) {
      std::array<int, 3> k{a, b, c};
      std::sort(k.begin(), k.end());
      return k;
    };
    std::map<std::array<int, 3>, std::string> anchorTri;
    for (const FemMesh::BoundaryTri& t : full.boundary)
      if (auto it = anchorFace.find(t.faceIndex); it != anchorFace.end())
        anchorTri.emplace(triKey(t.nodes[0], t.nodes[1], t.nodes[2]),
                          it->second);
    std::vector<char> keep(full.tets.size(), 1);
    for (size_t i = 0; i < keepOuts.size(); ++i) {
      const KeepOutShape& ks = keepOuts[i];
      std::vector<char> inside(full.tets.size(), 0);
      for (size_t e = 0; e < full.tets.size(); ++e) {
        const auto& t = full.tets[e];
        const glm::dvec3 c = 0.25 * (full.nodes[t[0]] + full.nodes[t[1]] +
                                     full.nodes[t[2]] + full.nodes[t[3]]);
        if (c.x < ks.lo.x || c.y < ks.lo.y || c.z < ks.lo.z || c.x > ks.hi.x ||
            c.y > ks.hi.y || c.z > ks.hi.z)
          continue;
        if (ks.shape != nullptr) {
          BRepClass3d_SolidClassifier cls(*ks.shape, gp_Pnt(c.x, c.y, c.z),
                                          1e-6);
          if (cls.State() != TopAbs_IN)
            continue;
        }
        static constexpr int kFace[4][3] = {
            {0, 1, 2}, {0, 1, 3}, {1, 2, 3}, {0, 2, 3}};
        for (const auto& fc : kFace)
          if (auto it = anchorTri.find(triKey(t[fc[0]], t[fc[1]], t[fc[2]]));
              it != anchorTri.end())
            return fail("keep-out " + std::to_string(i + 1) + " (" +
                        KeepOutLabel(study.keepOuts[i]) +
                        ") takes material that " + it->second +
                        " is on: a keep-out must clear the supports, loads "
                        "and keep faces");
        inside[e] = 1;
      }
      KeepOutReport rep;
      rep.what = KeepOutLabel(study.keepOuts[i]);
      for (size_t e = 0; e < inside.size(); ++e)
        if (inside[e] && keep[e]) {
          keep[e] = 0;
          ++rep.tets;
        }
      rep.volume = TetSetVolume(full, inside);
      out.keepOuts.push_back(rep);
    }
    meshed = ExtractSubMesh(full, keep);
    if (meshed->tets.empty())
      return fail("the keep-outs take the whole of body '" + body.name + "'");
  }
  FemMesh& mesh = *meshed;

  // Fixed nodes + face tractions per case + keep nodes, exactly as the static
  // solve resolves them (fixed faces contribute their nodes; force/pressure
  // faces their tri indices; keep faces their nodes).
  std::set<int> fixedSet, keepSet;
  const size_t nCases = std::max<size_t>(1, study.loadCases.size());
  std::vector<TopoCase> cases(nCases);
  for (size_t k = 0; k < study.loadCases.size(); ++k) {
    cases[k].name = study.loadCases[k].name;
    cases[k].weight = study.loadCases[k].weight;
    out.caseNames.push_back(study.loadCases[k].name);
  }
  bool hasFixed = false, hasApplied = false;
  for (const auto& [faceIndex, loadIdx] : bodyLoads[bodyIndex]) {
    const Load& load = study.loads[loadIdx];
    if (load.type == LoadType::Fixed) {
      hasFixed = true;
      for (const auto& tri : mesh.boundary)
        if (tri.faceIndex == faceIndex)
          for (int n : tri.nodes)
            fixedSet.insert(n);
    } else {
      hasApplied = true;
      Load complete = load;
      if (!CompleteLoadGeometry(FaceByIndex(body.shape.GetShape(), faceIndex),
                                complete, &err))
        return fail(err);
      FaceLoad fl = MakeFaceLoad(complete);
      for (int t = 0; t < static_cast<int>(mesh.boundary.size()); ++t)
        if (mesh.boundary[t].faceIndex == faceIndex)
          fl.tris.push_back(t);
      const int k =
          study.loadCases.empty() ? 0 : study.FindLoadCase(load.loadCase);
      cases[k].loads.push_back(std::move(fl));
    }
  }
  if (!hasFixed)
    return fail("body '" + body.name + "' has no fixed support");
  if (!hasApplied)
    return fail("topology optimization needs a force or pressure load");
  if (fixedSet.empty())
    return fail("body '" + body.name + "': fixed face meshed to no nodes");
  out.keepFaceTris.assign(study.keepFaces.size(), {});
  for (size_t i = 0; i < keepFaceIdx.size(); ++i) {
    if (keepFaceIdx[i] < 0)
      continue;
    for (int t = 0; t < static_cast<int>(mesh.boundary.size()); ++t)
      if (mesh.boundary[t].faceIndex == keepFaceIdx[i]) {
        out.keepFaceTris[i].push_back(t);
        for (int n : mesh.boundary[t].nodes)
          keepSet.insert(n);
      }
  }

  out.bodyIndex = bodyIndex;
  out.bodyName = body.name;
  out.mesh = std::move(mesh);
  // Density is unused (gravity is intentionally excluded from the TO
  // objective).
  out.material = Material{mp.youngGPa * 1000.0, mp.poisson, 0.0};
  out.fixedNodes.assign(fixedSet.begin(), fixedSet.end());
  out.cases = std::move(cases);
  out.keepNodes.assign(keepSet.begin(), keepSet.end());
  out.params.volumeFraction = study.volumeFraction;
  out.params.filterRadius = study.filterRadius;
  out.params.maxIterations = study.maxIterations > 0 ? study.maxIterations : 60;
  return true;
}

StudyRuntime MakeTopoRuntime(TopoInput& input, const TopoResult& result) {
  StudyRuntime rt;
  rt.topology = true;
  StudyBodyResult sbr;
  sbr.bodyIndex = input.bodyIndex;
  sbr.bodyName = input.bodyName;
  sbr.density = result.density;
  rt.iterations = result.iterations;
  rt.converged = result.converged;
  rt.cancelled = result.cancelled;
  rt.volumeFractionAchieved = result.volumeFractionAchieved;
  rt.complianceHistory = result.complianceHistory;
  rt.caseNames = input.caseNames;
  for (const TopoCase& c : input.cases)
    rt.caseWeights.push_back(c.weight);
  rt.caseCompliance = result.caseCompliance;
  rt.keepOuts = input.keepOuts;
  rt.islands = result.islands;
  // Which keep faces an island took: a face any of whose triangles' tets
  // were removed.
  if (!result.islandTets.empty()) {
    std::set<int> gone(result.islandTets.begin(), result.islandTets.end());
    std::set<int> goneNodes;
    for (int e : gone)
      for (int v : input.mesh.tets[e])
        goneNodes.insert(v);
    for (size_t i = 0; i < input.keepFaceTris.size(); ++i) {
      bool hit = false;
      for (int t : input.keepFaceTris[i]) {
        // A boundary triangle belongs to the removed tet whose corners
        // include all three of its own.
        const auto& tri = input.mesh.boundary[t].nodes;
        if (goneNodes.count(tri[0]) && goneNodes.count(tri[1]) &&
            goneNodes.count(tri[2])) {
          for (int e : gone) {
            int shared = 0;
            for (int c = 0; c < 4; ++c)
              shared += input.mesh.tets[e][c] == tri[0] ||
                        input.mesh.tets[e][c] == tri[1] ||
                        input.mesh.tets[e][c] == tri[2];
            if (shared == 3) {
              hit = true;
              break;
            }
          }
        }
        if (hit)
          break;
      }
      if (hit)
        rt.islands.keepFaces.push_back(static_cast<int>(i));
    }
  }
  sbr.mesh = std::move(input.mesh);
  rt.densityDigest = DensityDigest(sbr.mesh, sbr.density);
  rt.bodies.push_back(std::move(sbr));
  return rt;
}

bool RunTopology(const Document& doc, Study& study, StudyRuntime& rt,
                 std::string* error) {
  TopoInput input;
  if (!PrepareTopology(doc, study, input, error))
    return false;
  std::string err;
  auto res = TopologyOptimize(input.mesh, input.material, input.fixedNodes,
                              input.cases, input.params, &err, {},
                              input.keepNodes, nullptr);
  if (!res) {
    study.error = "body '" + input.bodyName + "': " + err;
    if (error)
      *error = study.error;
    return false;
  }
  rt = MakeTopoRuntime(input, *res);
  return true;
}

// ---- What a result keeps -------------------------------------------------
// docs/simulation/README.md#what-a-saved-result-keeps

namespace {

double ToFloat(double v) { return static_cast<double>(static_cast<float>(v)); }

glm::dvec3 ToFloat(const glm::dvec3& p) {
  return {ToFloat(p.x), ToFloat(p.y), ToFloat(p.z)};
}

void RoundAll(std::vector<double>& v) {
  for (double& x : v)
    x = ToFloat(x);
}

void RoundAll(std::vector<glm::dvec3>& v) {
  for (glm::dvec3& p : v)
    p = ToFloat(p);
}

void RoundAll(std::vector<Stress>& v) {
  for (Stress& s : v)
    for (double& x : s)
      x = ToFloat(x);
}

void RoundFields(FemResults& r) {
  RoundAll(r.displacement);
  RoundAll(r.vonMises);
  RoundAll(r.stress);
  RoundAll(r.plasticStrain);
  RoundAll(r.residualDisplacement);
  RoundAll(r.temperature);
  RoundAll(r.heatFlux);
  for (FemResults::ModeShape& m : r.modes)
    RoundAll(m.displacement);
}

// Every per-node array of `r` has one entry per node (or none).
bool PerNode(const FemResults& r, size_t n) {
  if (!r.displacement.empty() && r.displacement.size() != n)
    return false;
  if (!r.vonMises.empty() && r.vonMises.size() != n)
    return false;
  if (!r.stress.empty() && r.stress.size() != n)
    return false;
  if (!r.plasticStrain.empty() && r.plasticStrain.size() != n)
    return false;
  if (!r.residualDisplacement.empty() && r.residualDisplacement.size() != n)
    return false;
  if (!r.temperature.empty() && r.temperature.size() != n)
    return false;
  if (!r.heatFlux.empty() && r.heatFlux.size() != n)
    return false;
  for (const FemResults::ModeShape& m : r.modes)
    if (m.displacement.size() != n)
      return false;
  return true;
}

template <typename T>
void Gather(std::vector<T>& v, const std::vector<int>& kept) {
  if (v.empty())
    return;
  std::vector<T> out;
  out.reserve(kept.size());
  for (int old : kept)
    out.push_back(v[old]);
  v = std::move(out);
}

void Renumber(FemResults& r, const std::vector<int>& kept,
              const std::vector<int>& newIndex) {
  Gather(r.displacement, kept);
  Gather(r.vonMises, kept);
  Gather(r.stress, kept);
  Gather(r.plasticStrain, kept);
  Gather(r.residualDisplacement, kept);
  Gather(r.temperature, kept);
  Gather(r.heatFlux, kept);
  for (FemResults::ModeShape& m : r.modes)
    Gather(m.displacement, kept);
  if (r.maxVmNode >= 0)
    r.maxVmNode = newIndex[r.maxVmNode];
  if (r.maxPeNode >= 0)
    r.maxPeNode = newIndex[r.maxPeNode];
  if (r.maxDispNode >= 0)
    r.maxDispNode = newIndex[r.maxDispNode];
}

// A static or modal body down to its boundary surface.
void KeepSurface(StudyBodyResult& b) {
  FemMesh& m = b.mesh;
  const size_t n = m.nodes.size();
  if (!PerNode(b.results, n))
    return;
  for (const FemResults& c : b.cases)
    if (!PerNode(c, n))
      return;
  std::vector<char> used(n, 0);
  for (const FemMesh::BoundaryTri& t : m.boundary)
    for (int v : t.nodes)
      if (v < 0 || static_cast<size_t>(v) >= n)
        return; // not a mesh this can renumber; left as it is
      else
        used[v] = 1;
  const auto peak = [&](const FemResults& r) {
    for (int v : {r.maxVmNode, r.maxDispNode, r.maxPeNode})
      if (v >= 0 && static_cast<size_t>(v) < n)
        used[v] = 1;
  };
  peak(b.results);
  for (const FemResults& c : b.cases)
    peak(c);
  const auto valid = [&](const FemResults& r) {
    for (int v : {r.maxVmNode, r.maxDispNode, r.maxPeNode})
      if (v >= 0 && static_cast<size_t>(v) >= n)
        return false;
    return true;
  };
  if (!valid(b.results))
    return;
  for (const FemResults& c : b.cases)
    if (!valid(c))
      return;

  std::vector<int> kept, newIndex(n, -1);
  for (size_t i = 0; i < n; ++i)
    if (used[i]) {
      newIndex[i] = static_cast<int>(kept.size());
      kept.push_back(static_cast<int>(i));
    }
  if (kept.size() != n) {
    Gather(m.nodes, kept);
    for (FemMesh::BoundaryTri& t : m.boundary)
      for (int& v : t.nodes)
        v = newIndex[v];
    Renumber(b.results, kept, newIndex);
    for (FemResults& c : b.cases)
      Renumber(c, kept, newIndex);
    if (b.supportEdgeZone.size() == n)
      Gather(b.supportEdgeZone, kept);
  }
  m.tets.clear();
  m.tets.shrink_to_fit();
  m.tetDomain.clear();
  m.tetDomain.shrink_to_fit();
}

} // namespace

void KeepWhatIsRead(StudyRuntime& rt) {
  for (StudyBodyResult& b : rt.bodies) {
    FemMesh& m = b.mesh;
    // The solved mesh's counts survive the cut (a loaded result carries
    // them; a fresh run fills them from the mesh).
    if (m.stats.nodeCount == 0)
      m.stats.nodeCount = m.nodes.size();
    if (m.stats.tetCount == 0)
      m.stats.tetCount = m.tets.size();
    if (!rt.topology)
      KeepSurface(b);
    if (std::all_of(m.tetDomain.begin(), m.tetDomain.end(),
                    [](int d) { return d == 0; }))
      m.tetDomain.clear();
    RoundAll(m.nodes);
    RoundAll(b.density);
    RoundFields(b.results);
    for (FemResults& c : b.cases)
      RoundFields(c);
  }
}

} // namespace fem
