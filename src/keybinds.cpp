
#include "includes.hpp"
#include "ui/record_layer.hpp"
#include "ui/game_ui.hpp"
#include "ui/clickbot_layer.hpp"
#include "ui/macro_editor.hpp"
#include "ui/render_settings_layer.hpp"
#include "hacks/layout_mode.hpp"
#include "hacks/show_trajectory.hpp"

#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/CCTouchDispatcher.hpp>

#include <regex>

const std::vector<std::string> keybindIDs = {
    "open_menu", "toggle_recording", "toggle_playing",
    "toggle_speedhack", "toggle_frame_stepper", "step_frame",
    "toggle_render", "toggle_noclip", "show_trajectory"
};

class $modify(CCKeyboardDispatcher) {
  bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double timestamp) {
  
    auto& g = Global::get();

    int keyInt = static_cast<int>(key);
    if (g.allKeybinds.contains(keyInt) && !isKeyRepeat) {
      for (size_t i = 0; i < 6; i++) {
        if (std::find(g.keybinds[i].begin(), g.keybinds[i].end(), keyInt) != g.keybinds[i].end())
          g.heldButtons[i] = isKeyDown;
      }
    }

    // if (key == enumKeyCodes::KEY_L && !isKeyRepeat && isKeyDown) {
    // }

    // if (key == enumKeyCodes::KEY_F && !isKeyRepeat && isKeyDown && PlayLayer::get()) {
    //   log::debug("POS DEBUG {}", PlayLayer::get()->m_player1->getPosition());
    //   log::debug("POS2 DEBUG {}", PlayLayer::get()->m_player2->getPosition());
    // }


    // if (key == enumKeyCodes::KEY_J && !isKeyRepeat && isKeyDown && PlayLayer::get()) {
    //   std::string str = ZipUtils::decompressString(PlayLayer::get()->m_level->m_levelString.c_str(), true, 0);
    //   log::debug("{}", str);
    // }

    return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat, timestamp);
  }
};

// (port) Geode 5 keybind settings instead of the custom-keybinds mod: `id` is the setting's name
void onKeybind(bool down, std::string const& id) {

  auto& g = Global::get();

  if (!down || (LevelEditorLayer::get() && !g.mod->getSettingValue<bool>("editor_keybinds")) || g.mod->getSettingValue<bool>("disable_keybinds"))
    return;

  if (g.state != state::recording && g.mod->getSettingValue<bool>("recording_only_keybinds"))
    return;

  if (id == "open_menu") {
    if (g.layer) {
      static_cast<RecordLayer*>(g.layer)->onClose(nullptr);
      return;
    }

    RecordLayer::openMenu();
  }

  if (id == "toggle_recording")
    Macro::toggleRecording();

  if (id == "toggle_playing")
    Macro::togglePlaying();

  if (id == "toggle_frame_stepper" && PlayLayer::get())
    Global::toggleFrameStepper();

  if (id == "step_frame")
    Global::frameStep();

  if (id == "toggle_speedhack")
    Global::toggleSpeedhack();

  if (id == "show_trajectory") {
    g.mod->setSavedValue("macro_show_trajectory", !g.mod->getSavedValue<bool>("macro_show_trajectory"));

    if (g.layer) {
      if (static_cast<RecordLayer*>(g.layer)->trajectoryToggle)
        static_cast<RecordLayer*>(g.layer)->trajectoryToggle->toggle(g.mod->getSavedValue<bool>("macro_show_trajectory"));
    }

    g.showTrajectory = g.mod->getSavedValue<bool>("macro_show_trajectory");
    if (!g.showTrajectory) ShowTrajectory::trajectoryOff();
  }

  if (id == "toggle_render" && PlayLayer::get()) {
    bool result = Renderer::toggle();

    if (result && Global::get().renderer.recording)
      Notification::create("Started Rendering", NotificationIcon::Info)->show();

    if (g.layer) {
      if (static_cast<RecordLayer*>(g.layer)->renderToggle)
        static_cast<RecordLayer*>(g.layer)->renderToggle->toggle(Global::get().renderer.recording);
    }

  }

  if (id == "toggle_noclip") {
    g.mod->setSavedValue("macro_noclip", !g.mod->getSavedValue<bool>("macro_noclip"));

    if (g.layer) {
      if (static_cast<RecordLayer*>(g.layer)->noclipToggle)
        static_cast<RecordLayer*>(g.layer)->noclipToggle->toggle(g.mod->getSavedValue<bool>("macro_noclip"));
    }
  }
}

$on_mod(Loaded) {
  for (auto const& id : keybindIDs) {
    geode::listenForKeybindSettingPresses("key_" + id, [id](geode::Keybind const&, bool down, bool repeat, double) {
      // holding the step key keeps stepping (custom-keybinds' repeat option before)
      if (repeat && id != "step_frame") return false;
      onKeybind(down, id);
      return false;
    });
  }
}
