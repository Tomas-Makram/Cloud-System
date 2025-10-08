#include <iostream>
#include <limits>
#include <string>
#include <iostream>
#include <string>
#include <vector>
#include "httplib.h"
#include "Tokenizer.h"
#include "run.h"


class Cloud {
public:

	Cloud();
	~Cloud();

    std::string getIPfromIpconfig();

	void setupRoutes(httplib::Server& svr, Parser& parse, MiniHSFS& mini, Tokenizer& tokenizer);

private:

	struct auth {
		int index = -1;
		std::string username = "";
		std::string password = "";
		std::string dirname = "";
		std::string email;
		int strongPassword = 0;
	};

	//Active user sessions
    std::unordered_map<std::string, auth> activeSessions; // Session Id -> User Data
	
	// Function to create a unique sessionId
	std::string generateSessionId();

	// Function to verify the session
	auth checkSession(const httplib::Request& req);

	auth authenticateUser(const httplib::Request& req);

	std::string escapeHtml(const std::string& input);

	std::string formatTime(time_t timestamp);

	std::string formatSize(size_t bytes);

};
#pragma once