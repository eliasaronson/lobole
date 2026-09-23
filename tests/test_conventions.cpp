// Conventions the codebase relies on, checked against the source itself.
//
// These exist because the alternative is a comment, and comments do not fail
// the build. Every rule here was written after the same bug class arrived a
// second time:
//
//   - kFeatureTypes said "Keep in lockstep with the FeatureFromJson chain
//     below" and nothing enforced it, so a type could load fine and be
//     invisible to `describe`. That one is now enforced at runtime by
//     FeatureFromJson itself (test_serialization.cpp).
//
//   - Six string-to-enum parsers silently substituted a default where eight
//     others threw. A drawing's A2 sheet loaded as A4; add_mate(type:
//     "revolut") produced a RIGID joint that solved perfectly well with the
//     wrong degrees of freedom. Two of the six had ALREADY been fixed for
//     exactly this reason years apart, which is what makes a scanning test
//     worth its ugliness: the sweep stopped early and nothing noticed.
//
//   - A seventh, MaterialLibrary::FromSlug, went on substituting for another
//     year after that sweep — it was named FromSlug, and the scan below keys
//     off the name. An unknown material became DEFAULT, an ALUMINIUM
//     appearance carrying density 0. It is MaterialTypeFromString now, and
//     the patterns below accept a `Class::` qualifier and decl-specifiers so
//     a member parser cannot hide from them the way it did.
//
// A source-scanning test is a blunt instrument, so it is deliberately narrow:
// one clearly-named family (`*FromString`), two mechanical properties. It
// catches the NEXT parser someone adds, which no amount of fixing the current
// ones can do — provided it is NAMED into the family, which is the one way
// FromSlug escaped. A new string-to-enum parser goes in as `*FromString`.

#include "core/cam/CamNames.h"

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct SourceFile {
  fs::path path;
  std::string text;
};

std::vector<SourceFile> SourcesUnder(const std::string& subdir,
                                     const std::string& ext) {
  std::vector<SourceFile> out;
  const fs::path root = fs::path(DESIGNER_SOURCE_DIR) / subdir;
  REQUIRE(fs::exists(root));
  for (const auto& e : fs::recursive_directory_iterator(root)) {
    if (!e.is_regular_file() || e.path().extension() != ext)
      continue;
    if (e.path().string().find("thirdparty") != std::string::npos)
      continue;
    std::ifstream f(e.path());
    std::stringstream ss;
    ss << f.rdbuf();
    out.push_back({e.path(), ss.str()});
  }
  REQUIRE(!out.empty());
  return out;
}

std::string Relative(const fs::path& p) {
  return fs::relative(p, fs::path(DESIGNER_SOURCE_DIR)).string();
}

} // namespace

TEST_CASE("every bool *FromString parser is [[nodiscard]]", "[convention]") {
  // A parser that reports failure through its return value is useless if the
  // caller can drop it silently, which is exactly what SheetFromJson and
  // StudyFromJson did. [[nodiscard]] moves that from "spot it in review" to
  // "the compiler says so".
  const std::regex decl(
      R"((\[\[nodiscard\]\]\s*)?(?:static\s+|inline\s+|virtual\s+)*bool\s+([\w:]*FromString)\s*\()");
  std::vector<std::string> unmarked;
  for (const SourceFile& f : SourcesUnder("src", ".h")) {
    for (std::sregex_iterator it(f.text.begin(), f.text.end(), decl), end;
         it != end; ++it) {
      if ((*it)[1].matched)
        continue;
      unmarked.push_back(Relative(f.path) + ": " + (*it)[2].str());
    }
  }
  INFO("mark these [[nodiscard]] — a dropped result is a silent wrong value:\n"
       << [&] {
            std::string s;
            for (const std::string& u : unmarked)
              s += "  " + u + "\n";
            return s;
          }());
  CHECK(unmarked.empty());
}

TEST_CASE("an enum-returning *FromString parser has no silent fallback",
          "[convention]") {
  // The other shape of the same bug. MateTypeFromString returned MateType
  // directly and ended `return MateType::Fastened; // default / "fastened"`,
  // so ANY unrecognised string became a rigid joint — there was no bool for a
  // caller to check even if it wanted to. A parser that cannot report failure
  // must therefore throw.
  // std::multiline, or `^` only anchors at the start of the whole FILE — the
  // first version of this test silently matched nothing and passed against
  // the very bug it was written for. Sabotage said so.
  const std::regex def(
      R"(^([A-Za-z_][\w:]*)\s+([\w:]*FromString)\s*\(([^)]*)\)\s*\{)",
      std::regex_constants::ECMAScript | std::regex_constants::multiline);
  std::vector<std::string> silent;
  for (const SourceFile& f : SourcesUnder("src", ".cpp")) {
    for (std::sregex_iterator it(f.text.begin(), f.text.end(), def), end;
         it != end; ++it) {
      const std::string ret = (*it)[1].str();
      if (ret == "bool" || ret == "std::string" || ret == "const")
        continue; // bool reports failure; string parsers are not enums
      const size_t bodyStart = static_cast<size_t>(it->position()) +
                               static_cast<size_t>(it->length());
      const size_t bodyEnd = f.text.find("\n}\n", bodyStart);
      const std::string body = f.text.substr(
          bodyStart, bodyEnd == std::string::npos ? std::string::npos
                                                  : bodyEnd - bodyStart);
      if (body.find("throw ") != std::string::npos)
        continue;
      silent.push_back(Relative(f.path) + ": " + ret + " " + (*it)[2].str());
    }
  }
  INFO("these return an enum with no way to report an unrecognised string, "
       "and do not throw:\n"
       << [&] {
            std::string s;
            for (const std::string& u : silent)
              s += "  " + u + "\n";
            return s;
          }());
  CHECK(silent.empty());
}

