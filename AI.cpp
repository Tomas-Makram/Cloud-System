#include "AI.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <cctype>

MiniHSFSAI::MiniHSFSAI(MiniHSFS& filesystem) : fs(filesystem) {
    loadFileTypePatterns();
}

VirtualDisk::Extent MiniHSFSAI::suggestOptimalBlockPlacement(size_t requiredBlocks, const std::string& fileType) {
    std::lock_guard<std::mutex> lock(aiMutex);
    auto bitmap = fs.Disk().getBitmap();
    const size_t totalBlocks = bitmap.size();

    std::vector<double> zoneHeat(totalBlocks, 0.0);
    for (size_t i = 0; i < totalBlocks; ++i) {
        if (blockAccessCount.find(static_cast<int>(i)) != blockAccessCount.end()) {
            zoneHeat[i] = blockAccessCount[static_cast<int>(i)];
        }
    }

    int bestStart = -1;
    double bestScore = -1.0;
    size_t currentStart = fs.dataStartIndex;
    size_t contiguousCount = 0;

    for (size_t i = fs.dataStartIndex; i < totalBlocks; ++i) {
        if (!bitmap[i]) {
            if (contiguousCount == 0) currentStart = i;
            contiguousCount++;

            if (contiguousCount >= requiredBlocks) {
                double score = 0.0;
                double usageScore = 0.0;
                for (size_t j = currentStart; j < currentStart + requiredBlocks; ++j) {
                    usageScore += (1.0 / (1.0 + zoneHeat[j]));
                }
                usageScore /= requiredBlocks;

                double proximityScore = 0.0;
                if (!fileType.empty()) {
                    for (const auto& entry : fileTypes) {
                        if (entry.second == fileType) {
                            proximityScore += 1.0 / (1.0 + std::abs((int)currentStart - (int)fs.dataStartIndex));
                        }
                    }
                }

                score = 0.7 * usageScore + 0.3 * proximityScore;

                if (score > bestScore) {
                    bestScore = score;
                    bestStart = static_cast<int>(currentStart);
                }

                i = currentStart + 1;
                contiguousCount = 0;
            }
        }
        else {
            contiguousCount = 0;
        }
    }

    if (bestStart != -1) {
        return VirtualDisk::Extent(static_cast<uint32_t>(bestStart), static_cast<uint32_t>(requiredBlocks));
    }

    return fs.AllocateContiguousBlocks(static_cast<int>(requiredBlocks));
}

void MiniHSFSAI::analyzeAccessPattern(const std::string& currentFile) {
    std::lock_guard<std::mutex> lock(aiMutex);
    auto now = std::chrono::system_clock::now();
    fileLastAccess[currentFile] = now;

    for (const auto& entry : fileLastAccess) {
        if (entry.first != currentFile &&
            (now - entry.second) < std::chrono::minutes(5)) {
            fileAccessPatterns[currentFile].push_back(entry.first);
        }
    }
}

