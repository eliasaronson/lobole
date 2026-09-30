#include "core/io/Keymap.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>

using nlohmann::json;

namespace keymap {

namespace {

struct KeyName {
  Key key;
  const char* name;
};

// Text <-> Key. The letters and digits are generated (they are contiguous in
// the enum); everything else is spelled out. Punctuation is written as the
// character a person types, so a menu hint reads "Ctrl+," exactly as it did
// when it was a literal.
const KeyName kNamed[] = {
    {Key::Space, "Space"},
    {Key::Tab, "Tab"},
    {Key::Comma, ","},
    {Key::Period, "."},
    {Key::Slash, "/"},
    {Key::Semicolon, ";"},
    {Key::Apostrophe, "'"},
    {Key::Minus, "-"},
    {Key::Equal, "="},
    {Key::LeftBracket, "["},
    {Key::RightBracket, "]"},
    {Key::Backslash, "\\"},
    {Key::Grave, "`"},
    {Key::Home, "Home"},
    {Key::End, "End"},
    {Key::PageUp, "PageUp"},
    {Key::PageDown, "PageDown"},
    {Key::Insert, "Insert"},
    {Key::F1, "F1"},
    {Key::F2, "F2"},
    {Key::F3, "F3"},
    {Key::F4, "F4"},
    {Key::F5, "F5"},
    {Key::F6, "F6"},
    {Key::F7, "F7"},
    {Key::F8, "F8"},
    {Key::F9, "F9"},
    {Key::F10, "F10"},
    {Key::F11, "F11"},
    {Key::F12, "F12"},
};

bool IEqual(std::string_view a, std::string_view b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) ==
                  std::tolower(static_cast<unsigned char>(y));
         });
}

std::string_view Trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
    s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
    s.remove_suffix(1);
  return s;
}

Key KeyFromToken(std::string_view t) {
  if (t.size() == 1) {
    const char c = t[0];
    if (c >= 'a' && c <= 'z')
      return static_cast<Key>(static_cast<uint16_t>(Key::A) + (c - 'a'));
    if (c >= 'A' && c <= 'Z')
      return static_cast<Key>(static_cast<uint16_t>(Key::A) + (c - 'A'));
    if (c >= '0' && c <= '9')
      return static_cast<Key>(static_cast<uint16_t>(Key::Num0) + (c - '0'));
  }
  for (const KeyName& k : kNamed)
    if (IEqual(t, k.name))
      return k.key;
  return Key::None;
}

std::string KeyToText(Key k) {
  const auto v = static_cast<uint16_t>(k);
  if (k >= Key::A && k <= Key::Z)
    return std::string(
        1, static_cast<char>('A' + (v - static_cast<uint16_t>(Key::A))));
  if (k >= Key::Num0 && k <= Key::Num9)
    return std::string(
        1, static_cast<char>('0' + (v - static_cast<uint16_t>(Key::Num0))));
  for (const KeyName& n : kNamed)
    if (n.key == k)
      return n.name;
  return {};
}