TEST_CASE("no numeric field in src/ui bypasses ui::ExprInput", "[convention]") {
  // A raw ImGui::DragFloat/InputInt/... takes a number and CANNOT take an
  // expression — and worse, ImGui's own Ctrl+click text entry runs a bare
  // sscanf on whatever is typed. "10/2" reads as 10 and commits, silently:
  // the same plausible-wrong-value shape as the enum parsers above, in the
  // GUI instead of a parser. Thirty-two fields were like this until U14
  // (docs/ui/u14-numeric-input.md), CAM's entire panel among them.
  //
  // ui::ExprInput's wrappers are drop-ins with the same signatures, so the
  // fix for anything this catches is to add the Expr prefix and pass state.
  const std::regex raw(
      R"(ImGui::(Drag|Input)(Float|Double|Int|Scalar)[234N]?\s*\()");
  std::vector<std::string> offenders;
  for (const std::string& ext : {".cpp", ".h"}) {
    for (const SourceFile& f : SourcesUnder("src/ui", ext)) {
      // ExprInput.cpp is where the wrappers call ImGui; that is the point.
      if (f.path.filename() == "ExprInput.cpp")
        continue;
      std::istringstream lines(f.text);
      std::string line;
      int n = 0;
      while (std::getline(lines, line)) {
        n++;
        if (line.find("//") != std::string::npos &&
            line.find("//") < line.find("ImGui::"))
          continue; // a comment showing the old spelling
        if (std::regex_search(line, raw))
          offenders.push_back(Relative(f.path) + ":" + std::to_string(n));
      }
    }
  }
  INFO("use ui::ExprInput's wrapper instead — a raw one accepts \"10/2\" as "
       "10:\n"
       << [&] {
            std::string s;
            for (const std::string& o : offenders)
              s += "  " + o + "\n";
            return s;
          }());
  CHECK(offenders.empty());
}

TEST_CASE("no bare degree/radian conversion outside core/Angles.h",
          "[convention]") {
  // The same shape as the world up-axis before core/Conventions.h: a constant
  // every subsystem retyped, so no two agreed and nothing could tell. There
  // were FOUR definitions of the deg/rad factor (Expression.cpp,
  // SerializeFeatures.cpp, RemoteAssembly.cpp, RemoteMotion.cpp) and eleven
  // more sites spelling `180.0 / M_PI` inline.
  //
  // That matters more here than tidiness, because the boundary those literals
  // sit on is where every angle bug this project shipped actually lived —
  // set_mate's 60 radians, the arc's `end_angle: 90`, pattern's per-step
  // radians documented as a total sweep in degrees. A conversion routed
  // through angles::ToRadians/ToDegrees says which direction it goes at the
  // call site; `* 180.0 / M_PI` does not, and reads the same either way.
  //
  // glm::radians/glm::degrees are FINE and deliberately not matched: they
  // name their direction too. This catches the hand-rolled spelling only.
  const std::regex bare(R"((180(\.0)?f?\s*[*/]\s*(M_PI|3\.14[0-9]*))|)"
                        R"(((M_PI|3\.14[0-9]*)\s*[*/]\s*180(\.0)?f?)|)"
                        R"((57\.29577951)|(0\.01745329))");
  std::vector<std::string> offenders;
  for (const std::string& ext : {".cpp", ".h"}) {
    for (const SourceFile& f : SourcesUnder("src", ext)) {
      // Where the factor is DEFINED; that is the point.
      if (f.path.filename() == "Angles.h")
        continue;
      std::istringstream lines(f.text);
      std::string line;
      int n = 0;
      while (std::getline(lines, line)) {
        n++;
        const size_t slashes = line.find("//");
        if (slashes != std::string::npos && slashes < 4)
          continue; // a comment quoting the old spelling
        if (std::regex_search(line, bare))
          offenders.push_back(Relative(f.path) + ":" + std::to_string(n));
      }
    }
  }
  INFO("use angles::ToRadians / angles::ToDegrees (core/Angles.h) — a bare "
       "factor does not say which way it converts:\n"
       << [&] {
            std::string s;
            for (const std::string& o : offenders)
              s += "  " + o + "\n";
            return s;
          }());
  CHECK(offenders.empty());
}

TEST_CASE("the RPC's op-type list matches the table", "[convention]") {
  // `add_cam_op`'s row spells the op types out for the agent reading it --
  // "profile|pocket|face|drill|adaptive|edge_round" -- because a row that
  // says "one of the types below" helps nobody. A MethodDoc row is a compile
  // -time literal, so it cannot call OpTypeNameList(); this is what keeps the
  // two from drifting. Adding a type without touching the row leaves the
  // agent-facing documentation quietly wrong about what it may ask for.
  const fs::path path =
      fs::path(DESIGNER_SOURCE_DIR) / "src/remote/RemoteCam.cpp";
  std::ifstream f(path);
  REQUIRE(f.good());
  std::stringstream ss;
  ss << f.rdbuf();
  std::string src = ss.str();

  // Join adjacent string literals before searching. The row is one logical
  // string that clang-format wraps across source lines, so a grep of the raw
  // text would fail on the line break rather than on the content -- which is
  // the gate crying wolf about its own formatting.
  src = std::regex_replace(src, std::regex("\"[ \t\r\n]*\""), "");

  std::string expected;
  for (const cam::OpTypeRow& row : cam::OpTypes())
    expected += (expected.empty() ? "" : "|") + std::string(row.name);
  INFO("expected the row to spell: " << expected);
  CHECK(src.find(expected) != std::string::npos);
}

