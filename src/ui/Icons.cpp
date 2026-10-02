#include "ui/Icons.h"

#include "IconsDesigner.h"
#include "IconsFontAwesome6.h"

#include <array>

namespace ui {

namespace {

// Grouped by the toolbar the command sits on. Two commands on ONE toolbar
// must not share a glyph — ui/toolbar_icons_distinct fails if they do. The
// same command on several toolbars (fit, measure, section) is one entry.
constexpr std::array kIcons = std::to_array<CommandIconEntry>({
    // View verbs, on several bars.
    {"view.fit", ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER},
    {"view.toggle_planes", ICON_FA_EYE},
    {"view.section", ICON_DS_SECTION},
    {"view.measure", ICON_FA_RULER},
    {"view.render", ICON_FA_CAMERA},
    {"file.import", ICON_FA_FILE_IMPORT},

    // Part.
    {"part.new_sketch", ICON_FA_PENCIL},
    {"part.box", ICON_FA_CUBE},
    {"part.sphere", ICON_DS_SPHERE},
    {"part.cylinder", ICON_DS_CYLINDER},
    {"part.derive", ICON_FA_LINK},
    {"part.template", ICON_FA_STAMP},
    {"part.extrude", ICON_DS_EXTRUDE},
    {"part.revolve", ICON_DS_REVOLVE},
    {"part.curve3d", ICON_DS_HELIX},
    {"part.sweep", ICON_DS_SWEEP},
    {"part.loft", ICON_DS_LOFT},
    {"part.pipe", ICON_DS_PIPE},
    {"part.datum_plane", ICON_DS_DATUM_PLANE},
    {"part.datum_axis", ICON_DS_DATUM_AXIS},
    {"part.frame", ICON_FA_LOCATION_CROSSHAIRS},
    {"part.boolean", ICON_DS_BOOLEAN},
    {"part.mirror", ICON_DS_MIRROR},
    {"part.pattern", ICON_FA_GRIP},
    {"part.fillet", ICON_DS_FILLET},
    {"part.chamfer", ICON_DS_CHAMFER},
    {"part.rib", ICON_DS_RIB},
    {"part.hole", ICON_FA_BULLSEYE},
    {"part.thread", ICON_DS_THREAD},
    {"part.worm", ICON_FA_BARS_STAGGERED},
    {"part.worm_wheel", ICON_FA_DHARMACHAKRA},
    {"part.shell", ICON_DS_SHELL},
    {"part.draft", ICON_DS_DRAFT},
    {"part.split", ICON_DS_SPLIT},
    {"part.refine", ICON_FA_BROOM},
    {"part.sheet_base_flange", ICON_DS_BASE_FLANGE},
    {"part.sheet_edge_flange", ICON_DS_EDGE_FLANGE},
    {"part.sheet_hem", ICON_DS_HEM},
    {"part.sheet_jog", ICON_DS_JOG},
    {"part.sheet_corner_relief", ICON_DS_CORNER_RELIEF},
    {"part.sheet_normal_cut", ICON_DS_NORMAL_CUT},
    {"part.sheet_flat_pattern", ICON_DS_FLAT_PATTERN},
    {"part.sheet_unfold", ICON_DS_UNFOLD},
    {"part.sheet_refold", ICON_DS_REFOLD},
    {"part.simulation", ICON_FA_GAUGE_HIGH},

    // Sketch.
    {"sketch.line", ICON_FA_SLASH},
    {"sketch.circle", ICON_DS_CIRCLE},
    {"sketch.ellipse", ICON_DS_ELLIPSE_TOOL},
    {"sketch.rectangle", ICON_DS_RECTANGLE},
    {"sketch.rectangle_center", ICON_DS_CENTRE_RECTANGLE},
    {"sketch.arc_3point", ICON_DS_ARC_3POINT},
    {"sketch.arc_center", ICON_DS_ARC_CENTRE},
    {"sketch.ellipse_arc", ICON_DS_ELLIPSE_ARC},
    {"sketch.spline", ICON_FA_BEZIER_CURVE},
    {"sketch.point", ICON_FA_CIRCLE_DOT},
    {"sketch.text", ICON_FA_FONT},
    {"sketch.trim", ICON_FA_SCISSORS},
    {"sketch.dimension", ICON_FA_RULER_COMBINED},
    {"sketch.mirror", ICON_DS_MIRROR},
    {"sketch.fillet", ICON_DS_FILLET},
    {"sketch.pattern", ICON_FA_GRIP},
    {"sketch.offset", ICON_DS_OFFSET},
    {"sketch.project", ICON_DS_PROJECT},
    {"sketch.constraint_horizontal", ICON_DS_HORIZONTAL},
    {"sketch.constraint_vertical", ICON_DS_VERTICAL},
    {"sketch.constraint_coincident", ICON_DS_COINCIDENT},
    {"sketch.constraint_parallel", ICON_DS_PARALLEL},
    {"sketch.constraint_perpendicular", ICON_DS_PERPENDICULAR},
    {"sketch.toggle_construction", ICON_DS_CONSTRUCTION},
    {"sketch.toggle_autoconstrain", ICON_FA_MAGNET},
    {"sketch.toggle_glyphs", ICON_FA_EYE},
    // Slicing the part at the sketch plane IS a section, at that plane.
    {"sketch.toggle_slice", ICON_DS_SECTION},
    {"sketch.image", ICON_FA_IMAGE},
    {"sketch.extrude", ICON_DS_EXTRUDE},
    {"sketch.revolve", ICON_DS_REVOLVE},
    {"sketch.hole", ICON_FA_BULLSEYE},

    // Assembly.
    {"assembly.insert", ICON_FA_CIRCLE_PLUS},
    {"assembly.mate_fastened", ICON_DS_MATE_FASTENED},
    {"assembly.mate_revolute", ICON_DS_MATE_REVOLUTE},
    {"assembly.mate_slider", ICON_DS_MATE_SLIDER},
    {"assembly.mate_cylindrical", ICON_DS_MATE_CYLINDRICAL},
    {"assembly.mate_planar", ICON_DS_MATE_PLANAR},
    {"assembly.mate_ball", ICON_DS_MATE_BALL},
    {"assembly.fastener", ICON_FA_SCREWDRIVER},
    {"assembly.bearing", ICON_FA_LIFE_RING},
    {"assembly.explode", ICON_FA_EXPLOSION},
    {"assembly.motion", ICON_FA_CIRCLE_PLAY},
    {"assembly.simulation", ICON_FA_GAUGE_HIGH},
    {"assembly.bom", ICON_FA_TABLE_LIST},

    // Simulation workspace.
    {"sim.fixed", ICON_FA_LOCK},
    {"sim.force", ICON_FA_BOLT},
    {"sim.pressure", ICON_FA_DOWN_LONG},
    {"sim.run", ICON_FA_PLAY},

    // Drawing.
    {"drawing.add_view", ICON_FA_CIRCLE_PLUS},
    {"drawing.update", ICON_FA_ARROWS_ROTATE},
    {"drawing.dim_distance", ICON_DS_DIM_ALIGNED},
    {"drawing.dim_distance_x", ICON_DS_DIM_X},
    {"drawing.dim_distance_y", ICON_DS_DIM_Y},
    {"drawing.dim_radius", ICON_DS_DIM_RADIUS},
    {"drawing.dim_diameter", ICON_DS_DIM_DIAMETER},
    {"drawing.dim_angle", ICON_DS_DIM_ANGLE},
    {"drawing.dim_hole_callout", ICON_FA_BULLSEYE},
    {"drawing.dim_pattern_callout", ICON_FA_GRIP},
    {"drawing.dim_part_note", ICON_FA_TAG},
    {"drawing.fcf", ICON_FA_TABLE_CELLS},
    {"drawing.datum", ICON_FA_FLAG},
    {"drawing.detail", ICON_FA_MAGNIFYING_GLASS_PLUS},
    {"drawing.balloons", ICON_FA_CIRCLE_NODES},
    {"drawing.export", ICON_FA_FILE_EXPORT},

    // CAM job.
    {"job.top_face", ICON_FA_CROSSHAIRS},
    {"job.lay_face_up", ICON_FA_HAND},
    {"job.index_face", ICON_FA_ROTATE_RIGHT},
    {"job.look_down_spindle", ICON_FA_VIDEO},
    {"job.pocket", ICON_DS_CAM_POCKET},
    {"job.profile", ICON_DS_CAM_PROFILE},
    {"job.drill", ICON_FA_BULLSEYE},
    // Breaking an edge IS a fillet, cut rather than modelled.
    {"job.edge", ICON_DS_FILLET},
    // A pen's nib: the V-bit draws the sketch's lines into the part.
    {"job.engrave", ICON_FA_PEN_NIB},
    {"job.whole", ICON_FA_BORDER_ALL},
    {"job.face", ICON_DS_CAM_FACE},
    {"job.surface_rough", ICON_DS_CAM_SURFACE_ROUGH},
    {"job.surface", ICON_DS_CAM_SURFACE},
    {"job.walls", ICON_DS_CAM_WALLS},
    {"job.rotary_rough", ICON_DS_CAM_ROTARY_ROUGH},
    {"job.rotary_finish", ICON_DS_CAM_ROTARY_FINISH},
    {"job.rest", ICON_FA_WAND_MAGIC_SPARKLES},
    {"job.generate", ICON_FA_GEARS},
    {"job.simulate", ICON_FA_PLAY},
});

} // namespace

const char* const kUnknownIcon = ICON_FA_SHAPES;

std::span<const CommandIconEntry> CommandIcons() { return kIcons; }

const char* CommandIcon(std::string_view command) {
  for (const CommandIconEntry& e : kIcons)
    if (command == e.command)
      return e.icon;
  return kUnknownIcon;
}

} // namespace ui
