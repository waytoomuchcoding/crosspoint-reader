#pragma once

struct BoardConfig {
  struct ViewableInsets {
    int top = 0;
    int right = 0;
    int bottom = 0;
    int left = 0;
  };
  ViewableInsets viewableInsets{};
  static const BoardConfig ACTIVE;
};

inline const BoardConfig BoardConfig::ACTIVE{};