TEST_CASE("the adaptive key covers every field the pass reads",
          "[convention]") {
  // docs/ui/long-operations.md, phase 1: an adaptive pass runs on a worker
  // from a COPY of its inputs and its result is cached by a hash of them.
  // The one drift that design is exposed to is a new `setup.`/`op.`/`tool.`
  // read inside CamAdaptive.cpp that the key does not know about — the
  // cache would then answer a changed pass with the old moves, which is a
  // wrong toolpath that looks right. Invisible in review, so checked here:
  // every such field read anywhere in the file appears inside AdaptiveKey.
  const fs::path path =
      fs::path(DESIGNER_SOURCE_DIR) / "src/core/cam/CamAdaptive.cpp";
  std::ifstream f(path);
  REQUIRE(f.good());
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string text = ss.str();

  const size_t keyAt = text.find("uint64_t AdaptiveKey(");
  REQUIRE(keyAt != std::string::npos);
  const size_t keyEnd = text.find("\n}\n", keyAt);
  REQUIRE(keyEnd != std::string::npos);
  const std::string key = text.substr(keyAt, keyEnd - keyAt);

  const std::regex read(R"(\b(setup|op|tool)\.([A-Za-z_][A-Za-z0-9_]*)\b)");
  std::set<std::string> everywhere, inKey;
  for (auto it = std::sregex_iterator(text.begin(), text.end(), read);
       it != std::sregex_iterator(); ++it)
    everywhere.insert((*it)[1].str() + "." + (*it)[2].str());
  for (auto it = std::sregex_iterator(key.begin(), key.end(), read);
       it != std::sregex_iterator(); ++it)
    inKey.insert((*it)[1].str() + "." + (*it)[2].str());
  REQUIRE(inKey.size() >= 7); // the seven scalars the pass reads today

  std::vector<std::string> missing;
  for (const std::string& field : everywhere)
    if (!inKey.count(field))
      missing.push_back(field);
  INFO("CamAdaptive.cpp reads these and AdaptiveKey does not hash them:\n"
       << [&] {
            std::string s;
            for (const std::string& m : missing)
              s += "  " + m + "\n";
            return s;
          }());
  CHECK(missing.empty());
}

