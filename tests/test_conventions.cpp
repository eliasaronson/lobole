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
// A source-scanning test is a blunt instrument, so it is deliberately narrow:
// one clearly-named family (`*FromString`), two mechanical properties. It
// catches the NEXT parser someone adds, which no amount of fixing the current
// ones can do.

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
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
      R"((\[\[nodiscard\]\]\s*)?bool\s+(\w*FromString)\s*\()");
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
      R"(^([A-Za-z_][\w:]*)\s+(\w*FromString)\s*\(([^)]*)\)\s*\{)",
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
  // (PollSync left the list 2026-09-02: the sync job moved onto the registry
  // — A3 phase 2 — and has no join of its own to misplace.)
  const std::vector<std::string> joins = {"PollImport", "PollTopologyRun",
                                          "PollStudyRun", "PumpRenderJob"};
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
