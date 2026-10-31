
# Terminator: portable build for Linux and macOS
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
CXX := clang++
X11_CFLAGS := -I/opt/X11/include
X11_LIBS := -L/opt/X11/lib -lX11
else
CXX := g++
X11_CFLAGS :=
X11_LIBS := -lX11
endif

CXXFLAGS := -std=c++17 -Wall -Wextra -pthread $(X11_CFLAGS)
LDFLAGS := $(X11_LIBS) -pthread

SRC := src/main.cpp src/gui/TerminalWindow.cpp src/shell/CommandExecutor.cpp src/shell/ShellSession.cpp src/process/ProcessChannel.cpp src/process/AsyncProcessChannel.cpp src/process/ProcessTerminalWindow.cpp
TARGET := terminator

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET)
