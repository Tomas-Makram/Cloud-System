#include "Parser.h"

// Initialization
Parser::Parser(std::string username, std::string dirname, std::string password, std::string email,size_t strongPassword,size_t totalSize) :fsAI(nullptr) {
    this->myAccount.username = username;
    this->myAccount.email = email;
    this->myAccount.dirname = dirname;
    this->myAccount.password = password;
    this->myAccount.strongPassword = strongPassword;
    this->myAccount.totalSize = totalSize;
}

Parser::~Parser() {
    if (fsAI != nullptr) {
        delete fsAI;
    }
}

void Parser::SetAccount(std::string username, std::string dirname, std::string password, std::string email, size_t strongPassword, size_t totalSize) {
    this->myAccount.username = username != "" ? username : this->myAccount.username;
    this->myAccount.email = email != "" ? email : this->myAccount.email;
    this->myAccount.dirname = dirname != "" ? dirname : this->myAccount.dirname;
    this->myAccount.password = password != "" ? password : this->myAccount.password;
    this->myAccount.strongPassword = strongPassword != 0 ? strongPassword : this->myAccount.strongPassword;
    this->myAccount.totalSize = totalSize != 0 ? totalSize : this->myAccount.totalSize;
}

Parser::account Parser::GetAccount(){
    return this->myAccount;
}

void Parser::initializeAI(MiniHSFS& mini) {
    if (fsAI == nullptr) {
        fsAI = new MiniHSFSAI(mini);
    }
}

// Account Settings
bool Parser::createAccount(MiniHSFS& mini) {
    std::lock_guard<std::recursive_mutex> lock(mini.fsMutex);

    if (!mini.mounted) throw std::runtime_error("Filesystem not mounted");

    // Make sure the name does not already exist
    if (mini.inodeTable[mini.rootNodeIndex].entries.count(this->myAccount.dirname) > 0) {
        throw std::runtime_error("User already exists");
    }

    // Allocate inode
    int userInode = mini.AllocateInode(true);
    if (userInode == -1) {
        throw std::runtime_error("No space for new user directory");
    }

    // Setting inode
    auto& inode = mini.inodeTable[userInode];
    inode.isUsed = true;
    inode.isDirectory = true;
    inode.creationTime = time(nullptr);
    inode.modificationTime = inode.creationTime;
    inode.lastAccessed = inode.creationTime;
    inode.isDirty = true;

    // Fill in account information
    inode.inodeInfo.UserName = this->myAccount.username;
    CryptoUtils crypto;
    inode.inodeInfo.Password = crypto.CreatePassword(this->myAccount.password, this->myAccount.strongPassword);
    inode.inodeInfo.Email = this->myAccount.email;
    inode.inodeInfo.TotalSize = this->myAccount.totalSize;
    inode.inodeInfo.Usage = 0;

    // Add to root folder
    mini.inodeTable[0].entries[this->myAccount.dirname] = userInode;
    mini.inodeTable[0].isDirty = true;

    // Save
    mini.SaveInodeToDisk(userInode);
    mini.SaveInodeToDisk(0);

    mini.Disk().SetConsoleColor(mini.Disk().Green);
    std::cout << "Account created for user: " << this->myAccount.dirname << std::endl;
    mini.Disk().SetConsoleColor(mini.Disk().Default);
    return true;
}

void Parser::GetInfo(MiniHSFS& mini, int index)
{
    std::cout << mini.inodeTable[index].inodeInfo.Email << '\n'
        << mini.inodeTable[index].inodeInfo.UserName << '\n'
        << mini.inodeTable[index].inodeInfo.TotalSize << '\n'
        << mini.inodeTable[index].inodeInfo.Usage << '\n';
}

int Parser::checkingAccount(MiniHSFS& mini, size_t dataSize, bool read, std::string currentPath)
{
    CryptoUtils crypto;

    if (mini.inodeTable[mini.rootNodeIndex].entries.count(this->myAccount.dirname) == 1)
    {
        int indexpath = mini.PathToInode(mini.SplitPath("/" + this->myAccount.dirname));

        std::vector<std::string> realPath = mini.SplitPath(currentPath);

        if (!((realPath.size() >= 1) && (currentPath[0] == '/' && realPath[0] == this->myAccount.dirname)))
            throw std::runtime_error("Permission denied: not the owner of the target directory");

        if (!(mini.inodeTable[indexpath].inodeInfo.UserName == this->myAccount.username && crypto.ValidatePassword(this->myAccount.password, mini.inodeTable[indexpath].inodeInfo.Password, this->myAccount.strongPassword)))
            throw std::runtime_error("Unvalid Account");

        if(!read)
            if (!(mini.inodeTable[indexpath].inodeInfo.TotalSize > mini.inodeTable[indexpath].inodeInfo.Usage + mini.inodeSize + dataSize))
                throw std::runtime_error("No Space in Account");

        return indexpath;
    }
    else
        throw std::runtime_error("Error in Your Account");

    return -1;
}

void Parser::ChangeInfo(MiniHSFS& mini, std::string email, std::string password, std::string username, std::string currentPath)
{
    if (!mini.mounted)
        throw std::runtime_error("Filesystem not mounted");
    int index = checkingAccount(mini, 0, true, currentPath);

    CryptoUtils crypto;

    mini.inodeTable[index].inodeInfo.Email = email.empty() ? mini.inodeTable[index].inodeInfo.Email : email;
    mini.inodeTable[index].inodeInfo.UserName = username.empty() ? mini.inodeTable[index].inodeInfo.UserName : username;
    mini.inodeTable[index].inodeInfo.Password = password.empty() ? mini.inodeTable[index].inodeInfo.Password : crypto.CreatePassword(password, this->myAccount.strongPassword);

    this->myAccount.username = mini.inodeTable[index].inodeInfo.UserName;
    this->myAccount.email = mini.inodeTable[index].inodeInfo.Email;
    this->myAccount.password = std::string(mini.inodeTable[index].inodeInfo.Password.begin(), mini.inodeTable[index].inodeInfo.Password.end());

    mini.SaveInodeToDisk(index);
    mini.Disk().SetConsoleColor(mini.Disk().Green);
    std::cout << "Change Setting Successfully";
    mini.Disk().SetConsoleColor(mini.Disk().Default);

}

// Basic Operations
void Parser::cd(const std::string& path, MiniHSFS& mini, std::string& currentPath) {

    if (!mini.mounted) {
        mini.Disk().SetConsoleColor(mini.Disk().Red);
        throw std::runtime_error("Filesystem not mounted");
        mini.Disk().SetConsoleColor(mini.Disk().Default);
    }

    if (path.empty()) {
        return;
    }

    initializeAI(mini);

    std::string combinedPath;
    if (path[0] == '/') {
        combinedPath = path;
    }
    else {
        combinedPath = currentPath;
        if (combinedPath.back() != '/') {
            combinedPath += "/";
        }
        combinedPath += path;
    }

    std::vector<std::string> parts = mini.SplitPath(combinedPath);
    std::vector<std::string> normalized;

    for (const std::string& part : parts) {
        if (part == "." || part.empty()) {
            continue;
        }
        else if (part == "..") {
            if (!normalized.empty()) {
                normalized.pop_back();
            }
        }
        else {
            normalized.push_back(part);
        }
    }

    std::string newPath = "/";
    for (const auto& part : normalized) {
        newPath += part + "/";
    }

    if (newPath.length() > 1 && newPath.back() == '/') {
        newPath.pop_back();
    }

    if (newPath.empty()) {
        newPath = "/";
    }

    // Check if we are already in "/" and try to go back
    if (newPath == "/" && currentPath == "/") {
        mini.Disk().SetConsoleColor(mini.Disk().Gray);
        std::cout << "You are already in the root directory" << std::endl;
        mini.Disk().SetConsoleColor(mini.Disk().Default);
        return;
    }

    try {
        mini.ValidatePath(newPath);
        std::vector<std::string> checkParts = mini.SplitPath(newPath);
        int inode = mini.PathToInode(checkParts);

        if (inode == -1 || !mini.inodeTable[inode].isDirectory) {
            mini.Disk().SetConsoleColor(mini.Disk().Red);
            throw std::runtime_error("Directory not found: " + newPath);
            mini.Disk().SetConsoleColor(mini.Disk().Default);
        }

        currentPath = newPath;
    }
    catch (const std::exception& ex) {
        throw std::runtime_error(ex.what());
    }
    // AI Enhancement: Analyze access pattern
    fsAI->analyzeAccessPattern(path.empty() ? "/" : path);
}

