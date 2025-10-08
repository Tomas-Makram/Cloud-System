#include "SimpleAutoComplete.h"

void SimpleAutoComplete::Impl::setupConsole() {
#ifdef _WIN32
    hStdin = GetStdHandle(STD_INPUT_HANDLE);
    GetConsoleMode(hStdin, &fdwSaveOldMode);
    SetConsoleMode(hStdin, ENABLE_PROCESSED_INPUT);
#else
    tcgetattr(STDIN_FILENO, &origTermios);
    struct termios newTermios = origTermios;
    newTermios.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newTermios);
#endif
    updateTerminalWidth();
}

void SimpleAutoComplete::Impl::restoreConsole() {
#ifdef _WIN32
    SetConsoleMode(hStdin, fdwSaveOldMode);
#else
    tcsetattr(STDIN_FILENO, TCSANOW, &origTermios);
#endif
}

void SimpleAutoComplete::Impl::updateTerminalWidth() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        terminalWidth = csbi.dwSize.X;
    }
#else
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        terminalWidth = w.ws_col;
    }
#endif
}

int SimpleAutoComplete::Impl::getch() {
#ifdef _WIN32
    return _getch();
#else
    char ch;
    ssize_t n = read(STDIN_FILENO, &ch, 1);
    if (n == 1) {
        // فعل flushing آليًا لمخرجات std::cout مرة واحدة
        // عشان طبعات readInput() تظهر فورًا عند كل حرف
        static bool unitbuf_set = false;
        if (!unitbuf_set) {
            std::cout << std::unitbuf; // كل عملية << ستُفَضّ فوراً
            unitbuf_set = true;
}

        // لا نطبع هنا (لمنع التكرار)، نرجع الحرف فقط
        return static_cast<unsigned char>(ch);
    }
    return EOF;
#endif
}

void SimpleAutoComplete::Impl::showSuggestions(const std::string& input) {
    if (!showSuggestionsFlag) return;

    if (currentMatches.empty()) {
        currentMatches = getSuggestions(input);
        currentMatchIndex = 0;
        if (currentMatches.empty()) {
            showSuggestionsFlag = false;
            return;
        }
    }

    std::cout << std::endl;

    size_t maxLength = 0;
    for (const auto& word : currentMatches) {
        if (word.length() > maxLength) maxLength = word.length();
    }
    maxLength += 2;

    int columns = (std::max)(1, terminalWidth / static_cast<int>(maxLength));

    for (size_t i = 0; i < currentMatches.size(); ++i) {
        if (i % columns == 0 && i != 0)
            std::cout << "\n";
        std::cout << std::left << std::setw(static_cast<int>(maxLength)) << currentMatches[i];
    }

    std::cout << std::endl << prompt << input;
    std::cout.flush();
}

#ifdef _WIN32
void clearBelowCursor() {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hConsole == INVALID_HANDLE_VALUE) return;

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(hConsole, &csbi)) return;

    COORD startCoord = csbi.dwCursorPosition;
    DWORD cellsToClear = csbi.dwSize.X * (csbi.dwSize.Y - csbi.dwCursorPosition.Y) - csbi.dwCursorPosition.X;
    DWORD written;
    FillConsoleOutputCharacter(hConsole, ' ', cellsToClear, startCoord, &written);
    FillConsoleOutputAttribute(hConsole, csbi.wAttributes, cellsToClear, startCoord, &written);
}
#else
void clearBelowCursor() {
    std::cout << "\033[J";
}
#endif

void SimpleAutoComplete::Impl::clearSuggestions() {
    if (!currentMatches.empty()) {
        clearBelowCursor();
        currentMatches.clear();
        currentMatchIndex = 0;
    }
}

SimpleAutoComplete::SimpleAutoComplete(MiniHSFS& mini)
    : pimpl(std::make_unique<Impl>()) {
    pimpl->getSuggestions = [this, &mini](const std::string& input) {
        return this->getUnifiedSuggestions(input, mini);
        };
}

SimpleAutoComplete::~SimpleAutoComplete() = default;

void SimpleAutoComplete::init(SuggestionsCallback callback) {
    pimpl->getSuggestions = callback;
}