TEST_CASE("the surface key covers every field the pass reads", "[convention]") {
  // The twin of the adaptive gate above, for phase 5. A surfacing operation
  // now runs on a worker from a COPY of its inputs and its result is cached
  // by a hash of them, so the same drift applies: a `setup.`/`op.`/`tool.`
  // read inside the surfacing strategies that `SurfaceKey` does not know
  // about means the cache answers a changed operation with the old moves.
  //
  // Read against the SURFACING FUNCTIONS only (CamSurfaceStrategies.cpp) --
  // hashing the fields a profile reads would be a key that invalidates on
  // things surfacing never looks at.
  const fs::path strategies =
      fs::path(DESIGNER_SOURCE_DIR) / "src/core/cam/CamSurfaceStrategies.cpp";
  const fs::path surface =
      fs::path(DESIGNER_SOURCE_DIR) / "src/core/cam/CamSurface.cpp";
  const auto slurp = [](const fs::path& p) {
    std::ifstream f(p);
    REQUIRE(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  };
  const std::string text = slurp(strategies);
  const std::string keyText = slurp(surface);

  const size_t keyAt = keyText.find("uint64_t SurfaceKey(");
  REQUIRE(keyAt != std::string::npos);
  const size_t keyEnd = keyText.find("\n}\n", keyAt);
  REQUIRE(keyEnd != std::string::npos);
  const std::string key = keyText.substr(keyAt, keyEnd - keyAt);

  // The two generators, from their signatures to the next one.
  std::string bodies;
  for (const char* fn : {"void GenerateSurface(const Setup& setup",
                         "void GenerateRotarySurface("}) {
    const size_t at = text.find(fn);
    REQUIRE(at != std::string::npos);
    const size_t end = text.find("\n}\n", at);
    REQUIRE(end != std::string::npos);
    bodies += text.substr(at, end - at);
  }

  const std::regex read(R"(\b(setup|op|tool)\.([A-Za-z_][A-Za-z0-9_]*)\b)");
  std::set<std::string> everywhere, inKey;
  for (auto it = std::sregex_iterator(bodies.begin(), bodies.end(), read);
       it != std::sregex_iterator(); ++it)
    everywhere.insert((*it)[1].str() + "." + (*it)[2].str());
  for (auto it = std::sregex_iterator(key.begin(), key.end(), read);
       it != std::sregex_iterator(); ++it)
    inKey.insert((*it)[1].str() + "." + (*it)[2].str());
  // The key hashes the op-field table whole (`OpFieldValues(op)`), so every
  // member the table names counts as hashed -- read from the table's own
  // source, the way this gate reads everything else.
  if (key.find("OpFieldValues(op)") != std::string::npos) {
    const std::string table =
        slurp(fs::path(DESIGNER_SOURCE_DIR) / "src/core/cam/CamOpFields.cpp");
    const std::regex member(R"(&Operation::([A-Za-z_][A-Za-z0-9_]*))");
    size_t named = 0;
    for (auto it = std::sregex_iterator(table.begin(), table.end(), member);
         it != std::sregex_iterator(); ++it, ++named)
      inKey.insert("op." + (*it)[1].str());
    REQUIRE(named >= 15); // or the table moved and this reads nothing
  }
  REQUIRE(inKey.size() >= 10);

  std::vector<std::string> missing;
  for (const std::string& field : everywhere)
    if (!inKey.count(field))
      missing.push_back(field);
  INFO("the surfacing generators read these and SurfaceKey does not hash "
       "them:\n"
       << [&] {
            std::string s;
            for (const std::string& m : missing)
              s += "  " + m + "\n";
            return s;
          }());
  CHECK(missing.empty());
}

TEST_CASE("a background job is polled by the loop, never by a panel",
          "[convention]") {
  // Whether a long-running operation can FINISH must not depend on which
  // panel is open. It used to, twice (docs/ui/long-operations.md):
  //
  //   PollSync was called inside `if (ImGui::TreeNode("Sharing"))`, so
  //   collapsing that section during a push meant nothing ever joined the
  //   worker — the operation finished and the app never found out.
  //
  //   PollTopologyRun/PollStudyRun were called from DrawTopoResults, drawn
  //   only in Simulation mode, so leaving that mode stranded a finished solve.
  //   An RPC client still reaped it, which is why it survived every scripted
  //   test and only bit a person clicking.
  //
  // Both are the same mistake and it is invisible in review — a poll call
  // looks equally at home in a draw function. So it is checked here rather
  // than remembered: the joins live in ui/BackgroundJobs.cpp, which the three
  // loops call once each, and nothing under src/ui may call them directly.
  //
  // The REMOTE layer is deliberately exempt: a handler must apply a finished
  // result before it answers a question about it, so it polls on the way in.
  // (PollSync, PollTopologyRun, PollStudyRun and PollImport left the list
  // 2026-09-02: those jobs moved onto the registry — A3 phases 2-4 — and
  // have no join of their own to misplace. The registry's own reap is
  // ReapBackgroundJobs, which the REMOTE layer may also call.)
  const std::vector<std::string> joins = {"PumpRenderJob",
                                          "ReapBackgroundJobs"};
  std::vector<std::string> offenders;
  for (const SourceFile& f : SourcesUnder("src/ui", ".cpp")) {
    if (f.path.filename() == "BackgroundJobs.cpp")
      continue; // the one place they belong
    for (const std::string& join : joins) {
      // The definition itself is not a call: skip the file that defines it.
      const std::string definition = "void " + join + "(";
      if (f.text.find(definition) != std::string::npos)
        continue;
      if (f.text.find(join + "(") != std::string::npos)
        offenders.push_back(Relative(f.path) + " calls " + join);
    }
  }
  INFO("Poll these from ui::PollBackgroundJobs, which every loop already "
       "calls, not from a draw function that may not run:\n"
       << [&] {
            std::string s;
            for (const std::string& o : offenders)
              s += "  " + o + "\n";
            return s;
          }());
  CHECK(offenders.empty());
}

TEST_CASE("every tool-session sentinel is in the registry", "[convention]") {
  // A pick slot for a non-feature tool used to answer two questions in two
  // files — routing in SlotPickClicked, liveness in ViewportInput's per-frame
  // chain — with nothing joining them. Missing from the SECOND, a session is
  // disarmed the same frame it is armed: no error, no log, no assertion, the
  // tool just ignores every click. That is what the fastener session did, and
  // diagnosing it cost a round because the symptom names nothing.
  //
  // One table answers both now. This gate is the thing that keeps a fifth
  // session from being half-registered — the same move, and the same reason,
  // as the scene-renderer registry below.
  const auto read = [](const char* rel) {
    std::ifstream f(fs::path(DESIGNER_SOURCE_DIR) / rel);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  };
  const std::string header = read("src/ui/FeatureEditor.h");
  const std::string impl = read("src/ui/FeatureEditor.cpp");
  REQUIRE(!header.empty());
  REQUIRE(!impl.empty());

  // Every sentinel the header declares...
  std::vector<std::string> sentinels;
  for (size_t at = header.find("inline constexpr uint64_t k");
       at != std::string::npos;
       at = header.find("inline constexpr uint64_t k", at + 1)) {
    const size_t nameAt = at + std::strlen("inline constexpr uint64_t ");
    const size_t nameEnd = header.find(' ', nameAt);
    if (nameEnd == std::string::npos)
      continue;
    const std::string name = header.substr(nameAt, nameEnd - nameAt);
    if (name.size() > 4 && name.compare(name.size() - 4, 4, "Slot") == 0)
      sentinels.push_back(name);
  }
  INFO("sentinels found: " << sentinels.size());
  REQUIRE(sentinels.size() >= 4); // mate, section, cam, fastener

  // ...appears in the one table that routes AND validates it.
  const size_t at = impl.find("const ToolSession kToolSessions[]");
  REQUIRE(at != std::string::npos);
  const size_t end = impl.find("\n};", at);
  REQUIRE(end != std::string::npos);
  const std::string table = impl.substr(at, end - at);
  for (const std::string& name : sentinels) {
    INFO(name << " is declared in FeatureEditor.h but missing from "
                 "kToolSessions — its tool will ignore every click, silently");
    CHECK(table.find(name) != std::string::npos);
  }

  // And nothing hand-rolls the chain the table replaced.
  const std::string viewport = read("src/ui/ViewportInput.cpp");
  REQUIRE(!viewport.empty());
  INFO("ViewportInput should ask ToolSessionFor, not compare sentinels itself");
  CHECK(viewport.find("ToolSessionFor(") != std::string::npos);
  for (const std::string& name : sentinels) {
    INFO(name << " is compared directly in ViewportInput.cpp — that is the "
                 "second chain this registry exists to remove");
    CHECK(viewport.find("== " + name) == std::string::npos);
  }
}

TEST_CASE("every scene renderer is in the draw-order registry",
          "[convention]") {
  // One list is left after the wiring refactor — `RegisterSceneRenderers` in
  // main.cpp, which is the draw order — and everything per-frame loops over
  // it. A renderer that implements the frame contract and is not in it draws
  // nothing at all, in every mode, which is at least loud. Being missing from
  // the OLD list was worse and is what this gate was written for: it drew
  // full-window while the camera aspect was the sub-region between the
  // panels, and shipped twice.
  //
  //   - 0346a8e, the FEM field: every field silently offset.
  //   - 4a3083d, the CAM stock: a workpiece not parallel to the part.
  //
  // Neither was visible in review, because an `--offscreen` instance has no
  // panels and there the sub-region IS the whole framebuffer.
  //
  // The discriminator is deriving from SceneRenderer — the frame contract
  // itself. (It used to match `void SetViewport(int32_t`, which stopped
  // working the moment the interface existed: SceneRenderer.h is a
  // `*Renderer.h` declaring exactly that signature, so the gate demanded the
  // abstract base be registered. Asking who IMPLEMENTS the contract cannot
  // make that mistake.) The prepass renderers — shadow, SSAO, ray tracing —
  // do not implement it and are correctly absent.
  const std::string main = [] {
    std::ifstream f(fs::path(DESIGNER_SOURCE_DIR) / "src/app/main.cpp");
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }();
  const size_t at = main.find("void RegisterSceneRenderers()");
  REQUIRE(at != std::string::npos);
  const size_t end = main.find("\n}", at);
  REQUIRE(end != std::string::npos);
  const std::string body = main.substr(at, end - at);

  const size_t tat = main.find("void ShutdownSceneRenderers()");
  REQUIRE(tat != std::string::npos);
  const size_t tend = main.find("\n}", tat);
  REQUIRE(tend != std::string::npos);
  const std::string teardown = main.substr(tat, tend - tat);

  int checked = 0;
  for (const SourceFile& f : SourcesUnder("src/render", ".h")) {
    const std::string name = f.path.stem().string(); // e.g. StockRenderer
    if (name.size() < 9 || name.substr(name.size() - 8) != "Renderer")
      continue;
    if (f.text.find(": public SceneRenderer") == std::string::npos)
      continue; // a prepass renderer, or the interface itself
    INFO(Relative(f.path)
         << " implements the frame contract but RegisterSceneRenderers() "
            "never adds it — it will draw nothing. Add g_"
         << name << " in draw order.");
    CHECK(body.find("g_" + name) != std::string::npos);
    // And the teardown side, which is the OTHER list this bug class has come
    // out of: the underlay was missing from it and destructed at process exit
    // on a freed device (a222bc8, "the long-standing shutdown crash").
    INFO(Relative(f.path)
         << " is never torn down — ShutdownSceneRenderers() must reset g_"
         << name << " while the device is still alive.");
    CHECK(teardown.find("g_" + name) != std::string::npos);
    checked++;
  }
  // If the discriminator ever stops matching anything, this test would pass
  // by doing nothing at all.
  CHECK(checked >= 6);
}

// --- The binary you are testing is the source you are looking at -----------

TEST_CASE("conventions: this binary is not older than the source",
          "[conventions]") {
  // Twice in one round a commit from a PARALLEL SESSION changed C++ under a
  // build tree, and the suite failed in a way that read as the round's own
  // work: first the frozen CAM golden, then three version/golden gates. Both
  // times the answer was `ninja`, and both times it cost a diagnosis detour
  // to get there — a stale binary produces failures that are indistinguishable
  // from real regressions, because for the source in front of you they ARE
  // wrong answers.
  //
  // Rebuilds here are ccache-warm, so a false positive costs seconds and a
  // false negative costs what it cost twice.
  const fs::path exe = fs::path(DESIGNER_TEST_BINARY);
  if (!fs::exists(exe))
    return; // installed or relocated: nothing to compare against
  const auto built = fs::last_write_time(exe);
  fs::path newest;
  fs::file_time_type newestAt{};
  for (const char* dir : {"src", "tests"}) {
    const fs::path root = fs::path(DESIGNER_SOURCE_DIR) / dir;
    if (!fs::exists(root))
      continue;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
      if (!e.is_regular_file())
        continue;
      const std::string ext = e.path().extension().string();
      if (ext != ".cpp" && ext != ".h" && ext != ".hpp")
        continue;
      // These three link into the APP binary only, never into this one, so a
      // change to them leaves this binary complete. Worse than a false
      // alarm: the rebuild the message asks for cannot clear it, because
      // `ninja designer designer_tests` has no work to do for designer_tests
      // — the gate would then keep failing while telling you to do the thing
      // you just did.
      const std::string rel = Relative(e.path());
      if (rel == "src/app/main.cpp" || rel == "src/app/HeadlessMain.cpp" ||
          rel == "src/app/CrashHandler.cpp")
        continue;
      const auto at = fs::last_write_time(e.path());
      if (newest.empty() || at > newestAt) {
        newest = e.path();
        newestAt = at;
      }
    }
  }
  if (newest.empty())
    return;
  INFO(Relative(newest)
       << " is newer than the test binary — you are running a build that "
          "does not contain it, and any failure below may be its absence "
          "rather than a regression. Rebuild: nix-shell shell.nix --run "
          "\"cd build && ninja designer designer_tests\"");
  CHECK(newestAt <= built);
}

