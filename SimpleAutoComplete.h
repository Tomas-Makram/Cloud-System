#pragma once
#include <vector>
#include <string>
#include <functional>
#include <memory>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include "MiniHSFS.h"
#include "run.h"


#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#endif

class SimpleAutoComplete {
public:

    //Command System
    const std::vector<std::string> builtInCommands = {
    "exit", "quit", "ls", "move", "mv", "write", "open", "read", "copy", "cp",
    "mkfile", "mf", "mkdir", "md", "tree", "info", "cd",
    "redir", "refile", "rename", "rd", "del", "cls", "map", "AI" , "chatbot"
    };

    using SuggestionsCallback = std::function<std::vector<std::string>(const std::string&)>;

    SimpleAutoComplete(MiniHSFS& mini);

    ~SimpleAutoComplete();


    std::string getInput() const;
    std::string readInput(const std::string& prompt);
   

private:
    class Impl {
    public:
        std::string inputValue;
        SuggestionsCallback getSuggestions;
        std::vector<std::string> currentMatches;
        size_t currentMatchIndex = 0;
        std::string prompt;
        int terminalWidth = 80;
        bool showSuggestionsFlag = false;
        int tabPressCount = 0;
        std::chrono::steady_clock::time_point lastTabTime;
        std::vector<std::string> commandHistory;
        size_t historyIndex = 0;
        bool inHistoryNavigation = false;

#ifdef _WIN32
        HANDLE hStdin = nullptr;
        DWORD fdwSaveOldMode = 0;
#else
        struct termios origTermios {};
#endif

        void setupConsole();
        void restoreConsole();
        int getch();
        void updateTerminalWidth();
        void showSuggestions(const std::string& input);
        void clearSuggestions();
    };
    
    
    std::unique_ptr<Impl> pimpl;

    void init(SuggestionsCallback callback);

    std::vector<std::string> getCommandSuggestions(const std::string& fullInput);
    std::vector<std::string> getUnifiedSuggestions(const std::string& fullInput, MiniHSFS& mini);
    std::vector<std::string> getFileSystemSuggestions(const std::string& fullInput, MiniHSFS& mini);

};