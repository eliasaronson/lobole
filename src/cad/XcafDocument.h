// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <Standard_Failure.hxx>
#include <TDocStd_Document.hxx>
#include <XCAFApp_Application.hxx>

#include <mutex>

// OpenCASCADE's data exchange is not safe to run on two threads at once, and
// it is not only first use: XCAFApp_Application is ONE process-wide
// application whose session list NewDocument and Close edit with no lock, and
// the STEP controllers fill their Interface_Static tables lazily, marking
// themselves initialised before they are. A project whose parts load side by
// side (docs/ui/long-operations.md#opening-a-project) rebuilds its Import
// features in parallel, and a background open can meet an export on the main
// thread. So every STEP read or write, and every XCAF document, holds this for
// its whole length. Recursive, because a structured export builds its document
// and then writes it.
inline std::recursive_mutex& DataExchangeMutex() {
  static std::recursive_mutex mutex;
  return mutex;
}

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

  // NON-const, deliberately: OpenCASCADE 7.6's
  // STEPCAFControl_Reader::Transfer takes `Handle(TDocStd_Document)&`, so a
  // const-only conversion does not compile there (7.9 relaxed it to const).
  // The guard owns the handle either way; callers only fill it in.
  Handle(TDocStd_Document) & Get() { return m_doc; }
  operator Handle(TDocStd_Document) & () { return m_doc; }
  TDocStd_Document* operator->() const { return m_doc.get(); }

private:
  Handle(TDocStd_Document) m_doc;
};