// The whole app's bindings. Adding a command is ONE row here plus one row in
// ui/Commands.cpp (a GUI test asserts the pair). Ordering inside a context is
// the order Preferences and the shortcuts window list them in.
//
// Reading the defaults: bare letters go to what you reach for constantly,
// Shift+letter to the second tier, and the numbers to families that are
// naturally numbered (mate types, boundary conditions). A context binding
// deliberately shadows a global one — `N` is Coincident in a sketch and
// align-to-face everywhere else, `Shift+F` is Fillet in a sketch and
// zoom-to-selection everywhere else.
const Binding kDefaults[] = {
    // ---------------------------------------------------------------- Global
    {"file.new", Context::Global, "New Project", "File", "Ctrl+N"},
    {"file.open", Context::Global, "Open Project", "File", "Ctrl+O"},
    {"file.save", Context::Global, "Save Project", "File", "Ctrl+S"},
    {"file.save_as", Context::Global, "Save Project As", "File",
     "Ctrl+Shift+S"},
    {"file.import", Context::Global, "Import STEP / STL / BREP", "File",
     "Ctrl+I"},
    {"file.export", Context::Global, "Export (STEP / STL / BREP)", "File", ""},
    // Ctrl+Shift+Z rides along as a fixed alternate: this app shipped both,
    // and dropping either would break somebody's hands.
    {"edit.undo", Context::Global, "Undo", "Edit", "Ctrl+Z"},
    {"edit.redo", Context::Global, "Redo", "Edit", "Ctrl+Y", "Ctrl+Shift+Z"},
    {"edit.preferences", Context::Global, "Preferences", "Edit", "Ctrl+,"},
    {"palette.open", Context::Global, "Command palette", "Edit", "Ctrl+K"},
    {"help.shortcuts", Context::Global, "Keyboard shortcuts", "Help", "Ctrl+/"},

    {"view.fit", Context::Global, "Fit view", "View", "F"},
    {"view.zoom_selection", Context::Global, "Zoom to selection", "View",
     "Shift+F"},
    {"view.align_to_face", Context::Global,
     "Look straight at the selected face, or the section cut under the "
     "cursor",
     "View", "N"},
    {"view.toggle_planes", Context::Global, "Show / hide the base planes",
     "View", "Shift+P"},
    {"view.front", Context::Global, "Front view", "View", "Shift+1"},
    {"view.back", Context::Global, "Back view", "View", "Shift+2"},
    {"view.left", Context::Global, "Left view", "View", "Shift+3"},
    {"view.right", Context::Global, "Right view", "View", "Shift+4"},
    {"view.top", Context::Global, "Top view", "View", "Shift+5"},
    {"view.bottom", Context::Global, "Bottom view", "View", "Shift+6"},
    {"view.iso", Context::Global, "Isometric view", "View", "Shift+7"},
    {"view.reset_layout", Context::Global, "Reset the panel layout", "View",
     ""},
    {"view.controls_help", Context::Global, "Viewport controls", "View", ""},
    // Ctrl+Shift+letter for the global tools, so a context's bare/Shift
    // bindings can never shadow them.
    {"view.measure", Context::Global, "Measure", "Tools", "Ctrl+Shift+M"},
    {"view.section", Context::Global, "Section view", "Tools", "Ctrl+Shift+X"},
    {"view.render", Context::Global, "Render a still (quick or path traced)",
     "Tools", "Ctrl+Shift+R"},
    // Deliberately undocumented (PerfHud.h): kept out of Preferences and the
    // shortcuts window, but it is a binding like any other.
    {"view.perf_hud", Context::Global, "Frame timing HUD", "Tools",
     "Ctrl+Shift+F", "", true},

    // ------------------------------------------------------------------ Part
    {"part.new_sketch", Context::Part, "New Sketch", "Create", "S"},
    {"part.extrude", Context::Part, "Extrude a sketch", "Create", "E"},
    {"part.revolve", Context::Part, "Revolve a sketch", "Create", "R"},
    {"part.sweep", Context::Part, "Sweep a profile along a path", "Create",
     "W"},
    {"part.loft", Context::Part, "Loft through sections", "Create", "L"},
    {"part.pipe", Context::Part, "Pipe / tube along a path", "Create", "I"},
    {"part.curve3d", Context::Part, "3D Curve (helix / fit-spline)", "Create",
     "V"},
    {"part.box", Context::Part, "Box", "Create", "Shift+X"},
    {"part.sphere", Context::Part, "Sphere", "Create", "Shift+S"},
    {"part.cylinder", Context::Part, "Cylinder", "Create", "Shift+C"},
    {"part.datum_plane", Context::Part, "Datum Plane", "Create", "Shift+D"},
    {"part.datum_axis", Context::Part, "Datum Axis", "Create", "Shift+A"},
    // Shift+E for framE: F is Standard parts and Shift+F is zoom-to-selection,
    // both older and both global.
    {"part.frame", Context::Part, "Datum Frame", "Create", "Shift+E"},
    // Unbound: a copy of another part is placed once per part, not typed.
    {"part.derive", Context::Part, "Derive (copy of another part)", "Create",
     ""},

    // B for Blend — the term OCC, NX and Creo all use, and the one free
    // mnemonic: F is Fit and Shift+F is zoom-to-selection, both older and
    // both universal in CAD.
    {"part.fillet", Context::Part, "Fillet edges", "Modify", "B"},
    {"part.chamfer", Context::Part, "Chamfer edges", "Modify", "C"},
    {"part.hole", Context::Part, "Hole", "Modify", "H"},
    {"part.thread", Context::Part, "Thread (cosmetic)", "Modify", "T"},
    {"part.worm", Context::Part, "Worm (cut into a cylindrical blank)",
     "Modify", ""},
    {"part.worm_wheel", Context::Part,
     "Worm wheel (hobbed from a cylindrical blank)", "Modify", ""},
    {"part.draft", Context::Part, "Draft (taper faces)", "Modify", "D"},
    {"part.shell", Context::Part, "Shell (hollow a body)", "Modify", "K"},
    {"part.rib", Context::Part, "Rib (thin web from an open sketch)", "Modify",
     "G"},
    {"part.boolean", Context::Part, "Boolean (union / subtract / intersect)",
     "Modify", "U"},
    {"part.mirror", Context::Part, "Mirror a body", "Modify", "M"},
    {"part.pattern", Context::Part, "Pattern (linear / circular)", "Modify",
     "P"},
    {"part.split", Context::Part, "Split a body", "Modify", "X"},
    {"part.refine", Context::Part, "Refine (merge faces / seams)", "Modify",
     "Y"},

    // Sheet metal. Unbound: three tools used in sequence on a part that is
    // sheet metal, not typed mid-flow like a fillet.
    {"part.sheet_base_flange", Context::Part,
     "Base flange (a sheet part from a profile)", "Sheet metal", ""},
    {"part.sheet_edge_flange", Context::Part,
     "Edge flange (bend a wall up from an edge)", "Sheet metal", ""},
    {"part.sheet_flat_pattern", Context::Part, "Flat pattern (the blank)",
     "Sheet metal", ""},
    {"part.sheet_unfold", Context::Part,
     "Unfold (lay bends flat to cut across them)", "Sheet metal", ""},
    {"part.sheet_refold", Context::Part, "Refold (fold an Unfold back up)",
     "Sheet metal", ""},
    {"part.sheet_hem", Context::Part, "Hem (fold an edge back on itself)",
     "Sheet metal", ""},
    {"part.sheet_jog", Context::Part, "Jog (step a panel along a line)",
     "Sheet metal", ""},
    {"part.sheet_corner_relief", Context::Part,
     "Corner relief (where two bends cross)", "Sheet metal", ""},
    {"part.sheet_normal_cut", Context::Part,
     "Normal cut (square to the sheet wherever it lands)", "Sheet metal", ""},

    {"part.simulation", Context::Part, "Simulation (FEM stress study)", "Tools",
     "Q"},

    // ---------------------------------------------------------------- Sketch
    {"sketch.line", Context::Sketch, "Line", "Draw", "L"},
    {"sketch.circle", Context::Sketch, "Circle", "Draw", "C"},
    {"sketch.ellipse", Context::Sketch, "Ellipse", "Draw", "Shift+C"},
    {"sketch.rectangle", Context::Sketch, "Rectangle", "Draw", "G"},
    {"sketch.rectangle_center", Context::Sketch, "Centre Rectangle", "Draw",
     "Shift+G"},
    {"sketch.arc_3point", Context::Sketch, "3-Point Arc", "Draw", "A"},
    {"sketch.arc_center", Context::Sketch, "Centre Arc", "Draw", "Shift+A"},
    {"sketch.ellipse_arc", Context::Sketch, "Elliptic Arc", "Draw",
     "Ctrl+Shift+A"},
    {"sketch.spline", Context::Sketch, "Spline", "Draw", "S"},
    {"sketch.point", Context::Sketch, "Point", "Draw", "."},
    {"sketch.text", Context::Sketch, "Text", "Draw", "Shift+T"},
    {"sketch.project", Context::Sketch, "Project model edges", "Draw", "X"},
    {"sketch.image", Context::Sketch, "Insert reference image", "Draw",
     "Shift+I"},

    {"sketch.trim", Context::Sketch, "Trim", "Modify", "M"},
    {"sketch.offset", Context::Sketch, "Offset", "Modify", "O"},
    {"sketch.fillet", Context::Sketch, "Fillet a corner", "Modify", "Shift+F"},
    {"sketch.mirror", Context::Sketch, "Mirror", "Modify", "Shift+M"},
    {"sketch.pattern", Context::Sketch, "Pattern", "Modify", "P"},
    {"sketch.dimension", Context::Sketch, "Dimension", "Modify", "D"},
    {"sketch.toggle_construction", Context::Sketch,
     "Toggle construction on the selection", "Modify", "Q"},
    {"sketch.clear_selection", Context::Sketch, "Clear the selection", "Modify",
     "Space"},
    {"sketch.copy", Context::Sketch, "Copy", "Modify", "Ctrl+C"},
    {"sketch.paste", Context::Sketch, "Paste", "Modify", "Ctrl+V"},
    {"sketch.paste_linked", Context::Sketch, "Paste linked", "Modify",
     "Ctrl+Shift+V"},

    // F7 is Inventor's Slice Graphics, which is the same gesture.
    {"sketch.toggle_slice", Context::Sketch, "Slice at the sketch plane",
     "Modify", "F7"},

    {"sketch.constraint_horizontal", Context::Sketch, "Horizontal", "Constrain",
     "H"},
    {"sketch.constraint_vertical", Context::Sketch, "Vertical", "Constrain",
     "V"},
    {"sketch.constraint_coincident", Context::Sketch, "Coincident", "Constrain",
     "N"},
    {"sketch.constraint_parallel", Context::Sketch, "Parallel", "Constrain",
     "B"},
    {"sketch.constraint_perpendicular", Context::Sketch, "Perpendicular",
     "Constrain", "Shift+B"},
    {"sketch.constraint_tangent", Context::Sketch, "Tangent", "Constrain", "T"},
    {"sketch.constraint_equal", Context::Sketch, "Equal", "Constrain", "E"},
    {"sketch.constraint_concentric", Context::Sketch, "Concentric", "Constrain",
     "Shift+N"},
    // The "more constraints" flyout: bindable, unbound by default. Inventing
    // a mnemonic for a constraint used once a month is worse than a row in
    // Preferences that says "—" and takes one click to fill in.
    {"sketch.constraint_point_on_line", Context::Sketch, "Point on line",
     "Constrain", ""},
    {"sketch.constraint_symmetric", Context::Sketch, "Symmetric", "Constrain",
     ""},
    {"sketch.constraint_midpoint", Context::Sketch, "Midpoint", "Constrain",
     ""},
    {"sketch.constraint_fixed", Context::Sketch, "Fix / pin", "Constrain", ""},
    {"sketch.constraint_collinear", Context::Sketch, "Collinear", "Constrain",
     ""},

    {"sketch.toggle_autoconstrain", Context::Sketch, "Auto-constrain",
     "Options", "Shift+S"},
    {"sketch.toggle_glyphs", Context::Sketch, "Show constraint glyphs",
     "Options", "Y"},

    {"sketch.extrude", Context::Sketch, "Extrude the sketch", "Finish",
     "Shift+E"},
    {"sketch.revolve", Context::Sketch, "Revolve the sketch", "Finish",
     "Shift+R"},
    {"sketch.hole", Context::Sketch, "Drill holes at the sketch's points",
     "Finish", "Shift+H"},

    // -------------------------------------------------------------- Assembly
    {"assembly.insert", Context::Assembly, "Insert a part or sub-assembly",
     "Assembly", "I"},
    // A numbered family: six mate types with no distinct initials between
    // them (Fastened/Fixed, Slider/Ball...), and the toolbar lists them in
    // this order.
    {"assembly.mate_fastened", Context::Assembly, "Fastened mate (rigid)",
     "Mates", "1"},
    {"assembly.mate_revolute", Context::Assembly, "Revolute mate (hinge)",
     "Mates", "2"},
    {"assembly.mate_slider", Context::Assembly, "Slider mate", "Mates", "3"},
    {"assembly.mate_cylindrical", Context::Assembly,
     "Cylindrical mate (spin + slide)", "Mates", "4"},
    {"assembly.mate_planar", Context::Assembly, "Planar mate", "Mates", "5"},
    {"assembly.mate_ball", Context::Assembly, "Ball mate", "Mates", "6"},
    // Standard parts. "F" for fastener; the mate verbs own 1-6.
    {"assembly.fastener", Context::Assembly, "Fastener (pick a hole's rim)",
     "Standard parts", "F"},
    {"assembly.bearing", Context::Assembly,
     "Bearing (pick the shaft or bore it goes on)", "Standard parts", ""},

    // The two viewport modes (docs/ui/panels.md#assembly-tab-the-model-panel).
    // "X" for the explode, "M" for motion; both open the floating tool window,
    // and each closes the other because both place the occurrences.
    {"assembly.explode", Context::Assembly, "Exploded view", "Tools", "X"},
    {"assembly.motion", Context::Assembly, "Motion study", "Tools", "M"},
    // The bill of materials, a floating window with real columns
    // (docs/ui/panels.md#assembly-tab-the-model-panel).
    {"assembly.bom", Context::Assembly, "Bill of materials", "Tools", "B"},
    // The third floating mode (S1): a study on the assembly, on the part
    // tab's simulation key.
    {"assembly.simulation", Context::Assembly, "Simulation", "Tools", "Q"},

    // Display only — hiding a part does not suppress it, so nothing moves.
    // One key for both directions (it shows what it hid), and Shift+H for
    // the way back when the hidden row is off the bottom of the list.
    {"assembly.hide", Context::Assembly, "Hide / show the selected parts",
     "Display", "H"},
    {"assembly.show_all", Context::Assembly, "Show every hidden part",
     "Display", "Shift+H"},

    // --------------------------------------------------------------- Drawing
    {"drawing.add_view", Context::Drawing, "Add view", "Sheet", "I"},
    {"drawing.update", Context::Drawing, "Update views (re-project)", "Sheet",
     "U"},
    {"drawing.export", Context::Drawing, "Export drawing (SVG / PDF)", "Sheet",
     "E"},
    {"drawing.dim_distance", Context::Drawing, "Distance dimension",
     "Dimensions", "D"},
    {"drawing.dim_distance_x", Context::Drawing, "Horizontal (X) distance",
     "Dimensions", "X"},
    {"drawing.dim_distance_y", Context::Drawing, "Vertical (Y) distance",
     "Dimensions", "Y"},
    {"drawing.dim_radius", Context::Drawing, "Radius dimension", "Dimensions",
     "R"},
    {"drawing.dim_diameter", Context::Drawing, "Diameter dimension",
     "Dimensions", "Shift+R"},
    {"drawing.dim_angle", Context::Drawing, "Angle dimension", "Dimensions",
     "A"},
    {"drawing.dim_hole_callout", Context::Drawing, "Hole callout", "Dimensions",
     "H"},
    {"drawing.dim_pattern_callout", Context::Drawing, "Pattern callout",
     "Dimensions", "P"},
    {"drawing.dim_part_note", Context::Drawing, "Part note", "Dimensions", "L"},

    // ------------------------------------------------------------------- Job
    // A CAM job tab. The verbs are the toolbar (docs/cam/gui.md): each is one
    // sentence a person would say, and each is one click on a part. Letters
    // follow the verb -- Pocket, Cut, Drill, Break -- and Shift for the two
    // that need no pick. Z is zero; T is the view down the spindle (the plan
    // view, "Top" in CAM's frame).
    {"job.top_face", Context::Job, "Pick the top face (Z zero)", "Set up", "Z"},
    {"job.lay_face_up", Context::Job, "Lay a face up (how the part is clamped)",
     "Set up", "L"},
    {"job.index_face", Context::Job, "Turn a face up (rotary table)", "Set up",
     "A"},
    {"job.look_down_spindle", Context::Job, "Look down the spindle", "Set up",
     "T"},
    {"job.pocket", Context::Job, "Clear a pocket", "Machine", "P"},
    {"job.profile", Context::Job, "Cut it out", "Machine", "C"},
    {"job.drill", Context::Job, "Drill a hole", "Machine", "D"},
    {"job.edge", Context::Job, "Break an edge", "Machine", "B"},
    {"job.engrave", Context::Job, "Engrave a label or a curve", "Machine", "E"},
    {"job.whole", Context::Job, "Cut out the whole part", "Machine", "Shift+C"},
    {"job.face", Context::Job, "Face the top", "Machine", "Shift+T"},
    {"job.surface", Context::Job, "Finish a surface", "Machine", "R"},
    {"job.walls", Context::Job, "Finish walls", "Machine", "W"},
    {"job.rotary_finish", Context::Job, "Finish on the rotary", "Machine",
     "Shift+R"},
    {"job.rotary_rough", Context::Job, "Rough on the rotary", "Machine",
     "Shift+O"},
    {"job.surface_rough", Context::Job, "Rough a surface", "Machine",
     "Shift+S"},
    {"job.rest", Context::Job, "Clean up what's left", "Machine", "Shift+E"},
    {"job.generate", Context::Job, "Generate toolpaths and verify", "Output",
     "G"},
    {"job.simulate", Context::Job, "Simulate the cut", "Output", "S"},

    // ------------------------------------------------------------ Simulation
    {"sim.fixed", Context::Simulation, "Fixed support", "Loads", "1"},
    {"sim.force", Context::Simulation, "Force load", "Loads", "2"},
    {"sim.pressure", Context::Simulation, "Pressure load", "Loads", "3"},
    {"sim.run", Context::Simulation, "Run study (mesh + solve)", "Study", "R"},
};

} // namespace

