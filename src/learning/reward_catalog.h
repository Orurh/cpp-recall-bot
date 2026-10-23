#pragma once

#include <array>
#include <cstdint>
#include <string_view>

struct XpRewardDefinition {
  std::string_view id;
  std::string_view title;
  std::string_view description;
  std::int64_t threshold = 0;
  bool available = false;
};

inline constexpr std::array kXpRewardCatalog{
    XpRewardDefinition{
        "profile_frames",
        "Рамки и значки профиля",
        "Две рамки профиля и памятный значок за первые 150 XP.",
        150,
        true,
    },
    XpRewardDefinition{
        "second_preset",
        "Второй набор настроек тренировки",
        "Сохраняй два любимых сочетания темы, формата и сложности вместо одного.",
        400,
        true,
    },
    XpRewardDefinition{
        "themes",
        "Дополнительные темы оформления",
        "Новые цветовые варианты мини-приложения.",
        800,
        false,
    },
    XpRewardDefinition{
        "personal_marathon",
        "Личный тематический марафон",
        "Настраиваемая длительность и форматы тематической практики.",
        1'500,
        false,
    },
    XpRewardDefinition{
        "five_presets",
        "Пять наборов и витрина достижений",
        "Больше сохранённых тренировок и расширенное оформление профиля.",
        3'000,
        false,
    },
    XpRewardDefinition{
        "public_collections",
        "Публичные подборки",
        "Создание подборок из существующих карточек после модерации.",
        5'000,
        false,
    },
};

[[nodiscard]] inline const XpRewardDefinition* FindXpReward(std::string_view id) {
  for (const auto& reward : kXpRewardCatalog) {
    if (reward.id == id) {
      return &reward;
    }
  }
  return nullptr;
}
