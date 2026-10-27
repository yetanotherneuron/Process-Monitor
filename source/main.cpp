#include "process.h"
#include "utilities.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <locale>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>

namespace {

constexpr DWORD refreshIntervalMs = 2000;
constexpr SHORT minimumConsoleWidth = 72;
constexpr SHORT minimumConsoleHeight = 12;
constexpr WORD normalText = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
constexpr WORD brightText = normalText | FOREGROUND_INTENSITY;
constexpr WORD cyanText = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
constexpr WORD titleColors = brightText | BACKGROUND_BLUE | BACKGROUND_INTENSITY;
constexpr WORD headerColors = brightText | BACKGROUND_BLUE | BACKGROUND_GREEN;
constexpr WORD statusColors = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
constexpr WORD warningColors = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
constexpr wchar_t newConsoleArgument[] = L"--process-monitor-new-console";

struct AppState {
    HANDLE input = INVALID_HANDLE_VALUE;
    HANDLE output = INVALID_HANDLE_VALUE;
    DWORD originalInputMode = 0;
    CONSOLE_CURSOR_INFO originalCursorInfo{};
    WORD originalAttributes = normalText;
    bool inputModeSaved = false;
    bool cursorInfoSaved = false;
    std::wstring search;
    std::string sortColumn = "memory";
    bool descending = true;
    std::size_t page = 0;
    std::vector<monitor::ProcessEntry> processes;
    std::vector<std::wstring> previousRows;
    std::wstring previousTitle;
    std::wstring previousToolbar;
    std::wstring previousHeader;
    std::wstring previousFrame;
    std::wstring previousControls;
    std::wstring status = L"Starting process monitor...";
    std::wstring previousStatus;
    std::mutex resultMutex;
    std::condition_variable refreshCondition;
    monitor::ProcessSnapshot pendingSnapshot;
    bool resultReady = false;
    bool refreshRequested = false;
    std::atomic<bool> stopping{false};
    SHORT viewportLeft = 0;
    SHORT viewportTop = 0;
};

std::wstring toWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length) <= 0) {
        return {};
    }

    return result;
}

std::string toUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }

    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr) <= 0) {
        return {};
    }

    return result;
}

void setCursor(HANDLE output, SHORT x, SHORT y) {
    COORD position{x, y};
    SetConsoleCursorPosition(output, position);
}

void writeAt(
    AppState& state,
    SHORT x,
    SHORT y,
    const std::wstring& text,
    SHORT availableWidth,
    WORD attributes = normalText) {
    if (availableWidth <= 0) {
        return;
    }

    const std::size_t count = std::min<std::size_t>(text.size(), static_cast<std::size_t>(availableWidth));
    setCursor(state.output, static_cast<SHORT>(state.viewportLeft + x),
              static_cast<SHORT>(state.viewportTop + y));
    DWORD written = 0;
    if (!SetConsoleTextAttribute(state.output, attributes)) {
        state.stopping.store(true);
        return;
    }
    if (!WriteConsoleW(state.output, text.data(), static_cast<DWORD>(count), &written, nullptr)) {
        state.stopping.store(true);
    }
    if (!SetConsoleTextAttribute(state.output, state.originalAttributes)) {
        state.stopping.store(true);
    }
}

std::wstring fitText(const std::wstring& value, std::size_t width, bool rightAlign = false) {
    if (width == 0) {
        return {};
    }

    std::wstring fitted = value;
    if (fitted.size() > width) {
        if (width > 1) {
            fitted.resize(width - 1);
            fitted.push_back(L'.');
        } else {
            fitted.resize(width);
        }
    }

    if (fitted.size() < width) {
        const std::wstring padding(width - fitted.size(), L' ');
        fitted = rightAlign ? padding + fitted : fitted + padding;
    }

    return fitted;
}

std::wstring cpuText(const monitor::ProcessEntry& process) {
    if (!process.cpuAvailable) {
        return L"Unavailable";
    }

    std::wostringstream text;
    text.imbue(std::locale::classic());
    text << std::fixed << std::setprecision(1) << process.cpuPercent << L'%';
    return text.str();
}

std::vector<monitor::ProcessEntry> getVisibleProcesses(const AppState& state) {
    std::vector<monitor::ProcessEntry> visible =
        monitor::filterProcesses(state.processes, toUtf8(state.search));
    visible = monitor::sortProcesses(std::move(visible), state.sortColumn);
    if (!state.descending) {
        std::reverse(visible.begin(), visible.end());
    }
    return visible;
}

