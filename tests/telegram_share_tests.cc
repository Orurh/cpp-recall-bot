#include "miniapp/telegram_share.h"

#include "bot/telegram_bot.h"
#include "http/curl_http_client.h"

#include <boost/json.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

TelegramShare::Time FixedTime() {
  return {std::chrono::steady_clock::time_point(100s), std::chrono::system_clock::time_point(1'800'000'000s)};
}

boost::json::object Share() {
  return {{"url", "https://t.me/cpp_recall_bot?startapp=review_212_v1"},
          {"text", "C++ Recall · CR-000212\n\nНайди ошибку.\n\nreturn cache[key];"}};
}

boost::json::value ValidResponse() {
  return boost::json::object{{"id", "prepared_123-test"}, {"expiration_date", 1'800'000'060}};
}

void TestPreparationContract() {
  auto share = Share();
  share["user_id"] = 999;
  share["result"] = boost::json::object{{"message_text", "arbitrary client result"}};
  share["explanation"] = "This must never enter the message";
  const auto before = share;
  int calls = 0;
  const std::stop_source stop;
  TelegramShare adapter(
      [&](std::string_view method, const boost::json::object& params, const std::stop_token& token) {
        ++calls;
        Check(method == "savePreparedInlineMessage", "Only prepared-message storage is allowed");
        Check(token == stop.get_token(), "Forward the request cancellation token");
        Check(params.at("user_id") == 42, "Use authenticated user, never payload user_id");
        Check(params.at("allow_user_chats") == true && params.at("allow_group_chats") == true &&
                  params.at("allow_channel_chats") == true && params.at("allow_bot_chats") == false,
              "Explicitly allow users/groups/channels, not bots");
        const auto& result = params.at("result").as_object();
        Check(result.at("type") == "article", "Share a plain article");
        const auto& content = result.at("input_message_content").as_object();
        Check(std::string(content.at("message_text").as_string()) ==
                  std::string(share.at("url").as_string()) + "\n\n" + std::string(share.at("text").as_string()),
              "Preserve the exact validated URL/prompt/code");
        Check(!content.contains("parse_mode") && !content.contains("entities") && !result.contains("reply_markup"),
              "Do not interpret C++ as markup or introduce arbitrary message buttons");
        Check(content.at("link_preview_options").as_object().at("is_disabled") == true,
              "Disable misleading link previews");
        return ValidResponse();
      },
      FixedTime);
  const auto result = adapter.Prepare(42, share, stop.get_token());
  Check(result && result->id == "prepared_123-test" && result->expiration_date == 1'800'000'060,
        "Return only the validated prepared-message capability");
  Check(calls == 1 && share == before, "Sharing must not mutate the fallback payload");
}

void TestMalformedResponses() {
  const std::vector<boost::json::value> invalid{
      nullptr,
      boost::json::array{},
      true,
      boost::json::object{},
      boost::json::object{{"id", ""}, {"expiration_date", 1'800'000'060}},
      boost::json::object{{"id", 123}, {"expiration_date", 1'800'000'060}},
      boost::json::object{{"id", "white space"}, {"expiration_date", 1'800'000'060}},
      boost::json::object{{"id", "control\nvalue"}, {"expiration_date", 1'800'000'060}},
      boost::json::object{{"id", std::string(513, 'x')}, {"expiration_date", 1'800'000'060}},
      boost::json::object{{"id", "prepared"}},
      boost::json::object{{"id", "prepared"}, {"expiration_date", 1'799'999'999}},
      boost::json::object{{"id", "prepared"}, {"expiration_date", 1'800'000'000}},
      boost::json::object{{"id", "prepared"}, {"expiration_date", "1800000060"}},
      boost::json::object{{"id", "prepared"}, {"expiration_date", 1'800'000'060.5}},
      boost::json::object{{"id", "prepared"}, {"expiration_date", true}},
      boost::json::object{{"id", "prepared"}, {"expiration_date", std::numeric_limits<std::int64_t>::max()}},
      boost::json::object{{"id", "prepared"}, {"expiration_date", std::numeric_limits<std::uint64_t>::max()}},
  };
  for (const auto& response : invalid) {
    TelegramShare adapter([&](auto, const auto&, const auto&) { return response; }, FixedTime);
    const auto share = Share();
    Check(!adapter.Prepare(42, share), "Malformed/expired Telegram results must keep plain fallback");
    Check(share == Share(), "Rejected results must leave the fallback untouched");
  }
  TelegramShare unsigned_timestamp(
      [](auto, const auto&, const auto&) {
        return boost::json::object{{"id", "opaque:message+id="}, {"expiration_date", std::uint64_t{1'800'000'060}}};
      },
      FixedTime);
  Check(unsigned_timestamp.Prepare(42, Share()).has_value(), "Accept a valid opaque ID and integer timestamp");
}

void TestFallbackAndCooldown() {
  auto now = FixedTime();
  int calls = 0;
  TelegramShare adapter(
      [&](auto, const auto&, const auto&) -> boost::json::value {
        if (++calls == 1)
          throw std::runtime_error("secret token / HTTP internals must not escape");
        return ValidResponse();
      },
      [&] { return now; });
  Check(!adapter.Prepare(42, Share()), "Transport/Telegram failure is optional fallback, not exception");
  Check(!adapter.Prepare(42, Share()) && calls == 1, "A failed request also consumes the user cooldown");
  Check(adapter.Prepare(43, Share()).has_value() && calls == 2, "Different users have independent cooldowns");
  now.steady += 4999ms;
  Check(!adapter.Prepare(42, Share()) && calls == 2, "No repeat preparation before five seconds");
  now.steady += 1ms;
  Check(adapter.Prepare(42, Share()).has_value() && calls == 3, "Preparation resumes exactly at five seconds");
}

void TestBoundedCooldown() {
  auto now = FixedTime();
  int calls = 0;
  TelegramShare adapter(
      [&](auto, const auto&, const auto&) {
        ++calls;
        return ValidResponse();
      },
      [&] { return now; });
  Check(adapter.Prepare(1, Share()).has_value(), "Initial cooldown entry");
  for (std::size_t i = 0; i < TelegramShare::kMaxTrackedUsers; ++i) {
    now.steady += 1ms;
    Check(adapter.Prepare(static_cast<std::int64_t>(i) + 2, Share()).has_value(),
          "Admission stays available when the bounded registry is full");
  }
  Check(adapter.Prepare(1, Share()).has_value(), "Oldest entry is evicted at the fixed registry bound");
  Check(calls == static_cast<int>(TelegramShare::kMaxTrackedUsers) + 2, "Bounded registry does not drop requests");
}

void TestCancellationAndInvalidInput() {
  int calls = 0;
  std::stop_source stop;
  TelegramShare adapter(
      [&](auto, const auto&, const auto& token) {
        ++calls;
        if (calls == 1) {
          Check(token == stop.get_token(), "The real request token reaches the transport");
          stop.request_stop();
        }
        return ValidResponse();
      },
      FixedTime);
  Check(!adapter.Prepare(42, Share(), stop.get_token()) && calls == 1,
        "Cancellation during transport suppresses an otherwise valid capability");
  Check(!adapter.Prepare(43, Share(), stop.get_token()) && calls == 1,
        "A pre-cancelled request must not reach the transport");
  Check(adapter.Prepare(44, Share()).has_value() && calls == 2, "Cancellation releases the global admission slot");
  Check(!adapter.Prepare(0, Share()) && !adapter.Prepare(-1, Share()) && calls == 2,
        "An invalid authenticated user cannot prepare a message");
  for (const auto& malformed : std::vector<boost::json::object>{{},
                                                                {{"url", 123}, {"text", "code"}},
                                                                {{"url", "url"}},
                                                                {{"url", ""}, {"text", "code"}},
                                                                {{"url", "url"}, {"text", ""}},
                                                                {{"url", "url"}, {"text", std::string(4096, 'x')}}}) {
    Check(!adapter.Prepare(45, malformed) && calls == 2, "Invalid/oversized payload is never sent");
  }
}

void TestConcurrentAdmission() {
  std::promise<void> entered;
  std::promise<void> finish;
  const auto release = finish.get_future().share();
  std::atomic<int> calls = 0;
  TelegramShare adapter(
      [&](auto, const auto&, const auto&) {
        if (++calls == 1) {
          entered.set_value();
          release.wait();
        }
        return ValidResponse();
      },
      FixedTime);
  auto first = std::async(std::launch::async, [&] { return adapter.Prepare(42, Share()); });
  const bool started = entered.get_future().wait_for(2s) == std::future_status::ready;
  if (!started) {
    finish.set_value();
    first.wait();
    Check(false, "The first fake transport did not start");
  }
  auto second = std::async(std::launch::async, [&] { return adapter.Prepare(43, Share()); });
  const bool immediate = second.wait_for(200ms) == std::future_status::ready;
  finish.set_value();
  const auto second_result = second.get();
  const auto first_result = first.get();
  Check(immediate && !second_result && first_result && calls == 1,
        "A different user must get nonblocking fallback while Telegram preparation is active");
  Check(adapter.Prepare(43, Share()).has_value() && calls == 2,
        "Admission releases after success and busy fallback does not consume user cooldown");
}

void TestTimeoutValidation() {
  for (const auto timeout : {0ms, -1ms}) {
    bool rejected = false;
    try {
      [[maybe_unused]] CurlHttpClient client(timeout);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    Check(rejected, "Reject an unlimited or negative timeout");
  }
  // Constructing transports does not issue any network requests.
  [[maybe_unused]] TelegramBot default_polling_timeout("000:test");
  [[maybe_unused]] TelegramBot short_optional_timeout("000:test", 4s);
}

int HttpProbe(int argc, char** argv) {
  if ((argc != 4 && argc != 5) || std::string_view(argv[1]) != "--http-probe")
    return 2;
  const std::string_view url(argv[2]);
  constexpr std::string_view prefix = "http://127.0.0.1:";
  if (!url.starts_with(prefix) || !url.ends_with('/'))
    return 2;
  const auto parse = [](std::string_view input, int maximum) {
    int number = 0;
    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), number);
    if (error != std::errc{} || end != input.data() + input.size() || number < 1 || number > maximum)
      throw std::invalid_argument("Invalid local HTTP probe argument");
    return number;
  };
  try {
    [[maybe_unused]] const auto port = parse(url.substr(prefix.size(), url.size() - prefix.size() - 1), 65535);
    const auto timeout = std::chrono::milliseconds(parse(argv[3], 5000));
    std::stop_source stop;
    std::jthread cancellation;
    if (argc == 5) {
      const auto cancel_after = std::chrono::milliseconds(parse(argv[4], 5000));
      cancellation = std::jthread([&, cancel_after] {
        std::this_thread::sleep_for(cancel_after);
        stop.request_stop();
      });
    }
    std::string outcome = "unexpected_success";
    const auto started = std::chrono::steady_clock::now();
    try {
      CurlHttpClient client(timeout);
      [[maybe_unused]] const auto response = client.PostJson(url, "{}", stop.get_token());
    } catch (const std::system_error& error) {
      outcome = error.code() == std::errc::operation_canceled ? "cancelled" : "failed";
    } catch (...) {
      outcome = "failed";
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    // No token, arbitrary destination or transport error details enter this probe.
    std::cout << boost::json::serialize(boost::json::object{{"outcome", outcome}, {"elapsed_ms", elapsed}}) << '\n';
    return 0;
  } catch (...) {
    return 2;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1)
    return HttpProbe(argc, argv);
  try {
    TestPreparationContract();
    TestMalformedResponses();
    TestFallbackAndCooldown();
    TestBoundedCooldown();
    TestCancellationAndInvalidInput();
    TestConcurrentAdmission();
    TestTimeoutValidation();
    std::cout << "Telegram share transport checks passed (fake transport; no network).\n";
  } catch (const std::exception& error) {
    std::cerr << "Telegram share transport check failed: " << error.what() << '\n';
    return 1;
  }
}
