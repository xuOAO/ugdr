# Daemon Makefile

# Output
TARGET_BIN := $(BIN_DIR)/ugdr-daemon

# Dependencies
# DPDK
DPDK_CFLAGS := $(shell pkg-config --cflags libdpdk)
DPDK_LDFLAGS := $(shell pkg-config --libs libdpdk)

# CUDA
CUDA_PATH ?= /usr/local/cuda
CUDA_LDFLAGS := -L$(CUDA_PATH)/lib64 -lcudart -lcuda

# Sources
# Recursively find sources in daemon directory
DAEMON_CPP_SRCS := $(shell find . -name "*.cpp")
DAEMON_CU_SRCS := $(shell find . -name "*.cu")
COMMON_SRCS := $(shell find ../common -name "*.cpp")

# Objects
DAEMON_CPP_OBJS := $(patsubst ./%.cpp, $(OBJ_DIR)/daemon/%.o, $(DAEMON_CPP_SRCS))
DAEMON_CU_OBJS := $(patsubst ./%.cu, $(OBJ_DIR)/daemon/%.o, $(DAEMON_CU_SRCS))
COMMON_OBJS := $(patsubst ../common/%.cpp, $(OBJ_DIR)/common/%.o, $(COMMON_SRCS))

OBJS := $(DAEMON_CPP_OBJS) $(DAEMON_CU_OBJS) $(COMMON_OBJS)

# Flags
# Add DPDK and Include paths to CXXFLAGS
CXXFLAGS += $(INC_FLAGS) $(DPDK_CFLAGS)
NVCCFLAGS += $(INC_FLAGS)

# Linker Flags
LDFLAGS += $(DPDK_LDFLAGS) $(CUDA_LDFLAGS) -lpthread

# Dependencies
DEPS := $(OBJS:.o=.d)

.PHONY: all clean

all: $(TARGET_BIN)

$(TARGET_BIN): $(OBJS)
	@mkdir -p $(dir $@)
	@echo "  LD      $@"
	@$(CXX) -o $@ $^ $(LDFLAGS)

# Compile Daemon C++ Sources
$(OBJ_DIR)/daemon/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX     $<"
	@$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Compile Daemon CUDA Sources
# NVCC dependency generation is a bit tricky, usually -M or -MM works but output needs handling
# Simple way: just compile. For advanced dependency tracking with NVCC, more logic is needed.
# Here we stick to basic compilation for .cu
$(OBJ_DIR)/daemon/%.o: %.cu
	@mkdir -p $(dir $@)
	@echo "  NVCC    $<"
	@$(NVCC) $(NVCCFLAGS) -c $< -o $@

# Compile Common Sources (Re-use rule if possible, but distinct here for clarity/safety if flags differ)
# Note: In libugdr makefile we also compile common sources.
# If we run parallel builds, there might be a race if they write to same .o file.
# BUT, we defined OBJ_DIR as global.
# To avoid race conditions and potential flag mismatches (fPIC vs no-fPIC),
# we should ideally separate objects or ensure flags are identical.
# Here, libugdr forces -fPIC. Daemon doesn't strictly need it.
# To play safe and allow sharing, let's just add -fPIC here too or use different obj dir?
# The user asked to put objects in build/obj.
# Let's check if we can share. If we share, we must use -fPIC for both.
# Adding -fPIC to daemon compilation is safe.
CXXFLAGS += -fPIC

$(OBJ_DIR)/common/%.o: ../common/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX     $<"
	@$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

clean:
	rm -f $(TARGET_BIN) $(OBJS) $(DEPS)
