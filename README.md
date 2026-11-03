# Terminator — X11 Terminal Emulator and Custom Shell

## 1. Project Overview

Terminator is a terminal emulator and custom shell developed in C++17 using the X11 (Xlib) library and POSIX system calls.

The project supports multiple independent terminal tabs, command execution, pipelines, I/O redirection, job control, command history, auto-completion, keyboard shortcuts, and a custom shell-script interpreter.

Terminator provides two execution modes:

- **Thread mode (default):** Tabs operate within the same application process using threads.
- **Process mode:** Tabs use separate child processes communicating with the parent GUI through pipes.

The project does not use pseudoterminals (`forkpty()` or `openpty()`), `system()`, `popen()`, or `sh -c` for command execution.

## 2. Dependencies

The project requires:

- C++17-compatible compiler
- GNU Make
- X11 development libraries
- POSIX threads
- POSIX process and IPC facilities

### Ubuntu / Debian Linux

Install the dependencies:

```bash
sudo apt update
sudo apt install build-essential libx11-dev
```

An active X11 display is required to launch the graphical application.

### macOS

Install XQuartz from https://www.xquartz.org/.

Launch XQuartz before running Terminator.

The macOS build uses the X11 headers and libraries available at:

```text
/opt/X11/include
/opt/X11/lib
```

Apple Command Line Tools must also be installed.

## 3. Compilation

From the repository root:

```bash
make
```

This generates:

```text
./terminator
```

To rebuild:

```bash
make clean
make
```

The Makefile selects appropriate compiler and X11 settings for macOS and Linux.

## 4. Running Terminator

### Thread Mode

```bash
./terminator
```

This is the default mode.

### Process Mode

```bash
./terminator useProcess=True
```

In process mode, individual tabs execute through child processes, with communication handled by pipes.

Both modes provide the same shell interface and core functionality.

## 5. Implemented Features

### Task 1 — X11 GUI and Multiple Tabs

- X11 graphical window
- Multiple terminal tabs
- Independent working directory per tab
- Keyboard input and text rendering
- Output buffering and scrolling
- Tab switching and closing
- Responsive event handling

### Task 2 — Command Execution and Shell Built-ins

Supported commands include:

- `cd`
- `pwd`
- `echo`
- `exit`
- `history`
- `jobs`
- `fg`
- `bg`
- `source`
- `multiWatch`

External programs are executed using `fork()` and `execvp()`.

The shell maintains the previous command's exit status through `$?`.

### Task 3 — Multiline Input and Command Substitution

Features include:

- Multiline input using trailing backslashes
- Single-quoted and double-quoted strings
- Multiline quoted strings
- Command substitution using `$(...)`
- Backtick command substitution
- Nested command substitution
- Environment and shell-variable expansion

Example:

```bash
echo $(basename $(pwd))
echo "Current directory: $(pwd)"
echo '$HOME'
echo "$HOME"
```

### Task 4 — I/O Redirection

Supports input and output redirection:

```bash
echo Hello > output.txt
cat < output.txt
sort < input.txt > sorted.txt
```

Redirection uses POSIX file operations and `dup2()`.

### Task 5 — N-Stage Pipelines

Supports pipelines containing multiple commands.

Example:

```bash
seq 1 20 | grep 1 | sort -r | head -3
```

Pipelines can be combined with redirection:

```bash
seq 1 10 | sort -r > result.txt
```

### Task 6 — multiWatch

The `multiWatch` command monitors multiple commands concurrently.

Example:

```bash
multiWatch ["date", "pwd"]
```

Commands are executed periodically, approximately every two seconds, and their outputs are displayed separately.

Ctrl+C stops monitoring.

### Task 7 — Keyboard Shortcuts

| Shortcut | Function |
|---|---|
| Ctrl+T | Open new tab |
| Ctrl+Tab | Switch tabs |
| Ctrl+Shift+W | Close current tab |
| Ctrl+W | Delete previous word |
| Ctrl+A | Move cursor to beginning |
| Ctrl+E | Move cursor to end |
| Ctrl+U | Delete before cursor |
| Ctrl+K | Delete after cursor |
| Ctrl+L | Clear screen |
| Ctrl+D | Exit on empty input |
| Ctrl+C | Interrupt foreground command |
| Ctrl+Z | Suspend foreground command |
| Ctrl+R | Search command history |
| Ctrl+Shift+C | Copy selected text |
| Ctrl+Shift+V | Paste text |
| Up / Down | Navigate history |
| Tab | Filename completion |

