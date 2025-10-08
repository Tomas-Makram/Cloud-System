#include "Tokenizer.h"

//Constracter
Tokenizer::Tokenizer(std::string username, std::string dirname, std::string password, int strongPassword)
    : parse(username, dirname, password, strongPassword) {}


//Split '," Commands
std::vector<std::string> Tokenizer::ParseArguments(const std::string& input) {
    std::vector<std::string> args;
    std::string current;
    bool in_quotes = false;

    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];

        if (c == '"') {
            in_quotes = !in_quotes;
        }
        else if (c == ' ' && !in_quotes) {
            if (!current.empty()) {
                args.push_back(current);
                current.clear();
            }
        }
        else {
            current += c;
        }
    }

    if (!current.empty()) {
        args.push_back(current);
    }

    return args;
}

//Command Line
void Tokenizer::handleCommand(const std::vector<std::string>& args, MiniHSFS& mini) {

    if (args.empty() || !isReady(nextPid - 1)) return;
   
    findProcess(nextPid - 1)->state = ProcessState::Running;

    if (args[0] == "cd")
        parse.cd(args.size() == 1 ? "/" : args[1], mini);

    else if (args[0] == "ls") {
        std::string options;
        std::string path;

        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i][0] == '-') {
                options += args[i].substr(1);
            }
            else if (path.empty()) {
                path = args[i];
            }
        }

        // Constructing the unified argument
        std::string argument;
        if (!options.empty()) {
            argument += "-" + options;
        }
        if (!path.empty()) {
            if (!argument.empty()) argument += " ";
            argument += path;
        }

        parse.ls(argument, mini);
    }

    else if (args[0] == "info")
        parse.printFileSystemInfo(mini);

    else if (args[0] == "tree")
        parse.PrintBTreeStructure(mini);

    else if ((args[0] == "mkdir" && args.size() > 1) || (args[0] == "md" && args.size() > 1))
        for (int x = 1; x < args.size(); x++)
            parse.createDirectory((args[x][0] != '/' ? run::currentPath + (run::currentPath != "/" ? "/" : "") : args[x]), (args[x][0] != '/' ? args[x] : ""), mini);

    else if ((args[0] == "mkfile" && args.size() > 1) || ((args[0] == "mf" && args.size() > 1)))
        for (int x = 1; x < args.size(); x++)
            parse.createFile((args[1][0] == '/' ? "" : run::currentPath + (run::currentPath != "/" ? "/" : "")), args[x], mini);

    else if ((args[0] == "redir" && args.size() == 3) || (args[0] == "refile" && args.size() == 3) || (args[0] == "rename" && args.size() == 3))
        parse.rename(args[1][0] == '/' ? args[1] : run::currentPath + (run::currentPath != "/" ? "/" : "") + args[1], args[2], mini);

    else if (args[0] == "rd" && args.size() > 1)
        for (int x = 1; x < args.size(); x++)
            parse.deleteDirectory(args[x][0] != '/' ? run::currentPath + (run::currentPath != "/" ? "/" : "") + args[x] : args[x], mini);

    else if (args[0] == "del" && args.size() > 1)
        for (int x = 1; x < args.size(); x++)
            parse.deleteFile(args[x][0] != '/' ? run::currentPath + (run::currentPath != "/" ? "/" : "") + args[x] : args[x], mini);

    else if (args[0] == "open" && args.size() > 1) {
        for (size_t i = 1; i < args.size(); i++) {
            std::string full_path = (args[i][0] != '/')
                ? run::currentPath + (run::currentPath != "/" ? "/" : "") + args[i]
                : args[i];

            try {
                std::vector<char> data = parse.readFile(full_path, mini, 0, true, run::Password);
                std::string str(data.begin(), data.end());
                std::cout << "File content:\n" << str << std::endl;
            }
            catch (const std::exception& e) {
                mini.Disk().SetConsoleColor(mini.Disk().Red);
                std::cerr << "Error reading file " << args[i] << ": " << e.what() << std::endl;
                mini.Disk().SetConsoleColor(mini.Disk().Default);
            }
        }
    }

    //else if (args[0] == "open" && args.size() > 1) {
    //    // إعدادات القراءة
    //    const size_t MAX_MEMORY_CHUNK = 10 * 1024 * 1024; // 10MB لكل جزء
    //    const size_t PRINT_CHUNK_SIZE = 10 * 1024 * 1024; // حجم كل جزء عند الطباعة (1KB)
    //    bool read_full = false;
    //    bool show_content = true;
    //    bool chunked_mode = false;
