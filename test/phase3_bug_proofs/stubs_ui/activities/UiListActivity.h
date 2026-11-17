#pragma once
#include <FreeInkUICore.h>
struct RenderLock {};
class UiScreen {};
class GfxRenderer;
class MappedInputManager;
class Activity {
 public:
  Activity(const char*, GfxRenderer&, MappedInputManager&) {}
  virtual ~Activity() = default;
  virtual void onEnter() {}
  virtual void onExit() {}
  virtual void render(RenderLock&&) {}
  virtual bool handleHomeGesture() { return false; }
  virtual bool acceptsRemoteInput() const { return false; }
};
class UiListActivity : public Activity {
 public:
  UiListActivity(const char* name, GfxRenderer& r, MappedInputManager& m) : Activity(name, r, m) {}
  virtual int listCount() const = 0;
  virtual void buildScreen(UiScreen&) = 0;
  virtual void activateIndex(int) = 0;
  virtual bool handleCustomInput() { return false; }
  virtual bool handleButtons() { return false; }
  virtual void drawChrome() {}
  bool routingReady() const { return true; }
};
