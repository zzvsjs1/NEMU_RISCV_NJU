.DEFAULT_GOAL = app

# Add necessary options if the target is a shared library
ifeq ($(SHARE),1)
SO = -so
CFLAGS  += -fPIC -fvisibility=hidden
LDFLAGS += -shared -fPIC
endif

WORK_DIR  = $(shell pwd)
BUILD_DIR = $(WORK_DIR)/build

INC_PATH := $(WORK_DIR)/include $(INC_PATH)
OBJ_DIR  = $(BUILD_DIR)/obj-$(NAME)$(SO)
BINARY   = $(BUILD_DIR)/$(NAME)$(SO)

# Compilation flags
ifeq ($(CC),clang)
CXX := clang++
else
CXX := g++
endif
LD := $(CXX)
INCLUDES = $(addprefix -I, $(INC_PATH))
CFLAGS  := -g -MMD -Wall $(INCLUDES) $(CFLAGS)
LDFLAGS := -g $(LDFLAGS)

OBJS = $(SRCS:%.c=$(OBJ_DIR)/%.o) $(CXXSRC:%.cc=$(OBJ_DIR)/%.o)

COMPILE_COMMANDS_SCRIPT ?= $(abspath $(NEMU_HOME)/../scripts/update_compile_commands.py)
ifeq ($(COMPILE_COMMANDS),1)
record_compile_command = @python3 "$(COMPILE_COMMANDS_SCRIPT)" add --directory "$(WORK_DIR)" --file "$(abspath $(1))" --output "$(abspath $(2))" -- $(3)
else
record_compile_command = @:
endif

# Compilation patterns
$(OBJ_DIR)/%.o: %.c
	@echo + CC $<
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c -o $@ $<
	$(call record_compile_command,$<,$@,$(CC) $(CFLAGS) -c -o $(abspath $@) $(abspath $<))
	$(call call_fixdep, $(@:.o=.d), $@)

$(OBJ_DIR)/%.o: %.cc
	@echo + CXX $<
	@mkdir -p $(dir $@)
	@$(CXX) $(CFLAGS) $(CXXFLAGS) -c -o $@ $<
	$(call record_compile_command,$<,$@,$(CXX) $(CFLAGS) $(CXXFLAGS) -c -o $(abspath $@) $(abspath $<))
	$(call call_fixdep, $(@:.o=.d), $@)

# Dependencies
-include $(OBJS:.o=.d)

# Some convenient rules

.PHONY: app clean

app: $(BINARY)

$(BINARY):: $(OBJS) $(ARCHIVES)
	@echo + LD $@
	@$(LD) -o $@ $(OBJS) $(LDFLAGS) $(ARCHIVES) $(LIBS)

clean:
	-rm -rf $(BUILD_DIR)