MiniHSFS::Inode& Parser::getDirectoryItems(const std::string& path, MiniHSFS& mini, std::string& currentPath) {
    std::lock_guard<std::recursive_mutex> lock(mini.fsMutex);

    if (!mini.mounted) {
        mini.Disk().SetConsoleColor(mini.Disk().Red);
        throw std::runtime_error("Filesystem not mounted");
        mini.Disk().SetConsoleColor(mini.Disk().Default);
    }

    checkingAccount(mini, 0, true, currentPath);

    mini.ValidatePath(path);

    // Split the path and get inode index
    std::vector<std::string> parts = mini.SplitPath(path);
    int inodeIndex = mini.PathToInode(parts);

    if (inodeIndex == -1) {
        mini.Disk().SetConsoleColor(mini.Disk().Red);
        std::string err = "Directory not found: " + path;
        mini.Disk().SetConsoleColor(mini.Disk().Default);
        throw std::runtime_error(err);
    }

    auto& dirInode = mini.inodeTable[inodeIndex];
    if (!dirInode.isDirectory) {
        mini.Disk().SetConsoleColor(mini.Disk().Red);
        std::string err = "Path is not a directory: " + path;
        mini.Disk().SetConsoleColor(mini.Disk().Default);
        throw std::runtime_error(err);
    }

    return dirInode;
}

void Parser::printFileSystemInfo(MiniHSFS& mini, std::string& currentPath){
    GetInfo(mini, checkingAccount(mini, 0, true, currentPath));
    //    mini.PrintSuperblockInfo();
}

void Parser::PrintBTreeStructure(MiniHSFS& mini, std::string& currentPath){
    checkingAccount(mini, 0, true, currentPath);
    mini.PrintBTreeStructure();
}

void Parser::ls(const std::string& input, MiniHSFS& mini, std::string& currentPath) {
    std::lock_guard<std::recursive_mutex> lock(mini.fsMutex);

    if (!mini.mounted) 
        throw std::runtime_error("Filesystem not mounted");
    initializeAI(mini);

    checkingAccount(mini, 0, true, currentPath);

    // Parse command options
    bool showInodeInfo = false;
    bool longFormat = false;
    bool showHidden = false;
    bool tree = false;
    std::string path;

    // CORRECT way to split input into tokens
    std::vector<std::string> tokens;
    std::istringstream iss(input);  // Initialize with input string
    std::string token;
    while (iss >> token) {         // Extract tokens
        tokens.push_back(token);
    }

    // Process options and path
    for (size_t i = 0; i < tokens.size(); ++i) {  // Skip command name (tokens[0] == "ls")
        const auto& token = tokens[i];

        if (tokens[0][0] == '-') {
            // It's an option flag
            if (tokens[0][1] == 'i') showInodeInfo = true;
            if (tokens[0][1] == 'l') longFormat = true;
            if (tokens[0][1] == 'a') showHidden = true;
            if (tokens[0][1] == 'R') tree = true;
        }
        else {
            // It's a path (take the first one only)
            if (path.empty()) {
                path = token;
            }
        }
    }
    std::string target;
    // Determine target path
    if (tokens.size() == 2)
        target = tokens[1];
    else
        target = path.empty() ?
        (currentPath.empty() ? "/" : currentPath) :
        path;

    // Validate and get inode
    mini.ValidatePath(target);
    int targetInode = mini.PathToInode(mini.SplitPath(target));

    if (targetInode == -1) {
        mini.Disk().SetConsoleColor(mini.Disk().Red);
        throw std::runtime_error("Path not found: " + target);
        mini.Disk().SetConsoleColor(mini.Disk().Default);
    }

    const MiniHSFS::Inode& inode = mini.inodeTable[targetInode];

    if (showInodeInfo) {
        printInodeInfo(targetInode, target, longFormat, mini);
    }
    else if (inode.isDirectory) {
        try {
            printDirectoryContents(targetInode, target, longFormat, showHidden, tree, "", false, mini);

        }
        catch (std::exception e)
        {
            std::cout << "Error >> " << e.what() << '\n';
        }

    }
    else {
        printFileInfo(targetInode, target, longFormat, mini);
    }
    // AI Enhancement: Predict next files after listing
    predictNextAccess(mini, currentPath);
}

