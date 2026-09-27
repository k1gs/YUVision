CXX ?= c++
PKG_CONFIG ?= pkg-config
CXXFLAGS ?= -O3 -DNDEBUG
CXXFLAGS += -std=c++20 -Wall -Wextra -Wpedantic $(shell $(PKG_CONFIG) --cflags sdl3 alsa gl)
LDLIBS += $(shell $(PKG_CONFIG) --libs sdl3 alsa gl) -pthread

.PHONY: all clean

all: yuvision

yuvision: src/linux_main.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

clean:
	rm -f yuvision
