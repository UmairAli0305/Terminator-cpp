
CXX = clang++
CXXFLAGS = -std=c++17 -Wall -Wextra -I/opt/X11/include
LDFLAGS = -L/opt/X11/lib -lX11

SRC = src/main.cpp src/gui/TerminalWindow.cpp src/shell/CommandExecutor.cpp src/shell/ShellSession.cpp
TARGET = terminator

all:
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)
