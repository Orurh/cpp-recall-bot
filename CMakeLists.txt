cmake_minimum_required(VERSION 3.28)

project(cc_recall_bot LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(
    ENABLE_CLANG_TIDY
    "Run clang-tidy during compilation"
    OFF
)

find_package(
    Boost REQUIRED
    COMPONENTS json system
)

find_package(Threads REQUIRED)
find_package(spdlog REQUIRED)
find_package(CURL REQUIRED)
find_package(PostgreSQL REQUIRED)

add_executable(cc_recall_bot
    src/main.cc
    src/app/application.cc
    src/app/quiz_session_service.cc
    src/bot/telegram_bot.cc
    src/bot/card_renderer.cc
    src/bot/telegram_update_handler.cc
    src/quiz/question_bank.cc
    src/quiz/question_validator.cc
    src/quiz/quiz_service.cc
    src/storage/question_loader.cc
    src/storage/attempt_repository.cc
    src/learning/learning_queue.cc
    src/logging/logging.cc
    src/runtime/signal_watcher.cc
    src/http/curl_http_client.cc
)

add_executable(content_check
    src/tools/content_check.cc
    src/quiz/question_validator.cc
    src/storage/question_loader.cc
)

target_include_directories(
    cc_recall_bot
    PRIVATE src
)

target_include_directories(
    content_check
    PRIVATE src
)

target_compile_features(
    cc_recall_bot
    PRIVATE cxx_std_23
)

target_compile_features(
    content_check
    PRIVATE cxx_std_23
)

set_target_properties(
    cc_recall_bot
    content_check
    PROPERTIES
    CXX_EXTENSIONS OFF
)

target_link_libraries(
    cc_recall_bot
    PRIVATE
    Boost::json
    Boost::system
    Threads::Threads
    spdlog::spdlog
    CURL::libcurl
    PostgreSQL::PostgreSQL
)

target_link_libraries(
    content_check
    PRIVATE
    Boost::json
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(
        cc_recall_bot
        PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
    )

    target_compile_options(
        content_check
        PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
    )
endif()

if(ENABLE_CLANG_TIDY)
    find_program(
        CLANG_TIDY_EXECUTABLE
        clang-tidy
        REQUIRED
    )

    set_target_properties(
        cc_recall_bot
        content_check
        PROPERTIES
        CXX_CLANG_TIDY
        "${CLANG_TIDY_EXECUTABLE}"
    )
endif()

find_program(
    CPPCHECK_EXECUTABLE
    cppcheck
)

if(CPPCHECK_EXECUTABLE)
    add_custom_target(
        cppcheck
        COMMAND
            ${CPPCHECK_EXECUTABLE}
            --project=${CMAKE_BINARY_DIR}/compile_commands.json
            --enable=warning,style,performance,portability
            --inline-suppr
            --suppress=missingIncludeSystem
            --error-exitcode=1
        WORKING_DIRECTORY
            ${CMAKE_SOURCE_DIR}
        COMMENT
            "Running cppcheck"
        VERBATIM
    )
endif()