// Feature-to-feature references (the sketch-edit rollback, U-round): a feature
// that names another one by stable id must report it from VisitFeatureRefs, or
// the dependency graph has an edge missing. The compiler forces the OVERRIDE
// to exist (pure virtual, like Clone) but cannot check that it mentions every
// id-bearing field, and the failure is silent — a sketch edit simply leaves
// the solid built from that sketch sitting in front of the sketch, looking
// like the rollback does not work rather than like a reference was dropped.
//
// Narrow on purpose: only src/core/features/*.h, only members whose TYPE is
// one of the three that carry a feature id. The four primitives in Feature.h
// (Box/Sphere/Cylinder/Import) are built from numbers and a file path and
// have no such member for this to check.
TEST_CASE("every feature reference is reported by VisitFeatureRefs",
          "[convention]") {
  // Fields that hold an id naming something OUTSIDE this feature list, which
  // VisitFeatureRefs must NOT report. Each needs a reason, not just a name.
  const std::set<std::string> kNotThisHistory = {
      "sourcePart",      // Derived: a part SLOT, not a feature
      "bodies",          // Derived: created_by ids in the SOURCE document
      "sourceCreatedBy", // Derived: ditto, per snapshot body
  };

  std::vector<std::string> problems;
  for (const auto& f : SourcesUnder("src/core/features", ".h")) {
    const size_t at = f.text.find("VisitFeatureRefs");
    if (at == std::string::npos) {
      problems.push_back(Relative(f.path) + ": no VisitFeatureRefs");
      continue;
    }
    // The override's body, brace-matched from its opening '{'. An empty
    // one-liner ("... override {}") is the common case here and is exactly
    // what a line-based scan gets wrong: it runs on to the next '}' in the
    // file and swallows the member declarations it is supposed to check.
    const size_t open = f.text.find('{', at);
    REQUIRE(open != std::string::npos);
    size_t close = open;
    for (int depth = 0; close < f.text.size(); close++) {
      if (f.text[close] == '{')
        depth++;
      else if (f.text[close] == '}' && --depth == 0)
        break;
    }
    std::string body = f.text.substr(open, close - open);
    // Strip // comments: a field NAMED in the prose explaining why it is not
    // reported must not read as reported (DerivedFeature says exactly that
    // about sourcePart), and the reverse would let a comment satisfy the rule.
    body = std::regex_replace(body, std::regex(R"(//[^\n]*)"), "");

    // A MEMBER of one of the id-bearing types: the name is followed by '=',
    // ';' or '[', which a constructor parameter (followed by ',' or ')')
    // never is.
    const std::regex member(
        R"(^[ \t]+(?:std::vector<)?(?:uint64_t|ElementRef|AxisRef)>?[ \t]+)"
        R"((\w+)[ \t]*(?:=|;|\[))",
        std::regex::multiline);
    for (auto it = std::sregex_iterator(f.text.begin(), f.text.end(), member);
         it != std::sregex_iterator(); ++it) {
      const std::string name = (*it)[1];
      if (name.rfind("m_", 0) == 0)
        continue; // private state, not a reference
      if (kNotThisHistory.count(name) != 0) {
        if (body.find(name) != std::string::npos)
          problems.push_back(Relative(f.path) + ": VisitFeatureRefs reports '" +
                             name + "', which names another document");
        continue;
      }
      if (body.find(name) == std::string::npos)
        problems.push_back(Relative(f.path) + ": '" + name +
                           "' is not reported by VisitFeatureRefs");
    }
  }

  INFO([&] {
    std::string s;
    for (const std::string& p : problems)
      s += "\n  " + p;
    return s;
  }());
  CHECK(problems.empty());
}

