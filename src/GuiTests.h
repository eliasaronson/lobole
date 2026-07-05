#pragma once

struct ImGuiTestEngine;
class Scene;
class Document;

// Registers all GUI tests. Call once after the application objects exist.
void RegisterGuiTests(ImGuiTestEngine* engine, Scene* scene,
                      Document* document);
