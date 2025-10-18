#include "VirtualDisk.h"

// Constructor
VirtualDisk::VirtualDisk(int superblock, uint32_t blockSize)
    : systemBlock(0), diskSize(0), superBlockBlocks(superblock), blockSize(blockSize), isNewDisk(false) {
}

// Destructor
VirtualDisk::~VirtualDisk() {
    Close();
}

//Ensure everything you want is preserved on the virtual disk
void VirtualDisk::syncToDisk() {
    std::unique_lock<std::shared_mutex> lock(diskMutex);
    // call no-lock implementation inline (original code used platform-specific fsync)
    if (!ensureOpen_unlocked()) return;

#ifdef _WIN32
    
    if (!FlushFileBuffers(fileHandle)) {
        DWORD errorCode = GetLastError();
        Close();
        throw VirtualDiskException("Failed to sync disk (Windows error: " + std::to_string(errorCode) + ")");
    }

#elif __linux__
    if (fsync(fileDescriptor) != 0) {
        Close();
        throw VirtualDiskException("Failed to sync disk: " + std::string(strerror(errno)));
    }
#else
    // fallback to C++ stream flush if used
    diskFile.flush();
    if (!diskFile.good()) {
        Close();
        throw VirtualDiskException("Failed to sync disk stream");
    }

#endif
}

//Initialize Disk
void VirtualDisk::Initialize(const std::string& path, uint64_t diskSizeMB) {
    // lock once here and call no-lock implementations
    std::unique_lock<std::shared_mutex> lock(diskMutex);

    try {
        diskPath = path;
        diskSizeMB = (diskSizeMB < 1) ? VirtualDisk::defaultSizeDisk : diskSizeMB;
        diskSize = diskSizeMB;
        const uint64_t totalBlocks = (diskSizeMB * 1024 * 1024) / blockSize;
        if (totalBlocks == 0) {
            Close();
            throw VirtualDiskException("Invalid disk size - too small");
        }

        blockBitmap.clear();
        blockBitmap.resize(totalBlocks, false);

        systemBlock = static_cast<uint32_t>(std::min<uint64_t>(
            static_cast<uint64_t>(std::ceil((totalBlocks / 8.0) / blockSize) + extraSystemBlocks),
            totalBlocks
        ));

#ifdef _WIN32
        //HANDLE hFile
        fileHandle = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        bool exists = (fileHandle != INVALID_HANDLE_VALUE);
        if (exists) CloseHandle(fileHandle);
#elif __linux__
        fileDescriptor = open(path.c_str(), O_RDWR);
        bool exists = (fileDescriptor >= 0);
        if (exists) close(fileDescriptor);
#else
        // C++ standard implementation
        std::fstream testFile(path, std::ios::in | std::ios::binary);
        bool exists = testFile.is_open();
        if (exists) testFile.close();
#endif

        if (exists) {
            loadExistingDisk_nl(totalBlocks);
        }
        else {
            createNewDisk_nl(totalBlocks);
        }
    }
    catch (const std::exception& e) {
        std::cout << e.what() << std::endl;
        Close();
        throw VirtualDiskException(e.what());
    }
}

// Get Buffer Size from cache memory
size_t VirtualDisk::determineSmartBufferSize() {
    size_t bufferSize = 64 * 1024; // default: 64KB
    size_t availPhys = getAvailableMemory();

    if (availPhys == 0) {
        std::cerr << "Warning: Failed to get available memory, using default buffer size\n";
        return bufferSize;
    }

    // Step 1: Basic Thresholds (like the first function)
    if (availPhys > 8ULL * 1024 * 1024 * 1024)
        bufferSize = 2 * 1024 * 1024;  // 2MB
    else if (availPhys > 4ULL * 1024 * 1024 * 1024)
        bufferSize = 1 * 1024 * 1024;  // 1MB
    else if (availPhys > 1ULL * 1024 * 1024 * 1024)
        bufferSize = 512 * 1024;       // 512KB
    else
        bufferSize = 128 * 1024;       // 128KB

    // Step 2: Dynamic optimization using (0.05%)
    size_t percentBuffer = availPhys / 2000; // 0.05% of available memory
    if (percentBuffer > bufferSize) {
        bufferSize = percentBuffer;
    }

    // Step 3: Min/max limits
    const size_t minBufferSize = 16 * 1024;        // 16KB
    const size_t maxBufferSize = 8 * 1024 * 1024;  // 8MB
    bufferSize = (std::max)(minBufferSize, (std::min)(bufferSize, maxBufferSize));

    return bufferSize;
}

