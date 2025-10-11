#include "Cloud.h"

Cloud::Cloud(){};

Cloud::~Cloud(){};

std::string Cloud::generateSessionId() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);

    return "session_" + std::to_string(millis) + "_" + std::to_string(dis(gen));
}

Cloud::auth Cloud::checkSession(const httplib::Request& req) {

    if (req.has_header("Cookie")) {
        auto cookies = req.get_header_value("Cookie");
        size_t pos = cookies.find("session_id=");
        if (pos != std::string::npos) {
            std::string session_part = cookies.substr(pos + 11);
            size_t end_pos = session_part.find(';');
            std::string session_id = (end_pos == std::string::npos) ? session_part : session_part.substr(0, end_pos);

            auto it = activeSessions.find(session_id);
            if (it != activeSessions.end()) {
                return it->second;
            }
        }
    }
    return auth{};
}

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

Cloud::auth Cloud::authenticateUser(const httplib::Request& req) {
    auth user;

    if (!req.has_param("user") || !req.has_param("pass") || !req.has_param("dir"))
        return user;

    user.username = req.get_param_value("user");
    user.password = req.get_param_value("pass");
    user.dirname = req.get_param_value("dir");
    if (req.has_param("sp")) {
        std::string spValue = req.get_param_value("sp");
        if (!spValue.empty() && std::all_of(spValue.begin(), spValue.end(), ::isdigit)) {
            user.strongPassword = std::stoi(spValue);
        }
        else {
            user.strongPassword = 0;
        }
    }
    else {
        user.strongPassword = 0;
    }

    return user;
}