std::vector<std::string> MiniHSFSAI::predictNextFiles(const std::string& currentFile) {
    std::lock_guard<std::mutex> lock(aiMutex);
    std::map<std::string, int> nextFileCounts;

    if (fileAccessPatterns.find(currentFile) != fileAccessPatterns.end()) {
        for (const auto& pattern : fileAccessPatterns[currentFile]) {
            nextFileCounts[pattern]++;
        }
    }

    std::vector<std::pair<std::string, int>> sortedResults(nextFileCounts.begin(), nextFileCounts.end());
    std::sort(sortedResults.begin(), sortedResults.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    std::vector<std::string> predictions;
    for (const auto& entry : sortedResults) {
        predictions.push_back(entry.first);
    }

    return predictions;
}

bool MiniHSFSAI::detectAnomalousActivity(const std::string& filePath, const std::string& operation,
    const std::string& password) {
    std::lock_guard<std::mutex> lock(aiMutex);
    const int MAX_FAILED_AUTH = 5;
    const int MAX_RAPID_ACCESS = 10;
    const std::chrono::seconds TIME_WINDOW(10);

    auto now = std::chrono::system_clock::now();

    if (operation == "auth_failed") {
        failedAuthAttempts[filePath]++;

        if (failedAuthAttempts[filePath] > MAX_FAILED_AUTH) {
            return true;
        }
    }

    if (operation == "read" || operation == "write") {
        recentAccessCount[filePath]++;

        if (recentAccessCount[filePath] > MAX_RAPID_ACCESS) {
            return true;
        }

        if ((now - fileLastAccess[filePath]) > TIME_WINDOW) {
            recentAccessCount[filePath] = 0;
        }
    }

    return false;
}

void MiniHSFSAI::loadFileTypePatterns() {
    fileTypes["txt"] = "text";
    fileTypes["cpp"] = "code";
    fileTypes["h"] = "code";
    fileTypes["jpg"] = "image";
    fileTypes["png"] = "image";
    fileTypes["mp3"] = "audio";
    fileTypes["pdf"] = "document";
}

std::string MiniHSFSAI::detectFileType(const std::vector<char>& fileData) {
    if (isTextFile(fileData)) return "text";
    if (isImageFile(fileData)) return "image";
    if (isArchiveFile(fileData)) return "archive";
    if (isCodeFile(fileData)) return "code";
    return "binary";
}

bool MiniHSFSAI::isTextFile(const std::vector<char>& data) {
    if (data.empty()) return false;

    int printable = 0;
    for (char c : data) {
        if (isprint(c) || isspace(c)) printable++;
    }

    return (printable * 100 / data.size()) > 90;
}

bool MiniHSFSAI::isImageFile(const std::vector<char>& data) {
    if (data.size() > 8) {
        if (data[0] == (char)0xFF && data[1] == (char)0xD8 && data[2] == (char)0xFF) return true;
        if (data[0] == (char)0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') return true;
    }
    return false;
}

bool MiniHSFSAI::isArchiveFile(const std::vector<char>& data) {
    if (data.size() > 4) {
        if (data[0] == 'P' && data[1] == 'K' && data[2] == 0x03 && data[3] == 0x04) return true;
        if (data[0] == 'R' && data[1] == 'a' && data[2] == 'r' && data[3] == '!') return true;
    }
    return false;
}

bool MiniHSFSAI::isCodeFile(const std::vector<char>& data) {
    if (data.empty()) return false;

    std::string content(data.begin(), data.end());
    std::vector<std::string> keywords = {
        "#include", "public", "class", "function", "var", "let", "const",
        "if", "else", "for", "while", "return", "import", "from"
    };

    for (const auto& keyword : keywords) {
        if (content.find(keyword) != std::string::npos) return true;
    }

    return false;
}

void MiniHSFSAI::updateStorageStats(const std::string& filePath, size_t size, const std::string& fileType) {
    std::lock_guard<std::mutex> lock(aiMutex);
    storageUsage[fileType].first += size;
    storageUsage[fileType].second++;
}

void MiniHSFSAI::generateStorageReport() {
    std::lock_guard<std::mutex> lock(aiMutex);
    std::cout << "\n Storage Usage Report:\n";
    std::cout << "========================================\n";

    size_t totalSize = 0;
    int totalFiles = 0;

    for (const auto& entry : storageUsage) {
        totalSize += entry.second.first;
        totalFiles += entry.second.second;
    }

    for (const auto& entry : storageUsage) {
        double percentage = (totalSize > 0) ? (entry.second.first * 100.0 / totalSize) : 0.0;
        std::cout << "  " << entry.first << ": "
            << formatSize(entry.second.first) << " ("
            << entry.second.second << " files, "
            << std::fixed << std::setprecision(1) << percentage << "%)\n";
    }

    std::cout << "----------------------------------------\n";
    std::cout << "  Total: " << formatSize(totalSize)
        << " (" << totalFiles << " files)\n";
    std::cout << "========================================\n\n";
}

bool MiniHSFSAI::shouldCompressFile(const std::string& filePath, const std::string& fileType) {
    std::lock_guard<std::mutex> lock(aiMutex);
    auto now = std::chrono::system_clock::now();

    if (fileLastAccess.find(filePath) != fileLastAccess.end()) {
        auto lastAccess = fileLastAccess[filePath];
        auto daysSinceAccess = std::chrono::duration_cast<std::chrono::hours>(now - lastAccess).count() / 24;
        if (daysSinceAccess > 30) return true;
    }

    if (fileType == "log" || fileType == "temp") return true;
    return false;
}

std::vector<char> MiniHSFSAI::compressData(const std::vector<char>& data) {
    std::vector<char> compressed;

    if (!data.empty()) {
        char current = data[0];
        int count = 1;

        for (size_t i = 1; i < data.size(); ++i) {
            if (data[i] == current && count < 255) {
                count++;
            }
            else {
                compressed.push_back(current);
                compressed.push_back(static_cast<char>(count));
                current = data[i];
                count = 1;
            }
        }

        compressed.push_back(current);
        compressed.push_back(static_cast<char>(count));
    }

    return (compressed.size() < data.size()) ? compressed : data;
}

std::vector<char> MiniHSFSAI::decompressData(const std::vector<char>& compressed) {
    std::vector<char> data;

    for (size_t i = 0; i < compressed.size(); i += 2) {
        if (i + 1 >= compressed.size()) break;

        char value = compressed[i];
        int count = static_cast<unsigned char>(compressed[i + 1]);

        for (int j = 0; j < count; ++j) {
            data.push_back(value);
        }
    }

    return data;
}

std::string MiniHSFSAI::formatSize(size_t bytes) {
    const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    size_t unit = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024 && unit < 4) {
        size /= 1024;
        unit++;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << size << " " << units[unit];
    return oss.str();
}