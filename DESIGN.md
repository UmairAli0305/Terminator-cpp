# Terminator — Design and Implementation Document

## 1. Introduction

Terminator is an X11-based terminal emulator and custom shell implemented in C++17.

Its architecture separates graphical interaction from shell command execution. The application supports two execution modes: thread mode and process mode.

The design emphasizes POSIX system programming, including processes, threads, signals, pipes, file descriptors, shared memory, and synchronization.

## 2. High-Level Architecture

```text
                  +----------------------+
                  |     X11 GUI          |
                  | Keyboard / Rendering |
                  | Tabs / Scrolling     |
                  +----------+-----------+
                             |
                   Command Processing
                             |
                  +----------v-----------+
                  |    Shell Session     |
                  | Parsing / Variables  |
                  | History / Job State  |
                  +----------+-----------+
                             |
                  +----------v-----------+
                  |   Command Executor   |
                  | fork / execvp        |
                  | pipe / dup2 / wait   |
                  +----------+-----------+
                             |
                  +----------v-----------+
                  |   Child Processes    |
                  | External Commands    |
                  +----------------------+
```

The GUI is responsible for displaying information, while shell components handle command interpretation and execution.

## 3. Source Code Organization

| Component | Responsibility |
|---|---|
| `src/main.cpp` | Entry point and execution-mode selection |
| `src/gui/TerminalWindow.cpp` | Thread-mode GUI and tab interaction |
| `src/process/ProcessTerminalWindow.cpp` | Process-mode GUI |
| `src/process/ProcessChannel.cpp` | Process-mode communication and execution |
| `src/process/AsyncProcessChannel.cpp` | Asynchronous process-channel handling |
| `src/shell/CommandExecutor.cpp` | Command execution, pipelines, redirection, and jobs |
| `src/shell/ShellSession.cpp` | Shell-session state |
| `src/shell/SharedHistory.h` | Shared history and synchronization |
| `src/shell/HistorySearch.h` | History search |
| `src/shell/FilenameCompletion.h` | Filename completion |
| `src/shell/ScriptVariables.h` | Variable and argument expansion |
| `src/shell/ScriptConditionals.h` | Conditional parsing and validation |
| `src/tabs/Tab.h` | Tab-related state |

Additional helper headers implement line editing, multiline input, clipboard support, and customizable keyboard shortcuts.

## 4. X11 GUI and Event Loop

The graphical interface is implemented using Xlib.

Important X11 functions include:

- `XOpenDisplay()` — connects to the X server.
- `XCreateSimpleWindow()` — creates the terminal window.
- `XSelectInput()` — registers event types.
- `XNextEvent()` — retrieves X11 events.
- `XDrawString()` — renders text.
- `XClearWindow()` — clears the display.
- `XFlush()` — flushes drawing requests.

The event-handling system processes keyboard input, redraw events, mouse interaction, and window events.

Each tab maintains its own display-related state.

The GUI displays command output from per-tab buffers and supports scrolling through previously generated output.

The application uses asynchronous execution and polling mechanisms to avoid blocking the GUI while commands run.

Only the GUI side interacts with X11.

## 5. Thread Execution Mode

Thread mode is selected using:

```bash
./terminator
```

In this mode, tabs operate within the same application process.

Each tab maintains independent shell state, including:

- Current working directory
- Command input
- Output buffer
- Foreground/background job state
- Command execution status

Threads share the application's address space.

Shared resources require synchronization to prevent concurrent modification.

History operations are synchronized through the shared-history subsystem, which uses semaphore protection and file locking where appropriate.

External commands are still executed through child processes using `fork()` and `execvp()`.

## 6. Process Execution Mode

Process mode is selected using:

```bash
./terminator useProcess=True
```

In this mode, each tab is associated with a separate child process.

The parent process manages:

- X11 window
- Keyboard events
- Tab switching
- Output rendering
- Scrolling
- Communication with child processes

Child processes manage shell command execution.

### Process-mode communication

```text
              Parent GUI Process
                      |
                 Command Pipe
                      |
                      v
               Child Tab Process
                      |
                Shell Execution
                      |
                Output / Status
                      |
                      v
              Parent GUI Process
```

Pipes transfer commands and output between the parent and child.

This design isolates tab execution so a child-process failure need not terminate other tabs or the parent GUI.

## 7. Inter-Process Communication

The implementation uses POSIX pipes and file descriptors.

### pipe()

Creates a unidirectional communication channel:

```cpp
int fd[2];
pipe(fd);
```

`fd[0]` is the read end, and `fd[1]` is the write end.

### dup2()

Redirects file descriptors:

```cpp
dup2(fd[1], STDOUT_FILENO);
```

This can connect command output to a pipe or file.

### fork()

Creates a child process.

### execvp()

Replaces a child process with the requested external executable.

### poll()

Monitors file descriptors for available data, allowing asynchronous communication without continuous busy-waiting.

Unused pipe ends are closed to avoid resource leaks and blocked readers.

## 8. Shared History and Synchronization

History must remain accessible across tabs and persist between application restarts.

### Data structures

History is represented logically as a bounded, ordered collection of command strings.

The shared-history subsystem serializes commands into a fixed-size shared-memory region.

The region is created using:

```cpp
mmap(..., MAP_SHARED | MAP_ANONYMOUS, ...);
```

It is initialized before child tab processes are created.

### Synchronization

On Linux, a process-shared semaphore is initialized using:

```cpp
sem_init(&lock, 1, 1);
```

The second argument enables sharing between processes.

Critical sections use:

```cpp
sem_wait(...);
// Access or update shared history
sem_post(...);
```

On macOS, the implementation uses a named POSIX semaphore through `sem_open()` because unnamed process-shared semaphores are unsupported.

