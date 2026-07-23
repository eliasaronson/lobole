#include "render/Scene.h"
#include <algorithm>

uint32_t SceneObject::s_nextId = 1;

// Scene implementation
Scene::Scene() : m_nextObjectId(1) {
}

Scene::~Scene() {
}

void Scene::Update(float deltaTime) {
    for (auto& object : m_objects) {
        if (object->IsVisible()) {
            object->Update(deltaTime);
        }
    }
}

void Scene::Render() {
    for (auto& object : m_objects) {
        if (object->IsVisible()) {
            object->Render();
        }
    }
}

void Scene::AddObject(std::unique_ptr<SceneObject> object) {
    m_objects.push_back(std::move(object));
}

void Scene::RemoveObject(const std::string& name) {
    m_objects.erase(
        std::remove_if(m_objects.begin(), m_objects.end(),
            [&name](const std::unique_ptr<SceneObject>& obj) {
                return obj->GetName() == name;
            }),
        m_objects.end()
    );
}

SceneObject* Scene::GetObject(const std::string& name) {
    for (auto& object : m_objects) {
        if (object->GetName() == name) {
            return object.get();
        }
    }
    return nullptr;
}

// SceneObject implementation
SceneObject::SceneObject(const std::string& name) 
    : m_name(name), m_id(s_nextId++), m_visible(true) {
}