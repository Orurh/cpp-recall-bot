#pragma once

#include <array>
#include <cstdint>
#include <string_view>

struct AchievementDefinition {
  std::string_view id;
  std::string_view title;
  std::string_view description;
  std::string_view metric;
  std::int64_t target;
  std::string_view category;
  std::string_view tier;
  std::string_view icon;
};

inline constexpr int kAchievementRulesVersion = 1;

inline constexpr std::array kAchievementCatalog{
    AchievementDefinition{
        "first_step",
        "Первый шаг",
        "Завершить первую карточку.",
        "completed",
        1,
        "practice",
        "bronze",
        "step",
    },
    AchievementDefinition{
        "explorer",
        "Исследователь",
        "Познакомиться с 25 разными вопросами.",
        "seen",
        25,
        "practice",
        "bronze",
        "compass",
    },
    AchievementDefinition{
        "practitioner",
        "Практик",
        "Подтвердить три навыка.",
        "confirmed",
        3,
        "mastery",
        "bronze",
        "target",
    },
    AchievementDefinition{
        "reliability",
        "Надёжность",
        "Закрепить навык отложенным применением.",
        "retained",
        1,
        "mastery",
        "bronze",
        "shield",
    },
    AchievementDefinition{
        "regular_practice",
        "Ритм практики",
        "Завершить 10 тренировок или отдельных заданий.",
        "sessions",
        10,
        "practice",
        "silver",
        "calendar",
    },
    AchievementDefinition{
        "broad_practice",
        "Широкая практика",
        "50 разных вопросов в практике.",
        "seen",
        50,
        "practice",
        "silver",
        "compass",
    },
    AchievementDefinition{
        "five_skills",
        "Пять навыков",
        "Подтвердить пять навыков.",
        "confirmed",
        5,
        "mastery",
        "silver",
        "target",
    },
    AchievementDefinition{
        "retained_three",
        "Прочные знания",
        "Закрепить три навыка отложенным применением.",
        "retained",
        3,
        "mastery",
        "silver",
        "shield",
    },
};
