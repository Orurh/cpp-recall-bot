#include "system_design/service.h"

#include "system_design/catalog.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <utility>

namespace system_design {
namespace {

using boost::json::array;
using boost::json::object;

std::string Text(const object& request, std::string_view key) {
  const auto* value = request.if_contains(key);
  if (value == nullptr || !value->is_string())
    throw std::invalid_argument("Ожидается строка: " + std::string(key));
  return std::string(value->as_string());
}

std::int64_t Integer(const object& request, std::string_view key, bool zero = false) {
  const auto* value = request.if_contains(key);
  if (value == nullptr)
    throw std::invalid_argument("Не указан параметр: " + std::string(key));
  std::int64_t integer = -1;
  if (value->is_int64())
    integer = value->as_int64();
  else if (value->is_uint64() && value->as_uint64() <= std::numeric_limits<std::int64_t>::max())
    integer = static_cast<std::int64_t>(value->as_uint64());
  if (integer < (zero ? 0 : 1))
    throw std::invalid_argument("Некорректный параметр: " + std::string(key));
  return integer;
}

std::string RequestId(const object& request) {
  auto id = Text(request, "request_id");
  if (id.size() < 16 || id.size() > 128 ||
      !std::ranges::all_of(id, [](unsigned char c) { return std::isalnum(c) != 0 || c == '-' || c == '_'; }))
    throw std::invalid_argument("Некорректный идентификатор запроса.");
  return id;
}

void ValidateText(std::string_view text, bool empty_allowed) {
  if (text.size() > 16'000 || text.find('\0') != std::string_view::npos ||
      (!empty_allowed && text.find_first_not_of(" \t\r\n") == std::string_view::npos))
    throw std::invalid_argument("Напиши свой ответ: не более 16 000 байт, без нулевых символов.");
}

array Strings(const std::vector<std::string>& values) {
  array result;
  for (const auto& value : values)
    result.emplace_back(value);
  return result;
}

void ValidateSession(const Session& session) {
  const auto& exercise = UrlShortenerCase();
  if (session.case_id != exercise.id || session.content_revision != exercise.revision ||
      session.current_stage > exercise.stages.size())
    throw std::logic_error("Редакция занятия недоступна. Сохранённые ответы не удалены.");
  for (const auto key : {"answers", "drafts", "hints"}) {
    const auto* values = session.state.if_contains(key);
    if (values == nullptr || !values->is_array() || values->as_array().size() != exercise.stages.size())
      throw std::logic_error("Сохранённое занятие повреждено; оно не перезаписано.");
  }
  for (std::size_t i = 0; i < exercise.stages.size(); ++i) {
    const auto& answer = session.state.at("answers").as_array()[i];
    const auto& draft = session.state.at("drafts").as_array()[i];
    const auto& hints = session.state.at("hints").as_array()[i];
    if (!answer.is_string() || !draft.is_string() || !hints.is_int64() || hints.as_int64() < 0 ||
        static_cast<std::uint64_t>(hints.as_int64()) > exercise.stages[i].hints.size())
      throw std::logic_error("Сохранённое занятие повреждено; оно не перезаписано.");
  }
}

object Review(const Session& session) {
  const auto& exercise = UrlShortenerCase();
  array stages;
  for (std::size_t i = 0; i < exercise.stages.size(); ++i) {
    const auto& stage = exercise.stages[i];
    array criteria;
    for (const auto& criterion : stage.criteria)
      criteria.emplace_back(object{{"text", criterion}, {"status", "unreviewed"}});
    array sources;
    for (const auto& source : stage.sources)
      sources.emplace_back(object{{"title", source.title}, {"url", source.url}});
    stages.emplace_back(object{{"id", stage.id},
                               {"title", stage.title},
                               {"answer", session.state.at("answers").as_array()[i]},
                               {"hints_used", session.state.at("hints").as_array()[i]},
                               {"criteria", std::move(criteria)},
                               {"analysis", stage.debrief},
                               {"alternatives", Strings(stage.alternatives)},
                               {"sources", std::move(sources)}});
  }
  return {{"title", exercise.title},
          {"content_revision", exercise.revision},
          {"assessment_kind", "self_review_not_automatic"},
          {"notice",
           "Это разбор для самопроверки, не автоматическая оценка твоей архитектуры. "
           "Отметь, что обосновано, что упущено и в чём сомневаешься. Совпадение с примером не требуется."},
          {"stages", std::move(stages)}};
}

}  // namespace

Service::Service(std::shared_ptr<DatabaseConnectionPool> pool, std::int64_t owner_id)
    : repository_(std::move(pool)), owner_id_(owner_id) {}

bool Service::IsOwner(std::int64_t user_id) const noexcept {
  return owner_id_ > 0 && user_id > 0 && user_id == owner_id_;
}

void Service::RequireOwner(std::int64_t user_id) const {
  if (!IsOwner(user_id))
    throw AccessDenied();
}

object Service::Serialize(const Session& session) const {
  ValidateSession(session);
  const auto& exercise = UrlShortenerCase();
  object result{{"direction", "system_design"},
                {"session_id", session.id},
                {"version", session.version},
                {"case_id", session.case_id},
                {"title", exercise.title},
                {"content_revision", session.content_revision},
                {"status", session.status},
                {"current_stage", session.current_stage},
                {"stages_total", exercise.stages.size()}};
  if (session.status == "completed") {
    if (!session.review)
      throw std::logic_error("Разбор завершённого занятия недоступен.");
    result["review"] = *session.review;
    return result;
  }
  if (session.current_stage >= exercise.stages.size())
    throw std::logic_error("Некорректный этап занятия.");
  const auto index = session.current_stage;
  const auto& stage = exercise.stages[index];
  const auto opened = static_cast<std::size_t>(session.state.at("hints").as_array()[index].as_int64());
  array hints;
  for (std::size_t i = 0; i < opened; ++i)
    hints.emplace_back(stage.hints[i]);
  result["stage"] = object{{"id", stage.id},
                           {"title", stage.title},
                           {"kind", StageKindName(stage.kind)},
                           {"prompt", stage.prompt},
                           {"draft", session.state.at("drafts").as_array()[index]},
                           {"hints", std::move(hints)},
                           {"more_hints", opened < stage.hints.size()}};
  array answers;
  for (std::size_t i = 0; i < index; ++i)
    answers.emplace_back(
        object{{"title", exercise.stages[i].title}, {"text", session.state.at("answers").as_array()[i]}});
  result["accepted_answers"] = std::move(answers);
  return result;
}

object Service::Handle(std::int64_t user_id, std::string_view action, const object& request) {
  // Every SD action checks authenticated identity before catalog or data access.
  RequireOwner(user_id);
  const auto& module = InterviewModule();
  const auto& exercise = UrlShortenerCase();
  if (action == "bootstrap") {
    const bool read = repository_.ModuleCompleted(user_id, module.id, module.revision);
    return {{"direction", "system_design"},
            {"module",
             object{{"id", module.id}, {"title", module.title}, {"revision", module.revision}, {"completed", read}}},
            {"case",
             object{{"id", exercise.id}, {"title", exercise.title}, {"brief", exercise.brief}, {"available", read}}},
            {"statistics", repository_.Statistics(user_id)},
            {"history", repository_.History(user_id)}};
  }
  if (action == "module") {
    array sections;
    for (const auto& section : module.sections)
      sections.emplace_back(object{{"title", section.title}, {"text", section.text}});
    return {
        {"id", module.id}, {"title", module.title}, {"revision", module.revision}, {"sections", std::move(sections)}};
  }
  if (action == "complete-module") {
    const auto* read = request.if_contains("acknowledged");
    if (read == nullptr || !read->is_bool() || !read->as_bool())
      throw std::invalid_argument("Подтверди, что прочитал вводный модуль.");
    repository_.CompleteModule(user_id, module.id, module.revision);
    return {{"completed", true}, {"assessment_kind", "read_acknowledgement_not_mastery"}};
  }
  if (action == "current") {
    const auto session = repository_.Active(user_id);
    return {{"session", session ? boost::json::value(Serialize(*session)) : boost::json::value(nullptr)}};
  }
  if (action == "session")
    return Serialize(repository_.Load(user_id, Integer(request, "session_id")));
  if (action == "start") {
    if (Text(request, "case_id") != exercise.id)
      throw std::invalid_argument("Неизвестный учебный кейс.");
    const auto request_id = RequestId(request);
    if (!repository_.ModuleCompleted(user_id, module.id, module.revision))
      throw std::logic_error("Сначала прочитай вводный модуль.");
    array answers, drafts, hints;
    for (std::size_t i = 0; i < exercise.stages.size(); ++i) {
      answers.emplace_back("");
      drafts.emplace_back("");
      hints.emplace_back(std::int64_t{0});
    }
    return Serialize(repository_.Start(
        user_id, exercise.id, exercise.revision, request_id,
        {{"answers", std::move(answers)}, {"drafts", std::move(drafts)}, {"hints", std::move(hints)}}));
  }
  if (action != "save" && action != "hint" && action != "advance" && action != "self-review")
    throw std::invalid_argument("Неизвестное действие System Design.");
  const auto request_id = RequestId(request);
  const auto id = Integer(request, "session_id");
  const auto version = Integer(request, "version");
  auto payload = request;
  payload["operation"] = action;
  return Serialize(repository_.Mutate(user_id, id, version, request_id, payload, [&](Session& session) {
    ValidateSession(session);
    if (action == "self-review") {
      if (session.status != "completed" || !session.review)
        throw std::logic_error("Сначала заверши кейс.");
      const auto stage = static_cast<std::size_t>(Integer(request, "stage", true));
      const auto criterion = static_cast<std::size_t>(Integer(request, "criterion", true));
      const auto status = Text(request, "status");
      if (stage >= exercise.stages.size() || criterion >= exercise.stages[stage].criteria.size() ||
          (status != "covered" && status != "uncertain" && status != "missing" && status != "unreviewed"))
        throw std::invalid_argument("Некорректная отметка самопроверки.");
      session.review->at("stages")
          .as_array()[stage]
          .as_object()
          .at("criteria")
          .as_array()[criterion]
          .as_object()["status"] = status;
      return;
    }
    if (session.status != "active" || session.current_stage >= exercise.stages.size())
      throw std::logic_error("Кейс уже завершён.");
    const auto index = session.current_stage;
    if (Text(request, "stage_id") != exercise.stages[index].id)
      throw std::logic_error("Этап уже изменился; текущий черновик не перезаписан.");
    if (action == "hint") {
      auto& opened = session.state.at("hints").as_array()[index];
      if (static_cast<std::size_t>(opened.as_int64()) >= exercise.stages[index].hints.size())
        throw std::logic_error("Все подсказки этапа уже открыты.");
      opened = opened.as_int64() + 1;
      return;
    }
    auto text = Text(request, "text");
    ValidateText(text, action == "save");
    session.state.at("drafts").as_array()[index] = text;
    if (action == "advance") {
      session.state.at("answers").as_array()[index] = std::move(text);
      ++session.current_stage;
      if (session.current_stage == exercise.stages.size()) {
        session.status = "completed";
        session.review = Review(session);
      }
    }
  }));
}

}  // namespace system_design
