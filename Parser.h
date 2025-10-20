#ifndef PARSER_H
#define PARSER_H

#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <set>

#include "MiniHSFS.h"
#include "ChatBot.h"
#include "AI.h"

class Parser {
public:

	struct account {
		std::string username = "";
		std::string dirname = "";
		std::string password = "";
		std::string email = "";
		size_t strongPassword = 0;
		size_t totalSize = 0;
	};

	// Constructor/Destructor
	Parser(std::string username, std::string dirname, std::string password, std::string email, size_t strongPassword, size_t totalSize);
	~Parser();

	void SetAccount(std::string username = "", std::string dirname = "", std::string password = "", std::string email = "", size_t strongPassword = 0, size_t totalSize = 0);
	account GetAccount();

	// Basic Operations with AI
	void ls(const std::string& path, MiniHSFS& mini, std::string& currentPath);
	
	void cd(const std::string& path, MiniHSFS& mini, std::string& currentPath);
	void cls();
	void printBitmap(MiniHSFS& mini, std::string& currentPath);
	MiniHSFS::Inode& getDirectoryItems(const std::string& path, MiniHSFS& mini, std::string& currentPath);
	void exit(MiniHSFS& mini);
	void printFileSystemInfo(MiniHSFS& mini, std::string& currentPath);
	void PrintBTreeStructure(MiniHSFS& mini, std::string& currentPath);

	// Account Settings
	void GetInfo(MiniHSFS& mini, int index);
	int checkingAccount(MiniHSFS& mini, size_t dataSize = 0, bool read = false, std::string currentPath = "");
	bool createAccount(MiniHSFS& mini);
	void ChangeInfo(MiniHSFS& mini, std::string email = "", std::string username = "", std::string password = "", std::string currentPath = "");

	// Enhanced File Operations with AI
	bool createDirectory(const std::string path, const std::string name, MiniHSFS& mini, std::string& currentPath);
	bool deleteDirectory(const std::string& path, MiniHSFS& mini, std::string& currentPath);
	int createFile(const std::string& path, const std::string name, MiniHSFS& mini, std::string& currentPath);
	bool deleteFile(const std::string& path, MiniHSFS& mini, std::string& currentPath);
	bool rename(std::string& oldPath, const std::string& newName, MiniHSFS& mini, std::string& currentPath);
	bool move(const std::string& srcPath, const std::string& destPath, MiniHSFS& mini, std::string& currentPath);
	bool copy(const std::string& srcPath, const std::string& destPath, MiniHSFS& mini, std::string& currentPath);

	// Smart Read/Write with AI
	std::vector<char> readFile(const std::string& path, MiniHSFS& mini, size_t maxChunkSize = 0, bool showProgress = true, const std::string& password = "", std::string currentPath = "");
	bool writeFile(const std::string& path, const std::vector<char>& data, MiniHSFS& mini, bool append = false, const std::string& password = "", std::string currentPath = "");

	// AI Analysis Functions
	void Chat(std::string name);
	void analyzeStorage(MiniHSFS& mini);
	void predictNextAccess(MiniHSFS& mini, std::string& currentPath);
	void optimizeFilePlacement(const std::string& filePath, MiniHSFS& mini);
	void checkSecurity(const std::string& operation, const std::string& path,
		MiniHSFS& mini, const std::string& password = "");

private:

	MiniHSFSAI* fsAI;
	account myAccount;
   

	void initializeAI(MiniHSFS& mini);
	void printFileInfo(int file_inode, const std::string& path, bool longFormat, MiniHSFS& mini);
	void printInodeInfo(int inode_num, const std::string& path, bool longFormat, MiniHSFS& mini);
	void printDirectoryContents(int dirInode, const std::string& path,
		bool longFormat, bool showHidden, bool recursive,
		const std::string& indent, bool isLast, MiniHSFS& mini);
	std::string detectFileType(const std::vector<char>& data);

};
#endif