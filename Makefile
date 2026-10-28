BUILD := build
TIDY_BUILD := build-tidy
TARGET := cc_recall_bot

.PHONY: all configure build run clean rebuild test tidy cppcheck content-check content-audit content-coverage format format-check check

all: build

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