The persistent history file is:

```text
~/.terminator_history
```

File operations are protected using `flock()` where appropriate.

### History limits

- Up to 10,000 stored commands
- Recent 1,000 commands displayed by `history`
- Up/Down navigation
- Ctrl+R searching

The fixed shared-memory capacity imposes an additional storage bound.

## 9. Command Parsing and Execution

The shell distinguishes built-in commands from external commands.

Built-ins are processed internally because some operations, such as changing the current directory, must update shell-session state.

External commands use:

```text
fork() -> execvp() -> wait/waitpid()
```

Each shell session tracks the most recent command's exit status, accessible using `$?`.

The parser also supports quoting, multiline commands, variables, and nested command substitution.

## 10. I/O Redirection

Input and output redirection are implemented using `open()` and `dup2()`.

For output redirection:

```cpp
int fd = open("output.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
dup2(fd, STDOUT_FILENO);
close(fd);
```

For input redirection:

```cpp
int fd = open("input.txt", O_RDONLY);
dup2(fd, STDIN_FILENO);
close(fd);
```

Redirection is performed in the appropriate command-execution context.

## 11. N-Stage Pipelines

A pipeline connects the output of one command to the input of the next.

Example:

```bash
ls | sort | head -5
```

Conceptually:

```text
ls --pipe--> sort --pipe--> head
```

For N commands, the executor creates the required pipes and connects adjacent stages using `dup2()`.

The implementation closes unused descriptors to avoid deadlocks and waits for child processes as appropriate.

Pipelines can also be combined with input/output redirection.

## 12. Process and Job Management

The shell supports foreground and background processes.

A command ending with `&` is started in the background.

Example:

```bash
sleep 30 &
```

The job-management subsystem maintains information about active and stopped jobs.

Supported operations include:

- `jobs` — display jobs
- `fg` — resume a job in the foreground
- `bg` — resume a stopped job in the background

Job state is maintained per shell session.

## 13. Signal Handling

The shell handles signals for foreground process management.

Important signals include:

- `SIGINT` — interrupt foreground execution, typically Ctrl+C.
- `SIGTSTP` — suspend foreground execution, typically Ctrl+Z.
- `SIGCONT` — resume a stopped process.
- `SIGCHLD` — child-process state changes.

Signal handling ensures that interrupting a command does not terminate the entire GUI.

Process and job state are updated as child processes exit, stop, or resume.

## 14. multiWatch

The `multiWatch` feature monitors multiple commands concurrently.

Example:

```bash
multiWatch ["date", "pwd"]
```

Its design uses child processes, pipes, and polling to collect output from monitored commands.

Commands are rerun periodically, approximately every two seconds.

Output is associated with the corresponding command.

Ctrl+C terminates monitoring.

## 15. Command History Search

Ctrl+R supports command-history retrieval.

The search logic prioritizes exact matches.

When an exact match is unavailable, the implementation searches for the best longest-common-substring match, subject to its minimum matching-length rule.

History navigation and searching operate on the shared command collection.

## 16. Filename Auto-Completion

Filename completion uses the current input token and cursor position.

### Data structures

The completion logic maintains a collection of matching filename candidates.

The general procedure is:

1. Identify the filename token at the cursor.
2. Search the relevant directory for matching filenames.
3. Store matching candidates.
4. If there is one match, complete the filename.
5. If there are multiple matches, calculate the longest common prefix.
6. Offer numbered candidate selection when ambiguity remains.
7. Preserve the rest of the input line after the completed token.

This supports completion of filenames and directory paths.

## 17. Keyboard Shortcuts and Configuration

The input editor supports cursor movement, deletion, history navigation, and clipboard operations.

Customizable bindings are loaded from:

```text
~/.terminatorrc
```

The configuration parser validates shortcut definitions and handles invalid or conflicting bindings.

## 18. Script Interpreter

Terminator contains its own script interpreter rather than delegating scripts to Bash.

Supported features include:

- Comments
- Variable assignments
- Environment-variable expansion
- Positional arguments
- `$#` and `$?`
- Command substitution
- Pipelines
- Redirection
- Nested `if`, `elif`, `else`, and `fi`
- Numeric comparisons
- File and directory tests

### Syntax validation

The interpreter validates conditional structure before executing a script.

For example, an unmatched `if` produces a syntax error rather than partially executing the script.

### Source execution

The `source` command executes a script in the current shell context, allowing variable assignments and directory changes to persist.

### Script execution methods

```bash
./script.sh
bash script.sh
source script.sh
```

These forms are interpreted internally by Terminator.

## 19. Design Decisions and Limitations

### No pseudoterminal

The project intentionally avoids PTYs to satisfy the assignment constraints.

Consequently, programs that require full terminal-control behavior may not work exactly as they do in conventional terminal emulators.

### No external shell delegation

Commands are not executed using `system()`, `popen()`, or `sh -c`.

The project uses its own parser and POSIX process-execution mechanisms.

### Limited script language

The script interpreter implements the required subset of shell functionality. Full Bash compatibility, loops, and shell functions are outside its scope.

### Platform compatibility

The Makefile selects appropriate X11 settings for macOS and Linux.

Semaphore initialization differs by platform because macOS does not support the Linux-style unnamed process-shared semaphore configuration.

### Testing scope

All 11 tasks were manually tested on macOS/XQuartz in thread and process modes.

Final Linux runtime verification, exhaustive concurrency testing, and large-history stress testing remain outstanding.

## 20. Conclusion

Terminator demonstrates the integration of an X11 graphical interface with POSIX process management, multithreading, IPC, synchronization, file-descriptor manipulation, signal handling, and custom shell interpretation.

The two execution modes provide different approaches to tab isolation while preserving a common user-facing shell interface.