void Parser::printDirectoryContents(int dirInode, const std::string& path, bool longFormat, bool showHidden, bool recursive, const std::string& indent, bool isLast, MiniHSFS& mini)
{

    // Check the validity of the inode number
    if (dirInode < 0 || dirInode >= static_cast<int>(mini.inodeTable.size())) {
        mini.Disk().SetConsoleColor(mini.Disk().Red);
        std::cerr << "Error: Invalid inode index " << dirInode << " for path: " << path << '\n';
        mini.Disk().SetConsoleColor(mini.Disk().Default);
        return;
    }

    const MiniHSFS::Inode& dir = mini.inodeTable[dirInode];

    // The inode must be a directory
    if (!dir.isUsed || !dir.isDirectory) {
        mini.Disk().SetConsoleColor(mini.Disk().Red);
        std::cerr << "Error: Inode " << dirInode << " is not a valid directory" << '\n';
        mini.Disk().SetConsoleColor(mini.Disk().Default);
        return;
    }

    // Print the folder title at the top level
    if (indent.empty()) {
        std::cout << "\n";
        mini.Disk().SetConsoleColor(mini.Disk().White);
        std::cout << (longFormat ? "Detailed contents of " : "Contents of ");
        mini.Disk().SetConsoleColor(mini.Disk().Red);
        std::cout << path;
        mini.Disk().SetConsoleColor(mini.Disk().Default);

        std::cout << " (inode ";
        mini.Disk().SetConsoleColor(mini.Disk().Yellow);
        std::cout << dirInode;
        mini.Disk().SetConsoleColor(mini.Disk().White);
        std::cout << "):" << '\n';

        std::cout << "Total entries: ";
        mini.Disk().SetConsoleColor(mini.Disk().Green);
        std::cout << dir.entries.size() << '\n';

        mini.Disk().SetConsoleColor(mini.Disk().Gray);
        std::cout << "----------------------------------------" << '\n';
        mini.Disk().SetConsoleColor(mini.Disk().Default);
    }

    // Count the number of visible entries
    size_t visible_entries = 0;
    for (const auto& entry : dir.entries) {
        if (!showHidden && entry.first[0] == '.') continue;
        visible_entries++;
    }

    // File size format
    const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    auto formatSize = [&](uint64_t bytes) -> std::string {
        size_t unit = 0;
        double size = static_cast<double>(bytes);
        while (size >= 1024 && unit < 4) {
            size /= 1024;
            ++unit;
        }
        std::ostringstream out;
        out << std::fixed << std::setprecision(2) << size << " " << units[unit];
        return out.str();
        };

    size_t current_entry = 0;
    for (const auto& entry : dir.entries) {
        const std::string& name = entry.first;

        // Skip hidden files if not required
        if (!showHidden && name[0] == '.') continue;
        current_entry++;
        bool last_entry = (current_entry == visible_entries);

        // Check the validity of the input inode
        if (entry.second < 0 || entry.second >= static_cast<int>(mini.inodeTable.size())) {
            mini.Disk().SetConsoleColor(mini.Disk().Red);
            std::cerr << "Warning: Skipping invalid inode reference (" << entry.second
                << ") for entry '" << name << "'" << '\n';
            mini.Disk().SetConsoleColor(mini.Disk().Default);
            continue;
        }

        const MiniHSFS::Inode& inode = mini.inodeTable[entry.second];

        // Print the structure
        std::cout << indent;
        if (recursive && !indent.empty()) {
            std::cout << (isLast ? "    " : "| ");
        }
        if (recursive) {
            std::cout << (last_entry ? "|__ " : "|-- ");
        }

        // Set colors and columns
        if (longFormat) {
            if (inode.isDirectory)
                mini.Disk().SetConsoleColor(mini.Disk().Blue);
            else
                mini.Disk().SetConsoleColor(mini.Disk().White);

            std::cout << std::left << std::setw(20) << name;
            mini.Disk().SetConsoleColor(mini.Disk().Default);

            std::cout << std::setw(10) << (inode.isDirectory ? "DIR" : "FILE");
            std::cout << std::setw(10) << (inode.isDirectory ? "-" : std::to_string(inode.size));
            std::cout << std::setw(10) << entry.second;

            char time_buf[26];
#if _WIN32
            ctime_s(time_buf, sizeof(time_buf), &inode.modificationTime);
#else
            ctime_r(&inode.modificationTime, time_buf);
#endif
            time_buf[24] = '\0';
            mini.Disk().SetConsoleColor(mini.Disk().Yellow);
            std::cout << time_buf << '\n';
            mini.Disk().SetConsoleColor(mini.Disk().Default);
        }
        else {
            if (inode.isDirectory)
                mini.Disk().SetConsoleColor(mini.Disk().Blue);
            else
                mini.Disk().SetConsoleColor(mini.Disk().White);

            std::cout << name;
            mini.Disk().SetConsoleColor(mini.Disk().Default);

            if (inode.isDirectory) {
                std::cout << " <DIR>";
            }
            else {
                std::cout << " (" << formatSize(inode.size) << ")";
            }
            std::cout << '\n';
        }

        // Display folders in a recursive manner
        if (recursive && inode.isDirectory && name != "." && name != "..") {
            std::string new_indent = indent + (isLast ? "    " : "|   ");
            std::string child_path = (path == "/" ? "/" : path + "/") + name;
            printDirectoryContents(entry.second, child_path, longFormat,
                showHidden, recursive, new_indent, last_entry, mini);
        }
    }

    // Footer for root folder only
    if (indent.empty()) {
        mini.Disk().SetConsoleColor(mini.Disk().Gray);
        std::cout << "----------------------------------------" << '\n';
        mini.Disk().SetConsoleColor(mini.Disk().Default);

        mini.Disk().SetConsoleColor(mini.Disk().Green);
        std::cout << "Free space : "
            << formatSize(mini.Disk().freeBlocksCount() * mini.Disk().blockSize)
            << " | Inode: " << dirInode << '\n';
        mini.Disk().SetConsoleColor(mini.Disk().Default);
    }
}

void Parser::printFileInfo(int fileInode, const std::string& path, bool longFormat, MiniHSFS& mini) {

    // Validate inode number
    if (fileInode < 0 || fileInode >= mini.inodeTable.size()) {
        mini.Disk().SetConsoleColor(mini.Disk().Red);
        std::cerr << "Error: Invalid inode number " << fileInode;
        mini.Disk().SetConsoleColor(mini.Disk().Default);
        return;
    }

    const MiniHSFS::Inode& file = mini.inodeTable[fileInode];

    // Header with better visual separation
    mini.Disk().SetConsoleColor(mini.Disk().Magenta);
    std::cout << "File Information\n";
    mini.Disk().SetConsoleColor(mini.Disk().Green);
    std::cout << "-----------------------------------------\n";
    mini.Disk().SetConsoleColor(mini.Disk().Default);


    // Format size in human-readable way
    auto formatSize = [](size_t bytes) -> std::string {
        const char* units[] = { "B", "KB", "MB", "GB", "TB" };
        size_t unit = 0;
        double size = static_cast<double>(bytes);

        while (size >= 1024 && unit < 4) {
            size /= 1024;
            unit++;
        }

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << size << " " << units[unit];
        return oss.str();
        };

    // Main information with consistent formatting
    std::cout << std::left << std::setw(15) << "Name:";
    mini.Disk().SetConsoleColor(mini.Disk().Blue);
    std::cout << path << "\n";
    mini.Disk().SetConsoleColor(mini.Disk().Default);
    std::cout << std::setw(15) << "Inode:" << fileInode << "\n";
    std::cout << std::setw(15) << "Size:"
        << formatSize(file.size) << " (" << file.size << " bytes)\n";
    std::cout << std::setw(15) << "Blocks used:" << file.blocksUsed << "\n";
    std::cout << std::setw(15) << "First block:" << file.firstBlock << "\n";

    // Time formatting with error handling
    auto printTime = [](const char* label, time_t time) {
        std::cout << std::setw(15) << label;
        if (time == 0) {
            std::cout << "\033[33m" << "Unknown" << "\033[0m" << "\n";
        }
        else {
            char time_buf[26];
#if _WIN32
            if (ctime_s(time_buf, sizeof(time_buf), &time) == 0) {
                time_buf[24] = '\0'; // Remove newline
                
                std::cout << "\033[33m" << time_buf << "\033[0m";
            }
            else {
                std::cout << "\033[31m" << "Invalid timestamp" << "\033[0m";
            }
#else
            if (ctime_r(&time, time_buf) != nullptr) {
                time_buf[24] = '\0'; // Remove newline
                std::cout << "\033[33m" << time_buf << "\033[0m";
            }
            else {
                std::cout << "\033[31m" << "Invalid timestamp" << "\033[0m";
            }
#endif
            std::cout << "\n";
        }
        };

    printTime("Created:", file.creationTime);
    printTime("Modified:", file.modificationTime);

    // Footer
    mini.Disk().SetConsoleColor(mini.Disk().Green);
    std::cout << "-----------------------------------------\n";
    mini.Disk().SetConsoleColor(mini.Disk().Default);

}

