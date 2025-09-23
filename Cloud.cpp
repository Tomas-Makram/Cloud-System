#include "Cloud.h"

std::string Cloud::getIPfromIpconfig() {
    std::string result;
    std::array<char, 256> buffer;

#ifdef _WIN32
    const char* command = "ipconfig";
    std::unique_ptr<FILE, decltype(&_pclose)> pipe(_popen(command, "r"), _pclose);
#else
    const char* command = "ifconfig | grep -Eo 'inet (addr:)?([0-9]*\\.){3}[0-9]*' | grep -Eo '([0-9]*\\.){3}[0-9]*' | grep -v '127.0.0.1'";
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command, "r"), pclose);
#endif

    if (!pipe) {
        throw std::runtime_error("Failed to run ipconfig/ifconfig");
    }

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
        std::string line(buffer.data());

#ifdef _WIN32
        if (line.find("IPv4 Address") != std::string::npos || line.find("IPv4") != std::string::npos) {
#else
        if (!line.empty()) {
#endif
            std::smatch match;
            std::regex ipRegex("(\\d+\\.\\d+\\.\\d+\\.\\d+)");
            if (std::regex_search(line, match, ipRegex)) {
                return match.str(1);
            }
        }
    }

    return "127.0.0.1"; // Default to localhost if not found
}

std::string Cloud::escapeHtml(const std::string & input) {
    std::string output;
    output.reserve(input.size());
    for (char c : input) {
        switch (c) {
        case '&': output += "&amp;"; break;
        case '<': output += "&lt;"; break;
        case '>': output += "&gt;"; break;
        case '"': output += "&quot;"; break;
        case '\'': output += "&#39;"; break;
        default: output += c; break;
        }
    }
    return output;
}

std::string Cloud::formatTime(time_t timestamp) {
    char buffer[80];
    tm timeInfo;

#ifdef _WIN32
    localtime_s(&timeInfo, &timestamp);
#else
    localtime_r(&timestamp, &timeInfo);
#endif

    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeInfo);
    return std::string(buffer);
}

std::string Cloud::formatSize(size_t bytes) {
    const char* sizes[] = { "B", "KB", "MB", "GB" };
    int i = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024 && i < 3) {
        size /= 1024;
        i++;
    }
    char result[20];
    snprintf(result, sizeof(result), "%.2f %s", size, sizes[i]);
    return result;
}

