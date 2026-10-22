cmake_minimum_required(VERSION 3.28)

project(cc_recall_bot LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(CTest)

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
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(PostgreSQL REQUIRED)

add_executable(cc_recall_bot
    src/main.cc
    src/app/application.cc
    src/app/quiz_session_service.cc
    src/bot/telegram_bot.cc
    src/bot/card_renderer.cc
    src/bot/telegram_update_handler.cc
    src/quiz/question_bank.cc
    src/quiz/matching_round.cc
    src/quiz/question_validator.cc
    src/quiz/quiz_service.cc
    src/storage/question_loader.cc
    src/storage/attempt_repository.cc
    src/learning/adaptive_level.cc
    src/learning/learning_queue.cc
    src/learning/skill_catalog.cc
    src/logging/logging.cc
    src/runtime/signal_watcher.cc
    src/http/curl_http_client.cc
    src/miniapp/miniapp_http_server.cc
    src/miniapp/miniapp_service.cc
    src/miniapp/telegram_init_data.cc
)

add_executable(content_check
    src/tools/content_check.cc
    src/quiz/question_validator.cc
    src/storage/question_loader.cc
    src/learning/skill_catalog.cc
)

add_executable(core_tests
    tests/core_tests.cc
    src/app/quiz_session_service.cc
    src/bot/card_renderer.cc
    src/learning/adaptive_level.cc
    src/learning/learning_queue.cc
    src/learning/skill_catalog.cc
    src/quiz/matching_round.cc
    src/quiz/question_bank.cc
    src/quiz/quiz_service.cc
    src/miniapp/telegram_init_data.cc
)

add_executable(storage_tests
    tests/storage_tests.cc
    src/storage/attempt_repository.cc
    src/learning/adaptive_level.cc
)

target_include_directories(
    cc_recall_bot
    PRIVATE src
)

target_include_directories(
    content_check
    PRIVATE src
)

target_include_directories(
    core_tests
    PRIVATE src
)

target_include_directories(
    storage_tests
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

target_compile_features(
    core_tests
    PRIVATE cxx_std_23
)

target_compile_features(
    storage_tests
    PRIVATE cxx_std_23
)

set_target_properties(
    cc_recall_bot
    content_check
    core_tests
    storage_tests
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
    OpenSSL::Crypto
    PostgreSQL::PostgreSQL
)

target_link_libraries(
    content_check
    PRIVATE
    Boost::json
)

target_link_libraries(
    core_tests
    PRIVATE
    Boost::json
    OpenSSL::Crypto
)

target_link_libraries(
    storage_tests
    PRIVATE
    PostgreSQL::PostgreSQL
)

add_test(NAME core_tests COMMAND core_tests)
add_test(NAME storage_tests COMMAND storage_tests)
set_tests_properties(
    storage_tests
    PROPERTIES
    SKIP_RETURN_CODE 77
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
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

    target_compile_options(
        core_tests
        PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
    )

    target_compile_options(
        storage_tests
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
        core_tests
        storage_tests
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
            --suppress=useStlAlgorithm
            --error-exitcode=1
        WORKING_DIRECTORY
            ${CMAKE_SOURCE_DIR}
        COMMENT
            "Running cppcheck"
        VERBATIM
    )
endif()

# Optional unit tests; the bot itself does not require GoogleTest.
if(BUILD_TESTING)
    find_package(GTest QUIET)
    if(GTest_FOUND)
        add_executable(matching_round_tests
            tests/matching_round_test.cc
            src/quiz/matching_round.cc
            src/quiz/question_bank.cc
            src/quiz/question_validator.cc
            src/quiz/quiz_service.cc
            src/learning/adaptive_level.cc
            src/learning/learning_queue.cc
            src/learning/skill_catalog.cc
            src/app/quiz_session_service.cc
        )
        target_include_directories(matching_round_tests PRIVATE src)
        target_compile_features(matching_round_tests PRIVATE cxx_std_23)
        target_link_libraries(matching_round_tests PRIVATE GTest::gtest_main)
        include(GoogleTest)
        gtest_discover_tests(matching_round_tests)
    endif()
endif()

find_program(
    CLANG_FORMAT_EXECUTABLE
    clang-format
)

if(CLANG_FORMAT_EXECUTABLE)
    file(
        GLOB_RECURSE FORMAT_SOURCES
        CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/src/*.cc
        ${CMAKE_SOURCE_DIR}/src/*.h
        ${CMAKE_SOURCE_DIR}/tests/*.cc
    )

    add_custom_target(
        format
        COMMAND ${CLANG_FORMAT_EXECUTABLE} -i ${FORMAT_SOURCES}
        COMMENT "Formatting C++ sources"
        VERBATIM
    )

    add_custom_target(
        format-check
        COMMAND ${CLANG_FORMAT_EXECUTABLE} --dry-run --Werror ${FORMAT_SOURCES}
        COMMENT "Checking C++ source formatting"
        VERBATIM
    )
endif()
