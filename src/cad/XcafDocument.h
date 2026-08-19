#pragma once

#include <Standard_Failure.hxx>
#include <TDocStd_Document.hxx>
#include <XCAFApp_Application.hxx>

// A scoped XCAF document: NewDocument on construction, Close on destruction.
//
// TDocStd_Application keeps every document NewDocument() hands out in its
// session list, so a document that is never closed lives as long as the
// process — holding its shapes and, after a mesh export, their triangulations.
// LeakSanitizer does not report it (it is still REACHABLE from the
// application singleton), which is exactly why all three of our import/export
// paths had been retaining one per call, unnoticed, since they were written.
//
// Scoped because those paths return early and throw: `Close` at the end of the
// happy path would still have leaked on every failed import.
class XcafDocument {
public:
  XcafDocument() {
    XCAFApp_Application::GetApplication()->NewDocument("BinXCAF", m_doc);
  }
  ~XcafDocument() {
    if (m_doc.IsNull())
      return;
    try {
      XCAFApp_Application::GetApplication()->Close(m_doc);
    } catch (const Standard_Failure&) {
      // A destructor is no place to fail, and a document we cannot close is
      // not worth taking the process down for.
    }
  }

  XcafDocument(const XcafDocument&) = delete;
  XcafDocument& operator=(const XcafDocument&) = delete;

  const Handle(TDocStd_Document) & Get() const { return m_doc; }
  operator const Handle(TDocStd_Document) & () const { return m_doc; }
  TDocStd_Document* operator->() const { return m_doc.get(); }

private:
  Handle(TDocStd_Document) m_doc;
};
