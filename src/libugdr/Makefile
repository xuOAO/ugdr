# libugdr Makefile

# Output
TARGET_LIB := $(BIN_DIR)/libugdr.so

# Sources
LIB_SRCS := $(wildcard *.cpp)
COMMON_SRCS := $(wildcard ../common/*.cpp)
IPC_SRCS := $(wildcard ../common/ipc/*.cpp)

# Objects
LIB_OBJS := $(patsubst %.cpp, $(OBJ_DIR)/libugdr/%.o, $(LIB_SRCS))
COMMON_OBJS := $(patsubst ../common/%.cpp, $(OBJ_DIR)/common/%.o, $(COMMON_SRCS))
IPC_OBJS := $(patsubst ../common/ipc/%.cpp, $(OBJ_DIR)/common/ipc/%.o, $(IPC_SRCS))
OBJS := $(LIB_OBJS) $(COMMON_OBJS) $(IPC_OBJS)

# Flags
# libugdr needs -fPIC
CXXFLAGS += -fPIC
# Add include paths
CXXFLAGS += $(INC_FLAGS)

# Dependencies
DEPS := $(OBJS:.o=.d)

.PHONY: all clean

all: $(TARGET_LIB)

$(TARGET_LIB): $(OBJS)
	@mkdir -p $(dir $@)
	@echo "  LD      $@"
	@$(CXX) -shared -o $@ $^ $(LDFLAGS)

# Compile libugdr sources
$(OBJ_DIR)/libugdr/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX     $<"
	@$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Compile common sources
$(OBJ_DIR)/common/%.o: ../common/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX     $<"
	@$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Compile ipc sources
$(OBJ_DIR)/common/ipc/%.o: ../common/ipc/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX     $<"
	@$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

clean:
	rm -f $(TARGET_LIB) $(OBJS) $(DEPS)
