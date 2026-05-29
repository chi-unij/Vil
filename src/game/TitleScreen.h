#pragma once

class TitleScreen {
public:
  enum class Action {
    None,
    Start,
    Settings,
    Quit,
    Editor,
  };

  Action Draw(int viewportWidth, int viewportHeight);
};
