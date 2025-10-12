#include "run.h"

//1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31

int run::strongPassword = 1000;
std::string run::DirName = "Tomas";
std::string run::UserName = "Tomas";
std::string run::Email = "tomasmakram86627@gmail.com";
std::string run::Password = "ToTo";//"Tomas1234#????!!!!";

std::string run::currentPath = "/";

size_t run::TotalSize = 10 * 1024 * 1024;//bytes


void run::CloudSevrver(MiniHSFS& mini) {
    currentPath = "/" + DirName;

    Cloud cloud;
    std::string ip = cloud.getIPfromIpconfig();
    std::cout << "Local IP: " << "http://" << ip << ":8081" << std::endl;

    httplib::Server svr;
    Parser parse(UserName, DirName, Password, Email, strongPassword, TotalSize);
    Tokenizer tokenize(UserName, DirName, Password, Email, strongPassword, TotalSize);
    cloud.setupRoutes(svr, parse, mini, tokenize, currentPath, Password);
    std::cout << "Server is running at http://localhost:8081\n";

    svr.listen("0.0.0.0", 8081);
}

int main() {

    printColoredText();
    const std::string diskPath = "test_disk.vd";
    const uint64_t diskSizeMB = 0; // 10MB virtual disk
    const uint32_t blockSize = 4 * 1024; //4KB

    //Create Object Mini Hyper Stracture File System Design
    MiniHSFS mini(diskPath, diskSizeMB, blockSize);

    //Initialize virtual disk
    mini.Disk().SetConsoleColor(mini.Disk().Yellow);
    std::cout << "Initializing virtual disk..." << std::endl;
    mini.Disk().SetConsoleColor(mini.Disk().Default);

    if (mini.Disk().IsNew())
    {
        mini.Disk().SetConsoleColor(mini.Disk().Green);
        std::cout << "Disk initialized successfully! Total blocks: " << mini.Disk().totalBlocks() << std::endl;
        mini.Disk().SetConsoleColor(mini.Disk().Default);
    }
    else
    {
        mini.Disk().SetConsoleColor(mini.Disk().Green);
        std::cout << "Disk Open successfully! Total blocks: " << mini.Disk().totalBlocks() << std::endl;
        mini.Disk().SetConsoleColor(mini.Disk().Default);
    }
    mini.Mount(256); // inodeSize
    
    MiniHSFSAI fsAI(mini);  // Configure the AI ​​system

    // Initialize and mount the file system
    mini.Disk().SetConsoleColor(mini.Disk().Green);
    std::cout << "File system mounted successfully." << std::endl;
    mini.Disk().SetConsoleColor(mini.Disk().Default);
    
    // Start Token Arguments Constractor
    Tokenizer tokenizer(run::UserName, run::DirName, run::Password, run::Email, run::strongPassword, run::TotalSize);
    
    run::currentPath = run::currentPath + run::DirName;


    std::string input;

    mini.Disk().SetConsoleColor(mini.Disk().Magenta);
    std::cout << "Check Processor Table Commands" << std::endl;
    tokenizer.runAll(mini, run::currentPath, run::Password);
    mini.Disk().SetConsoleColor(mini.Disk().Default);

    SimpleAutoComplete autoComplete(mini, run::currentPath);

    while (true)
    {
        std::string prompt = run::currentPath + " >> ";
        std::string input = autoComplete.readInput(prompt);
        try {
            if (input == "cloud")
            {
                run run;
                run.CloudSevrver(mini);
            }
            else
            {
                tokenizer.processCommand(input, mini, run::currentPath, run::Password);
            }
            if (input == "exit") break;
        }
        catch (const std::exception& ex) {
            mini.Disk().SetConsoleColor(mini.Disk().Red);
            std::cerr << "\nFatal Error: " << ex.what() << "\n";
            mini.Disk().SetConsoleColor(mini.Disk().Default);
        }
    }
//------------------------------------------------------------------------//

    return 0;
}