//
    //    // تحليل الخيارات
    //    std::vector<std::string> files;
    //    for (size_t i = 1; i < args.size(); i++) {
    //        if (args[i] == "--full") {
    //            read_full = true;
    //        }
    //        else if (args[i] == "--chunked") {
    //            chunked_mode = true;
    //        }
    //        else if (args[i] == "--no-display") {
    //            show_content = false;
    //        }
    //        else {
    //            files.push_back(args[i]);
    //        }
    //    }
//
    //    if (files.empty()) {
    //        std::cerr << "Error: No files specified" << std::endl;
    //        return;
    //    }
    //
    //    for (const auto& file : files) {
    //        std::string full_path = (file[0] != '/') ?
    //            run::currentPath + '/' + file :
    //            file;
    //
    //        try {
    //            // الحصول على معلومات الملف
    //            size_t file_size = parse.getFileSize(full_path, mini);
    //            std::cout << "\nProcessing: " << file << " ("
    //                << file_size << " bytes / "
    //                << file_size / (1024 * 1024) << " MB)" << std::endl;
    //
    //            // التحقق من حجم الملف للوضع الكامل
    //            if (read_full && file_size > 100 * 1024 * 1024) {
    //                std::cout << "Warning: Large file size (>100MB).\n"
    //                    << "Use chunked mode? (y/n): ";
    //                char choice;
    //                std::cin >> choice;
    //                if (choice == 'y') {
    //                    read_full = false;
    //                    chunked_mode = true;
    //                }
    //            }
    //
    //            // عملية القراءة
    //            if (read_full || (!chunked_mode && file_size <= MAX_MEMORY_CHUNK)) {
    //                // القراءة الكاملة للملفات الصغيرة
    //                std::vector<char> data = parse.readFile(full_path, mini, true, "123");
    //                std::cout << "Successfully read " << data.size() << " bytes" << std::endl;
    //
    //                // طباعة المحتوى كاملاً مقسماً إلى أجزاء
    //                if (show_content) {
    //                    std::string content(data.begin(), data.end());
    //                    std::cout << "\nFile content (in chunks):\n";
    //
    //                    for (size_t i = 0; i < content.size(); i += PRINT_CHUNK_SIZE) {
    //                        size_t chunk_length = (std::min)(PRINT_CHUNK_SIZE, content.size() - i);
    //                        std::string chunk = content.substr(i, chunk_length);
    //
    //                        // طباعة الجزء الحالي
    //                        std::cout << "Chunk " << (i / PRINT_CHUNK_SIZE + 1)
    //                            << " (" << chunk_length << " bytes):\n"
    //                            << chunk << "\n---\n";
    //                    }
    //                }
    //            }
    //            else {
    //                // القراءة المجزأة للملفات الكبيرة
    //                std::cout << "\nReading and printing file in chunks:\n";
    //                size_t total_processed = 0;
    //                size_t chunk_count = 0;
    //                size_t print_chunk_count = 0;
    //
    //                while (total_processed < file_size) {
    //                    chunk_count++;
    //                    size_t remaining = file_size - total_processed;
    //                    size_t chunk_size = (std::min)(MAX_MEMORY_CHUNK, remaining);
    //
    //                    std::cout << "\rReading data chunk " << chunk_count
    //                        << " (" << total_processed / (1024 * 1024) << "MB/"
    //                        << file_size / (1024 * 1024) << "MB)" << std::flush;
    //
    //                    std::vector<char> chunk = parse.readFile(full_path, mini, chunk_size, true, "123");
    //                    total_processed += chunk.size();
    //
    //                    // طباعة المحتوى مقسماً إلى أجزاء صغيرة
    //                    if (show_content) {
    //                        std::string chunk_str(chunk.begin(), chunk.end());
    //
    //                        for (size_t i = 0; i < chunk_str.size(); i += PRINT_CHUNK_SIZE) {
    //                            size_t print_chunk_size = (std::min)(PRINT_CHUNK_SIZE, chunk_str.size() - i);
    //                            std::string print_chunk = chunk_str.substr(i, print_chunk_size);
