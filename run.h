#ifndef RUN_H
#define RUN_H

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

#include <string>
#include <iostream>
#include <limits>
#include <vector>

#include "SimpleAutoComplete.h"
#include "Tokenizer.h"
#include "IMG.cpp"
#include "Cloud.h"

class run {
public:
	std::string currentPath;
	std::string UserName;
	std::string DirName;
	std::string Password;
	int strongPassword;
	std::string Email;
	size_t TotalSize;

	void CloudSevrver(MiniHSFS& mini);
	bool Auth(MiniHSFS& mini, Parser& parse, std::string& currentPath);

private:


};
#endif