const char* ContextName(Context c) {
  switch (c) {
  case Context::Global:
    return "global";
  case Context::Part:
    return "part";
  case Context::Sketch:
    return "sketch";
  case Context::Assembly:
    return "assembly";
  case Context::Drawing:
    return "drawing";
  case Context::Job:
    return "job";
  case Context::Simulation:
    return "simulation";
  }
  return "global";
}

const char* ContextLabel(Context c) {
  switch (c) {
  case Context::Global:
    return "Everywhere";
  case Context::Part:
    return "Part";
  case Context::Sketch:
    return "Sketch";
  case Context::Assembly:
    return "Assembly";
  case Context::Drawing:
    return "Drawing";
  case Context::Job:
    return "CAM job";
  case Context::Simulation:
    return "Simulation";
  }
  return "Everywhere";
}

std::span<const Context> AllContexts() {
  static constexpr Context kAll[] = {
      Context::Global,  Context::Part, Context::Sketch,    Context::Assembly,
      Context::Drawing, Context::Job,  Context::Simulation};
  return kAll;
}

Chord ParseChord(std::string_view text, std::string* error) {
  const auto fail = [&](const std::string& why) {
    if (error)
      *error = why;
    return Chord{};
  };
  text = Trim(text);
  if (text.empty())
    return {}; // unbound is a state, not an error

  Chord out;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t plus = text.find('+', pos);
    // A trailing "+" is the key itself only if nothing follows it; otherwise
    // it is a separator. "Ctrl++" is not a chord this app needs.
    if (plus == std::string_view::npos)
      plus = text.size();
    const std::string_view token = Trim(text.substr(pos, plus - pos));
    const bool last = plus >= text.size();
    if (token.empty())
      return fail("empty part in '" + std::string(text) + "'");
    if (!last) {
      if (IEqual(token, "Ctrl") || IEqual(token, "Control"))
        out.mods |= ModCtrl;
      else if (IEqual(token, "Shift"))
        out.mods |= ModShift;
      else if (IEqual(token, "Alt"))
        out.mods |= ModAlt;
      else
        return fail("'" + std::string(token) + "' is not Ctrl, Shift or Alt");
    } else {
      out.key = KeyFromToken(token);
      if (out.key == Key::None)
        return fail("'" + std::string(token) + "' is not a key");
    }
    pos = plus + 1;
  }
  return out;
}

