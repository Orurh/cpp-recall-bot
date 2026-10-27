#pragma once

#include <string_view>

// Design guidance is not a rule of the C++ standard. Keep the source domains
// explicit instead of accepting arbitrary URLs or labelling SOLID as N4950.
enum class LearningSourceKind { kCppStandard, kDesignPrinciple };

constexpr bool IsLearningSource(std::string_view url, LearningSourceKind kind) {
  if (kind == LearningSourceKind::kCppStandard)
    return url.starts_with("https://timsong-cpp.github.io/cppwp/n4950/");
  if (kind != LearningSourceKind::kDesignPrinciple)
    return false;
  return url == "https://blog.cleancoder.com/uncle-bob/2014/05/08/SingleReponsibilityPrinciple.html" ||
         url == "https://blog.cleancoder.com/uncle-bob/2014/05/12/TheOpenClosedPrinciple.html" ||
         url == "https://blog.cleancoder.com/uncle-bob/2020/10/18/Solid-Relevance.html" ||
         url == "https://www.cs.cmu.edu/Groups/venari/subtype-toplas.html";
}
