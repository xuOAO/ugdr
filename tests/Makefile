# Tests Makefile

# Sources
TEST_SRCS := main.cpp connection_test.cpp ipc_test.cpp resource_management.cpp

# Output Executable
TEST_EXE := $(BIN_DIR)/tests/run_tests

# Objects
TEST_OBJS := $(patsubst %.cpp, $(OBJ_DIR)/tests/%.o, $(TEST_SRCS))

# Dependencies
DEPS := $(TEST_OBJS:.o=.d)

# Flags
CXXFLAGS += $(INC_FLAGS) $(shell pkg-config --cflags gtest 2>/dev/null)
# Link against libugdr.so
LDFLAGS += -L$(BIN_DIR) -lugdr -Wl,-rpath=$(BIN_DIR) $(shell pkg-config --libs gtest 2>/dev/null) -lgtest -lpthread

.PHONY: all clean

all: $(TEST_EXE)

# Build each test executable
$(TEST_EXE): $(TEST_OBJS)
	@mkdir -p $(dir $@)
	@echo "  LD      $@"
	@$(CXX) -o $@ $^ $(LDFLAGS)

# Compile Test Sources
$(OBJ_DIR)/tests/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX     $<"
	@$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

clean:
	rm -f $(TEST_EXE) $(TEST_OBJS) $(DEPS)
