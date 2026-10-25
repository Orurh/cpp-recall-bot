cmake_minimum_required(VERSION 3.28)

project(cc_recall_bot LANGUAGES CXX)

if(NOT DEFINED APP_VERSION)
    execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE APP_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(NOT APP_VERSION)
    set(APP_VERSION "unknown")
endif()
if(NOT APP_VERSION MATCHES "^[A-Za-z0-9_.-]+$")
    message(FATAL_ERROR "APP_VERSION contains invalid characters")
endif()
add_compile_definitions(CPP_RECALL_BUILD_VERSION="${APP_VERSION}")

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
    src/learning/reference_catalog.cc
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
    src/quiz/question_identity.cc
    src/storage/attempt_repository.cc
    src/learning/adaptive_level.cc
    src/learning/learning_queue.cc
    src/learning/module_catalog.cc
    src/learning/skill_mastery.cc
    src/learning/skill_catalog.cc
    src/learning/theory_catalog.cc
    src/learning/training_recommendation.cc
    src/logging/logging.cc
    src/runtime/bounded_executor.cc
    src/runtime/signal_watcher.cc
    src/http/curl_http_client.cc
    src/miniapp/miniapp_http_server.cc
    src/miniapp/session_snapshot.cc
    src/miniapp/miniapp_service.cc
    src/miniapp/telegram_init_data.cc
)

add_executable(content_check
    src/learning/reference_catalog.cc
    src/tools/content_check.cc
    src/quiz/question_validator.cc
    src/storage/question_loader.cc
    src/quiz/question_identity.cc
    src/learning/module_catalog.cc
    src/learning/skill_mastery.cc
    src/learning/skill_catalog.cc
    src/learning/theory_catalog.cc
)

add_executable(core_tests
    src/learning/reference_catalog.cc
    tests/core_tests.cc
    tests/content_review_checks.cc
    tests/training_plan_checks.cc
    src/app/quiz_session_service.cc
    src/bot/card_renderer.cc
    src/learning/adaptive_level.cc
    src/learning/learning_queue.cc
    src/learning/module_catalog.cc
    src/learning/skill_mastery.cc
    src/learning/skill_catalog.cc
    src/learning/theory_catalog.cc
    src/learning/training_recommendation.cc
    src/quiz/matching_round.cc
    src/quiz/question_bank.cc
    src/quiz/quiz_service.cc
    src/quiz/question_identity.cc
    src/miniapp/session_snapshot.cc
    src/miniapp/telegram_init_data.cc
    src/runtime/bounded_executor.cc
)

add_executable(snapshot_library_examples tests/snapshot_library_examples.cc)
target_compile_features(snapshot_library_examples PRIVATE cxx_std_23)
add_test(NAME snapshot_library_examples COMMAND snapshot_library_examples)

add_executable(storage_tests
    tests/storage_tests.cc
    src/storage/attempt_repository.cc
    src/learning/adaptive_level.cc
    src/learning/skill_mastery.cc
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
    OpenSSL::Crypto
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
set_tests_properties(core_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_executable(miniapp_feedback_tests
    tests/reference_help_checks.cc
    src/learning/reference_catalog.cc
    tests/miniapp_feedback_tests.cc
    tests/beta_statistics_checks.cc
    tests/learning_statistics_checks.cc
    tests/answer_metrics_checks.cc
    tests/training_balance_checks.cc
    src/miniapp/miniapp_service.cc
    src/miniapp/session_snapshot.cc
    src/app/quiz_session_service.cc
    src/storage/attempt_repository.cc
    src/storage/question_loader.cc
    src/quiz/question_identity.cc
    src/quiz/question_bank.cc
    src/quiz/question_validator.cc
    src/quiz/quiz_service.cc
    src/quiz/matching_round.cc
    src/learning/adaptive_level.cc
    src/learning/learning_queue.cc
    src/learning/module_catalog.cc
    src/learning/skill_mastery.cc
    src/learning/skill_catalog.cc
    src/learning/theory_catalog.cc
    src/learning/training_recommendation.cc
)
target_include_directories(miniapp_feedback_tests PRIVATE src)
target_compile_features(miniapp_feedback_tests PRIVATE cxx_std_23)
set_target_properties(miniapp_feedback_tests PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(miniapp_feedback_tests PRIVATE
    Boost::json
    PostgreSQL::PostgreSQL
    OpenSSL::Crypto
    spdlog::spdlog
    Threads::Threads
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(miniapp_feedback_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()
add_test(NAME miniapp_feedback_tests COMMAND miniapp_feedback_tests)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME content_review_integrity COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/scripts/validate_content_reviews.py)
    add_test(NAME beta_maintenance_tests COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/beta_maintenance_test.py)
    add_test(NAME auto_deploy_tests COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/auto_deploy_test.py)
    add_test(NAME reviewed_examples_tests COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/tests/reviewed_examples_test.py ${CMAKE_CXX_COMPILER})
    set_tests_properties(reviewed_examples_tests PROPERTIES TIMEOUT 120)
endif()
set_tests_properties(miniapp_feedback_tests PROPERTIES SKIP_RETURN_CODE 77 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME storage_tests COMMAND storage_tests)
find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE)
    add_test(NAME runtime_http_smoke COMMAND ${NODE_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/tests/runtime_http_smoke.mjs $<TARGET_FILE:cc_recall_bot>)
    set_tests_properties(runtime_http_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} RESOURCE_LOCK cpp_recall_test_database)
endif()
set_tests_properties(storage_tests miniapp_feedback_tests PROPERTIES RESOURCE_LOCK cpp_recall_test_database)
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
        miniapp_feedback_tests
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
            src/learning/module_catalog.cc
            src/learning/skill_catalog.cc
            src/app/quiz_session_service.cc
        )
        target_include_directories(matching_round_tests PRIVATE src)
        target_compile_features(matching_round_tests PRIVATE cxx_std_23)
        target_link_libraries(matching_round_tests PRIVATE GTest::gtest_main)
        include(GoogleTest)
        gtest_discover_tests(matching_round_tests)

        add_executable(skill_mastery_tests
            tests/skill_mastery_test.cc
            src/learning/skill_mastery.cc
        )
        target_include_directories(skill_mastery_tests PRIVATE src)
        target_compile_features(skill_mastery_tests PRIVATE cxx_std_23)
        target_link_libraries(skill_mastery_tests PRIVATE GTest::gtest_main)
        gtest_discover_tests(skill_mastery_tests)
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