//
    //                            print_chunk_count++;
    //                            std::cout << "\nPrint Chunk " << print_chunk_count
    //                                << " (" << print_chunk_size << " bytes):\n"
    //                                << print_chunk << "\n---";
    //                        }
    //                    }
    //                }
//
    //                std::cout << "\nFinished processing " << total_processed
    //                    << " bytes in " << chunk_count << " data chunks" << std::endl;
    //                std::cout << "Printed in " << print_chunk_count << " display chunks" << std::endl;
    //            }
    //        }
    //        catch (const std::exception& e) {
    //            std::cerr << "\nError processing " << file << ": "
    //                << e.what() << std::endl;
    //        }
    //    }
    //}


    else if (args[0] == "write" && args.size() == 2) {
        std::string str;
        std::cout << "Input Data >> ";
        std::getline(std::cin, str);
        std::vector<char> data(1 * 1024 * 1024, 't');
        //std::vector<char> data(str.begin(), str.end());

        std::string full_path = (args[1][0] != '/')
            ? run::currentPath + (run::currentPath != "/" ? "/" : "") + args[1]
            : args[1];

        try {
            if (parse.writeFile(full_path, data, mini, false, run::Password)) {
                std::cout << "File written successfully" << std::endl;
            }
            else {
                std::cerr << "Failed to write file" << std::endl;
            }
        }
        catch (const std::exception& e) {
            mini.Disk().SetConsoleColor(mini.Disk().Red);
            std::cerr << "Error writing file: " << e.what() << '\n';
            mini.Disk().SetConsoleColor(mini.Disk().Default);

        }
    }

    //else if (args[0] == "write" && args.size() == 2) {
    //    const uint64_t totalSize =  3; // 2 جيجابايت
    //    const size_t chunkSize = 100 * 1024 * 1024; // 100 ميجابايت لكل جزء
    //
    //    std::vector<char> chunk(chunkSize, 'W'); // بيانات عينة
    //
    //    uint64_t remaining = totalSize;
    //    uint64_t written = 0;
    //
    //    while (remaining > 0) {
    //        size_t currentChunkSize = static_cast<size_t>(std::min<uint64_t>(chunkSize, remaining));
    //
    //        // true إذا كنا كتبنا جزء سابق
    //        bool append = (written > 0);
    //
    //        const std::string fullPath = (args[1][0] != '/')
    //            ? run::currentPath + '/' + args[1]
    //            : args[1];
    //
    //        parse.writeFile(fullPath,
    //            std::vector<char>(chunk.begin(), chunk.begin() + currentChunkSize),
    //            mini, append);
    //
    //        written += currentChunkSize;
    //        remaining -= currentChunkSize;
    //
    //        // عرض التقدم
    //        double progress = 100.0 * written / totalSize;
    //        std::cout << "\rProgress: " << std::fixed << std::setprecision(2)
    //            << progress << "% ("
    //            << written / (1024 * 1024) << " MB of "
    //            << totalSize / (1024 * 1024) << " MB)" << std::flush;
    //
    //        // std::this_thread::sleep_for(std::chrono::milliseconds(200)); // للتجربة فقط
    //    }
    //
    //    std::cout << "\nFile written successfully! Size: "
    //        << (totalSize / (1024 * 1024)) << " MB" << std::endl;
    //}


    else if (args[0] == "move" && args.size() == 3)
    {
        if (args[1][0] != '/' && args[2][0] != '/')
            parse.move(run::currentPath + (run::currentPath != "/" ? "/" : "") + args[1], run::currentPath + (run::currentPath != "/" ? "/" : "") + args[2], mini);
        else if (args[1][0] == '/' && args[2][0] != '/')
            parse.move(args[1], run::currentPath + (run::currentPath != "/" ? "/" : "") + args[2], mini);
        else if (args[1][0] != '/' && args[2][0] == '/')
            parse.move(run::currentPath + (run::currentPath != "/" ? "/" : "") + args[1], args[2], mini);
        else
            parse.move(args[1], args[2], mini);
    }


    else if ((args[0] == "copy" || args[0] == "cp") && args.size() == 3) {
        std::string src = args[1][0] == '/' ? args[1] : run::currentPath + (run::currentPath != "/" ? "/" : "") + args[1];
        std::string dest = args[2][0] == '/' ? args[2] : run::currentPath + (run::currentPath != "/" ? "/" : "") + args[2];
        parse.copy(src, dest, mini);
    }

    //AI
    else if (args[0] == "chatbot" && args.size() > 0 && args.size() <= 2)
    {
        if (args.size() > 1)
            parse.Chat(args[1].empty() ? "chatbot.db" : args[1]);
        else
            parse.Chat("chatbot.db");
    }

    else if (args[0] == "AI")
    {
        std::string paths = args[1][0] == '/' ? args[1] : run::currentPath + '/' + args[1];

        std::cout << "Analysis Storage : " << std::endl;
        parse.analyzeStorage(mini);

        std::cout << "Analysis Next Access : " << std::endl;
        parse.predictNextAccess(mini);

        std::cout << "Analysis Optimize File Placement : " << std::endl;
        parse.optimizeFilePlacement(paths,mini);

        std::cout << "Analysis Check Security : " << std::endl;
        parse.checkSecurity(args[0], paths, mini, run::Password);

        std::cout << "Analysis Check Security : " << std::endl;
        parse.checkSecurity(args[0], paths, mini, run::Password);

    }

    else if (args[0] == "cls")
        parse.cls();

    else if (args[0] == "map" && args.size() == 1)
        parse.printBitmap(mini);

    else if (args[0] == "exit")
        parse.exit(mini);
    
    else
        std::cout << "Error: unknown command\n";
}