TEST_CASE("CADShape replaces its shape only through SetShape", "[convention]") {
  // GetBoundingBox memoizes on the shape, so whatever replaces `m_shape` has
  // to forget the boxes. SetShape is that one place. A bare `m_shape = `
  // added later would keep a box describing the shape it USED to be — and
  // that number reaches CAM as a stock size, which is the wrong place to
  // discover it.
  const fs::path src = fs::path(DESIGNER_SOURCE_DIR) / "src/cad/CADKernel.cpp";
  REQUIRE(fs::exists(src));
  std::ifstream f(src);
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string text = ss.str();

  // The definition of SetShape itself is the one allowed assignment; count
  // by line so the message can point at the offender.
  std::vector<int> offenders;
  int line = 0;
  bool inSetShape = false;
  std::istringstream lines(text);
  for (std::string l; std::getline(lines, l);) {
    line++;
    if (l.find("void CADShape::SetShape(") != std::string::npos)
      inSetShape = true;
    else if (inSetShape && l.rfind("}", 0) == 0)
      inSetShape = false;
    if (l.find("m_shape =") == std::string::npos || inSetShape)
      continue;
    offenders.push_back(line);
  }
  INFO("CADKernel.cpp assigns m_shape outside SetShape at line(s):\n"
       << [&] {
            std::string s;
            for (int n : offenders)
              s += "  " + std::to_string(n) + "\n";
            return s;
          }());
  CHECK(offenders.empty());
}

TEST_CASE("every verdict RPC attaches a receipt", "[convention][receipt]") {
  // A verdict RPC is one that answers "is this all right?" — and there are
  // twelve of them. Each used to report a clean result and a result it could
  // not compute in the same shape, so a caller had to know which flag to read
  // before believing the headline: `truncated` beside `clash: false`, `stale`
  // beside a factor of safety, `valid` over zero bodies.
  //
  // A receipt makes the standing unmissable (core/Receipt.h). The list below
  // is the surface, and it is deliberately explicit: the failure this catches
  // is a THIRTEENTH verdict RPC arriving with no receipt and nobody noticing,
  // which no amount of fixing the current twelve prevents. Add the row when
  // you add the RPC — `pending` is a to-do the build reminds you of, not a
  // way to opt out.
  struct Verdict {
    const char* method;
    const char* file;
    bool wired;
  };
  const std::vector<Verdict> kVerdictRpcs = {
      {"interference", "src/remote/RemoteInspection.cpp", true},
      {"clash", "src/remote/RemoteInspection.cpp", true},
      {"check_geometry", "src/remote/RemoteInspection.cpp", true},
      {"study_results", "src/remote/RemoteSimulation.cpp", true},
      {"contact_report", "src/remote/RemoteInspection.cpp", true},
      {"check_fasteners", "src/remote/RemoteAssembly.cpp", true},
      {"check_drawing", "src/remote/RemoteDrawings.cpp", true},
      {"check_layout", "src/remote/RemoteCam.cpp", true},
      {"cam_verify", "src/remote/RemoteCam.cpp", true},
      // cam_simulate was on this list by its NAME. It is not a verdict RPC:
      // it plays the posted file to a cursor and reports what has come off by
      // that point — a scrubber, not a check, and its own doc row says so.
      // The verdict over the same simulation is cam_verify's, and that one
      // carries the receipt. A receipt manufactured here would be a claim
      // nobody made.
      {"compare_geometry", "src/remote/RemoteInspection.cpp", true},
      {"run_motion_study", "src/remote/RemoteMotion.cpp", true},
      // Arrived with the tolerances round, and this list is why it did not
      // arrive unnoticed. Its own doc row had already reached the rule —
      // "anything not fully answerable is `unverified` WITH the reason and
      // never a pass" — in its own words; the receipt makes it the common
      // ones.
      {"check_fits", "src/remote/RemoteInspection.cpp", true},
  };

  std::set<std::string> attachesReceipt;
  for (const SourceFile& f : SourcesUnder("src/remote", ".cpp"))
    if (f.text.find("AttachReceipt") != std::string::npos)
      attachesReceipt.insert(Relative(f.path));

  std::vector<std::string> missing;
  for (const Verdict& v : kVerdictRpcs) {
    if (!v.wired)
      continue;
    if (!attachesReceipt.count(v.file))
      missing.push_back(std::string(v.method) + " (" + v.file + ")");
  }
  INFO("these are marked wired but their module attaches no receipt:\n"
       << [&] {
            std::string s;
            for (const std::string& m : missing)
              s += "  " + m + "\n";
            return s;
          }());
  CHECK(missing.empty());

  // The roll-up is only honest if `Unverifiable` is actually reachable. A
  // receipt surface where nothing is ever unverifiable has re-invented a
  // boolean with more words.
  bool anyUnverifiable = false;
  for (const SourceFile& f : SourcesUnder("src/remote", ".cpp"))
    if (f.text.find(".Unverifiable(") != std::string::npos)
      anyUnverifiable = true;
  CHECK(anyUnverifiable);
}

