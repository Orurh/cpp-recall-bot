#pragma once

#include <string>
#include <vector>

namespace system_design {

struct Source {
  std::string title;
  std::string url;
};

enum class StageKind { kRequirements, kEstimation, kContract, kDataModel, kArchitecture, kTradeoffs };

struct Stage {
  std::string id;
  std::string title;
  StageKind kind;
  std::string prompt;
  std::vector<std::string> hints;
  std::vector<std::string> criteria;
  std::string debrief;
  std::vector<std::string> alternatives;
  std::vector<Source> sources;
};

struct Case {
  std::string id;
  std::string title;
  std::string revision;
  std::string brief;
  std::vector<Stage> stages;
};

struct LessonSection {
  std::string title;
  std::string text;
};

struct Module {
  std::string id;
  std::string title;
  std::string revision;
  std::vector<LessonSection> sections;
};

[[nodiscard]] const Module& InterviewModule();
[[nodiscard]] const Case& UrlShortenerCase();
[[nodiscard]] std::string StageKindName(StageKind kind);

}  // namespace system_design
