#include "miniapp/answer_help.h"
#include "storage/question_loader.h"

#include <boost/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace {
void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

template <typename F>
void Reject(F&& operation) {
  bool rejected = false;
  try {
    operation();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  Check(rejected, "answer help accepted an unrelated revision");
}
}  // namespace

void CheckAnswerHelpContent() {
  const auto cards = LoadQuestions("content/questions");
  for (const auto kind : {QuestionKind::kTheory, QuestionKind::kCodeReading}) {
    const auto card = std::ranges::find_if(cards, [kind](const auto& question) {
      return question.kind == kind && std::holds_alternative<SingleChoiceTask>(question.task);
    });
    Check(card != cards.end(), "missing real single-choice help fixture");
    const auto content = ParseFeedbackContent(card->content_snapshot_json, card->id, card->content_revision);
    const auto help = SingleChoiceAnswerHelp(content);
    const auto& task = std::get<SingleChoiceTask>(card->task);
    const auto correct = std::ranges::find(task.options, task.correct_option_id, &AnswerOption::id);
    Check(help && help->at("correct_answer").as_string() == correct->text &&
              help->at("explanation").as_string() == card->explanation && !help->contains("task"),
          "answer help did not preserve the verified answer and explanation");
    Reject(
        [&] { static_cast<void>(ParseFeedbackContent(card->content_snapshot_json, "other", card->content_revision)); });
    auto changed = content;
    changed["explanation"] = "another revision";
    Reject([&] {
      static_cast<void>(ParseFeedbackContent(boost::json::serialize(changed), card->id, card->content_revision));
    });
    changed = content;
    changed.at("task").as_object()["correct_option_id"] = "a different key";
    Reject([&] {
      static_cast<void>(ParseFeedbackContent(boost::json::serialize(changed), card->id, card->content_revision));
    });
    for (const auto* missing : {"explanation", "source"}) {
      changed = content;
      changed.erase(missing);
      const auto fallback = SingleChoiceAnswerHelp(changed);
      Check(fallback && fallback->contains("correct_answer"), "missing explanation or source hid the answer");
      if (std::string_view(missing) == "explanation")
        Check(fallback->at("explanation").as_string().empty(), "missing explanation was invented");
    }
    for (const auto text : {"", " \n\t", "<script>alert(1)</script> & std::vector<T>"}) {
      changed = content;
      changed["explanation"] = text;
      Check(SingleChoiceAnswerHelp(changed)->at("explanation").as_string() == text,
            "answer help altered explanation text");
    }
    changed = content;
    changed.at("task").as_object()["type"] = "short_answer";
    Check(!SingleChoiceAnswerHelp(changed), "explicit answer help expanded to another task type");
  }
}