std::string FormatChord(Chord c) {
  if (!c)
    return {};
  std::string out;
  if (c.mods & ModCtrl)
    out += "Ctrl+";
  if (c.mods & ModShift)
    out += "Shift+";
  if (c.mods & ModAlt)
    out += "Alt+";
  return out + KeyToText(c.key);
}

std::span<const Binding> Defaults() { return kDefaults; }

const Binding* Find(std::string_view id) {
  for (const Binding& b : kDefaults)
    if (id == b.id)
      return &b;
  return nullptr;
}

Chord Keymap::Get(std::string_view id) const {
  if (auto it = m_overrides.find(std::string(id)); it != m_overrides.end())
    return ParseChord(it->second);
  const Binding* b = Find(id);
  return b ? ParseChord(b->chord) : Chord{};
}

Chord Keymap::Alt(std::string_view id) const {
  const Binding* b = Find(id);
  // A rebound command loses its alternate: the alternate exists to preserve
  // a chord somebody's hands already know, and once they have chosen their
  // own it is just a second thing to collide with.
  if (b == nullptr || !IsDefault(id))
    return {};
  return ParseChord(b->altChord);
}

std::string Keymap::Text(std::string_view id) const {
  return FormatChord(Get(id));
}

bool Keymap::IsDefault(std::string_view id) const {
  return m_overrides.find(std::string(id)) == m_overrides.end();
}