void applyCpuSample(
    std::vector<monitor::ProcessEntry>& current,
    const std::vector<monitor::ProcessEntry>& previous,
    std::chrono::steady_clock::duration sampleDuration) {
    const double elapsed100Ns = std::chrono::duration<double>(sampleDuration).count() * 10'000'000.0;
    if (elapsed100Ns <= 0.0) {
        return;
    }

    const DWORD processorCount = std::max<DWORD>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), 1);
    std::unordered_map<std::uint32_t, const monitor::ProcessEntry*> previousByPid;
    previousByPid.reserve(previous.size());
    for (const auto& process : previous) {
        previousByPid.emplace(process.pid, &process);
    }

    for (auto& process : current) {
        process.cpuAvailable = false;
        const auto found = previousByPid.find(process.pid);
        if (found == previousByPid.end() || !process.cpuTimeAvailable ||
            !found->second->cpuTimeAvailable ||
            process.creationTime100Ns != found->second->creationTime100Ns ||
            process.totalCpuTime100Ns < found->second->totalCpuTime100Ns) {
            continue;
        }

        const std::uint64_t delta = process.totalCpuTime100Ns - found->second->totalCpuTime100Ns;
        process.cpuPercent = (static_cast<double>(delta) / elapsed100Ns) * 100.0 /
                             static_cast<double>(processorCount);
        process.cpuAvailable = true;
    }
}

void refreshWorker(AppState& state) {
    std::vector<monitor::ProcessEntry> previous;
    auto previousSampleTime = std::chrono::steady_clock::now();
    bool hasPrevious = false;

    while (!state.stopping.load()) {
        monitor::ProcessSnapshot snapshot;
        try {
            snapshot = monitor::listProcesses();
        } catch (const std::exception& error) {
            snapshot.error = std::string("Process refresh failed: ") + error.what();
        }
        const auto sampleTime = std::chrono::steady_clock::now();
        if (snapshot.error.empty() && hasPrevious) {
            applyCpuSample(snapshot.processes, previous, sampleTime - previousSampleTime);
        }

        if (snapshot.error.empty()) {
            previous = snapshot.processes;
            previousSampleTime = sampleTime;
            hasPrevious = true;
        }

        {
            std::lock_guard<std::mutex> lock(state.resultMutex);
            state.pendingSnapshot = std::move(snapshot);
            state.resultReady = true;
        }

        std::unique_lock<std::mutex> lock(state.resultMutex);
        state.refreshCondition.wait_for(
            lock,
            std::chrono::milliseconds(refreshIntervalMs),
            [&state] { return state.stopping.load() || state.refreshRequested; });
        state.refreshRequested = false;
    }
}

bool initializeConsole(AppState& state) {
    state.input = GetStdHandle(STD_INPUT_HANDLE);
    state.output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (state.input == INVALID_HANDLE_VALUE || state.output == INVALID_HANDLE_VALUE ||
        state.input == nullptr || state.output == nullptr) {
        return false;
    }

    DWORD outputMode = 0;
    if (!GetConsoleMode(state.input, &state.originalInputMode) ||
        !GetConsoleMode(state.output, &outputMode)) {
        return false;
    }
    state.inputModeSaved = true;

    CONSOLE_SCREEN_BUFFER_INFO screen{};
    if (!GetConsoleScreenBufferInfo(state.output, &screen)) {
        return false;
    }
    state.originalAttributes = screen.wAttributes;

    if (!GetConsoleCursorInfo(state.output, &state.originalCursorInfo)) {
        return false;
    }
    state.cursorInfoSaved = true;

    const DWORD inputMode = (state.originalInputMode | ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS) &
                            ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_QUICK_EDIT_MODE |
                              ENABLE_PROCESSED_INPUT);
    if (!SetConsoleMode(state.input, inputMode)) {
        return false;
    }

    CONSOLE_CURSOR_INFO cursorInfo = state.originalCursorInfo;
    cursorInfo.bVisible = FALSE;
    if (!SetConsoleCursorInfo(state.output, &cursorInfo)) {
        return false;
    }

    SetConsoleTitleW(L"Process Monitor");
    return true;
}

void restoreConsole(AppState& state) {
    if (state.inputModeSaved) {
        SetConsoleMode(state.input, state.originalInputMode);
    }
    SetConsoleTextAttribute(state.output, state.originalAttributes);
    if (state.cursorInfoSaved) {
        SetConsoleCursorInfo(state.output, &state.originalCursorInfo);
    }

    CONSOLE_SCREEN_BUFFER_INFO screen{};
    if (GetConsoleScreenBufferInfo(state.output, &screen)) {
        setCursor(state.output, 0, static_cast<SHORT>(screen.srWindow.Bottom));
        DWORD written = 0;
        WriteConsoleW(state.output, L"\r\n", 2, &written, nullptr);
    }
}