void Parser::printInodeInfo(int inodeNum, const std::string& path, bool longFormat, MiniHSFS& mini) {
    
    // Validate inode number
    if (inodeNum < 0 || inodeNum >= mini.inodeTable.size()) {
        mini.Disk().SetConsoleColor(mini.Disk().Red);
        std::cerr << "\033[31m" << "Error: Invalid inode number " << inodeNum;
        mini.Disk().SetConsoleColor(mini.Disk().Default);
        return;
    }

    const MiniHSFS::Inode& inode = mini.inodeTable[inodeNum];

    // Header with better visual separation
    mini.Disk().SetConsoleColor(mini.Disk().Yellow);
    std::cout << "Inode Information\n";
    mini.Disk().SetConsoleColor(mini.Disk().Green);
    std::cout << "-----------------------------------------\n" << "\033[0m";
    mini.Disk().SetConsoleColor(mini.Disk().Default);

    // Main information with consistent formatting
    std::cout << std::left << std::setw(15) << "Path:";
    mini.Disk().SetConsoleColor(mini.Disk().Blue);
    std::cout << path<<'\n';
    mini.Disk().SetConsoleColor(mini.Disk().Default);
    std::cout << std::setw(15) << "Inode:" << inodeNum << "\n";

    // Type information with better visual distinction
    std::cout << std::setw(15) << "Type:";
    if (inode.isDirectory) {
        mini.Disk().SetConsoleColor(mini.Disk().Blue);
        std::cout << "Directory";
        mini.Disk().SetConsoleColor(mini.Disk().Default);
        std::cout << " (" << inode.entries.size() << " entries)\n";
    }
    else {
        mini.Disk().SetConsoleColor(mini.Disk().Yellow);
        std::cout << "File\n";
        mini.Disk().SetConsoleColor(mini.Disk().Default);


        // File-specific information with human-readable size
        auto formatSize = [](size_t bytes) {
            const char* units[] = { "B", "KB", "MB", "GB", "TB" };
            size_t unit = 0;
            double size = static_cast<double>(bytes);
            while (size >= 1024 && unit < 4) {
                size /= 1024;
                unit++;
            }
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2) << size << " " << units[unit];
            return oss.str();
            };

        std::cout << std::setw(15) << "Size:"
            << formatSize(inode.size) << " (" << inode.size << " bytes)\n";
        std::cout << std::setw(15) << "Blocks used:" << inode.blocksUsed << "\n";
        std::cout << std::setw(15) << "First block:" << inode.firstBlock << "\n";
    }

    // Time formatting with error handling
    auto printTime = [](const char* label, time_t time) {
        std::cout << std::setw(15) << label;
        if (time == 0) {
            std::cout << "\033[33m" << "Unknown" << "\033[0m" << "\n";
        }
        else {
            char time_buf[26];
#if _WIN32
            if (ctime_s(time_buf, sizeof(time_buf), &time) == 0) {
                time_buf[24] = '\0';
                std::cout << "\033[33m" << time_buf << "\033[0m";
            }
            else {
                std::cout << "\033[31m" << "Invalid timestamp" << "\033[0m";
            }
#else
            if (ctime_r(&time, time_buf) != nullptr) {
                time_buf[24] = '\0';
                std::cout << "\033[33m" << time_buf << "\033[0m";
            }
            else {
                std::cout << "\033[31m" << "Invalid timestamp" << "\033[0m";
            }
#endif
            std::cout << "\n";
        }
        };

    printTime("Created:", inode.creationTime);
    printTime("Modified:", inode.modificationTime);

    // Enhanced directory listing for long format
    if (longFormat && inode.isDirectory && !inode.entries.empty()) {
        mini.Disk().SetConsoleColor(mini.Disk().Yellow);
        std::cout << "\nDirectory Contents:\n";
        mini.Disk().SetConsoleColor(mini.Disk().Green);
        std::cout << "-----------------------------------------\n";
        mini.Disk().SetConsoleColor(mini.Disk().Default);

        for (const auto& entry : inode.entries) {
            const auto& child_inode = mini.inodeTable[entry.second];
            std::cout << "  ";
            (child_inode.isDirectory ? mini.Disk().SetConsoleColor(mini.Disk().Yellow) : mini.Disk().SetConsoleColor(mini.Disk().Blue));
            std::cout << std::left << std::setw(30) << entry.first;
            mini.Disk().SetConsoleColor(mini.Disk().Default);
            std::cout << " (inode: " << entry.second << ")";

            if (!child_inode.isDirectory) {
                std::cout << " - " << child_inode.size << " bytes";
            }
            std::cout << "\n";
        }
    }

    // Footer
    
    mini.Disk().SetConsoleColor(mini.Disk().Green);
    std::cout << "-----------------------------------------\n";
    mini.Disk().SetConsoleColor(mini.Disk().Default);

}

bool Parser::createDirectory(const std::string path, const std::string name, MiniHSFS& mini, std::string& currentPath) {
    std::lock_guard<std::recursive_mutex> lock(mini.fsMutex);

    if (!mini.mounted) {
        throw std::runtime_error("Filesystem not mounted");
    }

    //Verify account validity
    int ownerInode = checkingAccount(mini, 0, false, currentPath);

    // Build the full path
    std::string fullPath = path;
    if (fullPath != "/" && !fullPath.empty() && fullPath.back() != '/') {
        fullPath += "/";
    }
    fullPath += name;

    // Check the path
    mini.ValidatePath(fullPath);

    // Split the path only once
    std::vector<std::string> pathComponents = mini.SplitPath(fullPath);
    if (pathComponents.empty()) {
        throw std::invalid_argument("Invalid path components");
    }

    std::string dirname = pathComponents.back();

    if (!mini.ValidateEntry(dirname))
        throw std::invalid_argument("Invalid name directory");

    // Get parent path
    pathComponents.pop_back();
    int parentInode = pathComponents.empty() ? 0 : mini.PathToInode(pathComponents);

    if (parentInode == -1) {
        throw std::runtime_error("Parent directory does not exist");
    }
    if (parentInode < 0 || static_cast<size_t>(parentInode) >= mini.inodeTable.size()) {
        throw std::runtime_error("Invalid parent directory inode: " + std::to_string(parentInode));
    }
    if (!mini.inodeTable[parentInode].isDirectory) {
        throw std::runtime_error("Parent is not a directory");
    }

    // Check for duplication
    if (mini.inodeTable[parentInode].entries.count(dirname) > 0) {
        throw std::runtime_error("Directory already exists: " + dirname);
    }

    // Allocate a new inode
    int newInode = mini.AllocateInode(true);
    if (newInode == -1) {
        throw std::runtime_error("No space for new directory");
    }

    // Initialize the new volume's inode
    MiniHSFS::Inode& newDir = mini.inodeTable[newInode];
    newDir.isUsed = true;
    newDir.isDirectory = true;
    newDir.creationTime = time(nullptr);
    newDir.modificationTime = newDir.creationTime;
    newDir.lastAccessed = newDir.creationTime;
    newDir.isDirty = true;
    newDir.inodeInfo.UserName = this->myAccount.username;

    // Add to parent folder
    mini.inodeTable[parentInode].entries[dirname] = newInode;
    mini.inodeTable[parentInode].modificationTime = time(nullptr);
    mini.inodeTable[parentInode].isDirty = true;

    // Update account space
    mini.inodeTable[ownerInode].inodeInfo.Usage += mini.inodeSize;
    mini.inodeTable[ownerInode].isDirty = true;

    try {
        // Save changes
        mini.SaveInodeToDisk(newInode);
        mini.SaveInodeToDisk(parentInode);
        mini.SaveInodeToDisk(ownerInode);

        mini.lastTimeWrite = time(nullptr);

        mini.Disk().SetConsoleColor(mini.Disk().Green);
        std::cout << "Directory '" << dirname << "' created successfully " << "\n";
        mini.Disk().SetConsoleColor(mini.Disk().Default);

        return true;
    }
    catch (const std::exception& e) {
        // Undo changes in case of failure
        mini.inodeTable[parentInode].entries.erase(dirname);
        mini.inodeTable[parentInode].isDirty = true;
        mini.inodeTable[ownerInode].inodeInfo.Usage -= mini.inodeSize;
        mini.inodeTable[newInode] = MiniHSFS::Inode();

        throw std::runtime_error("Failed to create directory: " + std::string(e.what()));
    }
}

