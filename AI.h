#ifndef MINI_HSFS_AI_H
#define MINI_HSFS_AI_H

#include "MiniHSFS.h"
#include <vector>
#include <map>
#include <string>
#include <chrono>
#include <mutex>

class MiniHSFSAI {
public:
    // Constructor
    explicit MiniHSFSAI(MiniHSFS& filesystem);

    // Smart Block Placement
    VirtualDisk::Extent suggestOptimalBlockPlacement(size_t requiredBlocks, const std::string& fileType = "");

    // File Access Prediction
    void analyzeAccessPattern(const std::string& currentFile);
    std::vector<std::string> predictNextFiles(const std::string& currentFile);

    // Anomaly Detection
    bool detectAnomalousActivity(const std::string& filePath, const std::string& operation, const std::string& password = "");

    // File Classification
    std::string detectFileType(const std::vector<char>& fileData);

    // Storage Analytics
    void updateStorageStats(const std::string& filePath, size_t size, const std::string& fileType);
    void generateStorageReport();

    // Smart Compression
    bool shouldCompressFile(const std::string& filePath, const std::string& fileType);
    std::vector<char> compressData(const std::vector<char>& data);
    std::vector<char> decompressData(const std::vector<char>& compressed);

private:
    MiniHSFS& fs;
    std::mutex aiMutex;

    // Block access tracking
    std::map<int, int> blockAccessCount;
    std::map<int, std::chrono::system_clock::time_point> blockLastAccess;

    // File access patterns
    std::map<std::string, std::vector<std::string>> fileAccessPatterns;
    std::map<std::string, std::chrono::system_clock::time_point> fileLastAccess;

    // Security monitoring
    std::map<std::string, int> failedAuthAttempts;
    std::map<std::string, int> recentAccessCount;

    // File type database
    std::map<std::string, std::string> fileTypes;

    // Storage analytics
    std::map<std::string, std::pair<size_t, int>> storageUsage;

    // Helper methods
    void loadFileTypePatterns();
    bool isTextFile(const std::vector<char>& data);
    bool isImageFile(const std::vector<char>& data);
    bool isArchiveFile(const std::vector<char>& data);
    bool isCodeFile(const std::vector<char>& data);
    std::string formatSize(size_t bytes);
};

#endif // MINI_HSFS_AI_H