bool clearViewport(AppState& state) {
    CONSOLE_SCREEN_BUFFER_INFO screen{};
    if (!GetConsoleScreenBufferInfo(state.output, &screen)) {
        return false;
    }

    const DWORD width = static_cast<DWORD>(screen.srWindow.Right - screen.srWindow.Left + 1);
    for (SHORT row = screen.srWindow.Top; row <= screen.srWindow.Bottom; ++row) {
        COORD start{screen.srWindow.Left, row};
        DWORD written = 0;
        if (!FillConsoleOutputCharacterW(state.output, L' ', width, start, &written) ||
            written != width ||
            !FillConsoleOutputAttribute(state.output, screen.wAttributes, width, start, &written) ||
            written != width) {
            return false;
        }
    }

    setCursor(state.output, screen.srWindow.Left, screen.srWindow.Top);
    return true;
}

std::wstring processRow(const monitor::ProcessEntry& process, SHORT width) {
    const std::size_t usableWidth = static_cast<std::size_t>(std::max<SHORT>(width, minimumConsoleWidth - 1));
    const std::size_t nameWidth = usableWidth - 52;
    const std::size_t pidWidth = 9;
    const std::size_t memoryWidth = 20;
    const std::size_t cpuWidth = 10;

    const std::wstring name = fitText(toWide(process.name), nameWidth);
    const std::wstring pid = fitText(std::to_wstring(process.pid), pidWidth, true);
    const std::wstring memory = fitText(
        process.memoryAvailable ? toWide(monitor::formatMemory(process.memoryBytes)) : L"Unavailable",
        memoryWidth,
        true);
    const std::wstring cpu = fitText(cpuText(process), cpuWidth, true);

    return L"| " + name + L" | " + pid + L" | " + memory + L" | " + cpu + L" |";
}

