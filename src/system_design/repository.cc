#include "system_design/repository.h"

#include "storage/postgres_connection.h"

#include <charconv>
#include <stdexcept>
#include <utility>
#include <vector>

namespace system_design {
namespace {

using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;

Result Query(PostgresConnection& connection, const char* sql, const std::vector<std::string>& params = {},
             bool rows = true, bool reconnect = false) {
  Result result(connection.Execute(sql, &params, reconnect), PQclear);
  if (!result || PQresultStatus(result.get()) != (rows ? PGRES_TUPLES_OK : PGRES_COMMAND_OK)) {
    // No SQL, submitted text or connection credentials in the error.
    throw std::runtime_error("System Design storage operation failed");
  }
  return result;
}

class Transaction {
 public:
  explicit Transaction(PostgresConnection& connection) : connection_(connection) {
    static_cast<void>(Query(connection_, "BEGIN", {}, false, true));
  }
  ~Transaction() {
    if (active_)
      connection_.Rollback();
  }
  void Commit() {
    static_cast<void>(Query(connection_, "COMMIT", {}, false));
    active_ = false;
  }
  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;

 private:
  PostgresConnection& connection_;
  bool active_ = true;
};

std::int64_t Integer(const char* value) {
  const std::string_view text(value);
  std::int64_t result = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
  if (error != std::errc{} || end != text.data() + text.size())
    throw std::runtime_error("invalid System Design storage integer");
  return result;
}

Session Read(PGresult* result, int row = 0) {
  Session session;
  session.id = Integer(PQgetvalue(result, row, 0));
  session.version = Integer(PQgetvalue(result, row, 1));
  session.case_id = PQgetvalue(result, row, 2);
  session.content_revision = PQgetvalue(result, row, 3);
  session.status = PQgetvalue(result, row, 4);
  const auto stage = Integer(PQgetvalue(result, row, 5));
  if (stage < 0)
    throw std::runtime_error("invalid stored System Design stage");
  session.current_stage = static_cast<std::size_t>(stage);
  session.state = boost::json::parse(PQgetvalue(result, row, 6)).as_object();
  if (!PQgetisnull(result, row, 7))
    session.review = boost::json::parse(PQgetvalue(result, row, 7)).as_object();
  return session;
}

constexpr const char* kLoad = R"sql(
SELECT s.id,s.state_version,s.case_id,s.content_revision,s.status,s.current_stage,s.state_json::TEXT,r.review_json::TEXT
FROM system_design_sessions s LEFT JOIN system_design_results r ON r.session_id=s.id AND r.user_id=s.user_id
WHERE s.user_id=$1 AND s.id=$2
)sql";

Session LoadRecord(PostgresConnection& connection, std::int64_t user_id, std::int64_t session_id, bool lock = false) {
  const std::string sql = std::string(kLoad) + (lock ? " FOR UPDATE OF s" : "");
  auto result = Query(connection, sql.c_str(), {std::to_string(user_id), std::to_string(session_id)}, true, !lock);
  if (PQntuples(result.get()) != 1)
    throw std::logic_error("Занятие System Design недоступно.");
  return Read(result.get());
}

void EnsureIdentity(PostgresConnection& connection, std::int64_t user_id) {
  static_cast<void>(Query(connection, "INSERT INTO users(user_id) VALUES($1) ON CONFLICT DO NOTHING",
                          {std::to_string(user_id)}, false));
}

}  // namespace

Repository::Repository(std::shared_ptr<DatabaseConnectionPool> pool) : pool_(std::move(pool)) {
  if (!pool_)
    throw std::invalid_argument("System Design requires a database pool");
}

