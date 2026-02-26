LIB_NAME    = agt
SRC_DIR     = src
INC_DIR     = include

THIRD_PARTY = deps
JSON_INC    = $(THIRD_PARTY)/json/include
SCHEMA_DIR  = $(THIRD_PARTY)/json-schema-validator

CXX         = g++
CXXFLAGS_COMMON = -std=c++23 -fPIC \
                  -I$(INC_DIR) -I$(JSON_INC) -I$(SCHEMA_DIR)/src
LDFLAGS     = -lcurl -lsqlite3

# --- Build modes ---
BUILD_DEBUG   = build/debug
BUILD_RELEASE = build/release

CXXFLAGS_DEBUG   = $(CXXFLAGS_COMMON) \
                   -O0 -g3 -DDEBUG \
                   -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
                   -Wdouble-promotion -Wformat=2 -Wnull-dereference \
                   -fsanitize=address,undefined -fno-omit-frame-pointer

CXXFLAGS_RELEASE = $(CXXFLAGS_COMMON) \
                   -O3 -DNDEBUG -march=native -flto=auto

LDFLAGS_DEBUG    = $(LDFLAGS) -fsanitize=address,undefined
LDFLAGS_RELEASE  = $(LDFLAGS) -flto=auto

# --- Sources ---
SRCS        = $(wildcard $(SRC_DIR)/*.cpp)
SCHEMA_SRCS = $(wildcard $(SCHEMA_DIR)/src/*.cpp)

DBG_OBJS        = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DEBUG)/%.o,$(SRCS))
DBG_SCHEMA_OBJS = $(patsubst $(SCHEMA_DIR)/src/%.cpp,$(BUILD_DEBUG)/schema-%.o,$(SCHEMA_SRCS))

REL_OBJS        = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_RELEASE)/%.o,$(SRCS))
REL_SCHEMA_OBJS = $(patsubst $(SCHEMA_DIR)/src/%.cpp,$(BUILD_RELEASE)/schema-%.o,$(SCHEMA_SRCS))

# --- Outputs ---
STATIC_DBG  = $(BUILD_DEBUG)/lib$(LIB_NAME).a
SHARED_DBG  = $(BUILD_DEBUG)/lib$(LIB_NAME).so
STATIC_REL  = $(BUILD_RELEASE)/lib$(LIB_NAME).a
SHARED_REL  = $(BUILD_RELEASE)/lib$(LIB_NAME).so

VERSION     = 0.1.0
PREFIX      = /usr/local
INSTALL_INC = $(PREFIX)/include
INSTALL_LIB = $(PREFIX)/lib
INSTALL_PC  = $(INSTALL_LIB)/pkgconfig

# ============================
# Targets
# ============================

all: debug release

debug:   $(STATIC_DBG) $(SHARED_DBG)
release: $(STATIC_REL) $(SHARED_REL)

# --- Debug rules ---
$(BUILD_DEBUG)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DEBUG)
	$(CXX) $(CXXFLAGS_DEBUG) -c $< -o $@

$(BUILD_DEBUG)/schema-%.o: $(SCHEMA_DIR)/src/%.cpp | $(BUILD_DEBUG)
	$(CXX) $(CXXFLAGS_DEBUG) -w -c $< -o $@

$(STATIC_DBG): $(DBG_OBJS) $(DBG_SCHEMA_OBJS)
	ar rcs $@ $^

$(SHARED_DBG): $(DBG_OBJS) $(DBG_SCHEMA_OBJS)
	$(CXX) -shared $^ -o $@ $(LDFLAGS_DEBUG)

# --- Release rules ---
$(BUILD_RELEASE)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_RELEASE)
	$(CXX) $(CXXFLAGS_RELEASE) -c $< -o $@

$(BUILD_RELEASE)/schema-%.o: $(SCHEMA_DIR)/src/%.cpp | $(BUILD_RELEASE)
	$(CXX) $(CXXFLAGS_RELEASE) -w -c $< -o $@

$(STATIC_REL): $(REL_OBJS) $(REL_SCHEMA_OBJS)
	ar rcs $@ $^

$(SHARED_REL): $(REL_OBJS) $(REL_SCHEMA_OBJS)
	$(CXX) -shared $^ -o $@ $(LDFLAGS_RELEASE)

# --- Examples (debug only) ---
examples: $(BUILD_DEBUG)/complete $(BUILD_DEBUG)/agent $(BUILD_DEBUG)/mcp $(BUILD_DEBUG)/models

$(BUILD_DEBUG)/%: examples/%.cpp $(STATIC_DBG)
	$(CXX) $(CXXFLAGS_DEBUG) $< -o $@ $(STATIC_DBG) $(LDFLAGS_DEBUG)

# --- Directories ---
$(BUILD_DEBUG):
	mkdir -p $(BUILD_DEBUG)

$(BUILD_RELEASE):
	mkdir -p $(BUILD_RELEASE)

# --- Tests ---
BUILD_TEST = build/test

TEST_CXXFLAGS = $(CXXFLAGS_DEBUG) -I$(SRC_DIR) -Itests

# test_public: links against libagt.a
TEST_PUBLIC_SRCS = tests/test_main.cpp tests/test_session.cpp tests/test_sqlite_session.cpp tests/test_provider_utils.cpp
TEST_PUBLIC_OBJS = $(patsubst tests/%.cpp,$(BUILD_TEST)/%.o,$(TEST_PUBLIC_SRCS))
TEST_PUBLIC      = $(BUILD_TEST)/test_public

# test_internals: includes .cpp directly, does NOT link libagt.a
TEST_INT_SRCS = tests/test_main.cpp tests/test_openai.cpp tests/test_anthropic.cpp \
                tests/test_gemini.cpp tests/test_runner.cpp
TEST_INT_OBJS = $(patsubst tests/%.cpp,$(BUILD_TEST)/%.o,$(TEST_INT_SRCS))
TEST_INT       = $(BUILD_TEST)/test_internals

# Schema validator objects (debug build, reused)
TEST_SCHEMA_OBJS = $(DBG_SCHEMA_OBJS)

$(BUILD_TEST):
	mkdir -p $(BUILD_TEST)

$(BUILD_TEST)/%.o: tests/%.cpp | $(BUILD_TEST)
	$(CXX) $(TEST_CXXFLAGS) -c $< -o $@

$(TEST_PUBLIC): $(TEST_PUBLIC_OBJS) $(STATIC_DBG) | $(BUILD_TEST)
	$(CXX) $(TEST_PUBLIC_OBJS) $(STATIC_DBG) -o $@ $(LDFLAGS_DEBUG)

$(TEST_INT): $(TEST_INT_OBJS) $(TEST_SCHEMA_OBJS) $(BUILD_DEBUG)/session.o | $(BUILD_TEST)
	$(CXX) $(TEST_INT_OBJS) $(TEST_SCHEMA_OBJS) $(BUILD_DEBUG)/session.o -o $@ $(LDFLAGS_DEBUG)

test: debug $(TEST_PUBLIC) $(TEST_INT)
	$(TEST_PUBLIC)
	$(TEST_INT)

# test_integration: links against libagt.a, hits live APIs
TEST_INTEG_SRCS = tests/test_main.cpp tests/test_integration.cpp
TEST_INTEG_OBJS = $(patsubst tests/%.cpp,$(BUILD_TEST)/%.o,$(TEST_INTEG_SRCS))
TEST_INTEG      = $(BUILD_TEST)/test_integration

$(TEST_INTEG): $(TEST_INTEG_OBJS) $(STATIC_DBG) | $(BUILD_TEST)
	$(CXX) $(TEST_INTEG_OBJS) $(STATIC_DBG) -o $@ $(LDFLAGS_DEBUG)

test-integration: debug $(TEST_INTEG)
	$(TEST_INTEG)

# --- Housekeeping ---
clean:
	rm -rf build

$(BUILD_RELEASE)/$(LIB_NAME).pc: agt.pc.in | $(BUILD_RELEASE)
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@VERSION@|$(VERSION)|g' $< > $@

install: release $(BUILD_RELEASE)/$(LIB_NAME).pc
	install -d $(INSTALL_INC)/agt
	install -m 644 $(INC_DIR)/agt/*.hpp $(INSTALL_INC)/agt
	install -d $(INSTALL_LIB)
	install -m 644 $(STATIC_REL) $(INSTALL_LIB)
	install -m 755 $(SHARED_REL) $(INSTALL_LIB)
	install -d $(INSTALL_PC)
	install -m 644 $(BUILD_RELEASE)/$(LIB_NAME).pc $(INSTALL_PC)
	ldconfig

uninstall:
	rm -rf $(INSTALL_INC)/agt
	rm -f $(INSTALL_LIB)/lib$(LIB_NAME).a
	rm -f $(INSTALL_LIB)/lib$(LIB_NAME).so
	rm -f $(INSTALL_PC)/$(LIB_NAME).pc
	ldconfig

.PHONY: all debug release examples test test-integration clean install uninstall