void Cloud::setupRoutes(httplib::Server& svr, Parser& parse, MiniHSFS& mini, Tokenizer& tokenizer) {
    // واجهة المستخدم الرئيسية
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        std::ifstream file("index.html");
        if (!file) {
            res.set_content("Could not open index.html", "text/plain");
            return;
        }

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        res.set_content(content, "text/html");
    });

    // API نقاط النهاية
    svr.Get("/list", [&parse, &mini](const httplib::Request& req, httplib::Response& res) {
        try {
            //req.has_param("path") ? req.get_param_value("path") :
            std::string path = run::currentPath;

            // الحصول على عناصر المجلد
            auto items = parse.getDirectoryItems(path, mini);
            std::stringstream ss;

            for (const auto& item : items.entries) {
                const auto& child = mini.inodeTable[item.second]; // inode نفسه

                bool isDir = child.isDirectory;
                std::string icon = isDir ? "bi-folder-fill" : "bi-file-earmark";
                std::string size = isDir ? "" : formatSize(child.size);
                std::string name = item.first;

                //std::cout << item.entries.begin()->first << " " << item.isDirectory << '\n';

                ss << R"(<div class="file-item" data-type=")" << (isDir ? "dir" : "file") << R"(" 
                    data-path=")" << escapeHtml(path + (path == "/" ? "" : "/") + name) << R"(" 
                    data-name=")" << escapeHtml(name) << R"(" 
                    data-size=")" << size << R"(" 
                    data-modified=")" << formatTime(child.modificationTime) << R"(">)"
                    << R"(<div class="file-icon"><i class="bi )" << icon << R"("></i></div>)"
                    << R"(<div class="file-info"><div class="fw-bold file-name-text">)" << escapeHtml(name)
                    << R"(</div><div class="text-muted small">)"
                    << formatTime(child.modificationTime) << R"(</div></div>)"
                    << R"(<div class="file-actions">)"
                    << "</div></div>";
            }

            res.set_header("Content-Type", "text/html");
            res.set_content(ss.str(), "text/html");
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
        }
    });

    // تغيير المجلد الحالي
    svr.Post("/cd", [&parse, &mini](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string path = req.has_param("path") ? req.get_param_value("path") : "/";
            parse.cd(path, mini);
            res.set_content("Changed directory to: " + path, "text/plain");
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
        }
    });

    // عرض محتويات المجلد (ls)
    svr.Get("/ls", [&parse, &mini](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string options = req.has_param("options") ? req.get_param_value("options") : "";
            std::string path = req.has_param("path") ? req.get_param_value("path") : "";

            std::string argument;
            if (!options.empty()) {
                argument += "-" + options;
            }
            if (!path.empty()) {
                if (!argument.empty()) argument += " ";
                argument += path;
            }

            // حفظ الإخراج في buffer مؤقت
            std::stringstream buffer;
            std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

            parse.ls(argument, mini);

            std::cout.rdbuf(old);
            res.set_content(buffer.str(), "text/plain");
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
        }
    });

    // معلومات نظام الملفات
    svr.Get("/info", [&parse, &mini](const httplib::Request&, httplib::Response& res) {
        try {
            std::stringstream buffer;
            std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

            parse.printFileSystemInfo(mini);

            std::cout.rdbuf(old);
            res.set_content(buffer.str(), "text/plain");
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
        }
    });

    // هيكل الشجرة (tree)
    svr.Get("/tree", [&parse, &mini](const httplib::Request&, httplib::Response& res) {
        try {
            std::stringstream buffer;
            std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

            parse.PrintBTreeStructure(mini);

            std::cout.rdbuf(old);
            res.set_content(buffer.str(), "text/plain");
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
        }
        });

    // إنشاء مجلد جديد
    svr.Post("/mkdir", [&parse, &mini](const httplib::Request& req, httplib::Response& res) {
        std::string path = req.get_param_value("path");
        try {
            // استخراج اسم المجلد من المسار
            size_t lastSlash = path.find_last_of('/');
            std::string dirPath = path.substr(0, lastSlash);
            std::string dirName = path.substr(lastSlash + 1);

            if (dirPath.empty()) dirPath = "/";

            parse.createDirectory(path, "", mini);
            res.set_content("Directory created", "text/plain");
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
        }
    });

    // إنشاء ملف جديد
    svr.Post("/createfile", [&parse, &mini](const httplib::Request& req, httplib::Response& res) {
        std::string path = req.get_param_value("path");
        std::string name = req.get_param_value("name");
        try {
            int result = parse.createFile(path, name, mini);
            if (result >= 0) {
                res.set_content("File created successfully", "text/plain");
            }
            else {
                res.status = 400;
                res.set_content("Failed to create file", "text/plain");
            }
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
        }
    });

    // إعادة تسمية ملف أو مجلد
    svr.Post("/rename", [&parse, &mini](const httplib::Request& req, httplib::Response& res) {
        std::string oldPath = req.get_param_value("old_path");
        std::string newName = req.get_param_value("new_name");
        bool isDir = req.get_param_value("is_dir") == "true";

        try {
            parse.rename(oldPath, newName, mini);
            res.set_content("Renamed", "text/plain");
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
        }
    });

    // نسخ ملف أو مجلد
    svr.Post("/copy", [&parse, &mini](const httplib::Request& req, httplib::Response& res) {
        std::string srcPath = req.get_param_value("src");
        std::string destPath = req.get_param_value("dest");
        try {
            bool success = parse.copy(srcPath, destPath, mini);
            if (success) {
                res.set_content("Copied successfully", "text/plain");
            }
            else {
                res.status = 400;
                res.set_content("Failed to copy", "text/plain");
            }
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
        }
        });

    // قص/نقل ملف أو مجلد
    svr.Post("/move", [&parse, &mini](const httplib::Request& req, httplib::Response& res) {
        std::string srcPath = req.get_param_value("src");
        std::string destPath = req.get_param_value("dest");
        try {
            bool success = parse.move(srcPath, destPath, mini);
            if (success) {
                res.set_content("Moved successfully", "text/plain");
            }
            else {
                res.status = 400;
                res.set_content("Failed to move", "text/plain");
            }
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
        }
    });

    // حذف مجلد أو ملف
    svr.Post("/delete", [&parse, &mini](const httplib::Request& req, httplib::Response& res) {
        std::string path = req.get_param_value("path");
        bool isDir = req.get_param_value("is_dir") == "true";
        try {
            if (isDir) {
                parse.deleteDirectory(path, mini);
            }
            else {
                parse.deleteFile(path, mini);
            }
            res.set_content("Deleted", "text/plain");
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
        }
    });

    // الحصول على خصائص المجلد
    svr.Get("/properties", [&parse, &mini](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string path = req.get_param_value("path");

            // الحصول على معلومات المجلد
            std::vector<std::string> parts = mini.SplitPath(path);
            int inodeIndex = mini.PathToInode(parts);

            if (inodeIndex == -1) {
                throw std::runtime_error("Directory not found");
            }

            const MiniHSFS::Inode& inode = mini.inodeTable[inodeIndex];

            // إنشاء JSON مع المعلومات
            std::stringstream json;
            json << "{";
            json << "\"name\":\"" << parts.back() << "\",";
            json << "\"path\":\"" << path << "\",";
            json << "\"created\":\"" << formatTime(inode.creationTime) << "\",";
            json << "\"modified\":\"" << formatTime(inode.modificationTime) << "\",";

            if (inode.isDirectory) {
                json << "\"item_count\":" << inode.entries.size() << ",";
                json << "\"type\":\"directory\"";
            }
            else {
                json << "\"size\":" << inode.size << ",";
                json << "\"type\":\"file\"";
            }
            json << "}";

            res.set_header("Content-Type", "application/json");
            res.set_content(json.str(), "application/json");
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
        }
    });

    // قراءة محتوى الملف
    svr.Get("/readfile", [&parse, &mini](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string encoded_path = req.get_param_value("path");
            std::string path = httplib::detail::decode_url(encoded_path, false);

            if (!mini.mounted) {
                throw std::runtime_error("Filesystem not mounted");
            }

            mini.ValidatePath(path);

            int inode_index = mini.FindFile(path);
            if (inode_index == -1) {
                throw std::runtime_error("File not found");
            }

            MiniHSFS::Inode& inode = mini.inodeTable[inode_index];
            if (inode.isDirectory) {
                throw std::runtime_error("Cannot read directory as file");
            }

            // دعم لنطاقات البايتات
            size_t start = 0;
            size_t end = inode.size - 1;

            if (req.has_header("Range")) {
                std::string range = req.get_header_value("Range");
                if (range.find("bytes=") == 0) {
                    range = range.substr(6);
                    size_t dash_pos = range.find('-');
                    if (dash_pos != std::string::npos) {
                        start = std::stoull(range.substr(0, dash_pos));
                        if (dash_pos < range.length() - 1) {
                            end = std::stoull(range.substr(dash_pos + 1));
                        }
                    }
                }
            }

            // تأكد من أن النطاق صحيح
            start = (std::min)(start, inode.size - 1);
            end = (std::min)(end, inode.size - 1);
            size_t length = end - start + 1;

            // قراءة الجزء المطلوب من الملف
            std::vector<char> data = parse.readFile(path, mini, start, length, run::Password);

            if (req.has_header("Range")) {
                res.status = 206;
                std::ostringstream oss;
                oss << "bytes " << start << "-" << end << "/" << inode.size;
                res.set_header("Content-Range", oss.str());
            }

            res.set_header("Content-Type", "application/octet-stream");
            res.set_header("Content-Length", std::to_string(data.size()));
            res.set_header("Accept-Ranges", "bytes");
            res.set_content(data.data(), data.size(), "application/octet-stream");
        }
        catch (const std::exception& e) {
            std::cerr << "File read error: " << e.what() << std::endl;
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
        }
    });

    // كتابة محتوى الملف
    svr.Post("/writefile", [&parse, &mini](const httplib::Request& req, httplib::Response& res) {
        try {
            // استقبال البيانات كنموذج FormData
            auto path = req.get_file_value("path").content;
            auto content = req.get_file_value("content").content;

            std::vector<char> data(content.begin(), content.end());
            bool success = parse.writeFile(path, data, mini, false, run::Password);

            if (success) {
                res.set_content("File saved successfully", "text/plain");
            }
            else {
                res.status = 400;
                res.set_content("Failed to save file", "text/plain");
            }
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("Error: ") + e.what(), "text/plain");
        }
    });

    // تحليل التخزين (AI)
    svr.Get("/analyze", [&parse, &mini](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string path = req.has_param("path") ? req.get_param_value("path") : "/";

            std::stringstream buffer;
            std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

            std::cout << "Storage Analysis:" << std::endl;
            parse.analyzeStorage(mini);

            std::cout << "\nNext Access Prediction:" << std::endl;
            parse.predictNextAccess(mini);

            std::cout << "\nFile Placement Optimization:" << std::endl;
            parse.optimizeFilePlacement(path, mini);

            std::cout << "\nSecurity Check:" << std::endl;
            parse.checkSecurity("analyze", path, mini, "123");

            std::cout.rdbuf(old);
            res.set_content(buffer.str(), "text/plain");
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
        }
    });

    // عرض خريطة التخزين (map)
    svr.Get("/map", [&parse, &mini](const httplib::Request&, httplib::Response& res) {
        try {
            std::stringstream buffer;
            std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

            parse.printBitmap(mini);

            std::cout.rdbuf(old);
            res.set_content(buffer.str(), "text/plain");
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
        }
    });

    // مسح الشاشة (cls)
    svr.Post("/clear", [&parse](const httplib::Request&, httplib::Response& res) {
        try {
            parse.cls();
            res.set_content("Screen cleared", "text/plain");
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
        }
    });

    // إدارة العمليات
    svr.Get("/processes", [&tokenizer](const httplib::Request&, httplib::Response& res) {
        try {
            std::stringstream buffer;
            std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

            tokenizer.monitorProcesses();

            std::cout.rdbuf(old);
            res.set_content(buffer.str(), "text/plain");
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
        }
    });

    // تنفيذ أمر مخصص
    svr.Post("/command", [&tokenizer, &mini](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string command = req.get_param_value("command");

            std::stringstream buffer;
            std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

            tokenizer.processCommand(command, mini);

            std::cout.rdbuf(old);
            res.set_content(buffer.str(), "text/plain");
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
        }
    });

    // إيقاف النظام
    svr.Post("/shutdown", [&svr, &parse, &mini](const httplib::Request&, httplib::Response& res) {
        try {
            parse.exit(mini);  // عمليات حفظ أو تنظيف

            res.set_content("System is shutting down", "text/plain");

            // تشغيل إيقاف السيرفر في Thread منفصل حتى لا تتوقف استجابة HTTP
            std::thread shutdown_thread([&svr]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                svr.stop();  // ✅ إيقاف السيرفر بعد الرد
                });
            shutdown_thread.detach();  // افصله عن الـ main thread
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
        }
    });
}