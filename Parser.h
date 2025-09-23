#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <algorithm>
#include "run.h"
#include "MiniHSFS.h"
#include "CryptoUtils.h"
#include "AI.h"

#include <iomanip>
#include <set>


class Parser {
public:

	// Constructor/Destructor
	Parser();
	~Parser();

	// Basic Operations with AI
	void ls(const std::string& path, MiniHSFS& mini);
	
	void cd(const std::string& path, MiniHSFS& mini);
	void cls();
	void printBitmap(MiniHSFS& mini);
	MiniHSFS::Inode getDirectoryItems(const std::string& path, MiniHSFS& mini);
	void exit(MiniHSFS& mini);
	void printFileSystemInfo(MiniHSFS& mini);
	void PrintBTreeStructure(MiniHSFS& mini);

	// Account Settings
	void GetInfo(MiniHSFS& mini, int index);
	int checkingAccount(MiniHSFS& mini, size_t dataSize = 0 , bool read = false);
	bool createAccount(MiniHSFS& mini);
	void ChangeInfo(MiniHSFS& mini, std::string email = "", std::string username = "", std::string password = "");

	// Enhanced File Operations with AI
	bool createDirectory(const std::string path, const std::string name, MiniHSFS& mini);
	bool deleteDirectory(const std::string& path, MiniHSFS& mini);
	int createFile(const std::string& path, const std::string name, MiniHSFS& mini);
	bool deleteFile(const std::string& path, MiniHSFS& mini);
	bool rename(const std::string& oldPath, const std::string& newName, MiniHSFS& mini);
	bool move(const std::string& srcPath, const std::string& destPath, MiniHSFS& mini);
	bool copy(const std::string& srcPath, const std::string& destPath, MiniHSFS& mini);

	// Smart Read/Write with AI
	std::vector<char> readFile(const std::string& path, MiniHSFS& mini, size_t maxChunkSize = 0, bool showProgress = true, const std::string& password = "");
	bool writeFile(const std::string& path, const std::vector<char>& data, MiniHSFS& mini, bool append = false, const std::string& password = "");

	// AI Analysis Functions
	void analyzeStorage(MiniHSFS& mini);
	void predictNextAccess(MiniHSFS& mini);
	void optimizeFilePlacement(const std::string& filePath, MiniHSFS& mini);
	void checkSecurity(const std::string& operation, const std::string& path,
		MiniHSFS& mini, const std::string& password = "");

private:
	MiniHSFSAI* fsAI;

	void initializeAI(MiniHSFS& mini);
	void printFileInfo(int file_inode, const std::string& path, bool longFormat, MiniHSFS& mini);
	void printInodeInfo(int inode_num, const std::string& path, bool longFormat, MiniHSFS& mini);
	void printDirectoryContents(int dirInode, const std::string& path,
		bool longFormat, bool showHidden, bool recursive,
		const std::string& indent, bool isLast, MiniHSFS& mini);
	std::string detectFileType(const std::vector<char>& data);

};
#pragma once