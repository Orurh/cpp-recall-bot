#pragma once

#include <array>
#include <optional>
#include <span>
#include <string_view>

struct TopicDefinition {
  std::string_view id;
  std::string_view title;
};

inline constexpr std::array<TopicDefinition, 9> kTopicCatalog{{
    {.id = "language_core", .title = "Основы языка"},
    {.id = "oop", .title = "ООП"},
    {.id = "containers", .title = "Контейнеры"},
    {.id = "move_semantics", .title = "Семантика перемещения"},
    {.id = "special_member_functions", .title = "Специальные функции-члены"},
    {.id = "templates", .title = "Шаблоны"},
    {.id = "concurrency", .title = "Многопоточность"},
    {.id = "lifetime", .title = "Время жизни объектов"},
    {.id = "references", .title = "Ссылки"},
}};

inline std::span<const TopicDefinition> TrainingTopics() {
  return kTopicCatalog;
}

inline std::optional<std::string_view> TopicTitle(std::string_view topic_id) {
  for (const auto& topic : kTopicCatalog) {
    if (topic.id == topic_id) {
      return topic.title;
    }
  }

  return std::nullopt;
}

inline bool IsKnownTopic(std::string_view topic_id) {
  return TopicTitle(topic_id).has_value();
}