void render(AppState& state, bool force = false) {
    CONSOLE_SCREEN_BUFFER_INFO screen{};
    if (!GetConsoleScreenBufferInfo(state.output, &screen)) {
        state.stopping.store(true);
        return;
    }

    state.viewportLeft = screen.srWindow.Left;
    state.viewportTop = screen.srWindow.Top;
    const SHORT width = static_cast<SHORT>(screen.srWindow.Right - screen.srWindow.Left + 1);
    const SHORT height = static_cast<SHORT>(screen.srWindow.Bottom - screen.srWindow.Top + 1);
    if (width < minimumConsoleWidth || height < minimumConsoleHeight) {
        if (state.previousTitle != L"Resize the terminal to at least 72 columns by 12 rows.") {
            state.previousTitle = L"Resize the terminal to at least 72 columns by 12 rows.";
            writeAt(state, 0, 0, state.previousTitle, width);
        }
        return;
    }

    const std::vector<monitor::ProcessEntry> visible = getVisibleProcesses(state);
    const SHORT listTop = 5;
    const SHORT footerTop = static_cast<SHORT>(height - 2);
    const SHORT bottomBorder = static_cast<SHORT>(footerTop - 1);
    const std::size_t pageSize = static_cast<std::size_t>(bottomBorder - listTop);
    const SHORT frameWidth = static_cast<SHORT>(width - 1);
    const std::size_t pageCount = std::max<std::size_t>(1, (visible.size() + pageSize - 1) / pageSize);
    state.page = std::min(state.page, pageCount - 1);
    const std::size_t startIndex = state.page * pageSize;
    const std::size_t endIndex = std::min(visible.size(), startIndex + pageSize);

    const std::wstring title = L"Process Monitor  |  " + std::to_wstring(visible.size()) +
                               L" processes  |  Page " + std::to_wstring(state.page + 1) +
                               L"/" + std::to_wstring(pageCount);
    if (force || title != state.previousTitle) {
        writeAt(state, 0, 0, fitText(title, static_cast<std::size_t>(frameWidth)), frameWidth, titleColors);
        state.previousTitle = title;
    }

    const std::wstring sortText = toWide(state.sortColumn);
    const std::wstring toolbar = L"Search: " + state.search +
                                L"   Sort: " + sortText +
                                (state.descending ? L" (descending)" : L" (ascending)");
    if (force || toolbar != state.previousToolbar) {
        writeAt(state, 0, 1, fitText(toolbar, static_cast<std::size_t>(frameWidth)), frameWidth, cyanText);
        state.previousToolbar = toolbar;
    }

    const std::size_t usableWidth = static_cast<std::size_t>(std::max<SHORT>(frameWidth, minimumConsoleWidth - 1));
    const std::size_t nameWidth = usableWidth - 52;
    const auto columnTitle = [&state](const wchar_t* name, const char* column) {
        const std::wstring titleText = name;
        if (state.sortColumn != column) {
            return titleText;
        }
        return titleText + (state.descending ? L" v" : L" ^");
    };
    const std::wstring border = L"+" + std::wstring(static_cast<std::size_t>(frameWidth - 2), L'-') + L"+";
    if (force || border != state.previousFrame) {
        writeAt(state, 0, 2, border, frameWidth, cyanText);
        writeAt(state, 0, bottomBorder, border, frameWidth, cyanText);
        state.previousFrame = border;
    }

    const std::wstring header = L"| " + fitText(columnTitle(L"Process name", "name"), nameWidth) + L" | " +
                                fitText(columnTitle(L"PID", "pid"), 9, true) + L" | " +
                                fitText(columnTitle(L"Private memory", "memory"), 20, true) + L" | " +
                                fitText(columnTitle(L"CPU", "cpu"), 10, true) + L" |";
    if (force || header != state.previousHeader) {
        writeAt(state, 0, 3, fitText(header, static_cast<std::size_t>(frameWidth)), frameWidth, headerColors);
        writeAt(state, 0, 4, border, frameWidth, cyanText);
        state.previousHeader = header;
    }

    std::vector<std::wstring> rows;
    rows.reserve(endIndex - startIndex);
    for (std::size_t index = startIndex; index < endIndex; ++index) {
        rows.push_back(processRow(visible[index], frameWidth));
    }
    if (rows.empty()) {
        rows.push_back(L"No processes match the current search.");
    }

    const std::size_t compareCount = std::max(rows.size(), state.previousRows.size());
    for (std::size_t index = 0; index < compareCount && index < pageSize; ++index) {
        const bool currentExists = index < rows.size();
        const bool previousExists = index < state.previousRows.size();
        if (!force && currentExists && previousExists && rows[index] == state.previousRows[index]) {
            continue;
        }

        const std::wstring line = currentExists ? rows[index] : L"";
        const WORD rowColor = index % 2 == 0 ? normalText : brightText;
        writeAt(state, 0, static_cast<SHORT>(listTop + index),
                fitText(line, static_cast<std::size_t>(frameWidth)), frameWidth, rowColor);
    }
    state.previousRows = std::move(rows);

    const std::wstring controls = L"F5 Refresh  |  Ctrl+F Search  |  Ctrl+E Export CSV  |  1 Name  2 PID  3 Memory  4 CPU  |  PgUp/PgDn";
    if (force || controls != state.previousControls) {
        writeAt(state, 0, footerTop, fitText(controls, static_cast<std::size_t>(frameWidth)), frameWidth, cyanText);
        state.previousControls = controls;
    }
    if (force || state.status != state.previousStatus) {
        const bool failed = state.status.find(L"failed") != std::wstring::npos ||
                            state.status.find(L"error") != std::wstring::npos;
        writeAt(state, 0, static_cast<SHORT>(footerTop + 1),
                fitText(state.status, static_cast<std::size_t>(frameWidth)), frameWidth,
                failed ? warningColors : statusColors);
        state.previousStatus = state.status;
    }
}

void writeCsv(const std::vector<monitor::ProcessEntry>& processes, const std::wstring& path) {
    const std::string csv = monitor::toCsv(processes);
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Could not open the selected CSV path.");
    }

    static constexpr unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    DWORD written = 0;
    bool success = WriteFile(file, bom, sizeof(bom), &written, nullptr) && written == sizeof(bom);
    if (success) {
        success = WriteFile(file, csv.data(), static_cast<DWORD>(csv.size()), &written, nullptr) &&
                  written == csv.size();
    }
    CloseHandle(file);
    if (!success) {
        throw std::runtime_error("Could not write the CSV file completely.");
    }
}

