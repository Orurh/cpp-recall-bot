cmake_minimum_required(VERSION 3.28)

project(cc_recall_bot LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Boost REQUIRED COMPONENTS json system)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_executable(cc_recall_bot
    src/main.cc
    src/app/application.cc
    src/bot/telegram_bot.cc
    src/bot/card_renderer.cc
    src/quiz/quiz_service.cc
    src/storage/question_loader.cc
    src/storage/attempt_repository.cc
)

target_link_libraries(cc_recall_bot PRIVATE
    Boost::json
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)

target_compile_features(cc_recall_bot PRIVATE cxx_std_23)

set_target_properties(cc_recall_bot PROPERTIES
    CXX_EXTENSIONS OFF
)

target_include_directories(cc_recall_bot PRIVATE src)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cc_recall_bot PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
    )
endif()
