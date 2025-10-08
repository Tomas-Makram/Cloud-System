#ifndef CHATBOT_H
#define CHATBOT_H

#include <iostream>
#include <sqlite3.h>
#include <string>
#include <vector>
#include <map>
#include <cmath>
#include <sstream>
#include <algorithm>
#include <set>
#include <fstream>
#include <ctime>
#include <stdexcept>
#include <iterator>

using namespace std;

class ChatBot {
public:


    sqlite3* db;
    std::vector<std::string> conversationHistory;
    std::string context;

   // Constructor & Destructive
    ChatBot(std::string name);
    ~ChatBot();

    // Prevent copying
    ChatBot(const ChatBot&) = delete;
    ChatBot& operator=(const ChatBot&) = delete;

    // Database operations
    std::string findBestAnswer(const std::string& query, sqlite3* db);
    void saveAnswer(const std::string& question, const std::string& answer);


    std::string processQuery(const std::string& query);

    // Logging
    void logInteraction(const std::string& query, const std::string& response, double confidence);

private:

    enum Intent { GREETING, QUESTION, COMMAND, FAREWELL };

    // Download knowledge
    struct QA {
        int id = 0;
        std::string question = "";
        std::string answer = "";
        int weight = 0;
    };

    // NLP & Similarity
    std::vector<std::string> tokenize(const std::string& text);
    std::map<std::string, double> computeTF(const std::vector<std::string>& tokens);
    std::map<std::string, double> computeIDF(const std::vector<std::vector<std::string>>& docs);
    std::map<std::string, double> computeTFIDF(const std::vector<std::string>& tokens, const std::map<std::string, double>& idf);
    double cosineSim(const std::map<std::string, double>& v1, const std::map<std::string, double>& v2);
    double jaccardSim(const std::set<std::string>& set1, const std::set<std::string>& set2);

    // Database operations
    std::vector<QA> loadKnowledge();
    void increaseWeight(sqlite3* db, int id);

    // Learning & Context
    std::pair<int, double> findSimilarQuestions(const std::string& question);
    void mergeQuestions(int id, const std::string& question, const std::string& answer);
    void smartLearning(const std::string& question, const std::string& answer);
    void updateContext(const std::string& userInput, const std::string& botResponse);
    void teach(const std::string& question, const std::string& answer);

    // Intent & Response
    Intent classifyIntent(const std::string& text);
    bool containsAny(const std::string& text, const std::string& keywords);
    double calculateConfidence(const std::string& query, const std::string& answer);

    // Response generators
    std::string getGreetingResponse();
    std::string getFarewellResponse();
    void increaseWeight(int id);
    std::string learnNewResponse(const std::string& query);


};
#endif