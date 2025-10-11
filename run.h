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
	void CloudSevrver(MiniHSFS& mini);

	static std::string currentPath;
	static std::string UserName;
	static std::string DirName;
	static std::string Password;
	static int strongPassword;
	static std::string Email;
	static size_t TotalSize;

};
#endif
