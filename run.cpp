#ifdef _WIN32
#define _WINSOCKAPI_     // يمنع تضارب winsock
#define WIN32_LEAN_AND_MEAN
#define NOCRYPT          // يمنع تضارب crypto macros
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif


#include <iostream>
#include <limits>
#include <string>
#include "Parser.h"
#include "VirtualDisk.h"
#include "MiniHSFS.h"
#include "Tokenizer.h"
#include "SimpleAutoComplete.h"

#include <iostream>
#include <limits>
#include <string>
#include <vector>
#include "run.h"
#include "httplib.h"
#include "IMG.cpp"

#include "AI.h"

#include "CryptoUtils.h"

#include "Cloud.h"



int run::strongPassword = 1000;
std::string run::DirName = "Tomas";
std::string run::UserName = "Tomas";
std::string run::Email = "tomasmakram86627@gmail.com";
std::string run::Password = "ToTo";//"Tomas1234#????!!!!";

std::string run::currentPath = "/";

size_t run::TotalSize = 10 * 1024 * 1024;//bytes

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
    
    MiniHSFSAI fsAI(mini);  // تهيئة نظام الذكاء الاصطناعي

    //////////////////////////////////////////////////////////////
    // Initialize and mount the file system
    mini.Disk().SetConsoleColor(mini.Disk().Green);
    std::cout << "File system mounted successfully." << std::endl;
    mini.Disk().SetConsoleColor(mini.Disk().Default);
    
    Cloud cloud;
    std::string ip = cloud.getIPfromIpconfig();
    std::cout << "Local IP: " <<"http://"<< ip<<":8081" << std::endl;
    
    httplib::Server svr;
    Parser parse;
    Tokenizer tokenizer("Tomas", "Tomas", "ToTo", 1000);
    cloud.setupRoutes(svr, parse, mini, tokenizer);
    std::cout << "Server is running at http://localhost:8081" << std::endl;

    parse.createAccount(mini);
    run::currentPath = run::currentPath + run::DirName;
//    1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31
//    svr.listen("0.0.0.0", 8081);

    std::string input;

    mini.Disk().SetConsoleColor(mini.Disk().Magenta);
    std::cout << "Check Processor Table Commands" << std::endl;
    tokenizer.runAll(mini);
    mini.Disk().SetConsoleColor(mini.Disk().Default);

    SimpleAutoComplete autoComplete(mini);

    while (true)
    {
        std::string prompt = run::currentPath + " >> ";
        std::string input = autoComplete.readInput(prompt);
        try {
            tokenizer.processCommand(input, mini);
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