void exportCsv(AppState& state) {
    CONSOLE_SCREEN_BUFFER_INFO screen{};
    GetConsoleScreenBufferInfo(state.output, &screen);
    const SHORT width = static_cast<SHORT>(screen.srWindow.Right - screen.srWindow.Left + 1);
    const SHORT height = static_cast<SHORT>(screen.srWindow.Bottom - screen.srWindow.Top + 1);
    const SHORT promptLine = static_cast<SHORT>(std::max<SHORT>(0, height - 2));
    writeAt(state, 0, promptLine, fitText(L"CSV path (Enter to cancel): ", static_cast<std::size_t>(width - 1)), width - 1);
    state.previousControls.clear();
    setCursor(state.output, 28, promptLine);

    DWORD inputMode = 0;
    if (!GetConsoleMode(state.input, &inputMode) ||
        !SetConsoleMode(state.input, (inputMode | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT) &
                                      ~(ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT))) {
        state.status = L"CSV export failed: terminal input mode could not be changed.";
        return;
    }

    CONSOLE_CURSOR_INFO cursorInfo{};
    if (!GetConsoleCursorInfo(state.output, &cursorInfo)) {
        SetConsoleMode(state.input, (state.originalInputMode | ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS) &
                                    ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_QUICK_EDIT_MODE |
                                      ENABLE_PROCESSED_INPUT));
        state.status = L"CSV export failed: terminal cursor state could not be read.";
        return;
    }
    cursorInfo.bVisible = TRUE;
    if (!SetConsoleCursorInfo(state.output, &cursorInfo)) {
        SetConsoleMode(state.input, (state.originalInputMode | ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS) &
                                    ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_QUICK_EDIT_MODE |
                                      ENABLE_PROCESSED_INPUT));
        state.status = L"CSV export failed: terminal cursor could not be shown.";
        return;
    }

    wchar_t pathBuffer[2048]{};
    DWORD read = 0;
    const BOOL readSuccess = ReadConsoleW(state.input, pathBuffer, static_cast<DWORD>(std::size(pathBuffer) - 1), &read, nullptr);

    if (state.cursorInfoSaved) {
        SetConsoleCursorInfo(state.output, &state.originalCursorInfo);
    }
    const DWORD restoredMode = (state.originalInputMode | ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS) &
                              ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_QUICK_EDIT_MODE |
                                ENABLE_PROCESSED_INPUT);
    SetConsoleMode(state.input, restoredMode);
    if (!readSuccess) {
        state.status = L"CSV export failed while reading the path.";
        return;
    }

    std::wstring path(pathBuffer, read);
    while (!path.empty() && (path.back() == L'\r' || path.back() == L'\n')) {
        path.pop_back();
    }
    if (path.empty()) {
        state.status = L"CSV export cancelled.";
        return;
    }

    try {
        writeCsv(getVisibleProcesses(state), path);
        state.status = L"CSV export complete: " + path;
    } catch (const std::exception& error) {
        state.status = L"CSV export failed: " + toWide(error.what());
    }
}

void handleKey(AppState& state, const KEY_EVENT_RECORD& key) {
    if (!key.bKeyDown) {
        return;
    }

    const bool controlPressed = (key.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    if (key.wVirtualKeyCode == VK_ESCAPE) {
        state.search.clear();
        state.page = 0;
    } else if (key.wVirtualKeyCode == VK_F5) {
        {
            std::lock_guard<std::mutex> lock(state.resultMutex);
            state.refreshRequested = true;
        }
        state.refreshCondition.notify_one();
        state.status = L"Refresh requested...";
    } else if (key.wVirtualKeyCode == VK_F1) {
        state.status = L"Private bytes; CPU is sampled and normalized across logical processors. First CPU sample is unavailable.";
    } else if (controlPressed && key.wVirtualKeyCode == 'C') {
        state.stopping.store(true);
    } else if (controlPressed && key.wVirtualKeyCode == 'F') {
        state.status = L"Type to search. Press Escape to clear the search.";
    } else if (controlPressed && key.wVirtualKeyCode == 'E') {
        exportCsv(state);
    } else if (key.wVirtualKeyCode == VK_BACK) {
        if (!state.search.empty()) {
            state.search.pop_back();
        }
        state.page = 0;
    } else if (key.wVirtualKeyCode == VK_PRIOR) {
        if (state.page > 0) {
            --state.page;
        }
    } else if (key.wVirtualKeyCode == VK_NEXT) {
        ++state.page;
    } else if (!controlPressed && key.uChar.UnicodeChar >= L' ' && key.uChar.UnicodeChar != 0x7F) {
        state.search.push_back(key.uChar.UnicodeChar);
        state.page = 0;
    } else if (key.wVirtualKeyCode >= '1' && key.wVirtualKeyCode <= '4') {
        const int column = key.wVirtualKeyCode - '1';
        state.sortColumn = column == 0 ? "name" : column == 1 ? "pid" : column == 2 ? "memory" : "cpu";
        state.descending = column >= 2;
        state.page = 0;
    }
}

void applyPendingSnapshot(AppState& state) {
    monitor::ProcessSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(state.resultMutex);
        if (!state.resultReady) {
            return;
        }
        snapshot = std::move(state.pendingSnapshot);
        state.resultReady = false;
    }

    if (!snapshot.error.empty() && snapshot.processes.empty()) {
        state.status = toWide(snapshot.error);
        return;
    }

    state.processes = std::move(snapshot.processes);
    if (snapshot.error.empty()) {
        state.status = L"Updated " + toWide(std::to_string(state.processes.size())) + L" processes.";
    } else {
        state.status = toWide(snapshot.error);
    }

}

