BUILD := build
TIDY_BUILD := build-tidy
TARGET := cc_recall_bot

.PHONY: all configure build run clean rebuild tidy cppcheck content-check check

all: build

configure:
	cmake -S . -B $(BUILD) \
		-DCMAKE_BUILD_TYPE=Debug \
		-DENABLE_CLANG_TIDY=OFF

build: configure
	cmake --build $(BUILD) -j

run: build
	./$(BUILD)/$(TARGET)

content-check: configure
	cmake --build $(BUILD) --target content_check -j
	./$(BUILD)/content_check content/questions

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

check: content-check tidy cppcheck
