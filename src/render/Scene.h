#pragma once

#include <memory>
#include <string>
#include <vector>

class SceneObject;

class Scene {
public:
  Scene();
  ~Scene();

  void Update(float deltaTime);
  void Render();

  void AddObject(std::unique_ptr<SceneObject> object);
  void RemoveObject(const std::string& name);
  SceneObject* GetObject(const std::string& name);

  const std::vector<std::unique_ptr<SceneObject>>& GetObjects() const {
    return m_objects;
  }

private:
  std::vector<std::unique_ptr<SceneObject>> m_objects;
  uint32_t m_nextObjectId;
};

class SceneObject {
public:
  SceneObject(const std::string& name);
  virtual ~SceneObject() = default;

  virtual void Update(float deltaTime) {}
  virtual void Render() {}

  const std::string& GetName() const { return m_name; }
  uint32_t GetId() const { return m_id; }

  void SetVisible(bool visible) { m_visible = visible; }
  bool IsVisible() const { return m_visible; }

protected:
  std::string m_name;
  uint32_t m_id;
  bool m_visible;

private:
  static uint32_t s_nextId;
};