int runTerminal(AppState& state) {
    if (!initializeConsole(state)) {
        return 2;
    }

    if (!clearViewport(state)) {
        restoreConsole(state);
        return 1;
    }

    state.status = L"Sampling process information...";
    try {
        std::thread worker(refreshWorker, std::ref(state));
        state.previousRows.clear();
        render(state, true);

        bool running = true;
        while (running && !state.stopping.load()) {
            const DWORD waitResult = WaitForSingleObject(state.input, 150);
            if (waitResult == WAIT_OBJECT_0) {
                INPUT_RECORD records[32]{};
                DWORD recordCount = 0;
                if (!ReadConsoleInputW(state.input, records, static_cast<DWORD>(std::size(records)), &recordCount)) {
                    state.status = L"Could not read terminal input.";
                    state.stopping.store(true);
                    break;
                }

                for (DWORD index = 0; index < recordCount; ++index) {
                    if (records[index].EventType == KEY_EVENT) {
                        handleKey(state, records[index].Event.KeyEvent);
                    } else if (records[index].EventType == WINDOW_BUFFER_SIZE_EVENT) {
                        clearViewport(state);
                        state.previousRows.clear();
                        state.previousTitle.clear();
                        state.previousToolbar.clear();
                        state.previousHeader.clear();
                        state.previousFrame.clear();
                        state.previousControls.clear();
                        state.previousStatus.clear();
                    }
                }
            } else if (waitResult == WAIT_FAILED) {
                state.status = L"Terminal input wait failed.";
                state.stopping.store(true);
                break;
            }

            applyPendingSnapshot(state);
            render(state);
        }

        state.stopping.store(true);
        state.refreshCondition.notify_all();
        worker.join();
    } catch (const std::system_error& error) {
        state.status = L"Could not start process sampling: " + toWide(error.what());
        render(state, true);
        restoreConsole(state);
        return 1;
    }

    restoreConsole(state);
    return 0;
}

bool isRunningInSharedConsole() {
    DWORD processIds[2]{};
    const DWORD processCount = GetConsoleProcessList(processIds, static_cast<DWORD>(std::size(processIds)));
    return processCount > 1;
}

bool launchInNewConsole() {
    std::vector<wchar_t> executablePath(32768, L'\0');
    const DWORD pathLength = GetModuleFileNameW(
        nullptr,
        executablePath.data(),
        static_cast<DWORD>(executablePath.size()));
    if (pathLength == 0 || pathLength >= executablePath.size()) {
        return false;
    }

    std::wstring commandLine = L"\"" + std::wstring(executablePath.data(), pathLength) +
                               L"\" " + newConsoleArgument;
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(
            executablePath.data(),
            mutableCommandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NEW_CONSOLE,
            nullptr,
            nullptr,
            &startupInfo,
            &processInfo)) {
        const DWORD error = GetLastError();
        const std::wstring message = L"Process Monitor could not open a new terminal (Windows error " +
                                     std::to_wstring(error) + L").\r\n";
        DWORD written = 0;
        WriteConsoleW(GetStdHandle(STD_ERROR_HANDLE), message.c_str(),
                      static_cast<DWORD>(message.size()), &written, nullptr);
        return false;
    }

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return true;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    bool alreadyInNewConsole = false;
    for (int index = 1; index < argc; ++index) {
        if (std::wstring(argv[index]) == newConsoleArgument) {
            alreadyInNewConsole = true;
            break;
        }
    }

    if (!alreadyInNewConsole && isRunningInSharedConsole()) {
        return launchInNewConsole() ? 0 : 1;
    }

    AppState state;
    return runTerminal(state);
}
