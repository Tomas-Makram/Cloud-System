#ifndef TOKENIZER_H
#define TOKENIZER_H

#include <string>
#include <ostream>
#include <vector>
#include <memory>
#include <sstream>
#include <iostream>
#include <limits>
#include "Parser.h"

enum class ProcessState { Ready, Running, Pause, Terminated };

class Tokenizer {

public:

	//Constracter
	Tokenizer(std::string username, std::string dirname, std::string password, std::string email, size_t strongPassword, size_t totalSize);
	//Run Commands
	void processCommand(const std::string& command, MiniHSFS& mini, std::string& currentPath, std::string& password);

	// Process management functions
	int createProcess(const std::string& name, const std::vector<std::string>& args);
	void runAll(MiniHSFS& mini, std::string& currentPath, std::string& password);

	//Search PID Process
	bool isRunning(int pid) const;
	bool isTerminated(int pid) const;
	bool isReady(int pid) const;

	// Process PID Process
	void stopProcess(int pid);
	void monitorProcesses() const;

private:
	//Parser Object
	Parser parse;
	
	//Splits Commands
	std::vector<std::string> ParseArguments(const std::string& input);

	//Send Commant to Parser
	void handleCommand(const std::vector<std::string>& args, MiniHSFS& mini, std::string& currentPath, std::string& password);

	// Process related members
	struct Process {
		int pid;
		std::string name;
		std::vector<std::string> args;
		ProcessState state;
	};

	// Process PID Process
	const Process* findProcess(int pid) const;
	Process* findProcess(int pid);

	//Counter Processing
	int nextPid = 1;
	//Processing Table
	std::vector<Process> processTable;
};
#endif