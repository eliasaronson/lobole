#pragma once

#include <functional>

struct ImGuiTestEngine;
class Document;

// Registers all GUI tests. The document is resolved through a getter because
// File > New / Open replace the document object mid-session.
void RegisterGuiTests(ImGuiTestEngine* engine,
                      std::function<Document*()> getDocument);