int Parser::createFile(const std::string& path, const std::string name, MiniHSFS& mini, std::string& currentPath) {
    std::lock_guard<std::recursive_mutex> lock(mini.fsMutex);

    if (!mini.mounted) {
        throw std::runtime_error("Filesystem not mounted");
    }

    // Account Verification
    int ownerInode = checkingAccount(mini, 0, false, currentPath);

    // Build the full path
    std::string fullPath = path;
    if (fullPath != "/" && !fullPath.empty() && fullPath.back() != '/') {
        fullPath += "/";
    }
    fullPath += name;

    // Check the path
    mini.ValidatePath(fullPath);

    // Split path
    std::vector<std::string> pathComponents = mini.SplitPath(fullPath);
    if (pathComponents.empty()) {
        throw std::invalid_argument("Invalid path components");
    }

    std::string filename = pathComponents.back();

    if (!mini.ValidateEntry(filename))
        throw std::invalid_argument("Invalid name file");

    // Get parent path
    pathComponents.pop_back();
    int parentInode = pathComponents.empty() ? 0 : mini.PathToInode(pathComponents);

    if (parentInode == -1) {
        throw std::runtime_error("Parent directory does not exist");
    }
    if (parentInode < 0 || static_cast<size_t>(parentInode) >= mini.inodeTable.size()) {
        throw std::runtime_error("Invalid parent directory inode: " + std::to_string(parentInode));
    }
    if (!mini.inodeTable[parentInode].isDirectory) {
        throw std::runtime_error("Parent is not a directory");
    }

    // Check for duplication
    if (mini.inodeTable[parentInode].entries.count(filename) > 0) {
        throw std::runtime_error("File already exists: " + filename);
    }

    // Allocate new inode
    int newInode = mini.AllocateInode(false);
    if (newInode == -1) {
        throw std::runtime_error("No space for new file");
    }

    // Initialize the new file's inode
    MiniHSFS::Inode& newFile = mini.inodeTable[newInode];
    newFile.isUsed = true;
    newFile.isDirectory = false;
    newFile.blocksUsed = 0;
    newFile.firstBlock = -1;
    newFile.size = 0;
    newFile.creationTime = time(nullptr);
    newFile.modificationTime = newFile.creationTime;
    newFile.lastAccessed = newFile.creationTime;
    newFile.isDirty = true;
    newFile.inodeInfo.UserName = mini.inodeTable[ownerInode].inodeInfo.UserName;

    // Add to parent folder
    mini.inodeTable[parentInode].entries[filename] = newInode;
    mini.inodeTable[parentInode].modificationTime = time(nullptr);
    mini.inodeTable[parentInode].isDirty = true;

    // Update account space
    mini.inodeTable[ownerInode].inodeInfo.Usage += mini.inodeSize;
    mini.inodeTable[ownerInode].isDirty = true;

    try {
        // Save changes
        mini.SaveInodeToDisk(newInode);
        mini.SaveInodeToDisk(parentInode);
        mini.SaveInodeToDisk(ownerInode);

        mini.lastTimeWrite = time(nullptr);

        mini.Disk().SetConsoleColor(mini.Disk().Green);
        std::cout << "File '" << filename << "' created successfully" << '\n';
        mini.Disk().SetConsoleColor(mini.Disk().Default);

        return newInode;
    }
    catch (const std::exception& e) {
        // Undo changes in case of failure
        mini.inodeTable[parentInode].entries.erase(filename);
        mini.inodeTable[parentInode].isDirty = true;
        mini.inodeTable[ownerInode].inodeInfo.Usage -= mini.inodeSize;
        mini.inodeTable[newInode] = MiniHSFS::Inode();

        throw std::runtime_error("Failed to create file: " + std::string(e.what()));
    }
}

bool Parser::deleteDirectory(const std::string& path, MiniHSFS& mini,std::string& currentPath) {
    std::lock_guard<std::recursive_mutex> lock(mini.fsMutex);

    if (!mini.mounted) {
        throw std::runtime_error("Filesystem not mounted");
    }

    // Verify account
    int ownerInode = checkingAccount(mini, 0, true, currentPath);

    mini.ValidatePath(path);

    if (path == "/" || path.empty()) {
        throw std::runtime_error("Cannot delete root directory");
    }

    int targetInode = mini.FindFile(path);
    if (targetInode == -1) {
        throw std::runtime_error("Directory not found: " + path);
    }

    if (targetInode < 0 || static_cast<size_t>(targetInode) >= mini.inodeTable.size()) {
        throw std::runtime_error("Invalid inode index: " + std::to_string(targetInode));
    }

    if (!mini.inodeTable[targetInode].isDirectory) {
        throw std::runtime_error("Path is not a directory: " + path);
    }


    // Ask for confirmation if the folder is not empty
    if (!mini.inodeTable[targetInode].entries.empty()) {

        mini.Disk().SetConsoleColor(mini.Disk().Red);
        std::cout << "\033[1;31mDirectory is not empty. Contains "
            << mini.inodeTable[targetInode].entries.size()
            << " items. Delete all contents? [Y/N]: ";
        mini.Disk().SetConsoleColor(mini.Disk().Default);

        std::string answer;
        std::getline(std::cin, answer);
        if (answer != "Y" && answer != "y") {
            mini.Disk().SetConsoleColor(mini.Disk().Magenta);
            std::cout << "Operation cancelled.\n";
            mini.Disk().SetConsoleColor(mini.Disk().Default);
            return false;
        }

        // Delete contents recursively
        auto entries = mini.inodeTable[targetInode].entries;
        for (const auto& entry : entries) {
            std::string childPath = path + (path == "/" ? "" : "/") + entry.first;
            if (entry.second < static_cast<int>(mini.inodeTable.size())) {
                if (mini.inodeTable[entry.second].isDirectory) {
                    deleteDirectory(childPath, mini,currentPath);
                }
                else {
                    deleteFile(childPath, mini, currentPath);
                }
            }
        }
    }

    // Remove from parent folder
    std::vector<std::string> pathComponents = mini.SplitPath(path);
    std::string dirname = pathComponents.back();
    pathComponents.pop_back();

    int parentInode = pathComponents.empty() ? 0 : mini.PathToInode(pathComponents);
    if (parentInode == -1) {
        throw std::runtime_error("Parent directory not found");
    }

    if (parentInode >= 0 && static_cast<size_t>(parentInode) < mini.inodeTable.size()) {
        mini.inodeTable[parentInode].entries.erase(dirname);
        mini.inodeTable[parentInode].modificationTime = time(nullptr);
        mini.inodeTable[parentInode].isDirty = true;
    }

    // Update account space
    if (mini.inodeTable[ownerInode].inodeInfo.Usage >= mini.inodeSize) {
        mini.inodeTable[ownerInode].inodeInfo.Usage -= mini.inodeSize;
    }
    else {
        mini.inodeTable[ownerInode].inodeInfo.Usage = 0;
    }
    mini.inodeTable[ownerInode].isDirty = true;

    try {
        // Save changes first
        if (parentInode != 0) {
            mini.SaveInodeToDisk(parentInode);
        }
        mini.SaveInodeToDisk(ownerInode);

        // Then edit the inode
        mini.FreeInode(targetInode);

        mini.Disk().SetConsoleColor(mini.Disk().Green);
        std::cout << "Directory '" << dirname << "' deleted successfully.\n";
        mini.Disk().SetConsoleColor(mini.Disk().Default);

        mini.lastTimeWrite = time(nullptr);
        return true;

    }
    catch (const std::exception& e) {
        // Rollback in case of failure
        if (parentInode >= 0 && static_cast<size_t>(parentInode) < mini.inodeTable.size()) {
            mini.inodeTable[parentInode].entries[dirname] = targetInode;
        }
        mini.inodeTable[ownerInode].inodeInfo.Usage += mini.inodeSize;
        throw std::runtime_error("Failed to delete directory: " + std::string(e.what()));
    }
}