//Generate Command Line after parse argument
void Tokenizer::processCommand(const std::string& command, MiniHSFS& mini) {
    auto args = ParseArguments(command);

    if (args.empty())return;

    int PID = createProcess(args[0], args);

    try {
        // Execute the actual command
        handleCommand(args, mini);
        processTable.back().state = ProcessState::Terminated;
    }
    catch (const std::exception& e) {
        mini.Disk().SetConsoleColor(mini.Disk().Red);
        std::cerr << "Error executing command: " << e.what() << std::endl;
        mini.Disk().SetConsoleColor(mini.Disk().Default);
        processTable.back().state = ProcessState::Pause;
    }
}

int Tokenizer::createProcess(const std::string& name, const std::vector<std::string>& args) {
    processTable.push_back({ nextPid++, name, args, ProcessState::Ready });
    return nextPid - 1;
}

void Tokenizer::runAll(MiniHSFS& mini) {
    for (auto& p : processTable) {
        if (p.state == ProcessState::Ready || p.state == ProcessState::Pause) {
            p.state = ProcessState::Running;
            std::cout << "\n[Running PID " << p.pid << "]: " << p.name << "\n";
            processCommand(p.name, mini);
            p.state = ProcessState::Terminated;
        }
    }
    processTable.clear();
}

//------------------------------------------//Search PID Process

bool Tokenizer::isRunning(int pid) const {
    const auto* p = findProcess(pid);
    return p && p->state == ProcessState::Running;
}

bool Tokenizer::isTerminated(int pid) const {
    const auto* p = findProcess(pid);
    return p && p->state == ProcessState::Terminated;
}

bool Tokenizer::isReady(int pid) const {
    const auto* p = findProcess(pid);
    return p && p->state == ProcessState::Ready;
}
//------------------------------------------//Process PID Process

Tokenizer::Process* Tokenizer::findProcess(int pid) {
    for (auto& p : processTable) {
        if (p.pid == pid) return &p;
    }
    return nullptr;
}

const Tokenizer::Process* Tokenizer::findProcess(int pid) const {
    for (const auto& p : processTable) {
        if (p.pid == pid) return &p;
    }
    return nullptr;
}

void Tokenizer::stopProcess(int pid) {
    auto* p = findProcess(pid);
    if (p && p->state == ProcessState::Running) {
        p->state = ProcessState::Pause;
    }
}

void Tokenizer::monitorProcesses() const {
    std::cout << "\nActive Processes:\n";
    std::cout << "----------------\n";
    for (const auto& p : processTable) {
        std::cout << "PID: " << p.pid << " | ";
        std::cout << "Command: " << p.name << " | ";
        std::cout << "State: ";

        switch (p.state) {
        case ProcessState::Ready: std::cout << "Ready"; break;
        case ProcessState::Running: std::cout << "Running"; break;
        case ProcessState::Terminated: std::cout << "Terminated"; break;
        default: std::cout << "Not Fount This State"; break;
        }

        std::cout << "\n";
    }
}