std::string SimpleAutoComplete::readInput(const std::string& prompt) {
    pimpl->setupConsole();
    pimpl->prompt = prompt;
    std::string input;
    std::cout << prompt;
    std::cout.flush();

    auto lastTabTime = std::chrono::steady_clock::now();
    const auto doubleTabInterval = std::chrono::milliseconds(300);

    while (true) {
        int ch = pimpl->getch();

        if (ch == '\n' || ch == '\r') { // Enter
            pimpl->clearSuggestions();
            pimpl->showSuggestionsFlag = false;
            break;
        }

        if (ch == '\t') { // Tab
            pimpl->showSuggestionsFlag = true;

            if (pimpl->showSuggestionsFlag) {
                pimpl->currentMatches = pimpl->getSuggestions(input);
                pimpl->currentMatchIndex = 0;

                if (pimpl->currentMatches.size() == 1) {
                    std::string match = pimpl->currentMatches[0];

                    size_t lastSpace = input.find_last_of(' ');
                    std::string prefix = (lastSpace == std::string::npos) ? input : input.substr(lastSpace + 1);

                    if (prefix == match) {
                        input += " ";
                        std::cout << " ";
                    }
                    else {
                        std::string toAppend = match.substr(prefix.length());
                        input += toAppend;
                        std::cout << toAppend;
                    }

                    pimpl->showSuggestionsFlag = false;
                }
                else if (pimpl->currentMatches.size() > 1) {
                    pimpl->showSuggestions(input);
                }
            }
            continue;
        }

        if (pimpl->showSuggestionsFlag) {
            pimpl->clearSuggestions();
            pimpl->showSuggestionsFlag = false;
        }

        if (ch == 27 || ch == 0 || ch == 224) { // Arrow keys
            int ext = pimpl->getch();
#ifdef _WIN32
            if (ch == 224) {
                if (ext == 72 && !pimpl->currentMatches.empty()) { // Up
                    pimpl->currentMatchIndex = (pimpl->currentMatchIndex + pimpl->currentMatches.size() - 1) % pimpl->currentMatches.size();
                    pimpl->showSuggestions(input);
                }
                else if (ext == 80 && !pimpl->currentMatches.empty()) { // Down
                    pimpl->currentMatchIndex = (pimpl->currentMatchIndex + 1) % pimpl->currentMatches.size();
                    pimpl->showSuggestions(input);
                }
            }
#else
            if (ch == 27 && ext == 91) {
                int ext2 = pimpl->getch();
                if (ext2 == 65 && !pimpl->currentMatches.empty()) { // Up
                    pimpl->currentMatchIndex = (pimpl->currentMatchIndex + pimpl->currentMatches.size() - 1) % pimpl->currentMatches.size();
                    pimpl->showSuggestions(input);
                }
                else if (ext2 == 66 && !pimpl->currentMatches.empty()) { // Down
                    pimpl->currentMatchIndex = (pimpl->currentMatchIndex + 1) % pimpl->currentMatches.size();
                    pimpl->showSuggestions(input);
                }
            }
#endif
            continue;
        }

        if (ch == 127 || ch == 8) { // Backspace
            if (!input.empty()) {
                input.pop_back();
                std::cout << "\b \b";
            }
        }
        else if (ch >= 32 && ch <= 126) { // Printable characters
            std::cout << static_cast<char>(ch);
            input += static_cast<char>(ch);
        }
    }

    pimpl->restoreConsole();
    pimpl->inputValue = input;
    pimpl->restoreConsole();
    std::cout << "\n";
    return input;
}

std::string SimpleAutoComplete::getInput() const {
    return pimpl->inputValue;
}

std::vector<std::string> SimpleAutoComplete::getFileSystemSuggestions(const std::string& fullInput, MiniHSFS& mini) {
    std::vector<std::string> suggestions;

    size_t lastSpace = fullInput.find_last_of(' ');
    std::string input = (lastSpace == std::string::npos) ? fullInput : fullInput.substr(lastSpace + 1);

    try {
        int currentDir = mini.FindFile(run::currentPath);
        if (currentDir != -1 && mini.inodeTable[currentDir].isDirectory) {
            for (const auto& entry : mini.inodeTable[currentDir].entries) {
                if (entry.first.find(input) == 0) {
                    suggestions.push_back(entry.first);
                }
            }
            std::sort(suggestions.begin(), suggestions.end());
        }
    }
    catch (...) {}

    return suggestions;
}

std::vector<std::string> SimpleAutoComplete::getCommandSuggestions(const std::string& fullInput) {
    std::vector<std::string> suggestions;

    size_t lastSpace = fullInput.find_last_of(' ');
    std::string input = (lastSpace == std::string::npos) ? fullInput : fullInput.substr(lastSpace + 1);

    for (const std::string& cmd : builtInCommands) {
        if (cmd.find(input) == 0) {
            suggestions.push_back(cmd);
        }
    }

    std::sort(suggestions.begin(), suggestions.end());
    return suggestions;
}

std::vector<std::string> SimpleAutoComplete::getUnifiedSuggestions(const std::string& fullInput, MiniHSFS& mini) {
    std::vector<std::string> suggestions;

    bool suggestCommand = fullInput.empty() || fullInput.find(' ') == std::string::npos;

    if (suggestCommand) {
        suggestions = getCommandSuggestions(fullInput);
    }
    else {
        suggestions = getFileSystemSuggestions(fullInput, mini);
    }

    return suggestions;
}