bool Parser::deleteFile(const std::string& path, MiniHSFS& mini, std::string& currentPath) {
    std::lock_guard<std::recursive_mutex> lock(mini.fsMutex);

    if (!mini.mounted) {
        throw std::runtime_error("Filesystem not mounted");
    }

    // Verify account
    int ownerInode = checkingAccount(mini, 0, true, currentPath);

    mini.ValidatePath(path);

    int targetInode = mini.FindFile(path);
    if (targetInode == -1) {
        throw std::runtime_error("File not found: " + path);
    }

    if (targetInode < 0 || static_cast<size_t>(targetInode) >= mini.inodeTable.size()) {
        throw std::runtime_error("Invalid inode index: " + std::to_string(targetInode));
    }

    if (mini.inodeTable[targetInode].isDirectory) {
        throw std::runtime_error("Cannot delete directory using file delete: " + path);
    }

    // Save file information before deleting
    int firstBlock = mini.inodeTable[targetInode].firstBlock;
    int blocksUsed = mini.inodeTable[targetInode].blocksUsed;
    size_t fileSize = mini.inodeTable[targetInode].size;

    // Remove from parent folder
    std::vector<std::string> pathComponents = mini.SplitPath(path);
    std::string filename = pathComponents.back();
    pathComponents.pop_back();

    int parentInode = pathComponents.empty() ? 0 : mini.PathToInode(pathComponents);
    if (parentInode == -1) {
        throw std::runtime_error("Parent directory not found");
    }

    if (parentInode < 0 || static_cast<size_t>(parentInode) >= mini.inodeTable.size()) {
        throw std::runtime_error("Invalid parent directory inode: " + std::to_string(parentInode));
    }

    mini.inodeTable[parentInode].entries.erase(filename);
    mini.inodeTable[parentInode].modificationTime = time(nullptr);
    mini.inodeTable[parentInode].isDirty = true;

    // Calculate the freed space
    size_t blockSize = static_cast<size_t>(mini.Disk().blockSize);
    size_t spaceFreed = (blocksUsed > 0) ? blocksUsed * blockSize : blockSize;
    spaceFreed += mini.inodeSize; // Inode space

    // Update account space
    if (mini.inodeTable[ownerInode].inodeInfo.Usage >= spaceFreed) {
        mini.inodeTable[ownerInode].inodeInfo.Usage -= spaceFreed;
    }
    else {
        mini.inodeTable[ownerInode].inodeInfo.Usage = 0;
    }
    mini.inodeTable[ownerInode].isDirty = true;

    try {
        // Save changes first
        mini.SaveInodeToDisk(parentInode);
        mini.SaveInodeToDisk(ownerInode);

        // Then edit the file blocks
        if (firstBlock != -1 && blocksUsed > 0) {
            for (int i = 0; i < blocksUsed; ++i) {
                mini.BTreeDelete(mini.rootNodeIndex, firstBlock + i);
            }
        }

        // Then edit the inode
        mini.FreeInode(targetInode);

        mini.Disk().SetConsoleColor(mini.Disk().Green);
        std::cout << "File '" << filename << "' deleted successfully.\n";
        mini.Disk().SetConsoleColor(mini.Disk().Default);
        mini.lastTimeWrite = time(nullptr);
        return true;

    }
    catch (const std::exception& e) {
        // Rollback in case of failure
        mini.inodeTable[parentInode].entries[filename] = targetInode;
        mini.inodeTable[ownerInode].inodeInfo.Usage += spaceFreed;
        throw std::runtime_error("Failed to delete file: " + std::string(e.what()));
    }
}

std::vector<char> Parser::readFile(const std::string& path, MiniHSFS& mini, size_t maxChunkSize, bool showProgress, const std::string& password, std::string currentPath) {

    std::lock_guard<std::recursive_mutex> lock(mini.fsMutex);

    if (!mini.mounted)
        throw std::runtime_error("Filesystem not mounted");

    int indexpath = checkingAccount(mini, 0, true, currentPath);

    mini.ValidatePath(path);

    int inode_index = mini.FindFile(path);
    if (inode_index == -1) 
        throw std::runtime_error("File not found");

    MiniHSFS::Inode& inode = mini.inodeTable[inode_index];
    if (inode.isDirectory) 
        throw std::runtime_error("Cannot read a directory");

    if (inode.blocksUsed == 0 || inode.firstBlock == -1) {
        return {}; // Empty file
    }

    VirtualDisk::Extent extent(inode.firstBlock, inode.blocksUsed);
    std::vector<char> result = mini.Disk().readData(extent, password);

    if (maxChunkSize > 0 && result.size() > maxChunkSize) {
        result.resize(maxChunkSize);
    }

    return result;
}

bool Parser::writeFile(const std::string& path, const std::vector<char>& data, MiniHSFS& mini, bool append, const std::string& password, std::string currentPath) {

    std::lock_guard<std::recursive_mutex> lock(mini.fsMutex);

    if (!mini.mounted) {
        throw std::runtime_error("Filesystem not mounted");
    }

    CryptoUtils crypto;

    // Check account and available space
    size_t dataSize = data.size();
    size_t encryptedOverhead = password.empty() ? 0 : crypto.ExtraSize();
    size_t totalSizeNeeded = dataSize + encryptedOverhead;

    int ownerInode = checkingAccount(mini, totalSizeNeeded, false, currentPath);

    mini.ValidatePath(path);

    int targetInode = mini.FindFile(path);
    if (targetInode == -1) {
        throw std::runtime_error("File not found: " + path);
    }

    MiniHSFS::Inode& inode = mini.inodeTable[targetInode];
    if (inode.isDirectory) {
        throw std::runtime_error("Cannot write to a directory: " + path);
    }

    const size_t blockSize = static_cast<size_t>(mini.Disk().blockSize);

    // Calculate the number of blocks required
    size_t blocksNeeded = (totalSizeNeeded + blockSize - 1) / blockSize;

    if (blocksNeeded > static_cast<size_t>(mini.Disk().freeBlocksCount())) {
        throw std::runtime_error("Not enough space to write this file. Needed: " +
            std::to_string(blocksNeeded) + " blocks, Available: " +
            std::to_string(mini.Disk().freeBlocksCount()));
    }

    // Save old information for rollback in case of failure
    int oldFirstBlock = inode.firstBlock;
    int oldBlocksUsed = inode.blocksUsed;
    size_t oldSize = inode.size;
    size_t oldUsage = mini.inodeTable[ownerInode].inodeInfo.Usage;

    // Free up old space if append is not present
    if (!append && oldFirstBlock != -1) {
        mini.FreeFileBlocks(inode);
    }

    // Allocate new blocks
    VirtualDisk::Extent newExtent = mini.AllocateContiguousBlocks(static_cast<int>(blocksNeeded));
    if (newExtent.startBlock == -1) {
        // Undo: Restore the old space if it exists
        if (!append && oldFirstBlock != -1) {
            inode.firstBlock = oldFirstBlock;
            inode.blocksUsed = oldBlocksUsed;
            inode.size = oldSize;
        }
        throw std::runtime_error("Failed to allocate blocks for file");
    }

    // Writing data
    if (!mini.Disk().writeData(data, newExtent, password, true)) {
        // Undo: Edit new blocks and redo old ones
        mini.Disk().freeBlocks(newExtent);
        if (!append && oldFirstBlock != -1) {
            inode.firstBlock = oldFirstBlock;
            inode.blocksUsed = oldBlocksUsed;
            inode.size = oldSize;
        }
        throw std::runtime_error("Failed to write data to disk");
    }

    // Update file information
    if (append) {
        
        // Append support  we will assume that append only works for data that is currently unencrypted
        if (!password.empty()) {
            mini.Disk().freeBlocks(newExtent);
            throw std::runtime_error("Appending to encrypted files is not supported");
        }

        if (inode.firstBlock == -1) {
            inode.firstBlock = newExtent.startBlock;
            inode.blocksUsed = newExtent.blockCount;
            inode.size = dataSize;
        }
        else {
            
            // In a real-world situation, the old and new blocks must be merged
            mini.FreeFileBlocks(inode);
            inode.firstBlock = newExtent.startBlock;
            inode.blocksUsed = newExtent.blockCount;
            inode.size = dataSize;
        }
    }
    else {
        inode.firstBlock = newExtent.startBlock;
        inode.blocksUsed = newExtent.blockCount;
        inode.size = dataSize;
    }

    // Update account space
    size_t spaceUsed = blocksNeeded * blockSize;
    if (append) {

        // Subtract the old space and add the new one
        size_t oldSpace = oldBlocksUsed * blockSize;
        mini.inodeTable[ownerInode].inodeInfo.Usage = oldUsage - oldSpace + spaceUsed;
    }
    else {
        // Add new space only (old one was previously edited)
        mini.inodeTable[ownerInode].inodeInfo.Usage = oldUsage + spaceUsed;
    }

    mini.inodeTable[ownerInode].isDirty = true;

    inode.modificationTime = time(nullptr);
    inode.isDirty = true;

    try {
        // Save changes
        mini.SaveInodeToDisk(targetInode);
        mini.SaveInodeToDisk(ownerInode);

        mini.lastTimeWrite = time(nullptr);
        return true;

    }
    catch (const std::exception& e) {
        // Undo all changes if save fails
        mini.Disk().freeBlocks(newExtent);
        if (oldFirstBlock != -1) {
            inode.firstBlock = oldFirstBlock;
            inode.blocksUsed = oldBlocksUsed;
            inode.size = oldSize;
        }
        mini.inodeTable[ownerInode].inodeInfo.Usage = oldUsage;
        throw std::runtime_error("Failed to save file changes: " + std::string(e.what()));
    }
}