bool Repository::ModuleCompleted(std::int64_t user_id, std::string_view id, std::string_view revision) {
  bool completed = false;
  pool_->WithConnection([&](PostgresConnection& connection) {
    auto result = Query(connection,
                        "SELECT 1 FROM system_design_module_progress WHERE user_id=$1 "
                        "AND module_id=$2 AND content_revision=$3",
                        {std::to_string(user_id), std::string(id), std::string(revision)}, true, true);
    completed = PQntuples(result.get()) == 1;
  });
  return completed;
}

void Repository::CompleteModule(std::int64_t user_id, std::string_view id, std::string_view revision) {
  pool_->WithConnection([&](PostgresConnection& connection) {
    Transaction transaction(connection);
    EnsureIdentity(connection, user_id);
    static_cast<void>(Query(connection,
                            "INSERT INTO system_design_module_progress(user_id,module_id,content_revision) "
                            "VALUES($1,$2,$3) ON CONFLICT DO NOTHING",
                            {std::to_string(user_id), std::string(id), std::string(revision)}, false));
    transaction.Commit();
  });
}

Session Repository::Start(std::int64_t user_id, std::string_view case_id, std::string_view revision,
                          std::string_view request_id, const boost::json::object& initial_state) {
  Session session;
  pool_->WithConnection([&](PostgresConnection& connection) {
    Transaction transaction(connection);
    const std::string uid = std::to_string(user_id);
    static_cast<void>(Query(connection, "SELECT pg_advisory_xact_lock(1936028749,hashtext($1))", {uid}));
    EnsureIdentity(connection, user_id);
    auto existing = Query(connection,
                          "SELECT id,case_id FROM system_design_sessions WHERE user_id=$1 AND "
                          "(start_request_id=$2 OR (case_id=$3 AND status='active')) "
                          "ORDER BY (start_request_id=$2) DESC,id DESC LIMIT 1",
                          {uid, std::string(request_id), std::string(case_id)});
    if (PQntuples(existing.get()) != 0) {
      if (std::string_view(PQgetvalue(existing.get(), 0, 1)) != case_id)
        throw std::invalid_argument("Этот запрос уже использован для другого кейса.");
      session = LoadRecord(connection, user_id, Integer(PQgetvalue(existing.get(), 0, 0)), true);
    } else {
      auto inserted = Query(connection,
                            "INSERT INTO system_design_sessions(user_id,case_id,content_revision,start_request_id,"
                            "state_json) VALUES($1,$2,$3,$4,$5::jsonb) RETURNING id",
                            {uid, std::string(case_id), std::string(revision), std::string(request_id),
                             boost::json::serialize(initial_state)});
      session = LoadRecord(connection, user_id, Integer(PQgetvalue(inserted.get(), 0, 0)), true);
    }
    transaction.Commit();
  });
  return session;
}

std::optional<Session> Repository::Active(std::int64_t user_id) {
  std::optional<Session> session;
  pool_->WithConnection([&](PostgresConnection& connection) {
    auto result = Query(connection,
                        "SELECT id FROM system_design_sessions WHERE user_id=$1 AND status='active' "
                        "ORDER BY id DESC LIMIT 1",
                        {std::to_string(user_id)}, true, true);
    if (PQntuples(result.get()) != 0)
      session = LoadRecord(connection, user_id, Integer(PQgetvalue(result.get(), 0, 0)));
  });
  return session;
}

Session Repository::Load(std::int64_t user_id, std::int64_t session_id) {
  Session session;
  pool_->WithConnection([&](PostgresConnection& connection) { session = LoadRecord(connection, user_id, session_id); });
  return session;
}