//Create New Disk with lock
void VirtualDisk::createNewDisk(uint64_t totalBlocks) {
    std::unique_lock<std::shared_mutex> lock(diskMutex);
    createNewDisk_nl(totalBlocks);
}

//Create New Disk without lock
void VirtualDisk::createNewDisk_nl(uint64_t totalBlocks) {
    const size_t BUFFER_SIZE = determineSmartBufferSize();
    std::vector<char> buffer(BUFFER_SIZE, 0);
    uint64_t remaining = totalBlocks * blockSize;

#ifdef _WIN32
    fileHandle = CreateFileA(diskPath.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (fileHandle == INVALID_HANDLE_VALUE) {
        Close();
        throw VirtualDiskException("Failed to create new disk file");
    }

    while (remaining > 0) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(BUFFER_SIZE, remaining));
        DWORD written;
        if (!WriteFile(fileHandle, buffer.data(), chunk, &written, NULL) || written != chunk) {
            CloseHandle(fileHandle);
            fileHandle = INVALID_HANDLE_VALUE;
            Close();
            throw VirtualDiskException("Failed to write to disk");
        }
        remaining -= chunk;
    }

    FlushFileBuffers(fileHandle);
#elif __linux__
    fileDescriptor = open(diskPath.c_str(), O_CREAT | O_RDWR | O_TRUNC | O_SYNC, 0644);
    if (fileDescriptor < 0) {
        Close();
        throw VirtualDiskException("Failed to create new disk file");
    }

    while (remaining > 0) {
        size_t chunk = std::min(BUFFER_SIZE, remaining);
        if (write(fileDescriptor, buffer.data(), chunk) != (ssize_t)chunk) {
            close(fileDescriptor);
            fileDescriptor = -1;
            Close();
            throw VirtualDiskException("Failed to write to disk");
        }
        remaining -= chunk;
    }

    fsync(fileDescriptor);
#else
    // C++ standard implementation
    diskFile.open(diskPath, std::ios::out | std::ios::binary);
    if (!diskFile.is_open()) {
        Close();
        throw VirtualDiskException("Failed to create new disk file");
    }

    while (remaining > 0) {
        size_t chunk = std::min(BUFFER_SIZE, remaining);
        diskFile.write(buffer.data(), chunk);
        if (!diskFile.good()) {
            diskFile.close();
            Close();
            throw VirtualDiskException("Failed to write to disk");
        }
        remaining -= chunk;
    }

    diskFile.flush();
    diskFile.close();
#endif

    std::fill_n(blockBitmap.begin(), systemBlock + superBlockBlocks, true);
    saveBitmap_nl(true);
    isNewDisk = true;
}

//Load Disk with lock
void VirtualDisk::loadExistingDisk(uint64_t expectedBlocks) {
    std::unique_lock<std::shared_mutex> lock(diskMutex);
    loadExistingDisk_nl(expectedBlocks);
}

