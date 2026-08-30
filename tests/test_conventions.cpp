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

#include <filesystem>
#include <fstream>
#include <regex>
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

TEST_CASE("every scene renderer is aimed at the 3D viewport", "[convention]") {
  // The 3D view is a sub-region between the docked panels, and UI.cpp repoints
  // the renderers at it every frame. A renderer left out of that list keeps a
  // FULL-WINDOW viewport while the camera aspect is the sub-region, so its
  // geometry lands at a different scale and offset than everything drawn
  // around it — and nothing crashes, so it ships.
  //
  // It has now happened twice, and the second time was mine:
  //
  //   - 0346a8e, the FEM field: "the FieldRenderer was never added to that
  //     list... The Shape iso-surface was unaffected because it draws through
  //     the MeshRenderer, which is in the list."
  //   - 4a3083d, the CAM stock: reported as a workpiece not parallel to the
  //     part with its cuts shifted sideways, while the cutter — an instance of
  //     the mesh renderer — sat exactly right.
  //
  // Neither could be seen in review: an `--offscreen` instance has no panels,
  // so there the sub-region IS the whole framebuffer.
  //
  // The discriminator is `void SetViewport(int32_t`, which takes a viewport
  // RECT. The prepass renderers (shadow, SSAO, ray tracing) do not have it and
  // are correctly not in the list.
  const std::string ui = [] {
    std::ifstream f(fs::path(DESIGNER_SOURCE_DIR) / "src/ui/UI.cpp");
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }();
  REQUIRE(ui.find("auto setVp") != std::string::npos);
  // The list wraps across lines, so compare with the whitespace removed.
  std::string flat;
  flat.reserve(ui.size());
  for (const char c : ui)
    if (!std::isspace(static_cast<unsigned char>(c)))
      flat.push_back(c);

  int checked = 0;
  for (const SourceFile& f : SourcesUnder("src/render", ".h")) {
    const std::string name = f.path.stem().string(); // e.g. StockRenderer
    if (name.size() < 9 || name.substr(name.size() - 8) != "Renderer")
      continue;
    if (f.text.find("void SetViewport(int32_t") == std::string::npos)
      continue; // a prepass renderer: no scene viewport of its own
    // AppState names them camelCase: StockRenderer -> stockRenderer.
    std::string member = name;
    member[0] = static_cast<char>(std::tolower(member[0]));
    INFO(Relative(f.path)
         << " takes a viewport rect but UI.cpp never aims it at the 3D "
            "viewport — add setVp(state."
         << member << ") beside the others, or it will draw full-window.");
    CHECK(flat.find("setVp(state." + member + ")") != std::string::npos);
    checked++;
  }
  // If the discriminator ever stops matching anything, this test would pass
  // by doing nothing at all.
  CHECK(checked >= 6);
}