bool Parser::rename(const std::string& oldPath, const std::string& newName, MiniHSFS& mini, std::string& currentPath) {
    std::lock_guard<std::recursive_mutex> lock(mini.fsMutex);

    if (!mini.mounted)
    {
        mini.Disk().SetConsoleColor(mini.Disk().Red);
        throw std::runtime_error("Filesystem not mounted");
        mini.Disk().SetConsoleColor(mini.Disk().Default);
    }

    checkingAccount(mini, 0, true, currentPath);

    mini.ValidatePath(oldPath);

    auto pathComponents = mini.SplitPath(oldPath);
    if (pathComponents.empty())
    {
        mini.Disk().SetConsoleColor(mini.Disk().Red);
        throw std::invalid_argument("Invalid path");
        mini.Disk().SetConsoleColor(mini.Disk().Default);
    }

    std::string oldEntryName = pathComponents.back();
    pathComponents.pop_back();

    int parentInode = mini.PathToInode(pathComponents);
    if (parentInode == -1 || !mini.inodeTable[parentInode].isDirectory)
    {
        mini.Disk().SetConsoleColor(mini.Disk().Red);
        throw std::runtime_error("Parent directory not found");
        mini.Disk().SetConsoleColor(mini.Disk().Default);
    }

    auto& entries = mini.inodeTable[parentInode].entries;

    // Check if the old name exists
    if (entries.count(oldEntryName) == 0)
    {
        mini.Disk().SetConsoleColor(mini.Disk().Red);
        throw std::runtime_error("This name " + oldEntryName + " not found");
        mini.Disk().SetConsoleColor(mini.Disk().Default);
    }

    int targetInode = entries[oldEntryName];

    // Check that the new name does not already exist
    if (entries.count(newName) > 0)
        throw std::runtime_error("An entry with the new name " + newName + " already exists");

    const std::string invalidChars = R"(\/:*?"<>|)";

    if (std::any_of(newName.begin(), newName.end(), [&](char ch) {
        return invalidChars.find(ch) != std::string::npos;
        })) {
        mini.Disk().SetConsoleColor(mini.Disk().Red);
        throw std::invalid_argument("The file name contains illegal characters: " + newName);
        mini.Disk().SetConsoleColor(mini.Disk().Default);
    }

    // Rename
    entries.erase(oldEntryName);
    entries[newName] = targetInode;

    mini.UpdateInodeTimestamps(parentInode, true);
    return true;
}

bool Parser::move(const std::string& srcPath, const std::string& destPath, MiniHSFS& mini, std::string& currentPath) {
    std::lock_guard<std::recursive_mutex> lock(mini.fsMutex);

    if (!mini.mounted)
    {
        mini.Disk().SetConsoleColor(mini.Disk().Red);
        throw std::runtime_error("Filesystem not mounted");
        mini.Disk().SetConsoleColor(mini.Disk().Default);
    }

    mini.ValidatePath(srcPath);
    mini.ValidatePath(destPath);

    int srcInode = mini.FindFile(srcPath);
    if (srcInode == -1)
    {
        mini.Disk().SetConsoleColor(mini.Disk().Red);
        throw std::runtime_error("Source not found");
        mini.Disk().SetConsoleColor(mini.Disk().Default);
    }


    checkingAccount(mini, mini.inodeTable[srcInode].size, false, currentPath);


    int destInode = mini.FindFile(destPath);
    if (destInode == -1 || !mini.inodeTable[destInode].isDirectory)
    {
        mini.Disk().SetConsoleColor(mini.Disk().Red);
        throw std::runtime_error("Destination must be a valid directory");
        mini.Disk().SetConsoleColor(mini.Disk().Default);
    }
    const std::string name = mini.SplitPath(srcPath).back();

    if (mini.inodeTable[srcInode].isDirectory) {
        // Create a new folder in the destination with the same name
        std::string newFolderPath = destPath + "/" + name;
        createDirectory(destPath + "/", name, mini, currentPath);

        for (const auto& entry : mini.inodeTable[srcInode].entries) {
            const std::string childSrcPath = srcPath + "/" + entry.first;
            move(childSrcPath, newFolderPath, mini, currentPath); // Move elements internally recursive
        }

        deleteDirectory(srcPath, mini, currentPath);
    }
    else {
        // Just move the file

        // Remove from parent directory
        auto srcParts = mini.SplitPath(srcPath);
        std::string entryName = srcParts.back();
        srcParts.pop_back();
        int parentInode = mini.PathToInode(srcParts);

        if (parentInode != -1) {
            mini.inodeTable[parentInode].entries.erase(entryName);
            mini.UpdateInodeTimestamps(parentInode, true);
        }

        // Add to destination directory
        mini.inodeTable[destInode].entries[entryName] = srcInode;
        mini.UpdateInodeTimestamps(destInode, true);
    }
    return true;
}

