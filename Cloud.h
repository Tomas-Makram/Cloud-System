#include <iostream>
#include <limits>
#include <string>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
#include "httplib.h"
#include "Tokenizer.h"
#include "run.h"


class Cloud {
public:

	static std::string getIPfromIpconfig();

	void setupRoutes(httplib::Server& svr, Parser& parse, MiniHSFS& mini, Tokenizer& tokenizer);

private:

	static std::string escapeHtml(const std::string& input);

	static std::string formatTime(time_t timestamp);

	static std::string formatSize(size_t bytes);

};
#pragma once