Keyboard bindings can be configured through:

```text
~/.terminatorrc
```

For example:

```text
copy = Ctrl+Shift+Y
```

Invalid or conflicting bindings are handled by the configuration validation logic.

### Task 8 — Job Control

Supports:

- Foreground execution
- Background execution using `&`
- Job listing using `jobs`
- Bringing jobs to the foreground using `fg`
- Resuming stopped jobs using `bg`
- Ctrl+C and Ctrl+Z signal handling
- Independent jobs across tabs

Example:

```bash
sleep 30 &
jobs
fg %1
```

### Task 9 — Persistent Command History

Features include:

- History shared across tabs
- Persistence between application restarts
- Up to 10,000 stored commands
- Recent history display
- Up/Down history navigation
- Ctrl+R history searching
- Exact matching and longest-common-substring fallback

History is persisted at:

```text
~/.terminator_history
```

Shared memory and semaphore synchronization are used for active history management.

### Task 10 — Filename Auto-Completion

Features include:

- Tab-based filename completion
- Unique-match completion
- Longest common prefix for ambiguous matches
- Numbered candidate selection
- Directory-path completion
- Preservation of text after the cursor

### Task 11 — Custom Shell Script Interpreter

Terminator supports shell scripts through its own interpreter.

Supported invocation forms:

```bash
./script.sh
bash script.sh
source script.sh
```

The `bash script.sh` form is handled internally and does not launch the external Bash interpreter.

Supported scripting features include:

- Comments
- Variable assignment and expansion
- Environment variables
- Positional arguments (`$1`, `$2`, `$#`)
- Exit status (`$?`)
- Pipelines and redirection
- Command substitution
- Nested `if`, `elif`, `else`, and `fi`
- Numeric comparisons
- File and directory tests
- Syntax validation before execution

Example:

```bash
NAME=Terminator

if [ 10 -gt 5 ]; then
    echo "Condition passed"
fi

echo "Welcome to $NAME"
```

## 6. Example Commands

```bash
pwd
cd /tmp
echo Hello World
echo $HOME
echo $(pwd)
ls | sort
seq 1 5 | sort -r
echo Hello > output.txt
cat < output.txt
sleep 30 &
jobs
history
multiWatch ["date", "pwd"]
```

## 7. Project Structure

```text
Terminator-cpp/
├── src/
│   ├── main.cpp
│   ├── gui/
│   ├── process/
│   ├── shell/
│   └── tabs/
├── tests/
├── Makefile
├── README.md
├── DESIGN.md
└── .gitignore
```

The `tests/` directory contains scripts used during development and functional testing.

## 8. Testing

The project was manually tested on macOS with XQuartz in both thread and process modes.

Functional tests covered all 11 assignment tasks, including:

- Tab creation, switching, and scrolling
- Built-in and external commands
- Shared history
- Quoting and nested substitutions
- Redirection and pipelines
- multiWatch
- Keyboard shortcuts
- Signals and background jobs
- History search
- Filename completion
- Script interpretation and syntax validation

Linux build settings are included, but the latest project revision has not undergone final Linux runtime verification.

## 9. Known Limitations

- Terminator intentionally does not use a pseudoterminal (PTY). Full-screen interactive programs requiring PTY functionality may not behave like they do in a conventional terminal.
- The script interpreter supports the assignment's required subset of shell syntax, not complete Bash functionality. Loops and functions are not implemented.
- The shared-memory history region has a fixed capacity, so unusually large history entries may reduce the effective number of stored commands.
- Exhaustive concurrency and large-history stress testing have not been completed.
- Linux runtime behavior should be verified on the final target environment.

## 10. Design Documentation

Detailed information about the system architecture, execution modes, IPC, synchronization, data structures, and signal handling is available in `DESIGN.md`.