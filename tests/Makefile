# Tests Makefile

# Sources
TEST_SRCS := $(wildcard *.cpp)

# Output Executables (one per cpp file)
TEST_EXES := $(patsubst %.cpp, $(BIN_DIR)/tests/%, $(TEST_SRCS))

# Objects
TEST_OBJS := $(patsubst %.cpp, $(OBJ_DIR)/tests/%.o, $(TEST_SRCS))

# Dependencies
DEPS := $(TEST_OBJS:.o=.d)

# Flags
CXXFLAGS += $(INC_FLAGS)
# Link against libugdr.so
LDFLAGS += -L$(BIN_DIR) -lugdr -Wl,-rpath=$(BIN_DIR)

.PHONY: all clean

all: $(TEST_EXES)

# Build each test executable
$(BIN_DIR)/tests/%: $(OBJ_DIR)/tests/%.o
	@mkdir -p $(dir $@)
	@echo "  LD      $@"
	@$(CXX) -o $@ $< $(LDFLAGS)

# Compile Test Sources
$(OBJ_DIR)/tests/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX     $<"
	@$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

clean:
	rm -f $(TEST_EXES) $(TEST_OBJS) $(DEPS)
