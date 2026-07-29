ifeq ($(ISA), native)
COMPILE_COMMANDS_SCRIPT ?= $(if $(NAVY_HOME),$(abspath $(NAVY_HOME)/../scripts/update_compile_commands.py),$(abspath ../../../scripts/update_compile_commands.py))

build/native.so: src/native.cpp
	mkdir -p build/
	g++ -std=c++17 -O2 -fPIC -shared -o build/native.so src/native.cpp -ldl -lSDL2
	@if [ "$(COMPILE_COMMANDS)" = "1" ]; then \
		python3 "$(COMPILE_COMMANDS_SCRIPT)" add \
			--directory "$(abspath .)" \
			--file "$(abspath src/native.cpp)" \
			--output "$(abspath build/native.so)" \
			-- g++ -std=c++17 -O2 -fPIC -shared -o "$(abspath build/native.so)" "$(abspath src/native.cpp)" -ldl -lSDL2; \
	fi

else
NAME = libos
SRCS = src/syscall.c src/nanos_abi.c src/thread_runtime.c src/crt0/crt0.c src/crt0/start.S
include $(NAVY_HOME)/Makefile
endif