// Every boolean in core/Measure.cpp goes through BuildQuery.
//
// A measurement is a QUESTION, and OpenCASCADE's default answers it by
// EDITING the thing being measured — a boolean writes the p-curves it
// computes onto its arguments' edges. Two sweep threads asking about two
// pairs that share a body then write to the same edge at once, which cost
// about one run in five of tests/cncest6090_smoke.py as a heap corruption
// with no visible connection to any of this (see Measure.cpp's note above
// BuildQuery, and docs/performance/query-booleans.md).
//
// The check is deliberately shallow: it does not try to see whether
// SetNonDestructive was called, only that no boolean is built the other way.
// The two-shape constructor `BRepAlgoAPI_Cut cut(a, b)` builds IMMEDIATELY,
// so it is exactly the form that cannot be corrected afterwards — and the
// form someone reaches for, because it is the short one. Default-construct
// and hand it to BuildQuery.
TEST_CASE("every boolean in Measure.cpp is built through BuildQuery",
          "[convention]") {
  const fs::path path =
      fs::path(DESIGNER_SOURCE_DIR) / "src" / "core" / "Measure.cpp";
  REQUIRE(fs::exists(path));
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string text = ss.str();

  // `BRepAlgoAPI_Something name(` — the constructor-with-arguments form.
  // A bare `BRepAlgoAPI_Something name;` has no parenthesis and passes.
  const std::regex ctor(
      R"(BRepAlgoAPI_[A-Za-z]+\s+[A-Za-z_][A-Za-z0-9_]*\s*\()");
  std::vector<std::string> offenders;
  size_t line = 1;
  for (size_t i = 0, start = 0; i <= text.size(); i++) {
    if (i != text.size() && text[i] != '\n')
      continue;
    const std::string row = text.substr(start, i - start);
    // Comment lines are not code, and this rule has to be explainable in a
    // comment that names the form it forbids.
    const size_t firstNonSpace = row.find_first_not_of(" \t");
    const bool isComment = firstNonSpace != std::string::npos &&
                           row.compare(firstNonSpace, 2, "//") == 0;
    if (!isComment && std::regex_search(row, ctor))
      offenders.push_back(std::to_string(line) + ": " + row);
    start = i + 1;
    line++;
  }
  INFO("src/core/Measure.cpp builds a boolean without BuildQuery:\n" + [&] {
    std::string s;
    for (const std::string& o : offenders)
      s += "  " + o + "\n";
    return s;
  }());
  CHECK(offenders.empty());

  // ...and BuildQuery is still the thing that makes them safe. Without this
  // the rule above could be satisfied by a helper that forgot the flag.
  CHECK(text.find("op.SetNonDestructive(Standard_True);") != std::string::npos);
}