bool Keymap::Set(std::string_view id, std::string_view chordText,
                 std::string* error) {
  std::string why;
  const Chord c = ParseChord(chordText, &why);
  if (!why.empty()) {
    if (error)
      *error = why;
    return false;
  }
  m_overrides[std::string(id)] = FormatChord(c); // normalised, not as typed
  return true;
}

void Keymap::Reset(std::string_view id) { m_overrides.erase(std::string(id)); }

void Keymap::ResetAll() { m_overrides.clear(); }

void Keymap::ResetContext(Context c) {
  for (const Binding& b : kDefaults)
    if (b.ctx == c)
      m_overrides.erase(b.id);
}

void Keymap::SetOverrides(std::map<std::string, std::string> overrides) {
  m_overrides = std::move(overrides);
}

std::vector<std::string> Keymap::Bound(Context ctx, Chord c,
                                       std::string_view except) const {
  std::vector<std::string> out;
  if (!c)
    return out;
  for (const Binding& b : kDefaults) {
    if (b.ctx != ctx || b.id == except)
      continue;
    if (Get(b.id) == c)
      out.push_back(b.id);
  }
  return out;
}

std::vector<Conflict> Keymap::Conflicts() const {
  std::vector<Conflict> out;
  for (size_t i = 0; i < std::size(kDefaults); i++) {
    const Chord ci = Get(kDefaults[i].id);
    if (!ci)
      continue;
    for (size_t j = i + 1; j < std::size(kDefaults); j++) {
      // Same context only: a context binding shadowing a global one is the
      // design, not a collision.
      if (kDefaults[j].ctx != kDefaults[i].ctx)
        continue;
      if (Get(kDefaults[j].id) == ci)
        out.push_back({kDefaults[i].id, kDefaults[j].id, kDefaults[i].ctx, ci});
    }
  }
  return out;
}

json OverridesToJson(const Keymap& k) {
  json j = json::object();
  for (const auto& [id, chord] : k.Overrides())
    j[id] = chord;
  return j;
}

bool ApplyOverridesJson(Keymap& k, const json& j, std::string* error) {
  if (!j.is_object()) {
    if (error)
      *error = "keys must be an object of command -> chord";
    return false;
  }
  // Validate into a copy: a bad chord must not leave half the map applied.
  std::map<std::string, std::string> next;
  for (auto it = j.begin(); it != j.end(); ++it) {
    if (!it.value().is_string()) {
      if (error)
        *error = "keys." + it.key() + ": expected a chord string";
      return false;
    }
    const std::string text = it.value().get<std::string>();
    std::string why;
    const Chord c = ParseChord(text, &why);
    if (!why.empty()) {
      if (error)
        *error = "keys." + it.key() + ": " + why;
      return false;
    }
    // An id this build does not know is KEPT, not dropped: an older build
    // must not delete a newer build's bindings just by running once.
    next[it.key()] = FormatChord(c);
  }
  k.SetOverrides(std::move(next));
  return true;
}

} // namespace keymap
