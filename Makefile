BUILD := build
TIDY_BUILD := build-tidy
TARGET := cc_recall_bot

.PHONY: all configure build run clean rebuild test tidy cppcheck content-check content-audit content-coverage format format-check check help

all: build

help:
	@printf '%s\n' \
		'Основные команды:' \
		'  make help              Показать эту справку' \
		'  make all / make build  Собрать приложение (all — цель по умолчанию)' \
		'  make configure         Настроить Debug-сборку' \
		'  make run               Собрать и запустить приложение с .env' \
		'  make test              Собрать приложение и запустить CTest' \
		'  make content-check     Проверить структуру карточек и связи с каталогами' \
		'  make content-audit     Проверить редакционные эвристики' \
		'  make content-coverage  Показать покрытие тем, навыков, семейств и справки' \
		'  make tidy              Собрать с проверками clang-tidy' \
		'  make cppcheck          Запустить статический анализ cppcheck' \
		'  make format            Отформатировать исходники' \
		'  make format-check      Проверить форматирование' \
		'  make check             Выполнить сборку и основные проверки' \
		'  make clean             Удалить каталоги сборки' \
		'  make rebuild           Очистить и собрать заново'

configure:
	cmake -S . -B $(BUILD) \
		-DCMAKE_BUILD_TYPE=Debug \
		-DENABLE_CLANG_TIDY=OFF

build: configure
	cmake --build $(BUILD) -j

run: build
	@test -f .env || (echo "Create .env from .env.example first"; exit 1)
	@set -a; . ./.env; set +a; exec ./$(BUILD)/$(TARGET)

test: build
	ctest --test-dir $(BUILD) --output-on-failure

content-check: configure
	cmake --build $(BUILD) --target content_check -j
	./$(BUILD)/content_check content/questions

content-audit:
	python3 scripts/audit_content_quality.py --check

content-coverage: configure
	cmake --build $(BUILD) --target content_check -j
	python3 scripts/report_content_coverage.py --content-check $(BUILD)/content_check

clean:
	rm -rf $(BUILD) $(TIDY_BUILD)

rebuild: clean build

tidy:
	cmake -S . -B $(TIDY_BUILD) \
		-DCMAKE_BUILD_TYPE=Debug \
		-DENABLE_CLANG_TIDY=ON
	cmake --build $(TIDY_BUILD) -j

cppcheck: configure
	cmake --build $(BUILD) --target cppcheck

format:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Debug
	cmake --build $(BUILD) --target format

format-check:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Debug
	cmake --build $(BUILD) --target format-check

check: build content-check content-audit test format-check cppcheck