TEST_CASE("nothing in src/ is named `near` or `far`", "[convention]") {
  // `windows.h` defines both as EMPTY macros — `#define far` / `#define near`,
  // unconditionally, in minwindef.h — and OpenCASCADE's headers pull it in on
  // Windows. A local called `far` therefore compiles everywhere we test and
  // fails only on the one platform nothing in CI builds: `double far = -1.0;`
  // expands to `double = -1.0;`.
  //
  // Found by hand on 2026-09-14 in Measure.cpp, MeshCollide.cpp,
  // FastenerActions.cpp and two GUI-test files — every one written long after
  // the Windows port proved it worked, because nothing since had cross-built.
  // Undefining the macros centrally does not survive the next `#include
  // <windows.h>`, so the rule is the name.
  std::vector<std::string> offenders;
  for (const std::string& ext : {".cpp", ".h"})
    for (const SourceFile& src : SourcesUnder("src", ext)) {
      size_t line = 1;
      for (size_t i = 0, start = 0; i <= src.text.size(); i++) {
        if (i != src.text.size() && src.text[i] != '\n')
          continue;
        std::string row = src.text.substr(start, i - start);
        start = i + 1;
        const size_t n = line++;
        // Prose says "near" and "far" constantly — this rule is about code,
        // so drop line comments and string literals before looking.
        if (const size_t c = row.find("//"); c != std::string::npos)
          row.erase(c);
        row = std::regex_replace(row, std::regex(R"("(\\.|[^"\\])*")"), "\"\"");
        // An identifier, not a word: `near` alone, never `nearest` or
        // `Mesh::Near`. `.near`/`->near` count — a member named `near` breaks
        // at its uses just the same.
        static const std::regex ident(R"((^|[^A-Za-z0-9_:])(near|far)\b)");
        if (std::regex_search(row, ident))
          offenders.push_back(Relative(src.path) + ":" + std::to_string(n) +
                              ": " + row);
      }
    }
  INFO("`near`/`far` are windows.h macros; rename these:\n" + [&] {
    std::string s;
    for (const std::string& o : offenders)
      s += "  " + o + "\n";
    return s;
  }());
  CHECK(offenders.empty());
}

TEST_CASE("a toolbar glyph means one thing and a feature has one glyph",
          "[convention]") {
  // docs/ui/consistency-round-plan.md. The toolbars had grown five glyphs
  // that meant two different things each (Unfold and Fit, Loft and the arcs,
  // Thread and the sketch Circle, Derive and Offset, "more constraints" and
  // "pattern callout") and more beside, and the feature tree drew some
  // features with a different glyph from the button that creates them
  // (Revolve, Hole, Draft, Split). A glyph is how a person finds a tool
  // without reading, so neither kind of drift is cosmetic.
  //
  // Both now come from ONE table, ui/Icons.cpp (docs/ui/icons-plan.md): the
  // tree answers through CommandIcon, so it cannot disagree with the button,
  // and what is left to check is the table itself.
  auto read = [](const char* rel) {
    std::ifstream f(fs::path(DESIGNER_SOURCE_DIR) / rel);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  };
  const std::string table = read("src/ui/Icons.cpp");
  REQUIRE(!table.empty());
  std::map<std::string, std::set<std::string>> commandsByGlyph;
  std::set<std::string> commands;
  const std::regex entry(
      R"re(\{"([a-z0-9_]+\.[a-z0-9_]+)",\s*(ICON_(?:FA|DS)_[A-Z0-9_]+)\})re");
  for (std::sregex_iterator it(table.begin(), table.end(), entry), end;
       it != end; ++it) {
    commandsByGlyph[(*it)[2]].insert((*it)[1]);
    commands.insert((*it)[1]);
  }
  REQUIRE(commands.size() > 100); // the parse found the table, not a handful

  // Commands that share a glyph because they are the SAME idea on different
  // bars. Anything else sharing one is a clash.
  const std::vector<std::set<std::string>> sameIdea = {
      {"view.section", "sketch.toggle_slice"}, // a cut at a plane
      {"part.simulation", "assembly.simulation"},
      {"part.extrude", "sketch.extrude"},
      {"part.revolve", "sketch.revolve"},
      {"part.hole", "sketch.hole", "drawing.dim_hole_callout", "job.drill"},
      {"part.mirror", "sketch.mirror"},
      {"part.pattern", "sketch.pattern", "drawing.dim_pattern_callout"},
      {"part.fillet", "sketch.fillet", "job.edge"},
      {"view.toggle_planes", "sketch.toggle_glyphs"}, // show or hide
      {"assembly.insert", "drawing.add_view"},        // put a thing in
      {"sim.run", "job.simulate"},                    // run it
  };
  const auto groupOf = [&](const std::string& id) -> int {
    for (size_t g = 0; g < sameIdea.size(); g++)
      if (sameIdea[g].count(id))
        return static_cast<int>(g);
    return -1;
  };
  std::vector<std::string> clashes;
  for (const auto& [glyph, ids] : commandsByGlyph) {
    std::set<std::string> ideas; // a group index, or the command itself
    for (const std::string& id : ids) {
      const int g = groupOf(id);
      ideas.insert(g >= 0 ? "group" + std::to_string(g) : id);
    }
    if (ideas.size() > 1) {
      std::string line = glyph + ":";
      for (const std::string& id : ids)
        line += " " + id;
      clashes.push_back(line);
    }
  }
  INFO("One glyph, different tools. Pick a distinct glyph, or -- if they "
       "really are one idea on two bars -- add them to a group above:\n"
       << [&] {
            std::string s;
            for (const std::string& c : clashes)
              s += "  " + c + "\n";
            return s;
          }());
  CHECK(clashes.empty());

  // Every command a panel asks the table for is IN it: a misspelt one would
  // quietly show the unknown-command glyph on its tree row.
  std::vector<std::string> unknown;
  int asked = 0;
  const std::regex ask(
      R"re((?:CommandIcon\(|\{"[A-Za-z0-9]+", )"([a-z]+\.[a-z0-9_]+)")re");
  for (const char* rel :
       {"src/ui/FeatureTreePanel.cpp", "src/ui/OccurrencePanel.cpp",
        "src/ui/CamPanel.cpp", "src/ui/SketchToolbar.cpp"}) {
    const std::string text = read(rel);
    for (std::sregex_iterator it(text.begin(), text.end(), ask), end; it != end;
         ++it) {
      asked++;
      if (!commands.count((*it)[1]))
        unknown.push_back(std::string(rel) + ": " + std::string((*it)[1]));
    }
  }
  CHECK(asked > 40); // the tree's table, six mates, nine op types
  INFO("Not in src/ui/Icons.cpp:\n"
       << [&] {
            std::string s;
            for (const std::string& u : unknown)
              s += "  " + u + "\n";
            return s;
          }());
  CHECK(unknown.empty());
}

TEST_CASE("a command's icon comes from ui/Icons.cpp alone", "[convention]") {
  // Four hand-kept copies of "which glyph is this" had drifted: the mate list
  // disagreed with the assembly bar on four of six mates, and OpGlyph claimed
  // the job bar's glyphs while showing others (docs/ui/icons-plan.md). The
  // glyph is now CommandIcon's, and nothing else may name one for a command:
  // not a toolbar button, a flyout variant, or the three tree-row tables.
  const auto read = [](const char* rel) {
    std::ifstream f(fs::path(DESIGNER_SOURCE_DIR) / rel);
    std::stringstream ss;
    ss << f.rdbuf();
    REQUIRE(!ss.str().empty());
    return ss.str();
  };
  std::vector<std::string> named;

  // A glyph literal as the first argument of a command button — it still
  // compiles, since "###tb_*" concatenates onto it, and would show two
  // glyphs — or as a flyout variant's first field.
  const std::string toolbar = read("src/ui/Toolbar.cpp");
  const std::regex button(
      R"(\b(CommandButton|btn|mateBtn|bcBtn|dimBtn|conBtn|toolBtn)\(\s*(state,\s*)?ICON_(FA|DS)_\w+)");
  const std::regex variant(R"(\{\{?\s*ICON_(FA|DS)_\w+)");
  for (const std::regex* re : {&button, &variant})
    for (std::sregex_iterator it(toolbar.begin(), toolbar.end(), *re), end;
         it != end; ++it)
      named.push_back("src/ui/Toolbar.cpp: " + it->str());

  // The tree-row tables: each must answer through CommandIcon.
  for (const auto& [rel, fn] :
       {std::pair{"src/ui/FeatureTreePanel.cpp", "const char* FeatureIcon("},
        std::pair{"src/ui/OccurrencePanel.cpp", "const char* MateGlyph("},
        std::pair{"src/ui/CamPanel.cpp", "const char* OpGlyph("}}) {
    const std::string text = read(rel);
    const size_t at = text.find(fn);
    REQUIRE(at != std::string::npos); // renamed? then follow it here
    const std::string body = text.substr(at, text.find("\n}\n", at) - at);
    for (const char* prefix : {"ICON_FA_", "ICON_DS_"})
      if (body.find(prefix) != std::string::npos)
        named.push_back(std::string(rel) + ": " + fn + " names a glyph");
  }
  INFO([&] {
    std::string s = "Name the glyph in src/ui/Icons.cpp instead:\n";
    for (const std::string& n : named)
      s += "  " + n + "\n";
    return s;
  }());
  CHECK(named.empty());
}
