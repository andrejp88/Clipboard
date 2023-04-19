/*  The Clipboard Project - Cut, copy, and paste anything, anywhere, all from the terminal.
    Copyright (C) 2023 Jackson Huff and other contributors on GitHub.com
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.*/
#include "clipboard.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <clipboard/fork.hpp>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <locale>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#include <shlobj.h>
#include <windows.h>
#define isatty _isatty
#define fileno _fileno
#include "windows.hpp"
#endif

#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

Forker forker {};

GlobalFilepaths global_path;
Clipboard path;
Copying copying;

bool output_silent = false;
bool progress_silent = false;
bool confirmation_silent = false;
bool no_color = false;
bool no_emoji = false;

std::vector<std::string> arguments;

std::string clipboard_invocation;

std::string clipboard_name = "0";

std::string locale;

Action action;

IOType io_type;

Successes successes;

IsTTY is_tty;

std::condition_variable cv;
std::mutex m;
std::atomic<ProgressState> progress_state;

std::array<std::pair<std::string_view, std::string_view>, 7> colors = {
        {{"[error]", "\033[38;5;196m"},    // red
         {"[success]", "\033[38;5;40m"},   // green
         {"[progress]", "\033[38;5;214m"}, // yellow
         {"[info]", "\033[38;5;51m"},      // blue
         {"[help]", "\033[38;5;213m"},     // pink
         {"[bold]", "\033[1m"},
         {"[blank]", "\033[0m"}}};

#if defined(_WIN64) || defined(_WIN32)
UINT old_code_page;
#endif

bool stopIndicator(bool change_condition_variable) {
    ProgressState expect = ProgressState::Active;

    if (!change_condition_variable) return progress_state.exchange(ProgressState::Cancel) == expect;

    if (!progress_state.compare_exchange_strong(expect, ProgressState::Done)) return false;

    cv.notify_one();
    indicator.join();
    return true;
}

TerminalSize thisTerminalSize() {
#if defined(_WIN32) || defined(_WIN64)
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
    return TerminalSize(csbi.srWindow.Bottom - csbi.srWindow.Top + 1, csbi.srWindow.Right - csbi.srWindow.Left + 1);
#elif defined(__linux__) || defined(__unix__) || defined(__APPLE__)
    struct winsize w;
    ioctl(STDERR_FILENO, TIOCGWINSZ, &w);
    return TerminalSize(w.ws_row, w.ws_col);
#endif
    return TerminalSize(80, 24);
}

std::string fileContents(const fs::path& path) {
    std::stringstream buffer;
    buffer << std::ifstream(path, std::ios::binary).rdbuf();
    return buffer.str();
}

bool isPersistent(const std::string& clipboard) {
    return clipboard.find_first_of("_") != std::string::npos;
}

std::string pipedInContent() {
    std::string content;
#if !defined(_WIN32) && !defined(_WIN64)
    int len = -1;
    int stdinFd = fileno(stdin);
    constexpr int bufferSize = 65536;
    std::array<char, bufferSize> buffer;
    while (len != 0) {
        len = read(stdinFd, buffer.data(), bufferSize);
        content.append(buffer.data(), len);
        successes.bytes += len;
    }
#elif defined(_WIN32) || defined(_WIN64)
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD dwRead;
    CHAR chBuf[1024];
    BOOL bSuccess = FALSE;

    while (true) {
        bSuccess = ReadFile(hStdin, chBuf, 1024, &dwRead, NULL);
        if (!bSuccess || dwRead == 0) break;
        content.append(chBuf, dwRead);
        successes.bytes += dwRead;
    }
#endif
    return content;
}

size_t writeToFile(const fs::path& path, const std::string& content, bool append) {
    std::ofstream file(path, append ? std::ios::app : std::ios::trunc | std::ios::binary);
    file << content;
    return content.size();
}

void deduplicate(auto& items) {
    std::sort(items.begin(), items.end());
    items.erase(std::unique(items.begin(), items.end()), items.end());
}

bool userIsARobot() {
    return !is_tty.err || !is_tty.in || !is_tty.out || getenv("CI");
}

