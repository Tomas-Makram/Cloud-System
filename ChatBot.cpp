#include "ChatBot.h"

//ChatBot::ChatBot(std::string name) {
//
//    int rc = sqlite3_open(name.c_str(), &db);
//
//    if (rc)
//        throw ("Database cannot be opened: ", sqlite3_errmsg(this->db));
//
//    // Create knowledge table
//    const char* createTableSQL =
//        "CREATE TABLE IF NOT EXISTS KnowledgeBase ("
//        "ID INTEGER PRIMARY KEY AUTOINCREMENT, "
//        "Question TEXT UNIQUE, "
//        "Answer TEXT, "
//        "Weight INTEGER DEFAULT 1);";
//    sqlite3_exec(this->db, createTableSQL, nullptr, nullptr, nullptr);
//
//    std::cout << "-> Smart self-learning chatbot (type 'exit' to exit)\n";
//
//};

ChatBot::ChatBot(std::string name) {
    int rc = sqlite3_open(name.c_str(), &db);

    if (rc) {
        std::string error = "Database cannot be opened: " + std::string(sqlite3_errmsg(db));
        throw std::runtime_error(error);
    }

    // Create knowledge table
    const char* createTableSQL =
        "CREATE TABLE IF NOT EXISTS KnowledgeBase ("
        "ID INTEGER PRIMARY KEY AUTOINCREMENT, "
        "Question TEXT UNIQUE, "
        "Answer TEXT, "
        "Weight INTEGER DEFAULT 1);";

    char* errMsg = nullptr;
    if (sqlite3_exec(this->db, createTableSQL, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::string error = "Table creation failed: " + std::string(errMsg);
        sqlite3_free(errMsg);
        throw std::runtime_error(error);
    }

    std::cout << "-> Smart self-learning chatbot (type 'exit' to exit)\n";
}

ChatBot::~ChatBot() {
    sqlite3_close(this->db);
};

// Split the text into words

//std::vector<std::string> ChatBot::tokenize(const std::string& text) {
//    std::vector<std::string> tokens;
//    std::string word;
//    for (char c : text) {
//        if (isalnum(c))
//            word += tolower(c);
//        else if (!word.empty()) {
//            tokens.push_back(word);
//            word.clear();
//        }
//    }
//    if (!word.empty()) tokens.push_back(word);
//    return tokens;
//}
std::vector<std::string> ChatBot::tokenize(const std::string& text) {
    static const std::set<std::string> stopWords = {
        "the","is","am","are","was","were","be","been","being",
        "a","an","and","or","but","if","then","else",
        "in","on","at","by","for","with","about","against",
        "to","from","up","down","over","under","again","further",
        "this","that","these","those","here","there","when","where",
        "why","how","all","any","both","each","few","more","most",
        "other","some","such","no","nor","not","only","own","same","so",
        "too","very","can","will","just","don","should","now"
    };

    std::vector<std::string> tokens;
    std::string word;

    for (char c : text) {
        if (isalnum(c)) {
            word += tolower(c);
        }
        else if (!word.empty()) {
            // Stemming بسيط
            if (word.size() > 3) {
                if (word.substr(word.size() - 3) == "ing") word = word.substr(0, word.size() - 3);
                else if (word.substr(word.size() - 2) == "ed") word = word.substr(0, word.size() - 2);
                else if (word.back() == 's') word.pop_back();
            }
            if (!stopWords.count(word)) tokens.push_back(word);
            word.clear();
        }
    }
    if (!word.empty() && !stopWords.count(word)) {
        if (word.size() > 3) {
            if (word.substr(word.size() - 3) == "ing") word = word.substr(0, word.size() - 3);
            else if (word.substr(word.size() - 2) == "ed") word = word.substr(0, word.size() - 2);
            else if (word.back() == 's') word.pop_back();
        }
        tokens.push_back(word);
    }

    return tokens;
}

//TF account
std::map<std::string, double> ChatBot::computeTF(const std::vector<std::string>& tokens) {
    std::map<std::string, double> tf;
    for (auto& w : tokens) tf[w]++;
    for (auto& kv : tf) kv.second /= tokens.size();
    return tf;
}

// IDF account
std::map<std::string, double> ChatBot::computeIDF(const std::vector<std::vector<std::string>>& docs) {
    std::map<std::string, double> idf;
    size_t N = docs.size();
    for (auto& doc : docs) {
        std::map<std::string, bool> seen;
        for (auto& w : doc) seen[w] = true;
        for (auto& kv : seen) idf[kv.first]++;
    }
    for (auto& kv : idf) kv.second = log((double)N / (1 + kv.second));
    return idf;
}

// TF-IDF account
std::map<std::string, double> ChatBot::computeTFIDF(const std::vector<std::string>& tokens,
    const std::map<std::string, double>& idf) {
    auto tf = computeTF(tokens);
    std::map<std::string, double> tfidf;
    for (auto& kv : tf) {
        if (idf.count(kv.first)) tfidf[kv.first] = kv.second * idf.at(kv.first);
    }
    return tfidf;
}

// Cosine Similarity
double ChatBot::cosineSim(const std::map<std::string, double>& v1, const std::map<std::string, double>& v2) {
    double dot = 0, norm1 = 0, norm2 = 0;
    for (auto& kv : v1) {
        if (v2.count(kv.first)) dot += kv.second * v2.at(kv.first);
        norm1 += kv.second * kv.second;
    }
    for (auto& kv : v2) norm2 += kv.second * kv.second;
    return (norm1 && norm2) ? dot / (sqrt(norm1) * sqrt(norm2)) : 0;
}

std::vector<ChatBot::QA> ChatBot::loadKnowledge() {
    std::vector<QA> knowledge;
    sqlite3_stmt* stmt;
    std::string sql = "SELECT ID, Question, Answer, Weight FROM KnowledgeBase;";

    if (sqlite3_prepare_v2(this->db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            QA qa;
            qa.id = sqlite3_column_int(stmt, 0);
            const unsigned char* questionText = sqlite3_column_text(stmt, 1);
            const unsigned char* answerText = sqlite3_column_text(stmt, 2);

            qa.question = questionText ? reinterpret_cast<const char*>(questionText) : "";
            qa.answer = answerText ? reinterpret_cast<const char*>(answerText) : "";
            qa.weight = sqlite3_column_int(stmt, 3);

            if (!qa.question.empty()) {
                knowledge.push_back(qa);
            }
        }
        sqlite3_finalize(stmt);
    }
    return knowledge;
}

// Save question/answer
void ChatBot::saveAnswer(const std::string& question, const std::string& answer) {
    sqlite3_stmt* stmt;
    std::string sql = "INSERT OR REPLACE INTO KnowledgeBase (Question, Answer, Weight) VALUES (?, ?, COALESCE((SELECT Weight FROM KnowledgeBase WHERE Question=?), 1));";

    if (sqlite3_prepare_v2(this->db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, question.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, answer.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, question.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

// Weight gain (self-learning)
void ChatBot::increaseWeight(sqlite3* db, int id) {
    sqlite3_stmt* stmt;
    std::string sql = "UPDATE KnowledgeBase SET Weight = Weight + 1 WHERE ID = ?;";

    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

// Find the best answer

//std::string ChatBot::findBestAnswer(const std::string& query, sqlite3* db) {
//    auto knowledge = loadKnowledge(db);
//    if (knowledge.empty()) return "";
//
//    std::vector<std::vector<std::string>> docs;
//    for (auto& qa : knowledge) {
//        docs.push_back(tokenize(qa.question));
//    }
//    auto idf = computeIDF(docs);
//
//    auto queryTokens = tokenize(query);
//    auto queryVec = computeTFIDF(queryTokens, idf);
//
//    double bestScore = 0.0;
//    int bestId = -1;
//    std::string bestAnswer;
//
//    for (size_t i = 0; i < knowledge.size(); i++) {
//        auto qVec = computeTFIDF(docs[i], idf);
//        //double score = cosineSim(queryVec, qVec) * (1 + log(1 + knowledge[i].weight)); // Weight
//        double score = cosineSim(queryVec, qVec) *
//            (1 + log(1 + knowledge[i].weight)) *
//            (1 + jaccardSim(querySet, qSet)); // دمج أكثر من خوارزمية
//
//        if (score > bestScore) {
//            bestScore = score;
//            bestAnswer = knowledge[i].answer;
//            bestId = knowledge[i].id;
//        }
//    }
//
//    if (bestScore < 0.2) return "";
//    if (bestId != -1) increaseWeight(db, bestId); // Strengthen the existing question
//    return bestAnswer;
//}

// Find the best answer fixer
std::string ChatBot::findBestAnswer(const std::string& query, sqlite3* db) {
    auto knowledge = loadKnowledge();
    if (knowledge.empty()) return "";

    std::vector<std::vector<std::string>> docs;
    for (auto& qa : knowledge) {
        docs.push_back(tokenize(qa.question));
    }
    auto idf = computeIDF(docs);

    auto queryTokens = tokenize(query);
    auto queryVec = computeTFIDF(queryTokens, idf);
    std::set<std::string> querySet(queryTokens.begin(), queryTokens.end());

    double bestScore = 0.0;
    int bestId = -1;
    std::string bestAnswer;

    for (size_t i = 0; i < knowledge.size(); i++) {
        auto qTokens = tokenize(knowledge[i].question);
        auto qVec = computeTFIDF(qTokens, idf);
        std::set<std::string> qSet(qTokens.begin(), qTokens.end());

        double cosine = cosineSim(queryVec, qVec);
        double jaccard = jaccardSim(querySet, qSet);
        double weightFactor = log(1 + knowledge[i].weight);

        double score = (0.6 * cosine) + (0.3 * jaccard) + (0.1 * weightFactor);

        if (score > bestScore) {
            bestScore = score;
            bestAnswer = knowledge[i].answer;
            bestId = knowledge[i].id;
        }
    }

    std::cout << "Best match score: " << bestScore << std::endl; // للتصحيح

    if (bestScore < 0.3) return ""; // عتبة منخفضة قليلاً للبداية
    if (bestId != -1) increaseWeight(this->db, bestId);

    return bestAnswer;
}

bool ChatBot::containsAny(const std::string& text, const std::string& keywords) {
    std::istringstream iss(keywords);
    std::string keyword;
    std::string lowerText = text;
    std::transform(lowerText.begin(), lowerText.end(), lowerText.begin(), ::tolower);

    while (iss >> keyword) {
        if (lowerText.find(keyword) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string ChatBot::getGreetingResponse() {
    return "Hello! How can I help you today?";
}

std::string ChatBot::getFarewellResponse() {
    return "Goodbye! Feel free to ask if you have more questions.";
}

// Correct the trust account
double ChatBot::calculateConfidence(const std::string& query, const std::string& storedQuestion) {
    auto queryTokens = tokenize(query);
    auto storedTokens = tokenize(storedQuestion);

    if (queryTokens.empty() || storedTokens.empty()) return 0.0;

    std::set<std::string> querySet(queryTokens.begin(), queryTokens.end());
    std::set<std::string> storedSet(storedTokens.begin(), storedTokens.end());

    return jaccardSim(querySet, storedSet);
}


std::pair<int, double> ChatBot::findSimilarQuestions(const std::string& question) {
    auto knowledge = loadKnowledge();
    auto questionTokens = tokenize(question);
    std::set<std::string> questionSet(questionTokens.begin(), questionTokens.end());

    int bestId = -1;
    double bestScore = 0.0;

    for (auto& qa : knowledge) {
        auto qTokens = tokenize(qa.question);
        std::set<std::string> qSet(qTokens.begin(), qTokens.end());
        double score = jaccardSim(questionSet, qSet);

        if (score > bestScore) {
            bestScore = score;
            bestId = qa.id;
        }
    }

    return { bestId, bestScore };
}

double ChatBot::jaccardSim(const std::set<std::string>& set1, const std::set<std::string>& set2) {
    if (set1.empty() && set2.empty()) return 0.0;

    std::set<std::string> intersection;
    std::set_intersection(set1.begin(), set1.end(), set2.begin(), set2.end(),
        std::inserter(intersection, intersection.begin()));

    std::set<std::string> union_set;
    std::set_union(set1.begin(), set1.end(), set2.begin(), set2.end(),
        std::inserter(union_set, union_set.begin()));

    return union_set.empty() ? 0.0 : (double)intersection.size() / union_set.size();
}

// The main function fixed
std::string ChatBot::processQuery(const std::string& query) {

    // Check the learning style first
    if (query.find("teach:") == 0) {
        size_t separator = query.find('|');
        if (separator != std::string::npos) {
            std::string question = query.substr(6, separator - 6);
            std::string answer = query.substr(separator + 1);

            //Cleaning spaces
            question.erase(0, question.find_first_not_of(" \t"));
            question.erase(question.find_last_not_of(" \t") + 1);
            answer.erase(0, answer.find_first_not_of(" \t"));
            answer.erase(answer.find_last_not_of(" \t") + 1);

            this->teach(question, answer);
            return "Thank you! I've learned: '" + question + "' -> '" + answer + "'";
        }
    }

    Intent intent = classifyIntent(query);

    if (intent == GREETING) return getGreetingResponse();
    if (intent == FAREWELL) return getFarewellResponse();

    std::string answer = findBestAnswer(query, this->db);

    if (answer.empty()) {
        return learnNewResponse(query);
    }

    updateContext(query, answer);
    logInteraction(query, answer, 0.8);// Virtual trust
    return answer;
}

//Smart learning correction
void ChatBot::smartLearning(const std::string& question, const std::string& answer) {
    auto similar = findSimilarQuestions(question);
    if (similar.second > 0.8) { //Lower merge threshold
        std::cout << "Merging with existing question ID: " << similar.first << std::endl;
        mergeQuestions(similar.first, question, answer);
    }
    else {
        std::cout << "Saving as new question" << std::endl;
        saveAnswer(question, answer);
    }
}

// Explicit learning function
void ChatBot::teach(const std::string& question, const std::string& answer) {
    smartLearning(question, answer);
    std::cout << "Learned: " << question << " -> " << answer << std::endl;
}

void ChatBot::increaseWeight(int id) {
    sqlite3_stmt* stmt;
    std::string sql = "UPDATE KnowledgeBase SET Weight = Weight + 1 WHERE ID = ?;";

    if (sqlite3_prepare_v2(this->db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

// تحسين رد التعلم
std::string ChatBot::learnNewResponse(const std::string& query) {
    return "I don't know how to answer: \"" + query + "\". " +
        "Please teach me using: teach: your question | your answer";
}

void ChatBot::mergeQuestions(int id, const std::string& question, const std::string& answer) {
    // غير db إلى this->db
    increaseWeight(this->db, id);

    // يمكن إضافة تحديث الإجابة أيضاً
    sqlite3_stmt* stmt;
    std::string sql = "UPDATE KnowledgeBase SET Answer = ? WHERE ID = ?;";

    if (sqlite3_prepare_v2(this->db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, answer.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void ChatBot::updateContext(const std::string& userInput, const std::string& botResponse) {
    conversationHistory.push_back("User: " + userInput);
    conversationHistory.push_back("Bot: " + botResponse);

    // احتفظ بآخر 10 رسائل فقط (20 سطر = 10 محادثات)
    if (conversationHistory.size() > 20) {
        conversationHistory.erase(conversationHistory.begin(), conversationHistory.begin() + 2);
    }

    // تحديث السياق من آخر محادثتين
    if (conversationHistory.size() >= 4) {
        context = conversationHistory[conversationHistory.size() - 4] + " " +
            conversationHistory[conversationHistory.size() - 3] + " " +
            conversationHistory[conversationHistory.size() - 2] + " " +
            conversationHistory[conversationHistory.size() - 1];
    }
}

ChatBot::Intent ChatBot::classifyIntent(const std::string& text) {
    std::string lowerText = text;
    std::transform(lowerText.begin(), lowerText.end(), lowerText.begin(), ::tolower);

    // تحقق من الترحيب
    if (containsAny(lowerText, "hello hi hey"))
        return GREETING;

    // تحقق من الوداع
    if (containsAny(lowerText, "exit quit stop bye goodbye"))
        return FAREWELL;

    // تحقق من الأوامر
    if (containsAny(lowerText, "search find look"))
        return COMMAND;

    return QUESTION;
}

void ChatBot::logInteraction(const std::string& query, const std::string& response, double confidence) {
    std::ofstream logfile("chatbot_log.csv", std::ios::app);
    if (logfile.is_open()) {
        logfile << std::time(nullptr) << "," << query << "," << confidence << "\n";
        logfile.close();
    }
}