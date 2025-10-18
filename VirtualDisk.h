#ifndef VIRTUALDISK_H
#define VIRTUALDISK_H

#include <vector>
#include <string>
#include <stdexcept>
#include <iomanip>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <algorithm>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#elif __linux__
#include <unistd.h>
#include <sys/sysinfo.h>
#include <sys/mman.h>

#else
#include <fstream>
#endif

#include "CryptoUtils.h"

class VirtualDisk {
public:

    class VirtualDiskException : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    class DiskFullException : public VirtualDiskException {
    public:
        DiskFullException() : VirtualDiskException("Insufficient disk space") {}
    };

    class CorruptedDiskException : public VirtualDiskException {
    public:
        CorruptedDiskException() : VirtualDiskException("Disk corruption detected") {}
    };

    struct Extent {
        uint32_t startBlock;
        uint32_t blockCount;
        Extent(uint32_t start = 0, uint32_t count = 0)
            : startBlock(start), blockCount(count) {}
        uint32_t size(uint32_t blockSize) const {
            return blockCount * blockSize;
        }
    };

    uint32_t blockSize;

    static constexpr uint32_t extraSystemBlocks = 2;
    static const uint32_t toleranceBlocks = 4;
    static const uint32_t defaultSizeDisk = 50;

    int superBlockBlocks;
    size_t diskSize;

    VirtualDisk(int superBlock, uint32_t BlockSize);
    ~VirtualDisk();

    // Public API
    void Initialize(const std::string& path, uint64_t diskSizeMB = defaultSizeDisk);
    bool IsNew() { return isNewDisk; }
    void Close();

    uint32_t getSystemBlocks() const { return systemBlock; }
    Extent allocateBlocks(uint32_t blocksNeeded);
    void freeBlocks(const Extent& extent);
    size_t totalBlocks() { std::shared_lock<std::shared_mutex> g(diskMutex); return blockBitmap.size(); }
    uint64_t freeBlocksCount();
    void createNewDisk(uint64_t totalBlocks);
    void loadExistingDisk(uint64_t expectedBlocks);
    std::vector<bool> getBitmap();
    void setBitmap(int index, bool state);

    bool writeData(const std::vector<char>& data, const Extent& extent, const std::string& password = "", bool flushImmediately = false);
    std::vector<char> readData(const Extent& extent, const std::string& password = "");

    void printBitmap();

    // Helpers
    bool ensureOpen();
    void syncToDisk();
    size_t getAvailableMemory();

    enum ConsoleColor {
        Default,
        Red,
        Green,
        Blue,
        Yellow,
        Cyan,
        Magenta,
        White,
        Gray
    };

    void SetConsoleColor(ConsoleColor  color); // keep int to avoid header dependency


private:

    // platform handles
#ifdef _WIN32
    HANDLE fileHandle = INVALID_HANDLE_VALUE;
#elif __linux__
    int fileDescriptor = -1;
#else
    std::fstream diskFile;
#endif

    bool isNewDisk;
    uint32_t systemBlock;
    std::string diskPath;
    std::vector<bool> blockBitmap;

    // mutex
    mutable std::shared_mutex diskMutex;

    // original implementations
    void saveBitmap(bool forceFlush = false);
    void loadBitmap();
    uint32_t findContiguousBlocks(uint32_t count);

    // helpers
    size_t determineSmartBufferSize();

    void createNewDisk_nl(uint64_t totalBlocks);
    void loadExistingDisk_nl(uint64_t expectedBlocks);
    void saveBitmap_nl(bool forceFlush = false);
    void loadBitmap_nl();
    uint64_t freeBlocksCount_nl() const;
    bool ensureOpen_unlocked() const;

};

#endif // VIRTUALDISK_H