bool isAWriteAction() {
    using enum Action;
    return action != Paste && action != Show && action != Note && action != Status;
}

bool isAClearingAction() {
    using enum Action;
    return action == Copy || action == Cut || action == Clear;
}

auto thisPID() {
#if defined(_WIN32) || defined(_WIN64)
    return GetCurrentProcessId();
#elif defined(__linux__) || defined(__unix__) || defined(__APPLE__)
    return getpid();
#endif
}

void getLock() {
    if (fs::exists(path.metadata.lock)) {
        auto pid = std::stoi(fileContents(path.metadata.lock));
        while (true) {
#if defined(_WIN32) || defined(_WIN64)
            if (WaitForSingleObject(OpenProcess(SYNCHRONIZE, FALSE, pid), 0) == WAIT_OBJECT_0) break;
#elif defined(__linux__) || defined(__unix__) || defined(__APPLE__)
            if (kill(pid, 0) == -1) break;
#endif
            if (!fs::exists(path.metadata.lock)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
    writeToFile(path.metadata.lock, std::to_string(thisPID()));
}

void releaseLock() {
    fs::remove(path.metadata.lock);
}

[[nodiscard]] CopyPolicy userDecision(const std::string& item) {
    using enum CopyPolicy;

    if (userIsARobot()) return ReplaceAll;

    fprintf(stderr, item_already_exists_message().data(), item.data());
    std::string decision;
    while (true) {
        std::getline(std::cin, decision);
        fprintf(stderr, "%s", formatMessage("[blank]").data());

        if (decision == "y" || decision == "yes")
            return ReplaceOnce;
        else if (decision == "a" || decision == "all")
            return ReplaceAll;
        else if (decision == "n" || decision == "no")
            return SkipOnce;
        else if (decision == "s" || decision == "skip")
            return SkipAll;
        else
            fprintf(stderr, "%s", bad_response_message().data());
    }
}

void clearTempDirectory(bool force_clear = false) {
    using enum Action;
    if (force_clear || action == Cut || action == Copy) {
        fs::remove(path.metadata.originals);
        if (action == Clear && fs::is_regular_file(path.data.raw)) {
            successes.bytes += fs::file_size(path.data.raw);
            fs::remove(path.data.raw);
        }
        for (const auto& entry : fs::directory_iterator(path.data)) {
            fs::remove_all(entry.path());
            if (action == Clear) {
                incrementSuccessesForItem(entry);
            }
        }
    }
}

void convertFromGUIClipboard(const std::string& text) {
    if (fs::is_regular_file(path.data.raw) && fileContents(path.data.raw) == text) return;
    clearTempDirectory(true);
    writeToFile(path.data.raw, text);
}

void convertFromGUIClipboard(const ClipboardPaths& clipboard) {
    // Only clear the temp directory if all files in the clipboard are outside the temp directory
    // This avoids the situation where we delete the very files we're trying to copy
    auto allOutsideFilepath = std::all_of(clipboard.paths().begin(), clipboard.paths().end(), [](auto& path) {
        auto relative = fs::relative(path, ::path.data);
        auto firstElement = *(relative.begin());
        return firstElement == fs::path("..");
    });

    if (allOutsideFilepath) clearTempDirectory(true);

    for (auto&& path : clipboard.paths()) {
        if (!fs::exists(path)) continue;

        auto target = ::path.data / path.filename();

        if (fs::exists(target) && fs::equivalent(path, target)) continue;

        try {
            fs::copy(path, target, copying.opts | fs::copy_options::create_hard_links);
        } catch (const fs::filesystem_error& e) {
            try {
                fs::copy(path, target, copying.opts);
            } catch (const fs::filesystem_error& e) {} // Give up
        }
    }

    if (clipboard.action() == ClipboardPathsAction::Cut) {
        std::ofstream originalFiles {path.metadata.originals};
        for (auto&& path : clipboard.paths())
            originalFiles << path.string() << std::endl;
    }
}

[[nodiscard]] ClipboardContent thisClipboard() {
    if (fs::exists(path.metadata.originals) && GUIClipboardSupportsCut) {
        std::ifstream originalFiles {path.metadata.originals};
        std::vector<fs::path> files;

        for (std::string line; !originalFiles.eof();) {
            std::getline(originalFiles, line);
            if (!line.empty()) files.emplace_back(line);
        }

        return {std::move(files), ClipboardPathsAction::Cut};
    }

    if (!copying.buffer.empty()) return {copying.buffer, copying.mime};

    if (fs::exists(path.data.raw) && !fs::is_empty(path.data.raw)) return {fileContents(path.data.raw), std::string(inferMIMEType(fileContents(path.data.raw)).value_or("text/plain"))};

    if (!copying.items.empty()) {
        std::vector<fs::path> paths;

        paths.assign(fs::directory_iterator(path.data), fs::directory_iterator {});

        return ClipboardContent(ClipboardPaths(std::move(paths)));
    }

    return {};
}

void setupHandlers() {
    atexit([] {
        releaseLock();
        stopIndicator(true);
#if defined(_WIN64) || defined(_WIN32)
        SetConsoleOutputCP(old_code_page);
#endif
    });

    signal(SIGINT, [](int dummy) {
        if (!stopIndicator(false)) {
            // Indicator thread is not currently running. TODO: Write an unbuffered newline, and maybe a cancelation
            // message, directly to standard error. Note: There is no standard C++ interface for this, so this requires
            // an OS call.
            releaseLock();
            _exit(EXIT_FAILURE);
        } else {
            indicator.join();
            exit(EXIT_FAILURE);
        }
    });

    forker.atFork([]() {
        // As the indicator thread still exists in memory in the forked process,
        // the main process exiting creates an exception because it has not been joined in the X11 process.
        // So we need to remove it from our forked memory
        // indicator.detach();
    });

    forker.atNonFork([]() {
        // If the process didn't fork, we need to stop the indicator thread to ensure it won't
        // keep running in the background while we perform the required work
        stopIndicator();
    });
}

void setLocale() {
    try {
        locale = getenv("CLIPBOARD_LOCALE") ? getenv("CLIPBOARD_LOCALE") : std::locale("").name();
        std::locale::global(std::locale(locale));
    } catch (...) {}
    if (locale.substr(0, 2) == "es")
        setLanguageES();
    else if (locale.substr(0, 2) == "pt")
        setLanguagePT();
    else if (locale.substr(0, 2) == "tr")
        setLanguageTR();
}

void setClipboardName() {
    if (arguments.empty()) return;
    std::string temp = arguments.at(0);
    if (temp.find_first_of("_0123456789") != std::string::npos)
        clipboard_name = temp.substr(temp.find_first_of("_0123456789"));
    else
        return;
    arguments.at(0) = arguments.at(0).substr(0, arguments.at(0).length() - clipboard_name.length());
}

void setupVariables(int& argc, char* argv[]) {
    is_tty.in = getenv("CLIPBOARD_FORCETTY") ? true : isatty(fileno(stdin));
    is_tty.out = getenv("CLIPBOARD_FORCETTY") ? true : isatty(fileno(stdout));
    is_tty.err = getenv("CLIPBOARD_FORCETTY") ? true : isatty(fileno(stderr));

#if defined(_WIN64) || defined(_WIN32)
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE); // Windows terminal color compatibility
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    if (!SetConsoleMode(hOut, (dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT))) {
        no_color = true;
    }
    old_code_page = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8); // fix broken accents on Windows
#endif
    global_path.home = getenv("USERPROFILE") ? getenv("USERPROFILE") : getenv("HOME");

    bool CLICOLOR = getenv("CLICOLOR") && !strcmp(getenv("CLICOLOR"), "0");
    bool NO_COLOR = getenv("NO_COLOR");
    bool CLICOLOR_FORCE = getenv("CLICOLOR_FORCE") && strcmp(getenv("CLICOLOR_FORCE"), "0");
    bool FORCE_COLOR = getenv("FORCE_COLOR");

    no_color = (NO_COLOR || CLICOLOR) && !FORCE_COLOR && !CLICOLOR_FORCE;

    no_emoji = getenv("CLIPBOARD_NOEMOJI") ? true : false;

    output_silent = getenv("CLIPBOARD_SILENT") ? true : false;

    progress_silent = getenv("CLIPBOARD_NOPROGRESS") ? true : false;

    if (auto setting = getenv("CLIPBOARD_THEME"); setting != nullptr) setTheme(std::string(setting));

    if (argc == 0) return;

    arguments.assign(argv + 1, argv + argc);

    clipboard_invocation = argv[0];
}

void syncWithGUIClipboard(bool force) {
    if ((!isAClearingAction() && clipboard_name == constants.default_clipboard_name && !getenv("CLIPBOARD_NOGUI")) || (force && !getenv("CLIPBOARD_NOGUI"))) {
        using enum ClipboardContentType;
        auto content = getGUIClipboard();
        if (content.type() == Text) {
            convertFromGUIClipboard(content.text());
            copying.mime = !content.mime().empty() ? content.mime() : inferMIMEType(content.text()).value_or("text/plain");
        } else if (content.type() == Paths) {
            convertFromGUIClipboard(content.paths());
            copying.mime = !content.mime().empty() ? content.mime() : "text/uri-list";
        }
    }
}

template <typename T>
[[nodiscard]] auto flagIsPresent(const std::string_view& flag, const std::string_view& shortcut = "") {
    for (const auto& entry : arguments) {
        if (entry == flag || entry == (std::string(shortcut).append(flag))) {
            if constexpr (std::is_same_v<T, std::string>) {
                std::string temp(*arguments.erase(std::find(arguments.begin(), arguments.end(), entry)));
                arguments.erase(std::find(arguments.begin(), arguments.end(), temp));
                return temp;
            } else {
                arguments.erase(std::find(arguments.begin(), arguments.end(), entry));
                return true;
            }
        }
    }

    if constexpr (std::is_same_v<T, std::string>)
        return std::string();
    else
        return false;
}

Action getAction() {
    using enum Action;
    if (arguments.size() >= 1) {
        for (const auto& entry : {Cut, Copy, Paste, Clear, Show, Edit, Add, Remove, Note, Swap, Status, Info, Load}) {
            if (flagIsPresent<bool>(actions[entry], "--") || flagIsPresent<bool>(action_shortcuts[entry], "-")) {
                return entry;
            }
        }
        printf(no_valid_action_message().data(), arguments.at(0).data(), clipboard_invocation.data(), clipboard_invocation.data());
        exit(EXIT_FAILURE);
    } else if (!is_tty.in) {
        return Copy;
    } else if (!is_tty.out) {
        return Paste;
    }
    return Status;
}

IOType getIOType() {
    using enum Action;
    using enum IOType;
    if (action == Cut || action == Copy || action == Add) {
        if (copying.items.size() == 1 && !fs::exists(copying.items.at(0))) return Text;
        if (!is_tty.in) return Pipe;
    } else if (action == Paste || action == Show || action == Clear || action == Edit) {
        if (!is_tty.out) return Pipe;
    } else if (action == Remove) {
        if (copying.items.size() == 1) return Text;
        if (!is_tty.in) return Pipe;
    } else if (action == Note) {
        if (!is_tty.in && copying.items.size() == 0) return Pipe;
        return Text;
    } else if (action == Info) {
        return Text;
    }
    return File;
}

void setFlags() {
    if (flagIsPresent<bool>("--fast-copy") || flagIsPresent<bool>("-fc")) copying.use_safe_copy = false;
    if (flagIsPresent<bool>("--no-progress") || flagIsPresent<bool>("-np")) progress_silent = true;
    if (flagIsPresent<bool>("--no-confirmation") || flagIsPresent<bool>("-nc")) confirmation_silent = true;
    if (flagIsPresent<bool>("--ee")) {
        printf("%s", formatMessage("[bold][info]https://youtu.be/Lg_Pn45gyMs\n[blank]").data());
        exit(EXIT_SUCCESS);
    }
    if (auto flag = flagIsPresent<std::string>("-c"); flag != "") clipboard_name = flag;
    if (auto flag = flagIsPresent<std::string>("--clipboard"); flag != "") clipboard_name = flag;
    if (flagIsPresent<bool>("-h") || flagIsPresent<bool>("help", "--")) {
        printf(help_message().data(), constants.clipboard_version.data(), constants.clipboard_commit.data());
        exit(EXIT_SUCCESS);
    }
    if (auto pos = std::find_if(arguments.begin(), arguments.end(), [](const auto& entry) { return entry == "--"; }); pos != arguments.end()) arguments.erase(pos);
}

void verifyAction() {
    if (io_type == IOType::Pipe && arguments.size() >= 2 && action != Action::Show) {
        fprintf(stderr, redirection_no_items_message().data(), clipboard_invocation.data());
        exit(EXIT_FAILURE);
    }
}

void setFilepaths() {
    global_path.temporary = (getenv("CLIPBOARD_TMPDIR") ? getenv("CLIPBOARD_TMPDIR")
                             : getenv("TMPDIR")         ? getenv("TMPDIR")
                                                        : fs::temp_directory_path())
                            / constants.temporary_directory_name;

    global_path.persistent = (getenv("CLIPBOARD_PERSISTDIR") ? getenv("CLIPBOARD_PERSISTDIR") : (getenv("XDG_CACHE_HOME") ? getenv("XDG_CACHE_HOME") : global_path.home))
                             / constants.persistent_directory_name;

    path = Clipboard(clipboard_name);
}

void checkForNoItems() {
    using enum Action;
    if ((action == Cut || action == Copy || action == Add || action == Remove) && io_type == IOType::File && copying.items.size() < 1) {
        printf(choose_action_items_message().data(), actions[action].data(), actions[action].data(), clipboard_invocation.data(), actions[action].data());
        exit(EXIT_FAILURE);
    }
    if ((action == Paste || action == Show || action == Clear) && (!fs::exists(path.data) || fs::is_empty(path.data))) {
        PerformAction::status();
        exit(EXIT_SUCCESS);
    }
}

void setupIndicator() {
    if (!is_tty.err || output_silent || progress_silent) return;

    fprintf(stderr, "\033]0;%s - Clipboard\007", doing_action[action].data()); // set the terminal title
    fprintf(stderr, "\033[?25l");                                              // hide the cursor
    fflush(stderr);

    std::unique_lock<std::mutex> lock(m);
    int output_length = 0;
    const std::array<std::string_view, 22> spinner_steps {"╸         ", "━         ", "╺╸        ", " ━        ", " ╺╸       ", "  ━       ", "  ╺╸      ", "   ━      ",
                                                          "   ╺╸     ", "    ━     ", "    ╺╸    ", "     ━    ", "     ╺╸   ", "      ━   ", "      ╺╸  ", "       ━  ",
                                                          "       ╺╸ ", "        ━ ", "        ╺╸", "         ━", "         ╺", "          "};
    auto itemsToProcess = [&] {
        return std::distance(fs::directory_iterator(path.data), fs::directory_iterator());
    };
    static size_t items_size = (action == Action::Cut || action == Action::Copy) ? copying.items.size() : itemsToProcess();
    if (items_size == 0) items_size++;
    auto percent_done = [&] {
        return std::to_string(((successes.files + successes.directories + copying.failedItems.size()) * 100) / items_size) + "%";
    };
    for (int i = 0; progress_state == ProgressState::Active; i == 21 ? i = 0 : i++) {
        auto display_progress = [&](const auto& formattedNum) {
            output_length = fprintf(stderr, working_message().data(), doing_action[action].data(), formattedNum, spinner_steps.at(i).data());
            fflush(stderr);
            cv.wait_for(lock, std::chrono::milliseconds(25), [&] { return progress_state != ProgressState::Active; });
        };

        if (io_type == IOType::File)
            display_progress(percent_done().data());
        else if (io_type == IOType::Pipe)
            display_progress(formatBytes(successes.bytes.load(std::memory_order_relaxed)).data());
    }
    fprintf(stderr, "\r%*s\r", output_length, "");
    fprintf(stderr, "\033[?25h"); // restore the cursor
    fflush(stderr);
    if (progress_state == ProgressState::Cancel) {
        fprintf(stderr, cancelled_message().data(), actions[action].data());
        fflush(stderr);
        releaseLock();
        _exit(EXIT_FAILURE);
    }
    fflush(stderr);
}

void startIndicator() { // If cancelled, leave cancelled
    ProgressState expect = ProgressState::Done;
    progress_state.compare_exchange_strong(expect, ProgressState::Active);
    indicator = std::thread(setupIndicator);
}

unsigned long long totalItemSize() {
    unsigned long long total_item_size = 0;
    if ((action == Action::Cut || action == Action::Copy || action == Action::Add) && io_type == IOType::File) {
        for (const auto& i : copying.items) {
            try {
                if (fs::is_directory(i))
                    for (const auto& entry : fs::recursive_directory_iterator(i))
                        total_item_size += entry.is_regular_file() ? entry.file_size() : 16;
                else
                    total_item_size += fs::is_regular_file(i) ? fs::file_size(i) : 16;
            } catch (const fs::filesystem_error& e) {
                copying.failedItems.emplace_back(i.string(), e.code());
            }
        }
    } else if (action == Action::Paste && io_type == IOType::File) {
        for (const auto& entry : fs::recursive_directory_iterator(path.data))
            total_item_size += entry.is_regular_file() ? entry.file_size() : 16;
    }
    return total_item_size;
}

void checkItemSize(unsigned long long total_item_size) {
    unsigned long long space_available = 0;
    using enum Action;
    if ((action == Cut || action == Copy || action == Add) && io_type == IOType::File)
        space_available = fs::space(path.data).available;
    else if (action == Action::Paste && io_type == IOType::File)
        space_available = fs::space(fs::current_path()).available;
    if (total_item_size > space_available) {
        stopIndicator();
        fprintf(stderr, not_enough_storage_message().data(), actions[action].data(), total_item_size / (1024.0 * 1024.0), space_available / (1024.0 * 1024.0));
        exit(EXIT_FAILURE);
    }
}

void removeOldFiles() {
    if (!fs::is_regular_file(path.metadata.originals)) return;
    std::ifstream files(path.metadata.originals);
    std::string line;
    while (std::getline(files, line)) {
        try {
            fs::remove_all(line);
        } catch (const fs::filesystem_error& e) {
            copying.failedItems.emplace_back(line, e.code());
        }
    }
    files.close();
    if (copying.failedItems.empty()) fs::remove(path.metadata.originals);
}

void performAction() {
    using enum IOType;
    using enum Action;
    using namespace PerformAction;
    if (io_type == File) {
        if (action == Copy || action == Cut)
            copy();
        else if (action == Paste)
            paste();
        else if (action == Clear)
            clear();
        else if (action == Show)
            show();
        else if (action == Edit)
            edit();
        else if (action == Add)
            addFiles();
        else if (action == Remove)
            removeRegex();
        else if (action == Status)
            status();
        else if (action == Load)
            load();
    } else if (io_type == Pipe) {
        if (action == Copy || action == Cut)
            pipeIn();
        else if (action == Paste)
            pipeOut();
        else if (action == Add)
            addData();
        else if (action == Remove)
            removeRegex();
        else if (action == Note)
            notePipe();
        else if (action == Show)
            showFilepaths();
    } else if (io_type == Text) {
        if (action == Copy || action == Cut)
            copyText();
        else if (action == Add)
            addData();
        else if (action == Remove)
            removeRegex();
        else if (action == Note)
            noteText();
        else if (action == Info)
            info();
    }
}

std::string getMIMEType() {
    if (io_type == IOType::File) {
        return "text/uri-list";
    } else if (io_type == IOType::Pipe || io_type == IOType::Text) {
        return std::string(inferMIMEType(copying.buffer).value_or("text/plain"));
    }
    return "text/plain";
}

void updateGUIClipboard(bool force) {
    if ((isAWriteAction() && clipboard_name == constants.default_clipboard_name && !getenv("CLIPBOARD_NOGUI"))
        || (force && !getenv("CLIPBOARD_NOGUI"))) { // only update GUI clipboard on write operations
        writeToGUIClipboard(thisClipboard());
    }
}

void showFailures() {
    if (copying.failedItems.size() <= 0) return;
    TerminalSize available(thisTerminalSize());
    available.rows -= clipboard_failed_many_message.rawLength() / available.columns;

    if (copying.failedItems.size() > available.rows) available.rows -= and_more_fails_message.rawLength() / available.columns;

    available.rows -= 3;
    printf(copying.failedItems.size() > 1 ? clipboard_failed_many_message().data() : clipboard_failed_one_message().data(), actions[action].data());
    for (size_t i = 0; i < std::min(available.rows, copying.failedItems.size()); i++) {
        printf(formatMessage("[error]▏ [bold]%s[blank][error]: %s[blank]\n").data(), copying.failedItems.at(i).first.data(), copying.failedItems.at(i).second.message().data());
        if (i == available.rows - 1 && copying.failedItems.size() > available.rows) printf(and_more_fails_message().data(), int(copying.failedItems.size() - available.rows));
    }
    printf("%s", fix_problem_message().data());
}

void showSuccesses() {
    if (output_silent) return;
    if (successes.bytes > 0 && is_tty.err) {
        fprintf(stderr, byte_success_message().data(), did_action[action].data(), formatBytes(successes.bytes.load()).data());
    } else if ((successes.files == 1 && successes.directories == 0) || (successes.files == 0 && successes.directories == 1)) {
        printf(one_item_success_message().data(), did_action[action].data());
    } else {
        if ((successes.files > 1) && (successes.directories == 0))
            printf(many_files_success_message().data(), did_action[action].data(), successes.files.load());
        else if ((successes.files == 0) && (successes.directories > 1))
            printf(many_directories_success_message().data(), did_action[action].data(), successes.directories.load());
        else if ((successes.files == 1) && (successes.directories == 1))
            printf(one_file_one_directory_success_message().data(), did_action[action].data());
        else if ((successes.files > 1) && (successes.directories == 1))
            printf(many_files_one_directory_success_message().data(), did_action[action].data(), successes.files.load());
        else if ((successes.files == 1) && (successes.directories > 1))
            printf(one_file_many_directories_success_message().data(), did_action[action].data(), successes.directories.load());
        else if ((successes.files > 1) && (successes.directories > 1))
            printf(many_files_many_directories_success_message().data(), did_action[action].data(), successes.files.load(), successes.directories.load());
    }
}

int main(int argc, char* argv[]) {
    try {
        if (!strcmp(argv[0], "clipboard")) std::cerr << "Warning! \"clipboard\" is now deprecated. Please use \"cb\" instead" << std::endl;

        setupHandlers();

        setupVariables(argc, argv);

        setLocale();

        setClipboardName();

        setFlags();

        setFilepaths();

        (fs::create_directories(global_path.temporary), fs::create_directories(global_path.persistent));

        action = getAction();

        syncWithGUIClipboard();

        copying.items.assign(arguments.begin(), arguments.end());

        io_type = getIOType();

        verifyAction();

        checkForNoItems();

        startIndicator();

        deduplicate(copying.items);

        if (action != Action::Info) getLock();

        checkItemSize(totalItemSize());

        clearTempDirectory();

        performAction();

        copying.mime = getMIMEType();

        updateGUIClipboard();

        stopIndicator();

        deduplicate(copying.failedItems);

        showFailures();

        showSuccesses();
    } catch (const std::exception& e) {
        if (stopIndicator()) fprintf(stderr, internal_error_message().data(), e.what());
        exit(EXIT_FAILURE);
    }
    if (copying.failedItems.empty())
        exit(EXIT_SUCCESS);
    else
        exit(EXIT_FAILURE);
}