void Cloud::setupRoutes(httplib::Server& svr, Parser& parse, MiniHSFS& mini, Tokenizer& tokenizer, std::string& currentPath, std::string& password) {

    // Main authentication page
    svr.Get("/", [this](const httplib::Request& req, httplib::Response& res) {

        auto user = checkSession(req);
        if (user.username.empty()) {

            std::string loginPage = R"(
<!DOCTYPE html>
<html lang="ar" dir="rtl">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ABYDOS - Login</title>
    <link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.min.css" rel="stylesheet">
    <link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/bootstrap-icons@1.10.0/font/bootstrap-icons.css">
    <style>
        :root { --primary-color: #5c6bc0; --hover-color: #3949ab; --danger-color: #e53935; --success-color: #43a047; }
        body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: #f5f5f5; height: 100vh; display: flex; align-items: center; justify-content: center; }
        .auth-container { width: 100%; max-width: 400px; margin: 0 auto; }
        .auth-card { border: none; border-radius: 10px; box-shadow: 0 0.15rem 1.75rem 0 rgba(58, 59, 69, 0.15); overflow: hidden; }
        .auth-header { background: linear-gradient(180deg, var(--primary-color) 0%, #3f51b5 100%); color: white; padding: 20px; text-align: center; }
        .auth-body { padding: 30px; background-color: white; }
        .btn-primary { background-color: var(--primary-color); border-color: var(--primary-color); }
        .btn-primary:hover { background-color: var(--hover-color); border-color: var(--hover-color); }
    </style>
</head>
<body>
    <div class="auth-container">
        <div class="auth-card" id="login-card">
            <div class="auth-header">
                <div class="logo">ABYDOS</div>
                <div class="subtitle">Advanced File Management System</div>
            </div>
            <div class="auth-body">
                <h4 class="text-center mb-4">Log in</h4>
                <form id="login-form">
                    <div class="mb-3">
                        <label class="form-label">Username or Email or Dirname</label>
                        <input type="text" class="form-control" id="username" required>
                    </div>
                    <div class="mb-3">
                        <label class="form-label">Password</label>
                        <input type="password" class="form-control" id="password" required>
                    </div>
                    <div class="mb-3">
                        <label class="form-label">Strong Password</label>
                        <input type="number" class="form-control" id="strongPassword" required>
                    </div>
                    <button type="submit" class="btn btn-primary w-100">Log in</button>
                </form>
                <div class="text-center mt-3">
                    <a href="#" id="switch-to-signup">Create a new account</a>
                </div>
            </div>
        </div>
    </div>
    <script>
        document.getElementById('login-form').addEventListener('submit', function(e) {
            e.preventDefault();
            const username = document.getElementById('username').value;
            const password = document.getElementById('password').value;
            const strongPassword = document.getElementById('strongPassword').value;
            
            fetch('/auth/login', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: 'username=' + encodeURIComponent(username) + '&password=' + encodeURIComponent(password) + '&strongPassword=' + encodeURIComponent(strongPassword)
            }).then(response => {
                if (response.ok) {
                    window.location.href = '/files';
                } else {
                    alert('login failed');
                }
            });
        });
        
        document.getElementById('switch-to-signup').addEventListener('click', function() {
            window.location.href = '/auth/signup';
        });
    </script>
</body>
</html>
        )";
            res.set_content(loginPage, "text/html");
        }
        else
        {
            res.status = 302;
            res.set_header("Location", "/files");
        }
        });
    
    // صفحة إنشاء حساب جديد
    svr.Get("/auth/signup", [](const httplib::Request&, httplib::Response& res) {
        std::string signupPage = R"(
<!DOCTYPE html>
<html lang="ar" dir="rtl">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>MiniHSFS - إنشاء حساب</title>
    <link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.min.css" rel="stylesheet">
    <style>
        :root { --primary-color: #5c6bc0; --hover-color: #3949ab; }
        body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: #f5f5f5; height: 100vh; display: flex; align-items: center; justify-content: center; }
        .auth-container { width: 100%; max-width: 400px; margin: 0 auto; }
        .auth-card { border: none; border-radius: 10px; box-shadow: 0 0.15rem 1.75rem 0 rgba(58, 59, 69, 0.15); }
        .auth-header { background: linear-gradient(180deg, var(--primary-color) 0%, #3f51b5 100%); color: white; padding: 20px; text-align: center; }
        .btn-primary { background-color: var(--primary-color); border-color: var(--primary-color); }
    </style>
</head>
<body>
    <div class="auth-container">
        <div class="auth-card">
            <div class="auth-header">
                <div class="logo">MiniHSFS</div>
                <div class="subtitle">إنشاء حساب جديد</div>
            </div>
            <div class="auth-body" style="padding: 30px; background: white;">
                <h4 class="text-center mb-4">إنشاء حساب</h4>
                <form id="signup-form">
                    <div class="mb-3">
                        <label class="form-label">اسم المستخدم</label>
                        <input type="text" class="form-control" id="username" required>
                    </div>
                    <div class="mb-3">
                        <label class="form-label">البريد الإلكتروني</label>
                        <input type="email" class="form-control" id="email" required>
                    </div>
                    <div class="mb-3">
                        <label class="form-label">كلمة المرور</label>
                        <input type="password" class="form-control" id="password" required>
                    </div>
                    <div class="mb-3">
                        <label class="form-label">تأكيد كلمة المرور</label>
                        <input type="password" class="form-control" id="confirm-password" required>
                    </div>
                    <button type="submit" class="btn btn-primary w-100">إنشاء حساب</button>
                </form>
                <div class="text-center mt-3">
                    <a href="/">العودة لتسجيل الدخول</a>
                </div>
            </div>
        </div>
    </div>
    <script>
        document.getElementById('signup-form').addEventListener('submit', function(e) {
            e.preventDefault();
            const username = document.getElementById('username').value;
            const email = document.getElementById('email').value;
            const password = document.getElementById('password').value;
            const confirmPassword = document.getElementById('confirm-password').value;
            
            if (password !== confirmPassword) {
                alert('كلمتا المرور غير متطابقتين');
                return;
            }
            
            fetch('/auth/signup', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: 'username=' + encodeURIComponent(username) + '&email=' + encodeURIComponent(email) + '&password=' + encodeURIComponent(password)
            }).then(response => {
                if (response.ok) {
                    window.location.href = '/';
                } else {
                    alert('فشل إنشاء الحساب');
                }
            });
        });
    </script>
</body>
</html>
        )";
        res.set_content(signupPage, "text/html");
        });

    // Login processing
    svr.Post("/auth/login", [this, &mini](const httplib::Request& req, httplib::Response& res) {
        CryptoUtils crypto;
    
        std::string dirname = req.get_param_value("username");
        std::string password = req.get_param_value("password");
        int strongPassword = std::stoi(req.get_param_value("strongPassword").empty() ? "0" : req.get_param_value("strongPassword"));
    
    
        // Data validation
        if (dirname.empty() || password.empty() || strongPassword == 0) {
            res.status = 400;
            res.set_content("All fields must be completed", "text/plain");
            return;
        }
    
        std::string sessionId = this->generateSessionId();
        auth userSession;

        int indexAccount = -1;
        if (mini.inodeTable[mini.rootNodeIndex].entries.count(dirname) > 0)
            indexAccount = mini.inodeTable[mini.rootNodeIndex].entries.find(dirname)->second;
        
        if (indexAccount != -1) {
            if (crypto.ValidatePassword(password, mini.inodeTable[indexAccount].inodeInfo.Password, strongPassword)) {
                
                // Create a new session
                userSession.index = indexAccount;
                userSession.dirname = dirname;
                userSession.username = mini.inodeTable[indexAccount].inodeInfo.UserName;
                userSession.password = password;//std::string(mini.inodeTable[indexAccount].inodeInfo.Password.begin(), mini.inodeTable[indexAccount].inodeInfo.Password.end());
                userSession.email = mini.inodeTable[indexAccount].inodeInfo.Email;
                userSession.strongPassword = strongPassword;

                this->activeSessions[sessionId] = userSession;

                // Set session cookie
                res.set_header("Set-Cookie", "session_id=" + sessionId + "; Path=/; HttpOnly");
                res.set_content("You have successfully logged in", "text/plain");
            }
            else {
                res.status = 401;
                res.set_content("Incorrect password", "text/plain");
            }

        }
        else {

            auto& entries = mini.inodeTable[mini.rootNodeIndex].entries;
            bool found = false;

            for (auto it = entries.begin(); it != entries.end(); ++it) {
                if ((mini.inodeTable[it->second].inodeInfo.Email == dirname || mini.inodeTable[it->second].inodeInfo.UserName == dirname) && crypto.ValidatePassword(password, mini.inodeTable[it->second].inodeInfo.Password, strongPassword)) {

                    // Create a new session
                    userSession.index = it->second;
                    userSession.dirname = it->first;
                    userSession.username = mini.inodeTable[it->second].inodeInfo.UserName;
                    userSession.password = password;//std::string(mini.inodeTable[it->second].inodeInfo.Password.begin(), mini.inodeTable[it->second].inodeInfo.Password.end());
                    userSession.email = mini.inodeTable[it->second].inodeInfo.Email;
                    userSession.strongPassword = strongPassword;

                    this->activeSessions[sessionId] = userSession;

                    // Set session cookie
                    res.set_header("Set-Cookie", "session_id=" + sessionId + "; Path=/; HttpOnly");
                    res.set_content("You have successfully logged in", "text/plain");
                    found = true;
                    break;
                }
            }
            // After we finish the loop, if we don't find my account
            if (!found) {
                res.status = 404;
                res.set_content("User not found or Incorrect password", "text/plain");
            }
        }
        });
  
    // معالجة إنشاء حساب جديد
    svr.Post("/auth/signup", [&](const httplib::Request& req, httplib::Response& res) {
        std::string username = req.get_param_value("username");
        std::string email = req.get_param_value("email");
        std::string password = req.get_param_value("password");

        // التحقق من صحة البيانات
        if (username.empty() || email.empty() || password.empty()) {
            res.status = 400;
            res.set_content("يجب ملء جميع الحقول", "text/plain");
            return;
        }

        // التحقق من أن اسم المستخدم غير مستخدم
     /*   if (users.find(username) != users.end()) {
            res.status = 409;
            res.set_content("اسم المستخدم موجود مسبقاً", "text/plain");
            return;
        }*/

        // التحقق من صحة البريد الإلكتروني
        if (email.find('@') == std::string::npos) {
            res.status = 400;
            res.set_content("البريد الإلكتروني غير صحيح", "text/plain");
            return;
        }

        // التحقق من قوة كلمة المرور (6 أحرف على الأقل)
        if (password.length() < 6) {
            res.status = 400;
            res.set_content("كلمة المرور يجب أن تكون 6 أحرف على الأقل", "text/plain");
            return;
        }

        // إنشاء مجلد للمستخدم الجديد
        std::string userDir = "/home/" + username;

        // إضافة المستخدم إلى قاعدة البيانات
        //users[username] = std::make_tuple(password, email, userDir);

        // إنشاء مجلد المستخدم في نظام الملفات
        try {
            parse.createDirectory("/home", username, mini, currentPath);
            res.set_content("تم إنشاء الحساب بنجاح", "text/plain");
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("خطأ في إنشاء مجلد المستخدم: " + std::string(e.what()), "text/plain");
        }
        });

    // تسجيل الخروج
    svr.Post("/auth/logout", [&](const httplib::Request& req, httplib::Response& res) {
        auto userSession = checkSession(req);
        if (!userSession.username.empty()) {
            // البحث عن الجلسة وحذفها
            if (req.has_header("Cookie")) {
                auto cookies = req.get_header_value("Cookie");
                size_t pos = cookies.find("session_id=");
                if (pos != std::string::npos) {
                    std::string session_id = cookies.substr(pos + 11, 32);
                    activeSessions.erase(session_id);
                }
            }
            res.set_header("Set-Cookie", "session_id=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
            res.set_content("تم تسجيل الخروج", "text/plain");
        }
        else {
            res.status = 401;
            res.set_content("غير مصرح بالوصول", "text/plain");
        }
        });

    // Main user interface
    svr.Get("/files", [this, &parse, &mini, &currentPath](const httplib::Request& req, httplib::Response& res) {

        auto user = checkSession(req);
        if (user.username.empty()) {
            res.status = 302;
            res.set_header("Location", "/");
            return;
        }
        try {

            std::string path = "/";
            std::vector<std::string> parts = mini.SplitPath(req.get_param_value("path"));
            if (!parts.empty()) {
                for (size_t i = 0; i < parts.size(); ++i) {
                    if (!parts[i].empty())
                    {
                        path += parts[i];
                        path += "/";
                    }
                }
            }

            if (!req.get_param_value("path").empty()) {

                // This is a request for file data
                parse.SetAccount(user.username, user.dirname, user.password, user.email, user.strongPassword, mini.inodeTable[user.index].inodeInfo.TotalSize);


                if (!(parts.size() > 0 && req.get_param_value("path")[0] == '/' && parts[0] == user.dirname))
                    path = "/" + user.dirname + "/";

                auto items = parse.getDirectoryItems(path, mini, currentPath);

                res.set_content(path, "text/html");

                std::stringstream ss;
                for (const auto& item : items.entries) {
                    const auto& child = mini.inodeTable[item.second];
                    bool isDir = child.isDirectory;
                    std::string icon = isDir ? "bi-folder-fill" : "bi-file-earmark";

                    ss << R"(<div class="file-item" data-type=")" << (isDir ? "dir" : "file") << R"(" 
                    data-path=")" << escapeHtml(path + (path == "/" ? "" : "/") + item.first) << R"(" 
                    data-name=")" << escapeHtml(item.first) << R"(">)"
                        << R"(<div class="file-icon"><i class="bi )" << icon << R"("></i></div>)"
                        << R"(<div class="file-info">)" << escapeHtml(item.first) << R"(</div></div>)";
                }

                res.set_content(ss.str(), "text/html");
            }
            else {
                //This is a request for the home page
                std::ifstream file("index.html");
                if (!file) {
                    res.set_content("Interface not found", "text/plain");
                    return;
                }

                std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                res.set_content(content, "text/html");
            }

        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
            std::cout << "Error: " + std::string(e.what()) << "\n";

        }
        });

    // Create a new directory
    svr.Post("/mkdir", [this, &parse, &mini, &currentPath](const httplib::Request& req, httplib::Response& res) {

        auto user = checkSession(req);
        if (user.username.empty()) {
            res.status = 302;
            res.set_header("Location", "/");
            throw "You are not logged in yet. Please log in first";
            return;
        }

        std::string path = req.get_param_value("path");

        try {

            parse.SetAccount(user.username, user.dirname, user.password, user.email, user.strongPassword, mini.inodeTable[user.index].inodeInfo.TotalSize);

            std::vector<std::string> parts = mini.SplitPath(path);
            std::string dirPath = "/";
            if (!parts.empty()) {
                for (size_t i = 0; i < parts.size() - 1; ++i) {
                    if (!parts[i].empty())
                    {
                        dirPath += parts[i];
                        if (i < parts.size() - 2) {
                            dirPath += "/";
                        }
                    }
                }
            }
            else {
                dirPath = "/";
            }

            if (!(parts.size() > 0 && path[0] == '/' && parts[0] == user.dirname))
                dirPath = "/" + user.dirname + "/";

            std::string dirName = parts.back();

            parse.createDirectory(dirPath, dirName, mini, currentPath);
            res.set_content("Directory created", "text/plain");
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
            std::cout << "Error: " + std::string(e.what()) << "\n";
        }
        });
    
    // Create a new file
    svr.Post("/createfile", [this, &parse, &mini, &currentPath](const httplib::Request& req, httplib::Response& res) {

        auto user = checkSession(req);
        if (user.username.empty()) {
            res.status = 302;
            res.set_header("Location", "/");
            throw "You are not logged in yet. Please log in first";
            return;
        }

        std::string path = req.get_param_value("path");

        try {

            parse.SetAccount(user.username, user.dirname, user.password, user.email, user.strongPassword, mini.inodeTable[user.index].inodeInfo.TotalSize);

            std::vector<std::string> parts = mini.SplitPath(path);
            std::string filePath = "/";
            if (!parts.empty()) {
                for (size_t i = 0; i < parts.size() - 1; ++i) {
                    if (!parts[i].empty())
                    {
                        filePath += parts[i];
                        if (i < parts.size() - 2) {
                            filePath += "/";
                        }
                    }
                }
            }
            else {
                filePath = "/";
            }

            if (!(parts.size() > 0 && path[0] == '/' && parts[0] == user.dirname))
                filePath = "/" + user.dirname + "/";

            std::string fileName = parts.back();

            parse.createFile(filePath, fileName, mini, currentPath);
            res.set_content("File created Sucessfully", "text/plain");
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
            std::cout << "Error: " + std::string(e.what()) << "\n";
        }
        });

    // Get Inode properties
    svr.Post("/properties", [this, &parse, &mini](const httplib::Request& req, httplib::Response& res) {
        
        auto user = checkSession(req);
        if (user.username.empty()) {
            res.status = 302;
            res.set_header("Location", "/");
            throw "You are not logged in yet. Please log in first";
            return;
        }
        
        std::string path = req.get_param_value("path");
        std::vector<std::string> parts = mini.SplitPath(path);

        try {
            parse.SetAccount(user.username, user.dirname, user.password, user.email, user.strongPassword, mini.inodeTable[user.index].inodeInfo.TotalSize);

            std::string propPath = "/";
            if (!parts.empty()) {
                for (size_t i = 0; i < parts.size(); ++i) {
                    if (!parts[i].empty())
                    {
                        propPath += parts[i];
                        if (i < parts.size() - 2) {
                            propPath += "/";
                        }
                    }
                }
            }
            else {
                propPath = "/";
            }

            // Get inode index to get information
            int inodeIndex = mini.PathToInode(mini.SplitPath(propPath));

            if (inodeIndex == -1) {
                throw std::runtime_error("Directory not found");
            }

            const MiniHSFS::Inode& inode = mini.inodeTable[inodeIndex];

            // Create JSON with inode information
            std::stringstream json;
            json << "{";
            json << "\"name\":\"" << parts.back() << "\",";
            json << "\"propPath\":\"" << propPath << "\",";
            json << "\"created\":\"" << this->formatTime(inode.creationTime) << "\",";
            json << "\"modified\":\"" << this->formatTime(inode.modificationTime) << "\",";

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

    // تغيير المجلد الحالي
    svr.Post("/cd", [&parse, &mini](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string path = req.has_param("path") ? req.get_param_value("path") : "/";
            //parse.cd(path, mini);
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

//            parse.ls(argument, mini);

            std::cout.rdbuf(old);
            res.set_content(buffer.str(), "text/plain");
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
        }
    });

    // معلومات نظام الملفات
    svr.Get("/info", [&parse, &mini, &currentPath](const httplib::Request&, httplib::Response& res) {
        try {
            std::stringstream buffer;
            std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

            parse.printFileSystemInfo(mini, currentPath);

            std::cout.rdbuf(old);
            res.set_content(buffer.str(), "text/plain");
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
        }
    });

    // هيكل الشجرة (tree)
    svr.Get("/tree", [&parse, &mini, &currentPath] (const httplib::Request&, httplib::Response& res) {
        try {
            std::stringstream buffer;
            std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

            parse.PrintBTreeStructure(mini, currentPath);

            std::cout.rdbuf(old);
            res.set_content(buffer.str(), "text/plain");
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
        }
        });

    // إعادة تسمية ملف أو مجلد
    svr.Post("/rename", [&parse, &mini, &currentPath](const httplib::Request& req, httplib::Response& res) {
        std::string oldPath = req.get_param_value("old_path");
        std::string newName = req.get_param_value("new_name");
        bool isDir = req.get_param_value("is_dir") == "true";

        try {
            parse.rename(oldPath, newName, mini, currentPath);
            res.set_content("Renamed", "text/plain");
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
        }
    });

    // نسخ ملف أو مجلد
    svr.Post("/copy", [&parse, &mini, &currentPath](const httplib::Request& req, httplib::Response& res) {
        std::string srcPath = req.get_param_value("src");
        std::string destPath = req.get_param_value("dest");
        try {
            bool success = parse.copy(srcPath, destPath, mini, currentPath);
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
    svr.Post("/move", [&parse, &mini, &currentPath](const httplib::Request& req, httplib::Response& res) {
        std::string srcPath = req.get_param_value("src");
        std::string destPath = req.get_param_value("dest");
        try {
            bool success = parse.move(srcPath, destPath, mini, currentPath);
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
    svr.Post("/delete", [this, &parse, &mini, &currentPath](const httplib::Request& req, httplib::Response& res) {

        auto user = authenticateUser(req);

        parse.SetAccount(user.username, user.dirname, user.password, user.email, user.strongPassword, mini.inodeTable[user.index].inodeInfo.TotalSize);

        std::string path = req.get_param_value("path");
        bool isDir = req.get_param_value("is_dir") == "true";
        try {
            if (isDir) {
                parse.deleteDirectory(path, mini, currentPath);
            }
            else {
                parse.deleteFile(path, mini, currentPath);
            }
            res.set_content("Deleted", "text/plain");
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error: " + std::string(e.what()), "text/plain");
        }
    });

  
    // قراءة محتوى الملف
    svr.Get("/readfile", [&parse, &mini, &password, &currentPath] (const httplib::Request& req, httplib::Response& res) {
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
            std::vector<char> data = parse.readFile(path, mini, start, length, password, currentPath);

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
    svr.Post("/writefile", [&parse, &mini, &currentPath, &password] (const httplib::Request& req, httplib::Response& res) {
        try {
            // استقبال البيانات كنموذج FormData
            auto path = req.get_file_value("path").content;
            auto content = req.get_file_value("content").content;

            std::vector<char> data(content.begin(), content.end());
            bool success = parse.writeFile(path, data, mini, false, password, currentPath);

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
//            parse.predictNextAccess(mini);

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
    svr.Get("/map", [&parse, &mini, &currentPath] (const httplib::Request&, httplib::Response& res) {
        try {
            std::stringstream buffer;
            std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

            parse.printBitmap(mini, currentPath);

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

            //tokenizer.processCommand(command, mini);

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