//Load Disk without lock
void VirtualDisk::loadExistingDisk_nl(uint64_t expectedBlocks) {
#ifdef _WIN32
    fileHandle = CreateFileA(diskPath.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (!ensureOpen_unlocked()) {
        Close();
        throw VirtualDiskException("Failed to open disk file");
    }
#elif __linux__
    fileDescriptor = open(diskPath.c_str(), O_RDWR);
    if (fileDescriptor < 0) {
        Close();
        throw VirtualDiskException("Failed to open disk file");
    }
#else
    // C++ standard implementation
    diskFile.open(diskPath, std::ios::in | std::ios::out | std::ios::binary);
    if (!diskFile.is_open()) {
        Close();
        throw VirtualDiskException("Failed to open disk file");
    }
#endif

    loadBitmap_nl();
    isNewDisk = false;
}

//Use Blocks and Mark it
VirtualDisk::Extent VirtualDisk::allocateBlocks(uint32_t blocksNeeded) {
    std::unique_lock<std::shared_mutex> lock(diskMutex);

    if (freeBlocksCount_nl() < blocksNeeded) {
        throw DiskFullException();
    }

    if (blocksNeeded == 0) {
        throw std::invalid_argument("Block count cannot be zero");
    }

    int totalBlocks = static_cast<int>(blockBitmap.size());
    int start = -1;
    int count = 0;

    for (int currentBlock = systemBlock; currentBlock < totalBlocks; ++currentBlock) {
        if (!blockBitmap[currentBlock]) {
            if (start == -1)
                start = currentBlock;
            count++;

            if (count == blocksNeeded) {
                for (int j = start; j < start + static_cast<int>(blocksNeeded); ++j)
                    blockBitmap[j] = true;

                return Extent(start, blocksNeeded);
            }
        }
        else {
            start = -1;
            count = 0;
        }
    }

    throw DiskFullException();
}

//Get Status Bit Map (Meta Data)
std::vector<bool> VirtualDisk::getBitmap() {
    std::shared_lock<std::shared_mutex> lock(diskMutex);
    return blockBitmap;
}

//Set Status Bit Map (Meta Data)
void VirtualDisk::setBitmap(int index, bool state) {
    std::unique_lock<std::shared_mutex> lock(diskMutex);
    if (index >= 0 && static_cast<size_t>(index) < blockBitmap.size()) {
        blockBitmap[index] = state;
    }
}

//Free Blocks Was Used
void VirtualDisk::freeBlocks(const Extent& extent) {
    std::unique_lock<std::shared_mutex> lock(diskMutex);
    if (!ensureOpen_unlocked()) return;

    if (extent.startBlock + extent.blockCount > blockBitmap.size() && extent.startBlock != -1) {
        throw std::out_of_range("Extent exceeds disk bounds");
    }
    if (extent.startBlock != -1)
        std::fill_n(blockBitmap.begin() + extent.startBlock, extent.blockCount, false);

    saveBitmap_nl(true); // Force flush after freeing blocks
}

//Get Total Blocks Free Count with lock
uint64_t VirtualDisk::freeBlocksCount() {
    std::shared_lock<std::shared_mutex> lock(diskMutex);
    return freeBlocksCount_nl();
}

//Get Total Blocks Free Count without lock
uint64_t VirtualDisk::freeBlocksCount_nl() const {
    return static_cast<uint64_t>(std::count(blockBitmap.begin(), blockBitmap.end(), false));
}

// Write Data in Disk
bool VirtualDisk::writeData(const std::vector<char>& data, const Extent& extent, const std::string& password, bool flushImmediately) {
    std::unique_lock<std::shared_mutex> lock(diskMutex);

    if (!ensureOpen_unlocked()) return false;

    std::vector<uint8_t> finalData;

    if (password.empty()) {
        finalData.assign(data.begin(), data.end());
    }
    else {
        uint32_t originalSize = static_cast<uint32_t>(data.size());
        std::vector<uint8_t> fullData(sizeof(uint32_t) + data.size());
        std::memcpy(fullData.data(), &originalSize, sizeof(uint32_t));
        std::memcpy(fullData.data() + sizeof(uint32_t), data.data(), data.size());

        CryptoUtils crypto;  // constructor

        auto encrypted = crypto.EncryptWithSalt(fullData, password);

        uint32_t encryptedSize = static_cast<uint32_t>(encrypted.size());
        finalData.resize(sizeof(uint32_t) + encryptedSize);
        std::memcpy(finalData.data(), &encryptedSize, sizeof(uint32_t));
        std::memcpy(finalData.data() + sizeof(uint32_t), encrypted.data(), encryptedSize);
    }

    size_t totalBlockSize = extent.blockCount * blockSize;
    if (finalData.size() > totalBlockSize) return false;
    finalData.resize(totalBlockSize, 0);

#ifdef _WIN32
    LARGE_INTEGER offset;
    offset.QuadPart = static_cast<LONGLONG>(extent.startBlock) * blockSize;
    SetFilePointerEx(fileHandle, offset, NULL, FILE_BEGIN);
    DWORD written;
    BOOL result = WriteFile(fileHandle, finalData.data(), (DWORD)finalData.size(), &written, NULL);
    if (flushImmediately) FlushFileBuffers(fileHandle);
    return result && written == finalData.size();
#elif __linux__
    off_t offset = static_cast<off_t>(extent.startBlock) * blockSize;
    if (lseek(fileDescriptor, offset, SEEK_SET) == -1) {
        return false;
    }
    ssize_t written = write(fileDescriptor, finalData.data(), finalData.size());
    if (flushImmediately) fsync(fileDescriptor);
    return written == (ssize_t)finalData.size();
#else
    // C++ standard implementation
    diskFile.seekp(extent.startBlock * blockSize);
    diskFile.write(reinterpret_cast<const char*>(finalData.data()), finalData.size());
    if (flushImmediately) {
        diskFile.flush();
    }
    return diskFile.good();
#endif
}

// Read Data From Disk
std::vector<char> VirtualDisk::readData(const Extent& extent, const std::string& password) {
    std::shared_lock<std::shared_mutex> lock(diskMutex);

    if (!ensureOpen_unlocked()) return {};
    std::vector<char> buffer(extent.blockCount * blockSize);

#ifdef _WIN32
    LARGE_INTEGER offset;
    offset.QuadPart = static_cast<LONGLONG>(extent.startBlock) * blockSize;
    SetFilePointerEx(fileHandle, offset, NULL, FILE_BEGIN);
    DWORD bytesRead;
    BOOL result = ReadFile(fileHandle, buffer.data(), (DWORD)buffer.size(), &bytesRead, NULL);
    if (!result || bytesRead == 0) return {};
#elif __linux__
    off_t offset = static_cast<off_t>(extent.startBlock) * blockSize;
    if (lseek(fileDescriptor, offset, SEEK_SET) == -1) {
        return {};
    }
    ssize_t bytesRead = read(fileDescriptor, buffer.data(), buffer.size());
    if (bytesRead <= 0) return {};
#else
    // C++ standard implementation
    diskFile.seekg(extent.startBlock * blockSize);
    diskFile.read(buffer.data(), buffer.size());
    if (diskFile.gcount() <= 0) {
        return {};
    }
#endif

    if (password.empty()) {
        size_t actualSize = buffer.size();
        while (actualSize > 0 && buffer[actualSize - 1] == 0) {
            --actualSize;
        }
        buffer.resize(actualSize);
        return buffer;
    }

    if (buffer.size() < sizeof(uint32_t)) return {};
    uint32_t encryptedSize;
    std::memcpy(&encryptedSize, buffer.data(), sizeof(uint32_t));
    if (encryptedSize == 0 || encryptedSize + sizeof(uint32_t) > buffer.size()) return {};

    std::vector<uint8_t> encryptedBytes(buffer.begin() + sizeof(uint32_t), buffer.begin() + sizeof(uint32_t) + encryptedSize);
    std::vector<uint8_t> decryptedBytes;
    try {
        CryptoUtils crypto;  // constructor

        decryptedBytes = crypto.DecryptWithSalt(encryptedBytes, password);
    }
    catch (...) {
        return {};
    }

    if (decryptedBytes.size() < sizeof(uint32_t)) return {};
    uint32_t originalSize;
    std::memcpy(&originalSize, decryptedBytes.data(), sizeof(uint32_t));
    if (originalSize + sizeof(uint32_t) > decryptedBytes.size()) return {};

    return std::vector<char>(decryptedBytes.begin() + sizeof(uint32_t), decryptedBytes.begin() + sizeof(uint32_t) + originalSize);
}

// Close Disk
void VirtualDisk::Close() {
    std::unique_lock<std::shared_mutex> lock(diskMutex);

#ifdef _WIN32
    if (fileHandle != INVALID_HANDLE_VALUE) {
        try {
            saveBitmap_nl(true);
            FlushFileBuffers(fileHandle);
            CloseHandle(fileHandle);
            fileHandle = INVALID_HANDLE_VALUE;
        }
        catch (...) {
            if (fileHandle != INVALID_HANDLE_VALUE) {
                CloseHandle(fileHandle);
                fileHandle = INVALID_HANDLE_VALUE;
            }
            throw;
        }
    }
#elif __linux__
    if (fileDescriptor >= 0) {
        try {
            saveBitmap_nl(true);
            fsync(fileDescriptor);
            close(fileDescriptor);
            fileDescriptor = -1;
        }
        catch (...) {
            if (fileDescriptor >= 0) {
                close(fileDescriptor);
                fileDescriptor = -1;
            }
            throw;
        }
    }
#else
    if (diskFile.is_open()) {
        try {
            saveBitmap_nl(true);
            diskFile.flush();
            diskFile.close();
        }
        catch (...) {
            if (diskFile.is_open()) {
                diskFile.close();
            }
            throw;
        }
    }
#endif
}

// Print Bit map
void VirtualDisk::printBitmap() {
    std::shared_lock<std::shared_mutex> lock(diskMutex);

    ensureOpen_unlocked();

    const size_t blocksPerRow = 32;
    const size_t totalBlocks = blockBitmap.size();
    const size_t usedBlocks = totalBlocks - freeBlocksCount_nl();
    const float usedPercent = (totalBlocks == 0) ? 0.0f : (static_cast<float>(usedBlocks) / totalBlocks) * 100.0f;

    std::cout << "\n";
    std::cout << "=========================================================\n";
    std::cout << "|              VIRTUAL DISK BITMAP                     |\n";
    std::cout << "=========================================================\n";
    std::cout << "| Total Blocks: " << std::setw(10) << totalBlocks
        << " | Free: " << std::setw(5) << freeBlocksCount_nl()
        << " | Used: " << std::setw(6) << usedBlocks << " |\n";
    std::cout << "| Usage: " << std::setw(4) << std::fixed << std::setprecision(1)
        << usedPercent << "%"
        << std::string(42, ' ') << "|\n";
    std::cout << "| Legend: [ ] = Free, [X] = Used"
        << std::string(24, ' ') << "|\n";
    std::cout << "=========================================================\n\n";

    std::cout << "Block  +";
    for (size_t col = 0; col < blocksPerRow; ++col) {
        std::cout << "--";
    }
    std::cout << "+\n";

    std::cout << "       |";
    for (size_t col = 0; col < blocksPerRow; ++col) {
        std::cout << std::setw(2) << col % 10;
    }
    std::cout << "|\n";
    std::cout << "-------+";
    for (size_t col = 0; col < blocksPerRow; ++col) {
        std::cout << "--";
    }
    std::cout << "+\n";

    for (size_t i = 0; i < totalBlocks; i++) {
        if (i % blocksPerRow == 0) {
            if (i != 0) {
                std::cout << "|\n";
            }
            std::cout << std::setw(6) << i << " |";
        }
        std::cout << (blockBitmap[i] ? "X " : "  ");
    }

    std::cout << "|\n";
    std::cout << "-------+";
    for (size_t col = 0; col < blocksPerRow; ++col) {
        std::cout << "--";
    }
    std::cout << "+\n";

    std::cout << "\nUsage: [";
    const int barWidth = 50;
    int usedWidth = static_cast<int>((usedPercent / 100.0f) * barWidth);
    for (int i = 0; i < barWidth; i++) {
        std::cout << (i < usedWidth ? '#' : '-');
    }
    std::cout << "] " << std::fixed << std::setprecision(1) << usedPercent << "%\n";

    std::cout << "\n";
    std::cout << "====[ SUMMARY ]===================================\n";
    std::cout << "| Free Space: " << std::setw(10) << freeBlocksCount_nl()
        << " blocks (" << std::setw(6)
        << std::fixed << std::setprecision(1)
        << (100.0f - usedPercent) << "% free)   |\n";
    std::cout << "| Used Space: " << std::setw(10) << usedBlocks
        << " blocks (" << std::setw(6)
        << std::fixed << std::setprecision(1)
        << usedPercent << "% used)   |\n";
    std::cout << "=================================================\n\n";
}

//Save BitMap Status in Disk with lock
void VirtualDisk::saveBitmap(bool forceFlush) {
    std::unique_lock<std::shared_mutex> lock(diskMutex);
    saveBitmap_nl(forceFlush);
}

//Save BitMap Status in Disk without lock
void VirtualDisk::saveBitmap_nl(bool forceFlush) {
    if (!ensureOpen_unlocked()) return;

    size_t bitmapSize = blockBitmap.size();
    size_t byteSize = (bitmapSize + 7) / 8;
    size_t totalAvailableBytes = systemBlock * blockSize;

    if (byteSize > totalAvailableBytes)
        throw std::runtime_error("Error: System Blocks too small to hold bitmap.\n");

    std::vector<char> bitmap(byteSize, 0);
    for (size_t i = 0; i < bitmapSize; ++i) {
        if (blockBitmap[i]) {
            bitmap[i / 8] |= (1 << (i % 8));
        }
    }

    size_t bytesRemaining = byteSize;
    size_t offset = 0;

    for (size_t block = 1; block < systemBlock && bytesRemaining > 0; ++block) {
        size_t chunkSize = (std::min)(static_cast<size_t>(blockSize), bytesRemaining);
        size_t blockOffset = block * blockSize;

#ifdef _WIN32
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(blockOffset);
        SetFilePointerEx(fileHandle, li, NULL, FILE_BEGIN);
        DWORD written;
        WriteFile(fileHandle, &bitmap[offset], (DWORD)chunkSize, &written, NULL);
        if (chunkSize < blockSize) {
            std::vector<char> padding(blockSize - chunkSize, 0);
            WriteFile(fileHandle, padding.data(), (DWORD)padding.size(), &written, NULL);
        }
#elif __linux__
        lseek(fileDescriptor, blockOffset, SEEK_SET);
        write(fileDescriptor, &bitmap[offset], chunkSize);
        if (chunkSize < blockSize) {
            std::vector<char> padding(blockSize - chunkSize, 0);
            write(fileDescriptor, padding.data(), padding.size());
        }
#else

        // C++ standard implementation
        diskFile.seekp(blockOffset);
        diskFile.write(&bitmap[offset], chunkSize);
        if (chunkSize < blockSize) {
            std::vector<char> padding(blockSize - chunkSize, 0);
            diskFile.write(padding.data(), padding.size());
        }
#endif

        offset += chunkSize;
        bytesRemaining -= chunkSize;
    }

    if (forceFlush) {
#ifdef _WIN32
        FlushFileBuffers(fileHandle);
#elif __linux__
        fsync(fileDescriptor);
#else
        diskFile.flush();
#endif
    }
}

//Load BitMap Status From Disk with lock
void VirtualDisk::loadBitmap() {
    std::unique_lock<std::shared_mutex> lock(diskMutex);
    loadBitmap_nl();
}

//Load BitMap Status From Disk without lock
void VirtualDisk::loadBitmap_nl() {
    if (!ensureOpen_unlocked()) return;

    size_t bitmapSize = blockBitmap.size();
    size_t byteSize = (bitmapSize + 7) / 8;
    size_t totalAvailableBytes = systemBlock * blockSize;

    if (byteSize > totalAvailableBytes)
        throw std::runtime_error("Error: System Blocks too small to load bitmap.\n");

    std::vector<char> bitmap(byteSize, 0);
    size_t bytesRemaining = byteSize;
    size_t offset = 0;

    for (size_t block = 1; block < systemBlock && bytesRemaining > 0; ++block) {
        size_t chunkSize = (std::min)(static_cast<size_t>(blockSize), bytesRemaining);
        size_t blockOffset = block * blockSize;

#ifdef _WIN32
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(blockOffset);
        SetFilePointerEx(fileHandle, li, NULL, FILE_BEGIN);
        DWORD readBytes;
        BOOL success = ReadFile(fileHandle, &bitmap[offset], (DWORD)chunkSize, &readBytes, NULL);
        if (!success || readBytes != chunkSize) {
            std::cerr << "Failed to read bitmap block at offset " << blockOffset << "\n";
            return;
        }
#elif __linux__
        lseek(fileDescriptor, blockOffset, SEEK_SET);
        ssize_t readBytes = read(fileDescriptor, &bitmap[offset], chunkSize);
        if (readBytes != (ssize_t)chunkSize) {
            std::cerr << "Failed to read bitmap block at offset " << blockOffset << "\n";
            return;
        }
#else
        // C++ standard implementation
        diskFile.seekg(blockOffset);
        diskFile.read(&bitmap[offset], chunkSize);
        if (diskFile.gcount() != (std::streamsize)chunkSize) {
            std::cerr << "Failed to read bitmap block at offset " << blockOffset << "\n";
            return;
        }
#endif

        offset += chunkSize;
        bytesRemaining -= chunkSize;
    }

    for (size_t i = 0; i < bitmapSize; ++i) {
        blockBitmap[i] = (bitmap[i / 8] >> (i % 8)) & 1;
    }
}

//Get Free Blocks
uint32_t VirtualDisk::findContiguousBlocks(uint32_t count) {
    if (count == 0) return UINT32_MAX;

    uint32_t currentStart = UINT32_MAX;
    uint32_t currentLength = 0;

    for (uint32_t i = 0; i < blockBitmap.size(); ++i) {
        if (!blockBitmap[i]) {
            if (currentLength == 0) {
                currentStart = i;
            }

            ++currentLength;

            if (currentLength == count) {
                return currentStart;
            }
        }
        else {
            currentLength = 0;
        }
    }

    return UINT32_MAX;
}

//Check if Disk is open or not with lock
bool VirtualDisk::ensureOpen() {
    std::shared_lock<std::shared_mutex> lock(diskMutex);
    return ensureOpen_unlocked();
}

//Check if Disk is open or not without lock
bool VirtualDisk::ensureOpen_unlocked() const {
#ifdef _WIN32
    if (fileHandle == INVALID_HANDLE_VALUE) {
        throw VirtualDiskException("Disk is not open (Windows fileHandle invalid)");
    }
    return fileHandle != INVALID_HANDLE_VALUE;
#elif __linux__
    if (fileDescriptor < 0) {
        throw VirtualDiskException("Disk is not open (Linux fileDescriptor invalid)");
    }
    return fileDescriptor >= 0;
#else
    if (!diskFile.is_open()) {
        throw VirtualDiskException("Disk is not open (C++ file stream not open)");
    }
    return diskFile.is_open();
#endif
}

// Set Console Color
void VirtualDisk::SetConsoleColor(ConsoleColor color) {
#ifdef _WIN32
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    WORD attributes = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;

    switch (color) {
    case ConsoleColor::Red:     attributes = FOREGROUND_RED | FOREGROUND_INTENSITY; break;
    case ConsoleColor::Green:   attributes = FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
    case ConsoleColor::Blue:    attributes = FOREGROUND_BLUE | FOREGROUND_INTENSITY; break;
    case ConsoleColor::Yellow:  attributes = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
    case ConsoleColor::Cyan:    attributes = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY; break;
    case ConsoleColor::Magenta: attributes = FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY; break;
    case ConsoleColor::White:   attributes = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY; break;
    case ConsoleColor::Gray:    attributes = FOREGROUND_INTENSITY; break;
    default: break;
    }

    SetConsoleTextAttribute(hConsole, attributes);
#else
    switch (color) {
    case ConsoleColor::Red:     std::cout << "\033[31m"; break;
    case ConsoleColor::Green:   std::cout << "\033[32m"; break;
    case ConsoleColor::Yellow:  std::cout << "\033[33m"; break;
    case ConsoleColor::Blue:    std::cout << "\033[34m"; break;
    case ConsoleColor::Magenta: std::cout << "\033[35m"; break;
    case ConsoleColor::Cyan:    std::cout << "\033[36m"; break;
    case ConsoleColor::White:   std::cout << "\033[37m"; break;
    case ConsoleColor::Gray:    std::cout << "\033[90m"; break;
    case ConsoleColor::Default: std::cout << "\033[0m"; break;
    }
#endif
}

// Get Available Memory (Cach Memory)
size_t VirtualDisk::getAvailableMemory() {
#ifdef _WIN32
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    GlobalMemoryStatusEx(&status);
    return status.ullAvailPhys;
#elif __linux__
    struct sysinfo info;
    sysinfo(&info);
    return static_cast<size_t>(info.freeram) * static_cast<size_t>(info.mem_unit);
#else
    return 0; // Unsupported systems
#endif
}