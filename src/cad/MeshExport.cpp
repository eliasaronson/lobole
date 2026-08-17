#include "cad/MeshExport.h"

#include "common/Logger.h"

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Builder.hxx>
#include <Message_ProgressRange.hxx>
#include <Quantity_ColorRGBA.hxx>
#include <RWGltf_CafWriter.hxx>
#include <RWObj_CafWriter.hxx>
#include <Standard_Version.hxx>
// RWPly_CafWriter arrived in OCC 7.7; Ubuntu 24.04 still ships 7.6.3, and
// this include is what broke that build. PLY is the least-used of the three
// visual formats, so it degrades to a clear refusal rather than costing the
// whole distro path (same spirit as the STEP/STL library renaming handled in
// CMakeLists.txt).
#if OCC_VERSION_HEX >= 0x070700
#define DESIGNER_HAVE_PLY_WRITER 1
#include <RWPly_CafWriter.hxx>
#endif
#include <TColStd_IndexedDataMapOfStringString.hxx>
#include <TDF_Label.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Compound.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDoc_VisMaterial.hxx>
#include <XCAFDoc_VisMaterialTool.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <filesystem>
#include <string>

namespace MeshExport {
namespace {

// A rigid glm placement as an OCC transform. glm is column-major;
// gp_Trsf::SetValues takes the 3x4 row-major affine block (and enforces
// rigidity, which occurrence transforms guarantee).
gp_Trsf ToTrsf(const glm::mat4& m) {
  gp_Trsf t;
  t.SetValues(m[0][0], m[1][0], m[2][0], m[3][0], m[0][1], m[1][1], m[2][1],
              m[3][1], m[0][2], m[1][2], m[2][2], m[3][2]);
  return t;
}

} // namespace

bool SaveVisual(const std::string& path, const std::vector<Item>& items,
                double deflection, std::string* error) {
  std::string ext = std::filesystem::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  const bool isGltf = ext == ".glb" || ext == ".gltf";
  if (!isGltf && ext != ".obj" && ext != ".ply") {
    if (error)
      *error = "unsupported visual-export extension: " + ext;
    return false;
  }

  try {
    // The writers export triangulations, so tessellate first. Occurrences of
    // one part share the underlying TShape — the triangulation is computed
    // once and reused (and the writers emit instanced geometry for it).
    std::vector<const Item*> valid;
    for (const Item& item : items) {
      if (!item.shape.IsValid())
        continue;
      BRepMesh_IncrementalMesh mesher(item.shape.GetShape(), deflection);
      valid.push_back(&item);
    }
    if (valid.empty()) {
      if (error)
        *error = "nothing to export";
      return false;
    }

    // The shape→XCAF bridge: one assembly whose components are the placed
    // items, each carrying a name, a display color, and a PBR material.
    Handle(TDocStd_Document) doc;
    XCAFApp_Application::GetApplication()->NewDocument("BinXCAF", doc);
    Handle(XCAFDoc_ShapeTool) shapes =
        XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    Handle(XCAFDoc_ColorTool) colors =
        XCAFDoc_DocumentTool::ColorTool(doc->Main());
    Handle(XCAFDoc_VisMaterialTool) materials =
        XCAFDoc_DocumentTool::VisMaterialTool(doc->Main());

    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    for (const Item* item : valid)
      builder.Add(compound, item->shape.GetShape().Moved(
                                TopLoc_Location(ToTrsf(item->transform))));
    // makeAssembly: the compound becomes an assembly whose components are
    // the placed shapes (shared TShapes stay shared → instancing).
    const TDF_Label root = shapes->AddShape(compound, Standard_True);

    TDF_LabelSequence components;
    shapes->GetComponents(root, components);
    for (Standard_Integer i = 1;
         i <= components.Length() && i <= Standard_Integer(valid.size()); i++) {
      const Item& item = *valid[static_cast<size_t>(i - 1)];
      const TDF_Label component = components.Value(i);
      if (!item.name.empty())
        TDataStd_Name::Set(component,
                           TCollection_ExtendedString(item.name.c_str()));

      const Quantity_Color color(item.baseColor.r, item.baseColor.g,
                                 item.baseColor.b, Quantity_TOC_RGB);
      colors->SetColor(component, color, XCAFDoc_ColorSurf);

      Handle(XCAFDoc_VisMaterial) visMat = new XCAFDoc_VisMaterial();
      XCAFDoc_VisMaterialPBR pbr;
      pbr.IsDefined = Standard_True;
      pbr.BaseColor = Quantity_ColorRGBA(color, 1.0f);
      pbr.Metallic = item.metallic;
      pbr.Roughness = item.roughness;
      visMat->SetPbrMaterial(pbr);
      const TDF_Label matLabel = materials->AddMaterial(
          visMat, TCollection_AsciiString(item.name.c_str()));
      materials->SetShapeMaterial(component, matLabel);
    }

    TColStd_IndexedDataMapOfStringString fileInfo;
    fileInfo.Add("generator", "Designer");
    Message_ProgressRange progress;
    bool ok = false;
    if (isGltf) {
      RWGltf_CafWriter writer(path.c_str(), /*theIsBinary=*/ext == ".glb");
      ok = writer.Perform(doc, fileInfo, progress);
    } else if (ext == ".obj") {
      RWObj_CafWriter writer(path.c_str());
      ok = writer.Perform(doc, fileInfo, progress);
    } else {
#ifdef DESIGNER_HAVE_PLY_WRITER
      RWPly_CafWriter writer(path.c_str());
      ok = writer.Perform(doc, fileInfo, progress);
#else
      if (error) {
        *error = "PLY export needs OpenCASCADE 7.7 or newer; this build has " +
                 std::string(OCC_VERSION_COMPLETE) + ". glTF/GLB and OBJ work.";
      }
      return false;
#endif
    }
    if (!ok && error)
      *error = "the " + ext + " writer failed (see log)";
    if (ok)
      LOG_INFO("Exported {} item(s) to {}", valid.size(), path);
    return ok;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    LOG_ERROR("Visual export failed for {}: {}", path, e.what());
    return false;
  }
}

} // namespace MeshExport
