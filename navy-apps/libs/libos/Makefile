ifeq ($(ISA), native)
build/native.so: src/native.cpp
	mkdir -p build/
	g++ -std=c++17 -O2 -fPIC -shared -o build/native.so src/native.cpp -ldl -lSDL2

else
NAME = libos
SRCS = src/syscall.c src/crt0/crt0.c src/crt0/start.S
include $(NAVY_HOME)/Makefile
endif