boost::json::array Repository::History(std::int64_t user_id) {
  boost::json::array history;
  pool_->WithConnection([&](PostgresConnection& connection) {
    auto result = Query(connection,
                        "SELECT id,case_id,content_revision,status,current_stage,"
                        "to_char(started_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
                        "FROM system_design_sessions WHERE user_id=$1 ORDER BY id DESC LIMIT 50",
                        {std::to_string(user_id)}, true, true);
    for (int row = 0; row < PQntuples(result.get()); ++row) {
      history.emplace_back(boost::json::object{{"session_id", Integer(PQgetvalue(result.get(), row, 0))},
                                               {"case_id", PQgetvalue(result.get(), row, 1)},
                                               {"content_revision", PQgetvalue(result.get(), row, 2)},
                                               {"status", PQgetvalue(result.get(), row, 3)},
                                               {"current_stage", Integer(PQgetvalue(result.get(), row, 4))},
                                               {"started_at", PQgetvalue(result.get(), row, 5)}});
    }
  });
  return history;
}

boost::json::object Repository::Statistics(std::int64_t user_id) {
  boost::json::object statistics;
  pool_->WithConnection([&](PostgresConnection& connection) {
    auto result = Query(connection,
                        "SELECT count(*),count(*) FILTER(WHERE status='completed'),"
                        "count(*) FILTER(WHERE status='active') FROM system_design_sessions WHERE user_id=$1",
                        {std::to_string(user_id)}, true, true);
    statistics = {{"started", Integer(PQgetvalue(result.get(), 0, 0))},
                  {"completed", Integer(PQgetvalue(result.get(), 0, 1))},
                  {"in_progress", Integer(PQgetvalue(result.get(), 0, 2))}};
  });
  return statistics;
}

Session Repository::Mutate(std::int64_t user_id, std::int64_t session_id, std::int64_t expected_version,
                           std::string_view request_id, const boost::json::object& request,
                           const std::function<void(Session&)>& operation) {
  Session session;
  pool_->WithConnection([&](PostgresConnection& connection) {
    Transaction transaction(connection);
    session = LoadRecord(connection, user_id, session_id, true);
    const std::vector<std::string> command{std::to_string(user_id), std::to_string(session_id), std::string(request_id),
                                           boost::json::serialize(request)};
    auto previous = Query(connection,
                          "SELECT request_json=$4::jsonb FROM system_design_commands "
                          "WHERE user_id=$1 AND session_id=$2 AND request_id=$3",
                          command);
    if (PQntuples(previous.get()) != 0) {
      if (std::string_view(PQgetvalue(previous.get(), 0, 0)) != "t")
        throw std::invalid_argument("Повторный запрос содержит другие данные.");
      transaction.Commit();
      return;
    }
    if (session.version != expected_version)
      throw std::logic_error("Занятие изменилось в другом окне. Обнови его; текущий черновик не перезаписан.");
    operation(session);
    auto updated = Query(connection,
                         "UPDATE system_design_sessions SET status=$3,current_stage=$4,state_json=$5::jsonb,"
                         "state_version=state_version+1,updated_at=now(),"
                         "completed_at=CASE WHEN $3='completed' THEN COALESCE(completed_at,now()) ELSE NULL END "
                         "WHERE user_id=$1 AND id=$2 AND state_version=$6 RETURNING state_version",
                         {command[0], command[1], session.status, std::to_string(session.current_stage),
                          boost::json::serialize(session.state), std::to_string(expected_version)});
    if (PQntuples(updated.get()) != 1)
      throw std::logic_error("Занятие изменилось.");
    session.version = Integer(PQgetvalue(updated.get(), 0, 0));
    if (session.review) {
      static_cast<void>(
          Query(connection,
                "INSERT INTO system_design_results(user_id,session_id,review_json) VALUES($1,$2,$3::jsonb) "
                "ON CONFLICT(session_id) DO UPDATE SET review_json=EXCLUDED.review_json",
                {command[0], command[1], boost::json::serialize(*session.review)}, false));
    }
    static_cast<void>(
        Query(connection,
              "INSERT INTO system_design_commands(user_id,session_id,request_id,request_json,applied_version) "
              "VALUES($1,$2,$3,$4::jsonb,$5)",
              {command[0], command[1], command[2], command[3], std::to_string(session.version)}, false));
    transaction.Commit();
  });
  return session;
}

}  // namespace system_design