bool Parser::copy(const std::string& srcPath, const std::string& destPath, MiniHSFS& mini,std::string& currentPath) {
    std::lock_guard<std::recursive_mutex> lock(mini.fsMutex);

    if (!mini.mounted)
    {
        mini.Disk().SetConsoleColor(mini.Disk().Red);
        throw std::runtime_error("Filesystem not mounted");
        mini.Disk().SetConsoleColor(mini.Disk().Default);
    }

    mini.ValidatePath(srcPath);
    mini.ValidatePath(destPath);

    int srcInode = mini.FindFile(srcPath);
    if (srcInode == -1)
    {
        mini.Disk().SetConsoleColor(mini.Disk().Red);
        throw std::runtime_error("Source not found");
        mini.Disk().SetConsoleColor(mini.Disk().Default);
    }

    int destInode = mini.FindFile(destPath);
    if (destInode == -1 || !mini.inodeTable[destInode].isDirectory)
    {
        mini.Disk().SetConsoleColor(mini.Disk().Red);
        throw std::runtime_error("Destination must be a directory");
        mini.Disk().SetConsoleColor(mini.Disk().Default);
    }
        

    std::string name = mini.SplitPath(srcPath).back();

    if (mini.inodeTable[srcInode].isDirectory) {
        // Create a folder at the destination
        std::string newFolderPath = destPath + "/" + name;
        createDirectory(destPath + "/", name, mini, currentPath);

        for (const auto& entry : mini.inodeTable[srcInode].entries) {
            std::string childSrcPath = srcPath + "/" + entry.first;
            copy(childSrcPath, newFolderPath, mini, currentPath);
        }
    }
    else {
        // Read the content
        std::vector<char> content = readFile(srcPath, mini);
        size_t fileSize = content.size();
        int blockSize = mini.Disk().blockSize;
        int neededBlocks = static_cast<int>(std::ceil(static_cast<double>(fileSize) / blockSize));

        // Check available space
        if (neededBlocks > static_cast<int>(mini.Disk().freeBlocksCount())) {
            mini.Disk().SetConsoleColor(mini.Disk().Red);
            throw std::runtime_error("Not enough disk space to copy file: " + srcPath);
            mini.Disk().SetConsoleColor(mini.Disk().Default);
        }

        // Actual copy
        createFile(destPath + "/", name, mini, currentPath);
        writeFile(destPath + "/" + name, content, mini);
    }

    return true;
}

void Parser::cls() {
#ifdef _WIN32
    system("cls");   // Windows
#else
    system("clear"); // Linux/macOS
#endif
}

void Parser::printBitmap(MiniHSFS& mini, std::string& currentPath) {
    checkingAccount(mini, 0, true, currentPath);
    mini.Disk().printBitmap();
}

void Parser::exit(MiniHSFS& mini) {
    mini.Disk().SetConsoleColor(mini.Disk().Green);
    std::cout << "Bye :)" << std::endl;
    mini.Disk().SetConsoleColor(mini.Disk().Default);
}

void Parser::Network(MiniHSFS& mini)
{
    //run::currentPath = run::currentPath + run::DirName; //Get Current Directory

    //Cloud cloud;
    //std::string ip = cloud.getIPfromIpconfig();
    //std::cout << "Local IP: " << "http://" << ip << ":8081" << '\n';

    //httplib::Server svr;
    //cloud.setupRoutes(svr, this, mini, token);
    //std::cout << "Server is running at http://localhost:8081" << '\n';
    //svr.listen("0.0.0.0", 8081);

}

// AI Analysis Functions
void Parser::analyzeStorage(MiniHSFS& mini) {
    initializeAI(mini);
    fsAI->generateStorageReport();

    // Additional analysis
    auto bitmap = mini.Disk().getBitmap();
    size_t used_blocks = std::count(bitmap.begin(), bitmap.end(), true);
    size_t total_blocks = bitmap.size();

    std::cout << "\n\033[1mBlock Usage Analysis:\033[0m\n";
    std::cout << " - Used Blocks: " << used_blocks << " ("
        << (used_blocks * 100 / total_blocks) << "%)\n";
    std::cout << " - Free Blocks: " << (total_blocks - used_blocks) << " ("
        << ((total_blocks - used_blocks) * 100 / total_blocks) << "%)\n";

    // Fragmentation analysis
    size_t free_blocks = 0;
    size_t max_contiguous = 0;
    size_t current_contiguous = 0;

    for (size_t i = mini.dataStartIndex; i < total_blocks; i++) {
        if (!bitmap[i]) {
            free_blocks++;
            current_contiguous++;
            if (current_contiguous > max_contiguous) {
                max_contiguous = current_contiguous;
            }
        }
        else {
            current_contiguous = 0;
        }
    }

    std::cout << "\n\033[1mFragmentation Analysis:\033[0m\n";
    std::cout << " - Largest Contiguous Free Space: " << max_contiguous << " blocks\n";
    std::cout << " - Fragmentation Level: "
        << (free_blocks > 0 ? (100 - (max_contiguous * 100 / free_blocks)) : 0)
        << "%\n";
}

void Parser::predictNextAccess(MiniHSFS& mini, std::string& currentPath) {
    initializeAI(mini);

    std::vector<std::string> predicted_files = fsAI->predictNextFiles(
        currentPath.empty() ? "/" : currentPath
    );

    if (!predicted_files.empty()) {
        std::cout << "\n\033[1mAI Prediction:\033[0m Next likely files to access:\n";
        for (const auto& file : predicted_files) {
            std::cout << " - " << file << "\n";
        }
    }
}

void Parser::optimizeFilePlacement(const std::string& filePath, MiniHSFS& mini) {
    initializeAI(mini);

    int inode = mini.PathToInode(mini.SplitPath(filePath));
    if (inode == -1) return;

    MiniHSFS::Inode& file = mini.inodeTable[inode];
    if (file.isDirectory || file.firstBlock == -1) return;

    // Get current file data
    std::vector<char> data = readFile(filePath, mini);
    std::string file_type = detectFileType(data);

    // Get optimal placement suggestion
    VirtualDisk::Extent new_extent = fsAI->suggestOptimalBlockPlacement(
        file.blocksUsed,
        file_type
    );

    // If better placement found, move the file
    if (new_extent.startBlock != -1 && new_extent.startBlock != file.firstBlock) {
        if (mini.Disk().writeData(data, new_extent, "", true)) {
            // Free old blocks
            VirtualDisk::Extent old_extent(file.firstBlock, file.blocksUsed);
            mini.Disk().freeBlocks(old_extent);

            // Update inode
            file.firstBlock = new_extent.startBlock;
            file.modificationTime = time(nullptr);

            std::cout << "\033[32mOptimized placement for file: " << filePath
                << " (moved to blocks " << new_extent.startBlock
                << "-" << (new_extent.startBlock + new_extent.blockCount - 1)
                << ")\033[0m\n";
        }
    }
}

void Parser::checkSecurity(const std::string& operation, const std::string& path,
    MiniHSFS& mini, const std::string& password) {
    initializeAI(mini);

    if (fsAI->detectAnomalousActivity(path, operation, password)) {
        std::cerr << "\n\033[1m\033[31mSECURITY ALERT!\033[0m\n";
        std::cerr << "Suspicious activity detected:\n";
        std::cerr << " - Operation: " << operation << "\n";
        std::cerr << " - Path: " << path << "\n";

        // Additional security measures can be added here
        throw std::runtime_error("Security violation detected");
    }
}

void Parser::Chat(std::string name)
{
    ChatBot chat(name);
    while (true) {
        std::string question;
        std::cout << "--> You: ";
        std::getline(std::cin, question);

        if (question == "ex") break;

        std::string answer = chat.findBestAnswer(question, chat.db);

        if (answer.empty()) {
            std::cout << "-X> I didn't find a suitable answer. Can you tell me the answer and I will learn it?";
            std::string newAnswer;
            std::cout << "->> Answer: ";
            std::getline(std::cin, newAnswer);

            chat.saveAnswer(question, newAnswer);
            std::cout << "--> Saved and learning done!\n";
        }
        else {
            std::cout << "Bot: " << answer << "\n";
        }
    }
}

// Helper Functions
std::string Parser::detectFileType(const std::vector<char>& data) {
    if (fsAI == nullptr) return "unknown";
    return fsAI->detectFileType(data);
}