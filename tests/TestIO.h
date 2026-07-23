#pragma once

// Test-only adapter for the DocumentIO save/load API. Production code uses the
// std::expected<T, std::string> interface directly (the error is then non-
// discardable); tests just need "did it work / give me the document", so these
// thin wrappers unwrap back to the old bool / unique_ptr shape and keep the many
// call sites terse. They still exercise the real DocumentIO functions.

#include "core/io/Serialization.h"

#include <memory>
#include <string>
#include <utility>

namespace testio {

inline std::unique_ptr<Document> Load(const std::string& path) {
  auto r = DocumentIO::Load(path);
  return r ? std::move(*r) : nullptr;
}
inline std::unique_ptr<AssemblyDocument> LoadAssembly(const std::string& path) {
  auto r = DocumentIO::LoadAssembly(path);
  return r ? std::move(*r) : nullptr;
}
inline std::unique_ptr<DrawingDocument> LoadDrawing(const std::string& path) {
  auto r = DocumentIO::LoadDrawing(path);
  return r ? std::move(*r) : nullptr;
}

inline bool Save(const Document& doc, const std::string& path) {
  return DocumentIO::Save(doc, path).has_value();
}
inline bool SaveAssembly(const AssemblyDocument& asmDoc,
                         const std::string& path) {
  return DocumentIO::SaveAssembly(asmDoc, path).has_value();
}
inline bool SaveDrawing(const DrawingDocument& drawing,
                        const std::string& path) {
  return DocumentIO::SaveDrawing(drawing, path).has_value();
}

}  // namespace testio
