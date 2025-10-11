#include "MiniHSFS.h"

///////////////////////////////Start System

MiniHSFS::MiniHSFS(const std::string& path, uint32_t sizeMB, uint32_t blockSize)
    :disk(std::max<int>(1, static_cast<int>(std::ceil((double)sizeof(SuperblockInfo) / blockSize))), blockSize),
    mounted(false),
    initialized(false),
    autoSyncRunning(false), btreeBlocks(0), btreeStartIndex(0), dataStartIndex(0), inodeBlocks(0)
    , inodeCount(0), dynamicInodes(true), nextInodeIndex(1) {

    // Format the virtual disk
    disk.Initialize(path, sizeMB);

    btreeOrder = static_cast<int>(calculateBTreeOrder());

    // Root Formatting
    rootNode = BTreeNode(btreeOrder, true);

    dataStartBlock = inodeBlocks; // بعد بلوكات الـ inodes مباشرة
    freeBlocks = disk.freeBlocksCount() - inodeBlocks;
}

VirtualDisk& MiniHSFS::Disk()
{
    return disk;
}

MiniHSFS::~MiniHSFS()
{
    Unmount();
}

void MiniHSFS::Initialize() {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);
    if (initialized) {
        throw std::runtime_error("Filesystem already initialized");
    }

    if (!disk.ensureOpen()) {
        throw std::runtime_error("Virtual disk not open");
    }
    superBlockBlocks = std::max<int>(1, static_cast<int>(std::ceil((double)sizeof(SuperblockInfo) / disk.blockSize)));
    inodeCount = calculateInodeCount();
    inodeBlocks = calculateInodeBlocks();
    btreeBlocks = static_cast<int>(calculateBTreeBlocks());
    btreeStartIndex = static_cast<int>(inodeBlocks) + disk.getSystemBlocks() + superBlockBlocks;
    dataStartIndex = btreeStartIndex + btreeBlocks;

    try {
        if (disk.IsNew()) {
            InitializeSuperblock();

            InitializeBTree();
            disk.allocateBlocks(static_cast<uint32_t>(btreeBlocks ? btreeBlocks : 1));

            InitializeInodeTable();
            disk.allocateBlocks(static_cast<uint32_t>(inodeBlocks ? inodeBlocks : 1));

            //Create root directory
            inodeTable[0].isUsed = true;
            inodeTable[0].isDirectory = true;
            inodeTable[0].creationTime = time(nullptr);
            inodeTable[0].modificationTime = inodeTable[0].creationTime;

            SaveInodeTable();
            SaveBTree();
        }
        initialized = true;
    }
    catch (...) {
        //FlushDirtyInodes();
        SaveBTree();
        std::cerr << "!!Memory pressure during initialization. Flushing all caches.\n";

        btreeCache.clear();
        btreeLruMap.clear();
        btreeLruList.clear();
        inodeTable.clear();
        inodeLruMap.clear();
        inodeLruList.clear();
        throw;
    }
}

void MiniHSFS::Mount(size_t inodePercentage, size_t btreePercentage, size_t inodeSize) {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    this->inodeSize = inodeSize;

    if (inodePercentage != 0 && btreePercentage != 0)
        calculatePercentage(inodePercentage, btreePercentage);
    else if (inodePercentage != 0 && btreePercentage == 0)
        calculatePercentage(inodePercentage = inodePercentage);
    else if (inodePercentage == 0 && btreePercentage != 0)
        calculatePercentage(btreePercentage = btreePercentage);
    else
        calculatePercentage();

    if (mounted) {
        throw std::runtime_error("Filesystem already mounted");
    }

    if (!initialized) {
        Initialize();
    }
    try {
        LoadInodeTable();
        ValidateInodeTable(); // ← إضافة هذا السطر
        LoadBTree();
        //// Verify root directory
        if (!inodeTable[0].isUsed || !inodeTable[0].isDirectory) {
            throw std::runtime_error("Root directory corruption detected");
        }

        mounted = true;
    }
    catch (const std::exception& e) {
        // Clean up if mount fails
        inodeTable.clear();
        inodeTable.resize(inodeCount);
        rootNode = BTreeNode(true);
        std::cout << e.what() << std::endl;
        throw e;
    }
}


//mf 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 


//---------------------------------------------------------//
//---------------------------------------------------------//

void MiniHSFS::InitializeInodeTable() {
    // لو عندك قيم افتراضية في السوبر بلوك، خليك متسق معاها
    if (inodeSize == 0) {
        inodeSize = std::max<size_t>(
            128, // حد أدنى معقول
            sizeof(size_t) + sizeof(int) * 2 + 1 /*flags*/ + sizeof(time_t) * 3 + 16 /*محتوى*/ + sizeof(uint32_t) + 8
        );
    }

    if (inodeCount == 0) inodeCount = 64; // بداية معقولة
    inodeBlocks = static_cast<size_t>(BlocksForInodes(inodeCount));

    inodeTable.clear();
    inodeTable.resize(inodeCount);
    inodeBitmap.assign(inodeCount, false);

    // الجذر
    inodeTable[0].isUsed = true;
    inodeTable[0].isDirectory = true;
    time_t now = time(nullptr);
    inodeTable[0].creationTime = inodeTable[0].modificationTime = inodeTable[0].lastAccessed = now;
    inodeBitmap[0] = true;
}

// تخصيص عقدة جديدة
int MiniHSFS::AllocateInode(bool isDirectory) {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    std::cout << "Allocating new " << (isDirectory ? "directory" : "file") << " inode..." << std::endl;

    // المحاولة الأولى: استخدام عقدة حرة موجودة
    if (!freeInodesList.empty()) {
        int idx = freeInodesList.front();
        freeInodesList.erase(freeInodesList.begin());
        std::cout << "Using free inode from list: " << idx << std::endl;
        return InitializeInode(idx, isDirectory);
    }

    // المحاولة الثانية: البحث عن عقدة غير مستخدمة
    for (size_t i = 1; i < inodeTable.size(); ++i) {
        if (!inodeTable[i].isUsed) {
            std::cout << "Using unused inode: " << i << std::endl;
            return InitializeInode(static_cast<int>(i), isDirectory);
        }
    }

    // المحاولة الثالثة: التوسع باستخدام الـ Defragmentation
    std::cout << "No free inodes available, trying to expand inode table..." << std::endl;

    if (!DefragmentAndExtendInodes(10)) { // إضافة 10 عقد مرة واحدة
        throw std::runtime_error("Cannot allocate inode - no space even after defrag");
    }

    // البحث مرة أخرى بعد التوسعة
    for (size_t i = 1; i < inodeTable.size(); ++i) {
        if (!inodeTable[i].isUsed) {
            std::cout << "Using new inode after expansion: " << i << std::endl;
            return InitializeInode(static_cast<int>(i), isDirectory);
        }
    }

    throw std::runtime_error("Failed to allocate inode after expansion");
}

// تحرير عقدة
void MiniHSFS::FreeInode(int index) {
    if (index <= 0 || static_cast<size_t>(index) >= inodeTable.size()) return;

    // تحرير البلوكات أولاً
    if (inodeTable[index].isUsed) {
        if (!inodeTable[index].isDirectory) {
            FreeFileBlocks(inodeTable[index]);
        }
        else {
            inodeTable[index].entries.clear();
        }
    }

    // إعادة التعيين إلى قيم افتراضية آمنة
    inodeTable[index] = Inode();
    inodeTable[index].isUsed = false;

    if (inodeBitmap.size() > static_cast<size_t>(index)) {
        inodeBitmap[index] = false;
    }

    RebuildFreeInodesList();
    UpdateSuperblockForDynamicInodes();
    SaveInodeToDisk(index); // حفظ التغييرات على القرص
}

// توسيع مساحة العقد
void MiniHSFS::ExpandInodeArea(size_t additionalSize) {
    size_t newTotalSize = inodeAreaSize + additionalSize;
    size_t newBlocksNeeded = (newTotalSize + disk.blockSize - 1) / disk.blockSize - inodeBlocks;

    if (newBlocksNeeded > 0) {
        // تخصيص كتل جديدة على القرص
        VirtualDisk::Extent extent(
            disk.getSystemBlocks() + superBlockBlocks + inodeBlocks,
            newBlocksNeeded
        );
        disk.allocateBlocks(extent.blockCount);

        // تهيئة الكتل الجديدة
        std::vector<char> zeroBlock(disk.blockSize, 0);
        for (size_t i = 0; i < newBlocksNeeded; ++i) {
            disk.writeData(zeroBlock,
                VirtualDisk::Extent(extent.startBlock + i, 1),
                "", true
            );
        }

        inodeBlocks += newBlocksNeeded;
        inodeAreaSize = inodeBlocks * disk.blockSize;
    }

    // إذا كانت المساحة المطلوبة أقل من المساحة المتاحة
    inodeAreaSize = (std::max)(inodeAreaSize, newTotalSize);

    // إعادة التنظيم التلقائي إذا كان مفعلاً
    if (autoDefrag && inodeTable.size() > 100) {
        AutoDefragmentInodes();
    }
}

// تمديد جدول العقد
//void MiniHSFS::ExtendInodeTable(size_t additionalInodes) {
//    std::lock_guard<std::recursive_mutex> lock(fsMutex);
//
//    size_t oldSize = inodeTable.size();
//    size_t newSize = oldSize + additionalInodes;
//
//    if (newSize > maxPossibleInodes) {
//        throw std::runtime_error("Cannot extend inode table beyond maximum limit");
//    }
//
//    inodeTable.resize(newSize);
//    inodeBitmap.resize(newSize, false);
//    inodeCount = newSize;
//
//    // تحديث عدد البلوكات
//    size_t requiredBlocks = (inodeCount * inodeSize + disk.blockSize - 1) / disk.blockSize;
//    if (requiredBlocks > inodeBlocks) {
//        disk.allocateBlocks(requiredBlocks - inodeBlocks);
//        inodeBlocks = requiredBlocks;
//    }
//
//    inodeAreaSize = inodeBlocks * disk.blockSize;
//    RebuildFreeInodesList();
//    UpdateSuperblockForDynamicInodes();
//    SaveInodeTable();
//}

void MiniHSFS::ExtendInodeTable(size_t additionalInodes) {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    size_t oldSize = inodeTable.size();
    size_t newSize = oldSize + additionalInodes;

    if (newSize > maxPossibleInodes) {
        throw std::runtime_error("Cannot extend inode table beyond maximum limit");
    }

    // احتساب البلوكات المطلوبة
    size_t requiredBlocks = (newSize * inodeSize + disk.blockSize - 1) / disk.blockSize;

    // إذا كانت البلوكات الحالية غير كافية
    if (requiredBlocks > inodeBlocks) {
        // احجز مساحة إضافية على القرص
        size_t blocksToAdd = requiredBlocks - inodeBlocks;
        VirtualDisk::Extent extent(disk.getSystemBlocks() + superBlockBlocks + inodeBlocks, blocksToAdd);
        disk.allocateBlocks(extent.blockCount);

        // اكتب صفر في البلوكات الجديدة
        std::vector<char> zeroBlock(disk.blockSize, 0);
        for (size_t i = 0; i < blocksToAdd; ++i) {
            disk.writeData(zeroBlock,
                VirtualDisk::Extent(extent.startBlock + i, 1),
                "", true);
        }

        inodeBlocks = requiredBlocks;
    }

    // توسيع الهياكل في الذاكرة
    inodeTable.resize(newSize);
    inodeBitmap.resize(newSize, false);
    inodeCount = newSize;

    // تحديث السوبربلوك
    UpdateSuperblockForDynamicInodes();

    // إعادة بناء قائمة العقد الحرة
    RebuildFreeInodesList();

    // حفظ التغييرات
    SaveInodeTable();
}


bool MiniHSFS::DefragmentAndExtendInodes(size_t extraInodes) {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);
    std::cout << "Defragmenting to add " << extraInodes << " inodes..." << std::endl;

    // 1. حساب المتطلبات
    size_t currentInodes = inodeTable.size();
    size_t newTotalInodes = currentInodes + extraInodes;
    size_t currentBlocks = inodeBlocks;
    size_t neededBlocks = BlocksForInodes(newTotalInodes);

    std::cout << "Current: " << currentInodes << " inodes, " << currentBlocks << " blocks" << std::endl;
    std::cout << "Needed: " << newTotalInodes << " inodes, " << neededBlocks << " blocks" << std::endl;

    if (neededBlocks <= currentBlocks) {
        std::cout << "No additional blocks needed, just extending in-memory table" << std::endl;
        // فقط توسيع الجدول في الذاكرة
        inodeTable.resize(newTotalInodes);
        inodeBitmap.resize(newTotalInodes, false);
        inodeCount = newTotalInodes;

        // تحديث السوبر بلوك
        UpdateSuperblockForDynamicInodes();
        // 👇 أضِف هذا السطر لضمان ثبات الصورة على القرص قبل أي استخدام:
        SaveInodeTable();

        return true;
    }

    size_t additionalBlocks = neededBlocks - currentBlocks;
    std::cout << "Need " << additionalBlocks << " additional blocks for inodes" << std::endl;

    // 2. تحديد موقع التوسعة
    uint32_t expansionStart = disk.getSystemBlocks() + superBlockBlocks + inodeBlocks;

    // 3. البحث عن الملفات التي تحتاج للنقل
    auto filesToMove = FindFilesInRange(expansionStart, expansionStart + additionalBlocks);

    if (filesToMove.empty()) {
        std::cout << "No files need moving,可以直接扩张" << std::endl;
        return ExpandInodeAreaDirect(additionalBlocks, newTotalInodes);
    }

    std::cout << "Need to move " << filesToMove.size() << " files" << std::endl;

    // 4. البحث عن مساحة خالية في نهاية القرص
    size_t totalMoveSize = 0;
    for (const auto& file : filesToMove) {
        totalMoveSize += file.blockCount;
    }

    uint32_t freeSpaceStart = FindFreeSpaceAtEnd(additionalBlocks + totalMoveSize);
    if (freeSpaceStart == 0) {
        std::cout << "Not enough free space for defragmentation" << std::endl;
        return false;
    }

    // 5. نقل الملفات
    std::vector<DataMoveOperation> moveOperations;
    uint32_t currentFreePointer = freeSpaceStart;

    for (const auto& fileInfo : filesToMove) {
        DataMoveOperation op;
        op.inodeIndex = fileInfo.inodeIndex;
        op.oldStartBlock = fileInfo.startBlock;
        op.newStartBlock = currentFreePointer;
        op.blockCount = fileInfo.blockCount;
        op.success = MoveFileBlocks(fileInfo.inodeIndex, op.oldStartBlock, op.newStartBlock, op.blockCount);

        moveOperations.push_back(op);
        currentFreePointer += fileInfo.blockCount;

        if (!op.success) {
            std::cerr << "Failed to move file " << fileInfo.inodeIndex << std::endl;
            RollbackMoves(moveOperations);
            return false;
        }
    }

    // 6. التوسعة بعد تحرير المساحة
    if (!ExpandInodeAreaDirect(additionalBlocks, newTotalInodes)) {
        std::cerr << "Failed to expand inode area after moving files" << std::endl;
        RollbackMoves(moveOperations);
        return false;
    }

    std::cout << "Successfully added " << extraInodes << " inodes after defragmentation" << std::endl;
    return true;
}

std::vector<MiniHSFS::FileInfo> MiniHSFS::FindFilesInRange(uint32_t startBlock, uint32_t endBlock) {
    std::vector<FileInfo> files;

    for (size_t i = 1; i < inodeTable.size(); ++i) {
        if (inodeTable[i].isUsed && !inodeTable[i].isDirectory && inodeTable[i].blocksUsed > 0) {
            uint32_t fileStart = inodeTable[i].firstBlock;
            uint32_t fileEnd = fileStart + inodeTable[i].blocksUsed - 1;

            if (fileStart >= startBlock && fileStart < endBlock) {
                FileInfo info;
                info.inodeIndex = i;
                info.startBlock = fileStart;
                info.blockCount = inodeTable[i].blocksUsed;
                files.push_back(info);
            }
        }
    }

    // الترتيب حسب بلوك البداية
    std::sort(files.begin(), files.end(),
        [](const FileInfo& a, const FileInfo& b) { return a.startBlock < b.startBlock; });

    return files;
}

bool MiniHSFS::MoveFileBlocks(int inodeIndex, uint32_t oldStart, uint32_t newStart, uint32_t blockCount) {
    try {
        // 1. قراءة البيانات
        std::vector<char> fileData = disk.readData(VirtualDisk::Extent(oldStart, blockCount));

        // 2. كتابة البيانات في الموقع الجديد
        disk.writeData(fileData, VirtualDisk::Extent(newStart, blockCount), "", true);

        // 3. تحديث الـ Inode
        inodeTable[inodeIndex].firstBlock = newStart;
        inodeTable[inodeIndex].isDirty = true;

        // 4. تحرير المساحة القديمة
        std::vector<char> zeroBlock(disk.blockSize, 0);
        for (uint32_t i = 0; i < blockCount; ++i) {
            disk.writeData(zeroBlock, VirtualDisk::Extent(oldStart + i, 1), "", true);
        }

        // 5. تحديث الـ B-tree
        for (uint32_t i = 0; i < blockCount; ++i) {
            BTreeDelete(rootNodeIndex, oldStart + i);
            BTreeInsert(rootNodeIndex, newStart + i, 1);
        }

        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Error moving file blocks: " << e.what() << std::endl;
        return false;
    }
}

uint32_t MiniHSFS::FindFreeSpaceAtEnd(size_t requiredBlocks) {
    uint32_t totalBlocks = disk.totalBlocks();
    uint32_t freeBlocksFound = 0;
    uint32_t startBlock = 0;

    // البحث من النهاية إلى البداية
    for (int32_t block = totalBlocks - 1; block >= 0 && freeBlocksFound < requiredBlocks; --block) {
        if (!IsBlockUsed(block)) {
            if (freeBlocksFound == 0) {
                startBlock = block;
            }
            freeBlocksFound++;
        }
        else {
            freeBlocksFound = 0;
        }
    }

    return (freeBlocksFound >= requiredBlocks) ? startBlock : 0;
}

bool MiniHSFS::ExpandInodeAreaDirect(size_t additionalBlocks) {
    try {
        // 1. حجز الكتل الإضافية
        VirtualDisk::Extent extent(disk.getSystemBlocks() + superBlockBlocks + inodeBlocks, additionalBlocks);
        disk.allocateBlocks(extent.blockCount);

        // 2. تهيئة الكتل الجديدة
        std::vector<char> zeroBlock(disk.blockSize, 0);
        for (size_t i = 0; i < additionalBlocks; ++i) {
            disk.writeData(zeroBlock,
                VirtualDisk::Extent(extent.startBlock + i, 1), "", true);
        }

        // 3. تحديث المتغيرات الداخلية
        inodeBlocks += additionalBlocks;
        dataStartIndex += additionalBlocks;

        std::cout << "Inode area expanded by " << additionalBlocks << " blocks" << std::endl;
        return true;

    }
    catch (const std::exception& e) {
        std::cerr << "Error expanding inode area: " << e.what() << std::endl;
        return false;
    }
}

void MiniHSFS::RollbackMoves(const std::vector<DataMoveOperation>& moves) {
    std::cout << "Rolling back " << moves.size() << " file moves..." << std::endl;

    for (const auto& op : moves) {
        if (op.success) {
            // إعادة الملف إلى موقعه الأصلي
            MoveFileBlocks(op.inodeIndex, op.newStartBlock, op.oldStartBlock, op.blockCount);
        }
    }
}


// إعادة تنظيم العقد التلقائية
void MiniHSFS::AutoDefragmentInodes() {
    std::vector<Inode> compacted;
    compacted.reserve(inodeTable.size());

    // 1. نقل العقد المستخدمة فقط
    for (auto& inode : inodeTable) {
        if (inode.isUsed) {
            compacted.push_back(std::move(inode));
        }
    }

    // 2. حساب المساحة الجديدة المطلوبة
    size_t newSize = 0;
    for (const auto& inode : compacted) {
        newSize += inode.actualSize();
    }

    // 3. احتساب المساحة الإضافية (20% كهامش)
    size_t newTotalSize = newSize + newSize / 5;

    // 4. إذا كانت المساحة الجديدة أقل، نحرر الكتل الزائدة
    if (newTotalSize < inodeAreaSize) {
        size_t savedBlocks = (inodeAreaSize - newTotalSize) / disk.blockSize;
        if (savedBlocks > 0) {
            VirtualDisk::Extent extent(
                disk.getSystemBlocks() + superBlockBlocks + inodeBlocks - savedBlocks,
                savedBlocks
            );
            disk.freeBlocks(extent);
            inodeBlocks -= savedBlocks;
            inodeAreaSize = inodeBlocks * disk.blockSize;
        }
    }

    // 5. استبدال الجدول القديم بالجديد
    inodeTable = std::move(compacted);
    SaveInodeTable();
}

// إعادة بناء قائمة العقد الحرة
void MiniHSFS::RebuildFreeInodesList() {
    freeInodesList.clear();

    for (size_t i = 1; i < inodeTable.size(); ++i) {
        if (!inodeTable[i].isUsed) {
            freeInodesList.push_back(i);
        }
    }

    // تحديث مؤشر العقدة الحرة التالية
    nextFreeInode = freeInodesList.empty() ? inodeTable.size() : freeInodesList.front();
}

// حساب أقصى عدد ممكن من العقد
size_t MiniHSFS::CalculateMaxPossibleInodes() {
    size_t totalSpace = disk.totalBlocks() * disk.blockSize;
    size_t reservedSpace = superBlockBlocks * disk.blockSize +
        btreeBlocks * disk.blockSize +
        disk.getSystemBlocks() * disk.blockSize;
    size_t availableSpace = totalSpace - reservedSpace;

    // نخصص 20% من المساحة المتبقية للعقد
    return (availableSpace / 5) / inodeSize;
}

// تهيئة العقدة
int MiniHSFS::InitializeInode(int index, bool isDirectory) {
    if (index < 0 || static_cast<size_t>(index) >= inodeTable.size()) {
        throw std::out_of_range("Invalid inode index: " + std::to_string(index));
    }

    std::cout << "Initializing inode " << index << " as "
        << (isDirectory ? "directory" : "file") << std::endl;

    // تهيئة جديدة كاملة مع قيم افتراضية آمنة
    Inode newInode;
    newInode.isUsed = true;
    newInode.isDirectory = isDirectory;
    newInode.size = 0;
    newInode.blocksUsed = 0;
    newInode.firstBlock = -1;
    newInode.entries.clear();

    time_t now = time(nullptr);
    newInode.creationTime = now;
    newInode.modificationTime = now;
    newInode.lastAccessed = now;
    newInode.isDirty = true;

    inodeTable[index] = newInode;

    if (inodeBitmap.size() > static_cast<size_t>(index)) {
        inodeBitmap[index] = true;
    }

    // حفظ الـ Inode الجديد على القرص فوراً
    SaveInodeToDisk(index);

    // تحديث السوبر بلوك
    UpdateSuperblockForDynamicInodes();

    std::cout << "Inode " << index << " initialized successfully" << std::endl;
    return index;
}

// تمديد مساحة العقد
void MiniHSFS::ExtendInodeArea(size_t additionalInodes) {
    size_t oldSize = inodeTable.size();
    size_t newSize = oldSize + (additionalInodes > 0 ? additionalInodes :
        std::max<size_t>(32, oldSize / 2)); // زيادة ذكية

    if (newSize > maxPossibleInodes) {
        throw std::runtime_error("Inode limit reached");
    }

    // تخصيص مساحة على القرص أولاً
    size_t requiredBlocks = (newSize * inodeSize + disk.blockSize - 1) / disk.blockSize;
    if (requiredBlocks > inodeBlocks) {
        VirtualDisk::Extent extent(
            disk.getSystemBlocks() + superBlockBlocks + inodeBlocks,
            requiredBlocks - inodeBlocks
        );
        disk.allocateBlocks(extent.blockCount);

        // تهيئة الكتل الجديدة
        std::vector<char> zeroBlock(disk.blockSize, 0);
        for (size_t i = inodeBlocks; i < requiredBlocks; ++i) {
            disk.writeData(zeroBlock,
                VirtualDisk::Extent(
                    disk.getSystemBlocks() + superBlockBlocks + i,
                    1
                ),
                "",
                true
            );
        }
        inodeBlocks = requiredBlocks;
    }

    // توسيع الهياكل في الذاكرة
    inodeTable.resize(newSize);
    inodeBitmap.resize(newSize, false);

    // تحديث السوبربلوك
    UpdateSuperblockForDynamicInodes();
}

// تحديث السوبربلوك
void MiniHSFS::UpdateSuperblockForDynamicInodes() {
    SuperblockInfo info = LoadSuperblock();
    info.inodeSize = inodeSize;
    info.totalInodes = static_cast<uint32_t>(inodeTable.size());
    info.freeInodes = static_cast<uint32_t>(CountFreeInodes());
    info.lastWriteTime = time(nullptr);

    // تحديث حجم النظام ليعكس التوسع في منطقة الـ Inodes
    info.systemSize = static_cast<uint32_t>(disk.getSystemBlocks() + superBlockBlocks + inodeBlocks + btreeBlocks);

    SaveSuperblock(info);
    std::cout << "Superblock updated: " << info.totalInodes << " inodes, "
        << info.freeInodes << " free inodes" << std::endl;
}

void MiniHSFS::ValidateInodeTable() {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    // منع التكرار اللانهائي - تحقق إذا كنا بالفعل في عملية التحقق
    static bool is_validating = false;
    if (is_validating) {
        return;
    }
    is_validating = true;

    int validCount = 0;
    int invalidCount = 0;
    int repairedCount = 0;

    // الإصلاح الإجباري للعقدة الجذرية أولاً
    if (inodeTable.size() > 0) {
        if (!inodeTable[0].isValid() || !inodeTable[0].isUsed || !inodeTable[0].isDirectory) {
            std::cout << "Emergency repair of root inode..." << std::endl;

            // إعادة إنشاء العقدة الجذرية من الصفر
            inodeTable[0] = Inode();
            inodeTable[0].isUsed = true;
            inodeTable[0].isDirectory = true;
            inodeTable[0].creationTime = time(nullptr);
            inodeTable[0].modificationTime = inodeTable[0].creationTime;
            inodeTable[0].lastAccessed = inodeTable[0].creationTime;
            inodeTable[0].isDirty = true;

            validCount++;
            repairedCount++;
        }
        else {
            validCount++;
        }
    }

    // التحقق من باقي العقد
    for (size_t i = 1; i < inodeTable.size(); ++i) {
        if (inodeTable[i].isUsed) {
            if (inodeTable[i].isValid()) {
                validCount++;

                // إصلاح الطوابع الزمنية إذا كانت غير صالحة
                bool repaired = false;
                time_t currentTime = time(nullptr);

                if (inodeTable[i].creationTime <= 0 || inodeTable[i].creationTime > currentTime + 3600) {
                    inodeTable[i].creationTime = currentTime;
                    inodeTable[i].isDirty = true;
                    repaired = true;
                }

                if (inodeTable[i].modificationTime <= 0 || inodeTable[i].modificationTime > currentTime + 3600) {
                    inodeTable[i].modificationTime = inodeTable[i].creationTime;
                    inodeTable[i].isDirty = true;
                    repaired = true;
                }

                if (inodeTable[i].lastAccessed <= 0 || inodeTable[i].lastAccessed > currentTime + 3600) {
                    inodeTable[i].lastAccessed = inodeTable[i].creationTime;
                    inodeTable[i].isDirty = true;
                    repaired = true;
                }

                if (repaired) {
                    repairedCount++;
                    std::cout << "Repaired timestamps for inode " << i << std::endl;
                }

                // التحذير فقط إذا كان مجلداً ولا يحتوي على إدخالات (وليس الجذر)
                if (inodeTable[i].isDirectory && inodeTable[i].entries.empty() && i != 0) {
                    std::cout << "Warning: Directory inode " << i << " has no entries" << std::endl;
                    // هذه مجرد تحذير، لا نعتبرها خطأ需要 الإصلاح
                }
            }
            else {
                invalidCount++;
                // إصلاح العقد التالفة
                std::cout << "Repairing invalid inode " << i << std::endl;

                // محاولة الحفاظ على نوع الـ Inode إذا كان معلوماً
                bool wasDirectory = inodeTable[i].isDirectory;
                inodeTable[i] = Inode();
                inodeTable[i].isUsed = true;
                inodeTable[i].isDirectory = wasDirectory;
                inodeTable[i].creationTime = time(nullptr);
                inodeTable[i].modificationTime = inodeTable[i].creationTime;
                inodeTable[i].lastAccessed = inodeTable[i].creationTime;
                inodeTable[i].isDirty = true;

                repairedCount++;
            }
        }
    }

    std::cout << "Inode table validation: " << validCount << " valid, "
        << invalidCount << " invalid, " << repairedCount << " repaired" << std::endl;

    // إذا كان هناك عقد تالفة أو تم إصلاحها، نحفظ الجدول المعدل
    if (invalidCount > 0 || repairedCount > 0) {
        // منع الحلقة اللانهائية - حفظ بدون استدعاء ValidateInodeTable مرة أخرى
        is_validating = false;
        SaveInodeTable();
    }

    is_validating = false;
}

// عد العقد الحرة
size_t MiniHSFS::CountFreeInodes() {
    size_t count = 0;
    for (size_t i = 0; i < inodeTable.size(); ++i) {
        if (!inodeTable[i].isUsed) {
            count++;
        }
    }
    return count;
}

// ضغط جدول العقد
void MiniHSFS::CompactInodeTable() {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    // 1. جمع العقد المستخدمة
    std::vector<Inode> usedInodes;
    std::vector<bool> usedBitmap;

    for (size_t i = 0; i < inodeTable.size(); ++i) {
        if (inodeBitmap[i]) {
            usedInodes.push_back(std::move(inodeTable[i]));
            usedBitmap.push_back(true);
        }
    }

    // 2. إعادة إنشاء الجدول مع هامش 25%
    size_t newSize = usedInodes.size() + (usedInodes.size() / 4);
    newSize = (std::max)(newSize, static_cast<size_t>(32)); // الحد الأدنى

    inodeTable.clear();
    inodeTable.resize(newSize);
    inodeBitmap.assign(newSize, false);

    // 3. إعادة تعبئة العقد المستخدمة
    for (size_t i = 0; i < usedInodes.size(); ++i) {
        inodeTable[i] = std::move(usedInodes[i]);
        inodeBitmap[i] = true;
    }

    // 4. تحديث مؤشر العقد الحرة
    nextFreeInode = usedInodes.empty() ? 0 : usedInodes.size();

    // 5. حفظ التغييرات
    SaveInodeTable();
    UpdateSuperblockForDynamicInodes();
}

// إلغاء تجزئة العقد
void MiniHSFS::DefragmentInodes() {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    // 1. جمع جميع العقد المستخدمة
    std::vector<std::pair<int, Inode>> usedInodes;
    for (size_t i = 1; i < inodeTable.size(); ++i) {
        if (inodeTable[i].isUsed) {
            usedInodes.emplace_back(i, inodeTable[i]);
        }
    }

    // 2. إعادة إنشاء جدول العقد
    size_t newSize = (std::max)(inodeTable.size(), usedInodes.size() + 32); // هامش 32 عقدة
    std::vector<Inode> newTable(newSize);
    std::vector<bool> newBitmap(newSize, false);

    // 3. نسخ العقدة الجذرية
    newTable[0] = inodeTable[0];
    newBitmap[0] = true;

    // 4. إعادة تعبئة العقد المستخدمة
    for (size_t newIndex = 1; newIndex <= usedInodes.size(); ++newIndex) {
        newTable[newIndex] = usedInodes[newIndex - 1].second;
        newBitmap[newIndex] = true;

        // تحديث الإشارات في المجلدات
        if (newTable[newIndex].isDirectory) {
            for (auto& entry : newTable[newIndex].entries) {
                // لا نحتاج لتحديث القيم لأننا نعيد تعبئة كل العقد
            }
        }
    }

    // 5. استبدال الجداول القديمة بالجديدة
    inodeTable = std::move(newTable);
    inodeBitmap = std::move(newBitmap);
    inodeCount = inodeTable.size();

    // 6. إعادة بناء قائمة العقد الحرة
    RebuildFreeInodesList();

    // 7. حفظ التغييرات
    SaveInodeTable();
    UpdateSuperblockForDynamicInodes();
}

// الحصول على عدد العقد الحرة
size_t MiniHSFS::GetAvailableInodes() {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);
    return freeInodesList.size() + (maxPossibleInodes - inodeCount);
}

// التحقق من إمكانية إنشاء المزيد من العقد
bool MiniHSFS::CanCreateMoreInodes() {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);
    return !freeInodesList.empty() || (dynamicInodes && inodeCount < maxPossibleInodes);
}

// التحقق من وجود مساحة كافية
bool MiniHSFS::CanAllocateInodes(size_t count) {
    size_t freeSpace = disk.freeBlocksCount() * disk.blockSize;
    size_t requiredSpace = count * inodeSize;
    return freeSpace >= requiredSpace;
}

// إعادة بناء خريطة العقد
void MiniHSFS::RebuildInodeBitmap() {
    inodeBitmap.clear();
    inodeBitmap.resize(inodeTable.size());
    for (size_t i = 0; i < inodeTable.size(); ++i) {
        inodeBitmap[i] = inodeTable[i].isUsed;
    }
    nextFreeInode = 0;
}

//void MiniHSFS::LoadInodeTable() {
//    std::lock_guard<std::recursive_mutex> lock(fsMutex);
//
//    InitializeInodeTable(); // تجهّز الحجم/العدادات الأساسية
//
//    std::vector<char> inode_data(inodeBlocks * disk.blockSize, 0);
//
//    // اقرأ كل كتل جدول الـinodes
//    for (size_t block = 0; block < inodeBlocks; ++block) {
//        auto data = disk.readData(
//            VirtualDisk::Extent(disk.getSystemBlocks() + static_cast<uint32_t>(superBlockBlocks) + static_cast<uint32_t>(block), 1)
//        );
//        std::copy(data.begin(), data.end(), inode_data.begin() + block * disk.blockSize);
//    }
//
//    // فكّ كل inode من موضعه الثابت
//    size_t maxInodesByArea = (inodeBlocks * disk.blockSize) / inodeSize;
//    size_t toLoad = (std::min)(inodeTable.size(), maxInodesByArea);
//
//    for (size_t i = 0; i < toLoad; ++i) {
//        size_t off = i * inodeSize;
//        if (off + inodeSize > inode_data.size())
//            throw std::runtime_error("LoadInodeTable: truncated inode area");
//
//        DeserializeInode(inodeTable[i], inode_data.data() + off, inodeSize);
//    }
//
//    // تأكيد صلاحية الجذر
//    if (!inodeTable[0].isUsed || !inodeTable[0].isDirectory) {
//        throw std::runtime_error("Root inode is missing or corrupted");
//    }
//
//    // أعد بناء bitmap / قائمة الحرة بعد التحميل
//    RebuildInodeBitmap();  // إن كانت مستخدمة
//    RebuildFreeInodesList();
//}
//
//void MiniHSFS::SaveInodeTable() {
//    std::lock_guard<std::recursive_mutex> lock(fsMutex);
//
//    // مساحة جدول الـinodes بالكامل
//    std::vector<char> inode_data(inodeBlocks * disk.blockSize, 0);
//
//    // كل inode عند offset ثابت i * inodeSize
//    for (size_t i = 0; i < inodeTable.size(); ++i) {
//        size_t off = i * inodeSize;
//        if (off + inodeSize > inode_data.size())
//            throw std::runtime_error("SaveInodeTable: inode area too small");
//        if (SerializeInode(inodeTable[i], inode_data.data() + off, inodeSize) == 0)
//            throw std::runtime_error("SaveInodeTable: serialize failed");
//    }
//
//    // اكتب الكتل فعليًا
//    for (size_t block = 0; block < inodeBlocks; ++block) {
//        std::vector<char> block_data(
//            inode_data.begin() + block * disk.blockSize,
//            inode_data.begin() + (block + 1) * disk.blockSize
//        );
//        disk.writeData(
//            block_data,
//            VirtualDisk::Extent(disk.getSystemBlocks() + static_cast<uint32_t>(superBlockBlocks) + static_cast<uint32_t>(block), 1),
//            "",
//            true
//        );
//    }
//}
//
//size_t MiniHSFS::SerializeInode(const Inode& inode, char* buffer, size_t bufferSize) {
//
//    if (bufferSize < inodeSize) return 0;            // يجب أن يساوي inodeSize
//    std::memset(buffer, 0, bufferSize);              // صفّر المساحة كلها
//
//    size_t offset = 0;
//    auto put = [&](const void* src, size_t sz) {
//        if (offset + sz > bufferSize) throw std::runtime_error("SerializeInode: overflow");
//        std::memcpy(buffer + offset, src, sz);
//        offset += sz;
//        };
//
//    // رأس ثابت
//    put(&inode.size, sizeof(inode.size));
//    put(&inode.blocksUsed, sizeof(inode.blocksUsed));
//    put(&inode.firstBlock, sizeof(inode.firstBlock));
//    put(&inode.isDirectory, sizeof(inode.isDirectory));
//    put(&inode.isUsed, sizeof(inode.isUsed));
//    put(&inode.creationTime, sizeof(inode.creationTime));
//    put(&inode.modificationTime, sizeof(inode.modificationTime));
//    put(&inode.lastAccessed, sizeof(inode.lastAccessed));
//    put(&inode.isDirty, sizeof(inode.isDirty));
//
//    // عدد الإدخالات (سنحسب كم فعليًا كتبنا)
//    // نكتب عدد فعلي لاحقًا بعد الحجز، لذلك نحتفظ بمكانه:
//    uint32_t entryCount = 0;
//    size_t entryCountPos = offset;
//    uint32_t zero = 0;
//    put(&zero, sizeof(zero)); // placeholder
//
//    // مساحة باقية لإدخالات الدليل
//    if (inode.isDirectory) {
//        for (const auto& kv : inode.entries) {
//            const std::string& name = kv.first;
//            int inodeNum = kv.second;
//            uint16_t len = static_cast<uint16_t>(name.size());
//
//            // كل إدخال: len(2) + name + inodeNum(4)
//            size_t need = sizeof(len) + len + sizeof(inodeNum);
//            if (offset + need > bufferSize) break; // لا مساحة إضافية
//
//            put(&len, sizeof(len));
//            if (len) { put(name.data(), len); }
//            put(&inodeNum, sizeof(inodeNum));
//            entryCount++;
//        }
//        // اكتب entryCount في مكانه
//        std::memcpy(buffer + entryCountPos, &entryCount, sizeof(entryCount));
//    }
//
//    // نضمن أننا لم نتخطَّ bufferSize
//    return inodeSize;
//}
//
//size_t MiniHSFS::DeserializeInode(Inode& inode, const char* buffer, size_t bufferSize) {
//
//    if (bufferSize < inodeSize) return 0;
//
//    size_t offset = 0;
//    auto get = [&](void* dst, size_t sz) {
//        if (offset + sz > bufferSize) throw std::runtime_error("DeserializeInode: underflow");
//        std::memcpy(dst, buffer + offset, sz);
//        offset += sz;
//        };
//
//    // الرأس الثابت
//    get(&inode.size, sizeof(inode.size));
//    get(&inode.blocksUsed, sizeof(inode.blocksUsed));
//    get(&inode.firstBlock, sizeof(inode.firstBlock));
//    get(&inode.isDirectory, sizeof(inode.isDirectory));
//    get(&inode.isUsed, sizeof(inode.isUsed));
//    get(&inode.creationTime, sizeof(inode.creationTime));
//    get(&inode.modificationTime, sizeof(inode.modificationTime));
//    get(&inode.lastAccessed, sizeof(inode.lastAccessed));
//    get(&inode.isDirty, sizeof(inode.isDirty));
//
//    uint32_t entryCount = 0;
//    get(&entryCount, sizeof(entryCount));
//
//    inode.entries.clear();
//
//    if (inode.isDirectory) {
//        for (uint32_t i = 0; i < entryCount; ++i) {
//            if (offset + sizeof(uint16_t) > bufferSize) break;
//            uint16_t len = 0;
//            get(&len, sizeof(len));
//
//            if (offset + len + sizeof(int) > bufferSize) break;
//            std::string name(len, '\0');
//            if (len) {
//                std::memcpy(&name[0], buffer + offset, len);
//                offset += len;
//            }
//            int inodeNum = -1;
//            get(&inodeNum, sizeof(inodeNum));
//
//            inode.entries[std::move(name)] = inodeNum;
//        }
//    }
//
//    // تجاهل أي padding متبقّي… القراءة ثابتة
//    return inodeSize;
//}
//
//void MiniHSFS::LoadInodeTable() {
//    std::lock_guard<std::recursive_mutex> lock(fsMutex);
//
//    // احصل على عدد inodes الممكن تخزينها في المساحة الحالية (حسب inodeBlocks)
//    size_t maxInodesByArea = (inodeBlocks * disk.blockSize) / inodeSize;
//    if (maxInodesByArea == 0) throw std::runtime_error("Inode area size is zero");
//
//    // اضبط حجم جدول الـ inodes في الذاكرة ليتطابق مع المساحة الفعلية على القرص
//    inodeTable.clear();
//    inodeTable.resize(maxInodesByArea);
//    inodeBitmap.clear();
//    inodeBitmap.resize(maxInodesByArea, false);
//
//    // اقرأ كل كتل جدول الـinodes إلى buffer واحد كبير
//    std::vector<char> inode_data(inodeBlocks * disk.blockSize, 0);
//    for (size_t block = 0; block < inodeBlocks; ++block) {
//        // حساب بلوك النظام + سوبر بلوك + offset لجدول الـ inodes
//        uint32_t absBlock = static_cast<uint32_t>(disk.getSystemBlocks() + static_cast<uint32_t>(superBlockBlocks) + static_cast<uint32_t>(block));
//        auto data = disk.readData(VirtualDisk::Extent(absBlock, 1));
//        if (data.size() != disk.blockSize) {
//            // إذا كانت الدالة تعيد أحجام مختلفة، نتعامل بهدوء ولكن نملأ ما استرددناه
//            std::copy(data.begin(), data.end(), inode_data.begin() + block * disk.blockSize);
//        }
//        else {
//            std::copy(data.begin(), data.end(), inode_data.begin() + block * disk.blockSize);
//        }
//    }
//
//    // فكّ كل inode من موضعه الثابت
//    size_t toLoad = maxInodesByArea;
//    for (size_t i = 0; i < toLoad; ++i) {
//        size_t off = i * inodeSize;
//        if (off + inodeSize > inode_data.size()) {
//            throw std::runtime_error("LoadInodeTable: truncated inode area");
//        }
//        DeserializeInode(inodeTable[i], inode_data.data() + off, inodeSize);
//    }
//
//    // تحقق من وجود الجذر وصحته
//    if (inodeTable.size() == 0 || !inodeTable[0].isUsed || !inodeTable[0].isDirectory) {
//        throw std::runtime_error("Root inode is missing or corrupted");
//    }
//
//    // أعِد بناء البت ماب وقائمة الـ free inodes في الذاكرة
//    RebuildInodeBitmap();
//    RebuildFreeInodesList();
//}
//
//void MiniHSFS::SaveInodeTable() {
//    std::lock_guard<std::recursive_mutex> lock(fsMutex);
//
//    // تأكد أن المساحة التي سنكتب إليها مساوية لعدد بلوكات الـ inode
//    size_t areaBytes = inodeBlocks * disk.blockSize;
//    if (areaBytes == 0) throw std::runtime_error("SaveInodeTable: inode area is zero");
//
//    // اضبط buffer ليحتوي كامل منطقة جدول الـ inodes
//    std::vector<char> inode_data(areaBytes, 0);
//
//    // اكتب كل inode عند الإزاحة الثابتة i * inodeSize
//    size_t maxInodesByArea = areaBytes / inodeSize;
//    if (inodeTable.size() > maxInodesByArea) {
//        // إذا الذاكرة تحتوي على أكثر من ما تسمح به المساحة، نرفع استثناء
//        throw std::runtime_error("SaveInodeTable: inodeTable larger than allocated inode area");
//    }
//
//    for (size_t i = 0; i < inodeTable.size(); ++i) {
//        size_t off = i * inodeSize;
//        if (off + inodeSize > inode_data.size())
//            throw std::runtime_error("SaveInodeTable: inode area too small");
//
//        if (SerializeInode(inodeTable[i], inode_data.data() + off, inodeSize) == 0)
//            throw std::runtime_error("SaveInodeTable: serialize failed for inode " + std::to_string(i));
//    }
//
//    // اكتب الكتل فعليًا بلوك بلوك
//    for (size_t block = 0; block < inodeBlocks; ++block) {
//        size_t startOff = block * disk.blockSize;
//        std::vector<char> block_data(inode_data.begin() + startOff, inode_data.begin() + startOff + disk.blockSize);
//
//        uint32_t absBlock = static_cast<uint32_t>(disk.getSystemBlocks() + static_cast<uint32_t>(superBlockBlocks) + static_cast<uint32_t>(block));
//        disk.writeData(block_data, VirtualDisk::Extent(absBlock, 1), "", true);
//    }
//}
//void MiniHSFS::LoadInodeTable() {
//    std::lock_guard<std::recursive_mutex> lock(fsMutex);
//
//    // 1. احسب الحد الأدنى للعقد المطلوبة (يمكن تغيير 50 لأي قيمة تريدها)
//    const size_t minRequiredInodes = 35;
//
//    // 2. احسب عدد البلوكات المطلوبة بناءً على minRequiredInodes
//    size_t requiredBlocks = (minRequiredInodes * inodeSize + disk.blockSize - 1) / disk.blockSize;
//
//    // 3. إذا كانت البلوكات الحالية غير كافية، قم بتوسيعها
//    if (requiredBlocks > inodeBlocks) {
//        size_t additionalBlocks = requiredBlocks - inodeBlocks;
//        VirtualDisk::Extent extent(disk.getSystemBlocks() + superBlockBlocks + inodeBlocks, additionalBlocks);
//
//        // احجز البلوكات الإضافية
//        disk.allocateBlocks(extent.blockCount);
//
//        // اكتب صفر في البلوكات الجديدة
//        std::vector<char> zeroBlock(disk.blockSize, 0);
//        for (size_t i = 0; i < additionalBlocks; ++i) {
//            disk.writeData(zeroBlock,
//                VirtualDisk::Extent(extent.startBlock + i, 1),
//                "",
//                true);
//        }
//
//        inodeBlocks = requiredBlocks;
//    }
//
//    // 4. احسب العدد الفعلي للعقد التي يمكن قراءتها
//    size_t maxInodes = (inodeBlocks * disk.blockSize) / inodeSize;
//
//    // 5. تهيئة الجدول في الذاكرة بالحجم المطلوب (50 أو أكثر)
//    inodeTable.clear();
//    inodeTable.resize((std::max)(maxInodes, minRequiredInodes));
//    inodeBitmap.resize(inodeTable.size(), false);
//
//    // 6. قراءة البيانات من القرص
//    std::vector<char> inode_data(inodeBlocks * disk.blockSize);
//    for (size_t block = 0; block < inodeBlocks; ++block) {
//        auto data = disk.readData(
//            VirtualDisk::Extent(
//                disk.getSystemBlocks() + superBlockBlocks + block,
//                1
//            )
//        );
//        std::copy(data.begin(), data.end(),
//            inode_data.begin() + block * disk.blockSize);
//    }
//
//    // 7. تحميل كل العقد المتاحة
//    for (size_t i = 0; i < maxInodes; ++i) {
//        size_t offset = i * inodeSize;
//        DeserializeInode(inodeTable[i], inode_data.data() + offset, inodeSize);
//    }
//
//    // 8. التحقق من العقدة الجذرية
//    if (!inodeTable[0].isUsed || !inodeTable[0].isDirectory) {
//        throw std::runtime_error("Root inode is corrupted");
//    }
//
//    // 9. تحديث الهياكل المساعدة
//    RebuildInodeBitmap();
//    RebuildFreeInodesList();
//
//    // 10. تحديث السوبر بلوك ليعكس التغييرات
//    UpdateSuperblockForDynamicInodes();
//}
//
//void MiniHSFS::SaveInodeTable() {
//    std::lock_guard<std::recursive_mutex> lock(fsMutex);
//
//    // 1. احسب عدد البلوكات المطلوبة بناءً على العقد الحالية
//    size_t requiredBlocks = (inodeTable.size() * inodeSize + disk.blockSize - 1) / disk.blockSize;
//
//    // 2. إذا كانت البلوكات الحالية غير كافية، قم بتوسيعها
//    if (requiredBlocks > inodeBlocks) {
//        size_t additionalBlocks = requiredBlocks - inodeBlocks;
//        VirtualDisk::Extent extent(disk.getSystemBlocks() + superBlockBlocks + inodeBlocks, additionalBlocks);
//
//        // احجز البلوكات الإضافية
//        disk.allocateBlocks(extent.blockCount);
//
//        // اكتب صفر في البلوكات الجديدة
//        std::vector<char> zeroBlock(disk.blockSize, 0);
//        for (size_t i = 0; i < additionalBlocks; ++i) {
//            disk.writeData(zeroBlock,
//                VirtualDisk::Extent(extent.startBlock + i, 1),
//                "",
//                true);
//        }
//
//        inodeBlocks = requiredBlocks;
//    }
//
//    // 3. احفظ جميع العقد (حتى غير المستخدمة)
//    std::vector<char> inode_data(inodeBlocks * disk.blockSize, 0);
//    for (size_t i = 0; i < inodeTable.size(); ++i) {
//        size_t offset = i * inodeSize;
//        SerializeInode(inodeTable[i], inode_data.data() + offset, inodeSize);
//    }
//
//    // 4. اكتب البيانات إلى القرص
//    for (size_t block = 0; block < inodeBlocks; ++block) {
//        std::vector<char> block_data(
//            inode_data.begin() + block * disk.blockSize,
//            inode_data.begin() + (block + 1) * disk.blockSize
//        );
//        disk.writeData(block_data,
//            VirtualDisk::Extent(disk.getSystemBlocks() + superBlockBlocks + block, 1),
//            "",
//            true
//        );
//    }
//
//    // 5. تحديث السوبر بلوك
//    UpdateSuperblockForDynamicInodes();
//}
//
//// يعبّي buffer (الذي طوله = inodeSize) بمحتوى الـ inode بصيغة ثابتة
//size_t MiniHSFS::SerializeInode(const Inode& inode, char* buffer, size_t bufferSize) {
//    if (bufferSize < inodeSize) return 0;
//    std::memset(buffer, 0, bufferSize);
//
//    size_t offset = 0;
//    auto put = [&](const void* src, size_t sz) {
//        if (offset + sz > bufferSize) throw std::runtime_error("SerializeInode: overflow");
//        std::memcpy(buffer + offset, src, sz);
//        offset += sz;
//        };
//
//    // رأس ثابت
//    put(&inode.size, sizeof(inode.size));
//    put(&inode.blocksUsed, sizeof(inode.blocksUsed));
//    put(&inode.firstBlock, sizeof(inode.firstBlock));
//    put(&inode.isDirectory, sizeof(inode.isDirectory));
//    put(&inode.isUsed, sizeof(inode.isUsed));
//    put(&inode.creationTime, sizeof(inode.creationTime));
//    put(&inode.modificationTime, sizeof(inode.modificationTime));
//    put(&inode.lastAccessed, sizeof(inode.lastAccessed));
//    put(&inode.isDirty, sizeof(inode.isDirty));
//
//    // احجز مكانًا لعدد الإدخالات (entryCount) ثم املأ الإدخالات إن كان الدليل
//    uint32_t entryCount = 0;
//    size_t entryCountPos = offset;
//    uint32_t zero = 0;
//    put(&zero, sizeof(zero)); // placeholder
//
//    if (inode.isDirectory) {
//        for (const auto& kv : inode.entries) {
//            const std::string& name = kv.first;
//            int inodeNum = kv.second;
//            uint16_t len = static_cast<uint16_t>(name.size());
//
//            // كل إدخال: len(2) + name + inodeNum(4)
//            size_t need = sizeof(len) + len + sizeof(inodeNum);
//            if (offset + need > bufferSize) break; // لا مساحة إضافية
//
//            put(&len, sizeof(len));
//            if (len) { put(name.data(), len); }
//            put(&inodeNum, sizeof(inodeNum));
//            entryCount++;
//        }
//        // اكتب عدد الإدخالات في المكان المحجوز
//        std::memcpy(buffer + entryCountPos, &entryCount, sizeof(entryCount));
//    }
//
//    // نُعيد طول السجل (ثابت = inodeSize)
//    return inodeSize;
//}
//
//// يفك من buffer إلى هيكل inode
//size_t MiniHSFS::DeserializeInode(Inode& inode, const char* buffer, size_t bufferSize) {
//    if (bufferSize < inodeSize) return 0;
//
//    size_t offset = 0;
//    auto get = [&](void* dst, size_t sz) {
//        if (offset + sz > bufferSize) throw std::runtime_error("DeserializeInode: underflow");
//        std::memcpy(dst, buffer + offset, sz);
//        offset += sz;
//        };
//
//    // الرأس الثابت
//    get(&inode.size, sizeof(inode.size));
//    get(&inode.blocksUsed, sizeof(inode.blocksUsed));
//    get(&inode.firstBlock, sizeof(inode.firstBlock));
//    get(&inode.isDirectory, sizeof(inode.isDirectory));
//    get(&inode.isUsed, sizeof(inode.isUsed));
//    get(&inode.creationTime, sizeof(inode.creationTime));
//    get(&inode.modificationTime, sizeof(inode.modificationTime));
//    get(&inode.lastAccessed, sizeof(inode.lastAccessed));
//    get(&inode.isDirty, sizeof(inode.isDirty));
//
//    uint32_t entryCount = 0;
//    get(&entryCount, sizeof(entryCount));
//
//    inode.entries.clear();
//
//    if (inode.isDirectory) {
//        for (uint32_t i = 0; i < entryCount; ++i) {
//            if (offset + sizeof(uint16_t) > bufferSize) break;
//            uint16_t len = 0;
//            get(&len, sizeof(len));
//
//            if (offset + len + sizeof(int) > bufferSize) break;
//            std::string name(len, '\0');
//            if (len) {
//                std::memcpy(&name[0], buffer + offset, len);
//                offset += len;
//            }
//            int inodeNum = -1;
//            get(&inodeNum, sizeof(inodeNum));
//
//            inode.entries[std::move(name)] = inodeNum;
//        }
//    }
//
//    return inodeSize;
//}

void MiniHSFS::LoadInodeTable() {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    SuperblockInfo sb = LoadSuperblock();
    inodeSize = sb.inodeSize;

    inodeCount = sb.totalInodes;               // 👈 المصدر الوحيد للحجم
    inodeBlocks = BlocksForInodes(inodeCount); // 👈 احسب كم بلوك لازم نقرأ
    inodeTable.assign(inodeCount, Inode{});

    // اقرأ كل بلوكات جدول الـinodes
    size_t bytes = inodeBlocks * disk.blockSize;
    std::vector<char> buf(bytes, 0);
    for (size_t b = 0; b < inodeBlocks; ++b) {
        auto data = disk.readData(
            VirtualDisk::Extent(disk.getSystemBlocks() + superBlockBlocks + b, 1));
        std::copy(data.begin(), data.end(), buf.begin() + b * disk.blockSize);
    }

    // فكّ التسلسل لكل inode
    for (size_t i = 0; i < inodeCount; ++i) {
        DeserializeInode(inodeTable[i], buf.data() + i * inodeSize, inodeSize);
    }

    RebuildInodeBitmap(); // 👈 ابنِ bitmap من isUsed بعد التحميل
}

void MiniHSFS::SaveInodeTable() {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    size_t requiredBlocks = BlocksForInodes(inodeTable.size());
    if (requiredBlocks > inodeBlocks) {
        size_t add = requiredBlocks - inodeBlocks;
        VirtualDisk::Extent ext(disk.getSystemBlocks() + superBlockBlocks + inodeBlocks, add);
        disk.allocateBlocks(ext.blockCount);

        std::vector<char> zero(disk.blockSize, 0);
        for (size_t i = 0; i < add; ++i)
            disk.writeData(zero, VirtualDisk::Extent(ext.startBlock + i, 1), "", true);

        inodeBlocks = requiredBlocks;
        UpdateSuperblockForDynamicInodes();
    }

    std::vector<char> big(inodeBlocks * disk.blockSize, 0);
    size_t ok = 0;
    for (size_t i = 0; i < inodeTable.size(); ++i) {
        size_t off = i * inodeSize;
        if (off + inodeSize > big.size())
            throw std::runtime_error("SaveInodeTable: inode area too small");

        if (SerializeInode(inodeTable[i], big.data() + off, inodeSize) == 0)
            throw std::runtime_error("SaveInodeTable: serialize failed for inode " + std::to_string(i));

        inodeTable[i].isDirty = false;
        ok++;
    }

    for (size_t b = 0; b < inodeBlocks; ++b) {
        uint32_t phys = disk.getSystemBlocks() + superBlockBlocks + static_cast<uint32_t>(b);
        size_t start = b * disk.blockSize;
        size_t end = (std::min)(start + (size_t)disk.blockSize, big.size());
        std::vector<char> blk(big.begin() + start, big.begin() + end);
        disk.writeData(blk, VirtualDisk::Extent(phys, 1), "", true);
    }

    UpdateSuperblockForDynamicInodes();
}

size_t MiniHSFS::SerializeInode(const Inode& inode, char* buffer, size_t bufferSize) {
    if (bufferSize < inodeSize) {
        std::cerr << "Buffer too small for inode serialization. Needed: "
            << inodeSize << ", Got: " << bufferSize << std::endl;
        return 0;
    }

    std::memset(buffer, 0, bufferSize);
    size_t offset = 0;

    // (1) حقول أساسية بثبات
    std::memcpy(buffer + offset, &inode.size, sizeof(inode.size));                 offset += sizeof(inode.size);
    std::memcpy(buffer + offset, &inode.blocksUsed, sizeof(inode.blocksUsed));     offset += sizeof(inode.blocksUsed);
    std::memcpy(buffer + offset, &inode.firstBlock, sizeof(inode.firstBlock));     offset += sizeof(inode.firstBlock);

    // (2) flags في بايت واحد لتجنّب مشاكل حجم bool
    uint8_t flags = 0;
    if (inode.isDirectory) flags |= 0x01;
    if (inode.isUsed)      flags |= 0x02;
    if (inode.isDirty)     flags |= 0x04;
    std::memcpy(buffer + offset, &flags, sizeof(flags));                           offset += sizeof(flags);

    // (3) timestamps مع تصحيح القيم الصفرية
    time_t c = inode.creationTime > 0 ? inode.creationTime : time(nullptr);
    time_t m = inode.modificationTime > 0 ? inode.modificationTime : c;
    time_t a = inode.lastAccessed > 0 ? inode.lastAccessed : c;

    std::memcpy(buffer + offset, &c, sizeof(c));                                   offset += sizeof(c);
    std::memcpy(buffer + offset, &m, sizeof(m));                                   offset += sizeof(m);
    std::memcpy(buffer + offset, &a, sizeof(a));                                   offset += sizeof(a);

    // (4) محتوى الدليل داخل نفس الـinode (لو كان Directory ومستخدم)
    if (inode.isDirectory && inode.isUsed) {
        uint32_t count = static_cast<uint32_t>(inode.entries.size());
        if (offset + sizeof(count) > bufferSize) count = 0; // مفيش مساحة
        if (count) {
            std::memcpy(buffer + offset, &count, sizeof(count));                   offset += sizeof(count);

            for (const auto& kv : inode.entries) {
                const std::string& name = kv.first;
                uint16_t nameLen = static_cast<uint16_t>(name.size());
                size_t need = sizeof(nameLen) + nameLen + sizeof(int);
                if (offset + need > bufferSize) break;

                std::memcpy(buffer + offset, &nameLen, sizeof(nameLen));           offset += sizeof(nameLen);
                if (nameLen) {
                    std::memcpy(buffer + offset, name.data(), nameLen);            offset += nameLen;
                }
                std::memcpy(buffer + offset, &kv.second, sizeof(kv.second));       offset += sizeof(kv.second);
            }
        }
    }

    // (5) checksum لِما سبق
    uint32_t checksum = CalculateChecksum(buffer, offset);
    if (offset + sizeof(checksum) <= bufferSize) {
        std::memcpy(buffer + offset, &checksum, sizeof(checksum));                 offset += sizeof(checksum);
    }

    return offset;
}

size_t MiniHSFS::DeserializeInode(Inode& inode, const char* buffer, size_t bufferSize) {
    if (bufferSize < inodeSize) {
        std::cerr << "Buffer too small for inode deserialization. Needed: "
            << inodeSize << ", Got: " << bufferSize << std::endl;
        return 0;
    }

    std::vector<char> copy(buffer, buffer + bufferSize);
    size_t offset = 0;

    try {
        // (1) حقول أساسية
        std::memcpy(&inode.size, buffer + offset, sizeof(inode.size));        offset += sizeof(inode.size);
        std::memcpy(&inode.blocksUsed, buffer + offset, sizeof(inode.blocksUsed));  offset += sizeof(inode.blocksUsed);
        std::memcpy(&inode.firstBlock, buffer + offset, sizeof(inode.firstBlock));  offset += sizeof(inode.firstBlock);

        // (2) flags
        uint8_t flags = 0;
        std::memcpy(&flags, buffer + offset, sizeof(flags));                         offset += sizeof(flags);
        inode.isDirectory = (flags & 0x01) != 0;
        inode.isUsed = (flags & 0x02) != 0;
        inode.isDirty = (flags & 0x04) != 0;

        // (3) timestamps مع تصحيح القيم غير المنطقية
        std::memcpy(&inode.creationTime, buffer + offset, sizeof(inode.creationTime));    offset += sizeof(inode.creationTime);
        std::memcpy(&inode.modificationTime, buffer + offset, sizeof(inode.modificationTime)); offset += sizeof(inode.modificationTime);
        std::memcpy(&inode.lastAccessed, buffer + offset, sizeof(inode.lastAccessed));    offset += sizeof(inode.lastAccessed);

        time_t now = time(nullptr);
        if (inode.creationTime <= 0 || inode.creationTime > now + 3600)        inode.creationTime = now;
        if (inode.modificationTime <= 0 || inode.modificationTime > now + 3600)inode.modificationTime = inode.creationTime;
        if (inode.lastAccessed <= 0 || inode.lastAccessed > now + 3600)        inode.lastAccessed = inode.creationTime;

        // (4) محتوى الدليل لو Directory + Used
        inode.entries.clear();
        if (inode.isDirectory && inode.isUsed && offset < bufferSize) {
            if (offset + sizeof(uint32_t) <= bufferSize) {
                uint32_t count = 0;
                std::memcpy(&count, buffer + offset, sizeof(count));                   offset += sizeof(count);
                for (uint32_t i = 0; i < count && offset < bufferSize; ++i) {
                    if (offset + sizeof(uint16_t) > bufferSize) break;
                    uint16_t nameLen = 0;
                    std::memcpy(&nameLen, buffer + offset, sizeof(nameLen));           offset += sizeof(nameLen);

                    if (offset + nameLen > bufferSize) break;
                    std::string name(nameLen, '\0');
                    if (nameLen) {
                        std::memcpy(&name[0], buffer + offset, nameLen);               offset += nameLen;
                    }

                    if (offset + sizeof(int) > bufferSize) break;
                    int child = -1;
                    std::memcpy(&child, buffer + offset, sizeof(child));               offset += sizeof(child);

                    if (child >= 0) inode.entries.emplace(std::move(name), child);
                }
            }
        }

        // (5) تحقق من الـchecksum إن وُجد
        if (offset + sizeof(uint32_t) <= bufferSize) {
            uint32_t stored = 0;
            std::memcpy(&stored, buffer + offset, sizeof(stored));
            uint32_t calc = CalculateChecksum(copy.data(), offset);
            if (stored != calc) {
                std::cerr << "Checksum mismatch in inode — treating as invalid.\n";
                inode = Inode();
                return 0;
            }
            offset += sizeof(uint32_t);
        }

        return offset;
    }
    catch (...) {
        inode = Inode();
        return 0;
    }
}

uint32_t MiniHSFS::CalculateChecksum(const char* data, size_t length) {
    uint32_t checksum = 0;
    for (size_t i = 0; i < length; ++i) {
        checksum = (checksum << 3) ^ data[i] ^ (checksum >> 29);
    }
    return checksum;
}


void MiniHSFS::ValidateAndRepairInode(int inodeIndex) {
    if (inodeIndex < 0 || static_cast<size_t>(inodeIndex) >= inodeTable.size()) {
        return;
    }

    Inode& inode = inodeTable[inodeIndex];

    // إذا كانت العقدة غير مستخدمة، لا داعي للتحقق
    if (!inode.isUsed) {
        return;
    }

    // إصلاح الطوابع الزمنية غير الصالحة
    time_t currentTime = time(nullptr);
    if (inode.creationTime <= 0 || inode.creationTime > currentTime + 3600) {
        std::cout << "Repairing creation time for inode " << inodeIndex << std::endl;
        inode.creationTime = currentTime;
        inode.isDirty = true;
    }

    if (inode.modificationTime <= 0 || inode.modificationTime > currentTime + 3600) {
        std::cout << "Repairing modification time for inode " << inodeIndex << std::endl;
        inode.modificationTime = inode.creationTime;
        inode.isDirty = true;
    }

    if (inode.lastAccessed <= 0 || inode.lastAccessed > currentTime + 3600) {
        std::cout << "Repairing last accessed time for inode " << inodeIndex << std::endl;
        inode.lastAccessed = inode.creationTime;
        inode.isDirty = true;
    }

    // إذا كان المجلد لا يحتوي على إدخالات ولكن من المفترض أن يكون مجلداً
    if (inode.isDirectory && inode.entries.empty() && inodeIndex != 0) {
        std::cout << "Warning: Directory inode " << inodeIndex << " has no entries" << std::endl;
    }

    // إذا كان الملف يحتوي على كتل ولكن firstBlock غير صالح
    if (!inode.isDirectory && inode.blocksUsed > 0 && inode.firstBlock < 0) {
        std::cout << "Repairing file blocks for inode " << inodeIndex << std::endl;
        inode.blocksUsed = 0;
        inode.firstBlock = -1;
        inode.isDirty = true;
    }

    // إذا كان حجم الملف لا يتطابق مع عدد الكتل
    if (!inode.isDirectory && inode.blocksUsed > 0 &&
        inode.size > static_cast<size_t>(inode.blocksUsed * disk.blockSize)) {
        std::cout << "Adjusting file size for inode " << inodeIndex << std::endl;
        inode.size = inode.blocksUsed * disk.blockSize;
        inode.isDirty = true;
    }

    // إذا تم إصلاح أي شيء، نحفظ التغييرات
    if (inode.isDirty) {
        SaveInodeToDisk(inodeIndex);
    }
}

void MiniHSFS::SaveInodeToDisk(int inodeIndex) {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    if (inodeIndex < 0 || static_cast<size_t>(inodeIndex) >= inodeTable.size())
        throw std::out_of_range("SaveInodeToDisk: invalid inode index " + std::to_string(inodeIndex));

    // حضّر بافر inode ثابت بالحجم inodeSize
    std::vector<char> buf(inodeSize, 0);
    if (SerializeInode(inodeTable[inodeIndex], buf.data(), inodeSize) == 0)
        throw std::runtime_error("SaveInodeToDisk: serialize failed for inode " + std::to_string(inodeIndex));

    // احسب مكانه داخل منطقة inodes (المسافة من بداية منطقة inodes)
    const size_t absByte = static_cast<size_t>(inodeIndex) * inodeSize;
    size_t       startBlockRel = absByte / disk.blockSize;
    size_t       offsetInBlock = absByte % disk.blockSize;

    const size_t bytesToWrite = inodeSize;
    size_t       blocksNeeded = (offsetInBlock + bytesToWrite + disk.blockSize - 1) / disk.blockSize;

    // لو خارج المساحة الحالية → وسّع
    if (startBlockRel + blocksNeeded > static_cast<size_t>(inodeBlocks)) {
        size_t needInodes = ((startBlockRel + blocksNeeded) * disk.blockSize + inodeSize - 1) / inodeSize;
        size_t extra = (needInodes > inodeTable.size()) ? (needInodes - inodeTable.size()) : 0;
        if (extra < 1) extra = 1;
        ExpandInodeAreaByInodes(extra);
    }

    size_t remaining = inodeSize, src = 0;
    for (size_t k = 0; k < blocksNeeded; ++k) {
        uint32_t relBlock = static_cast<uint32_t>(startBlockRel + k);
        uint32_t absBlock = disk.getSystemBlocks() + static_cast<uint32_t>(superBlockBlocks) + relBlock;
        if (absBlock >= disk.totalBlocks())
            throw std::runtime_error("SaveInodeToDisk: absBlock out of range: " + std::to_string(absBlock));

        // اقرأ البلوك، عدّل الجزء الخاص بهذا الـinode، ثم اكتب
        auto diskData = disk.readData(VirtualDisk::Extent(absBlock, 1));
        std::vector<char> blockBuf(disk.blockSize, 0);
        if (!diskData.empty()) {
            std::memcpy(blockBuf.data(), diskData.data(), (std::min)(blockBuf.size(), diskData.size()));
        }

        size_t dest = (k == 0) ? offsetInBlock : 0;
        size_t can = (std::min)(disk.blockSize - dest, remaining);
        std::memcpy(blockBuf.data() + dest, buf.data() + src, can);

        disk.writeData(blockBuf, VirtualDisk::Extent(absBlock, 1), "", true);

        remaining -= can;
        src += can;
    }

    if (remaining != 0)
        throw std::runtime_error("SaveInodeToDisk: incomplete write, remaining bytes = " + std::to_string(remaining));

    inodeTable[inodeIndex].isDirty = false;
}

bool MiniHSFS::ExpandInodeAreaByInodes(size_t extraInodes) {
    if (extraInodes == 0) return true;

    size_t oldCount = inodeTable.size();
    size_t newCount = oldCount + extraInodes;

    size_t oldBlocks = BlocksForInodes(oldCount);
    size_t newBlocks = BlocksForInodes(newCount);

    if (newBlocks > inodeBlocks) {
        size_t addBlocks = newBlocks - inodeBlocks;
        VirtualDisk::Extent ext(disk.getSystemBlocks() + superBlockBlocks + inodeBlocks, addBlocks);
        disk.allocateBlocks(ext.blockCount);

        std::vector<char> zero(disk.blockSize, 0);
        for (size_t i = 0; i < addBlocks; ++i)
            disk.writeData(zero, VirtualDisk::Extent(ext.startBlock + i, 1), "", true);

        inodeBlocks = newBlocks;
    }

    // كبّر الهياكل في الذاكرة
    inodeTable.resize(newCount);
    inodeBitmap.resize(newCount, false);
    inodeCount = newCount;

    UpdateSuperblockForDynamicInodes();
    return true;
}


// تحرير بلوك واحد
void MiniHSFS::FreeBlock(uint32_t blockIndex) {
    std::vector<char> zero(disk.blockSize, 0);
    disk.writeData(zero, VirtualDisk::Extent(blockIndex, 1), "", true);
    try {
        BTreeInsert(rootNodeIndex, static_cast<int>(blockIndex), 0);
    }
    catch (...) {}
    if (freeBlocks < disk.totalBlocks()) ++freeBlocks;
}

bool MiniHSFS::CheckInodeSpace(size_t additionalInodes) {
    size_t currentInodes = inodeTable.size();
    size_t newTotalInodes = currentInodes + additionalInodes;

    size_t currentBlocks = inodeBlocks;
    size_t requiredBlocks = BlocksForInodes(newTotalInodes);

    std::cout << "Space check: Current=" << currentInodes << " inodes (" << currentBlocks
        << " blocks), Need=" << newTotalInodes << " inodes (" << requiredBlocks << " blocks)" << std::endl;

    return requiredBlocks <= currentBlocks;
}

// حجز منطقة متجاورة من البلوكات
bool MiniHSFS::AllocateContiguousRegion(uint32_t start, size_t count) {
    if (start + count > disk.totalBlocks()) return false;

    for (size_t i = 0; i < count; ++i) {
        auto cur = static_cast<uint32_t>(start + i);
        auto data = disk.readData(VirtualDisk::Extent(cur, 1));
        bool allZero = std::all_of(data.begin(), data.end(), [](char c) { return c == 0; });
        if (!allZero) return false;
    }

    for (size_t i = 0; i < count; ++i) {
        uint32_t cur = static_cast<uint32_t>(start + i);
        std::vector<char> zero(disk.blockSize, 0);
        disk.writeData(zero, VirtualDisk::Extent(cur, 1), "", true);
        try { BTreeInsert(rootNodeIndex, static_cast<int>(cur), 1); }
        catch (...) {}
    }

    if (freeBlocks >= count) freeBlocks -= count;
    else freeBlocks = 0;
    return true;
}

std::vector<std::tuple<int, uint32_t, uint32_t>> MiniHSFS::CollectFileExtents() {
    std::vector<std::tuple<int, uint32_t, uint32_t>> out;
    for (size_t i = 1; i < inodeTable.size(); ++i) {
        if (inodeTable[i].isUsed && !inodeTable[i].isDirectory && inodeTable[i].blocksUsed > 0) {
            out.emplace_back(static_cast<int>(i),
                static_cast<uint32_t>(inodeTable[i].firstBlock),
                static_cast<uint32_t>(inodeTable[i].blocksUsed));
        }
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) {
        return std::get<1>(a) < std::get<1>(b);
        });
    return out;
}

void MiniHSFS::MoveInodeExtent(int inodeIndex, uint32_t srcStart, uint32_t dstStart, uint32_t count) {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    if (dstStart > srcStart) {
        for (int k = static_cast<int>(count) - 1; k >= 0; --k) {
            uint32_t s = srcStart + k;
            uint32_t d = dstStart + k;
            auto data = disk.readData(VirtualDisk::Extent(s, 1));
            disk.writeData(data, VirtualDisk::Extent(d, 1), "", true);
            try { BTreeInsert(rootNodeIndex, static_cast<int>(d), 1); }
            catch (...) {}
        }
    }
    else {
        for (uint32_t k = 0; k < count; ++k) {
            uint32_t s = srcStart + k;
            uint32_t d = dstStart + k;
            auto data = disk.readData(VirtualDisk::Extent(s, 1));
            disk.writeData(data, VirtualDisk::Extent(d, 1), "", true);
            try { BTreeInsert(rootNodeIndex, static_cast<int>(d), 1); }
            catch (...) {}
        }
    }

    for (uint32_t k = 0; k < count; ++k) {
        uint32_t s = srcStart + k;
        try { BTreeInsert(rootNodeIndex, static_cast<int>(s), 0); }
        catch (...) {}
        std::vector<char> zero(disk.blockSize, 0);
        disk.writeData(zero, VirtualDisk::Extent(s, 1), "", true);
    }

    inodeTable[inodeIndex].firstBlock = static_cast<int>(dstStart);
    inodeTable[inodeIndex].isDirty = true;
    SaveInodeToDisk(inodeIndex);
}

bool MiniHSFS::DefragmentDataAreaToFreePrefix(size_t neededBlocks) {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);
    if (neededBlocks == 0) return true;

    uint32_t dataStart = dataStartIndex;
    uint32_t total = static_cast<uint32_t>(disk.totalBlocks());
    uint32_t dataBlocks = total > dataStart ? total - dataStart : 0;
    if (dataBlocks <= neededBlocks) return false;

    auto extents = CollectFileExtents();
    if (extents.empty()) {
        return AllocateContiguousRegion(dataStart, neededBlocks);
    }

    uint32_t writePtr = dataStart + dataBlocks;
    std::vector<std::tuple<int, uint32_t, uint32_t>> extCopy = extents;
    std::sort(extCopy.begin(), extCopy.end(), [](auto& a, auto& b) {
        return std::get<1>(a) > std::get<1>(b);
        });

    try {
        for (auto& t : extCopy) {
            int inodeIdx = std::get<0>(t);
            uint32_t srcStart = std::get<1>(t);
            uint32_t cnt = std::get<2>(t);
            if (cnt == 0) continue;

            uint32_t dstStart = writePtr - cnt;
            if (dstStart < dataStart) return false;
            if (dstStart != srcStart) {
                MoveInodeExtent(inodeIdx, srcStart, dstStart, cnt);
            }
            writePtr = dstStart;
        }

        uint32_t freedBlocksAtStart = writePtr - dataStart;
        if (freedBlocksAtStart >= neededBlocks) {
            return AllocateContiguousRegion(dataStart, neededBlocks);
        }
        else {
            return false;
        }
    }
    catch (...) {
        return false;
    }
}

bool MiniHSFS::ExpandInodeAreaInPlace(size_t addInodes) {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    if (addInodes == 0) return true;

    // حساب العدّ القديم والجديد للـ inodes والبلوكات المطلوبة
    size_t oldInodeCount = inodeTable.size();
    size_t newInodeCount = oldInodeCount + addInodes;

    // لا نتجاوز الحد الأقصى الممكن
    size_t maxInodes = CalculateMaxPossibleInodes();
    if (newInodeCount > maxInodes) {
        // لا يمكن التوسيع أكثر من الحد الأقصى
        return false;
    }

    // حساب عدد البلوكات المطلوبة لمخزون الـ inodes الجديد
    size_t oldBlocksNeed = BlocksForInodes(oldInodeCount);
    size_t newBlocksNeed = BlocksForInodes(newInodeCount);

    // إذا البلوكات الحالية تكفي في الذاكرة/القرص، فقط نوسّع الهياكل
    if (newBlocksNeed <= inodeBlocks) {
        try {
            inodeTable.resize(newInodeCount);
            inodeBitmap.resize(newInodeCount, false);
            inodeCount = newInodeCount;
            inodeAreaSize = inodeBlocks * disk.blockSize;
            RebuildFreeInodesList();
            UpdateSuperblockForDynamicInodes();     // حدِّث معلومات السوبر بلوك
            SaveInodeTable();                       // احفظ جدول الـ inodes المحدث
            return true;
        }
        catch (...) {
            return false;
        }
    }

    // نحتاج لبلوكات إضافية على القرص
    size_t blocksDelta = newBlocksNeed - inodeBlocks;

    // نَحاول حجز البلوكات المتجاورة عند بداية منطقة البيانات
    if (!ReserveContiguousBlocksAtDataStart(blocksDelta)) {
        // إذا لم نستطع الحجز نجرّب إلغاء التجزئة لنقل ملفات البيانات وفتح المجال
        if (!DefragmentDataAreaToFreePrefix(blocksDelta)) {
            return false;
        }
        // بعد defrag نحاول الحجز مرة أخرى
        if (!ReserveContiguousBlocksAtDataStart(blocksDelta)) {
            return false;
        }
    }

    // الآن تم حجز البلوكات، حدّث الحسابات الداخلية
    try {
        // زوّد inodeBlocks وحدث مؤشر بداية البيانات (dataStartIndex / dataStartBlock)
        inodeBlocks = static_cast<uint32_t>(newBlocksNeed);
        // dataStartIndex هو موضع بلوكات البيانات بعد جدول الـ inodes
        dataStartIndex = disk.getSystemBlocks() + superBlockBlocks + static_cast<uint32_t>(inodeBlocks) + static_cast<uint32_t>(btreeBlocks);
        // (لو في متغيّر dataStartBlock، حدّثه أيضاً)
        dataStartBlock = static_cast<uint32_t>(disk.getSystemBlocks() + superBlockBlocks + inodeBlocks + btreeBlocks);

        // وسّع هياكل الذاكرة
        inodeTable.resize(newInodeCount);
        inodeBitmap.resize(newInodeCount, false);

        inodeCount = newInodeCount;
        inodeAreaSize = inodeBlocks * disk.blockSize;

        // إعادة بناء قائمة الحُرّين
        RebuildFreeInodesList();

        // حدّث السوبر بلوك بالمعلومات الجديدة
        try {
            SuperblockInfo sb = LoadSuperblock();
            sb.inodeSize = inodeSize;
            sb.totalInodes = static_cast<uint32_t>(inodeCount);
            sb.freeInodes = static_cast<uint32_t>(CountFreeInodes());
            sb.systemSize = static_cast<uint32_t>(dataStartIndex); // أو القيمة المناسبة لديك
            sb.lastWriteTime = time(nullptr);
            SaveSuperblock(sb);
        }
        catch (...) {
            // لو فشل حفظ السوبر بلوك، نحاول على الأقل حفظ جدول الـ inodes ثم نرجع خطأ
        }

        // وأخيراً احفظ جدول الـ inodes فعلياً على القرص
        SaveInodeTable();

        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "ExpandInodeAreaInPlace exception: " << e.what() << std::endl;
        return false;
    }
}

size_t MiniHSFS::BlocksForInodes(size_t inodeCount) {
    if (inodeCount == 0) return 0;

    size_t totalBytes = inodeCount * inodeSize;
    size_t blocksNeeded = (totalBytes + disk.blockSize - 1) / disk.blockSize;

    std::cout << "BlocksForInodes: " << inodeCount << " inodes * " << inodeSize
        << " bytes = " << totalBytes << " bytes = " << blocksNeeded << " blocks" << std::endl;

    return blocksNeeded;
}

bool MiniHSFS::ReserveContiguousBlocksAtDataStart(size_t count) {
    if (count == 0) return true;
    uint32_t start = dataStartBlock;
    uint32_t total = static_cast<uint32_t>(disk.totalBlocks());
    if (start + count > total) return false;

    // تحقق أن البلوكات فارغة
    for (size_t i = 0; i < count; ++i) {
        auto data = disk.readData(VirtualDisk::Extent(start + i, 1));
        bool allZero = std::all_of(data.begin(), data.end(), [](char c) { return c == 0; });
        if (!allZero) return false;
    }

    // حجز البلوكات
    for (size_t i = 0; i < count; ++i) {
        try { BTreeInsert(rootNodeIndex, static_cast<int>(start + i), 1); }
        catch (...) {}
    }
    if (freeBlocks >= count) freeBlocks -= count; else freeBlocks = 0;
    return true;
}

// تصفير inode جديد
void MiniHSFS::ResetInode(int idx, bool isDir) {
    Inode& node = inodeTable[idx];
    node.isUsed = true;
    node.isDirectory = isDir;
    node.size = 0;
    node.blocksUsed = 0;
    node.firstBlock = -1;
    node.entries.clear();
    node.creationTime = time(nullptr);
    node.modificationTime = node.creationTime;
}

// محاولة التوسعة مع إلغاء التجزئة إذا لزم الأمر
bool MiniHSFS::TryExtendInodeTable() {
    size_t requiredBytes = (inodeTable.size() + 1) * inodeSize;
    size_t haveBytes = inodeBlocks * disk.blockSize;
    if (requiredBytes > haveBytes) {
        if (!DefragmentAndExtendInodes(1)) // تنقل البيانات + توسع
            return false;
    }
    inodeTable.emplace_back();
    return true;
}

std::vector<int> MiniHSFS::GetFileBlocks(int inodeIndex) {
    std::vector<int> blocks;
    auto& inode = inodeTable[inodeIndex];
    if (inode.blocksUsed == 0 || inode.firstBlock < 0) return blocks;

    for (int b = 0; b < inode.blocksUsed; ++b) {
        blocks.push_back(inode.firstBlock + b);
    }
    return blocks;
}

void MiniHSFS::UpdateFileBlocks(int inodeIndex, const std::vector<int>& newBlocks) {
    auto& inode = inodeTable[inodeIndex];
    if (newBlocks.empty()) {
        inode.firstBlock = -1;
        inode.blocksUsed = 0;
        return;
    }
    inode.firstBlock = newBlocks[0];
    inode.blocksUsed = static_cast<int>(newBlocks.size());
}

int MiniHSFS::FindFreeBlockFrom(size_t startBlock) {
    for (size_t blk = startBlock; blk < disk.totalBlocks(); ++blk) {
        if (disk.getBitmap()[blk] == false) {
            return static_cast<int>(blk);
        }
    }
    return -1; // مفيش بلوك فاضي
}

bool MiniHSFS::ExpandInodeAreaDirect(size_t additionalBlocks, size_t newTotalInodes) {
    try {
        // 1. حجز الكتل الإضافية
        VirtualDisk::Extent extent(disk.getSystemBlocks() + superBlockBlocks + inodeBlocks, additionalBlocks);
        disk.allocateBlocks(extent.blockCount);

        // 2. تهيئة الكتل الجديدة
        std::vector<char> zeroBlock(disk.blockSize, 0);
        for (size_t i = 0; i < additionalBlocks; ++i) {
            disk.writeData(zeroBlock,
                VirtualDisk::Extent(extent.startBlock + i, 1), "", true);
        }

        // 3. تحديث المتغيرات الداخلية
        inodeBlocks += additionalBlocks;
        dataStartIndex += additionalBlocks;

        // 4. توسيع الجدول في الذاكرة
        inodeTable.resize(newTotalInodes);
        inodeBitmap.resize(newTotalInodes, false);
        inodeCount = newTotalInodes;

        // 5. تحديث السوبر بلوك
        UpdateSuperblockForDynamicInodes();

        std::cout << "Inode area expanded by " << additionalBlocks << " blocks to "
            << newTotalInodes << " inodes" << std::endl;
        return true;

    }
    catch (const std::exception& e) {
        std::cerr << "Error expanding inode area: " << e.what() << std::endl;
        return false;
    }
}

//---------------------------------------------------------//
//---------------------------------------------------------//

void MiniHSFS::Unmount() {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    if (!mounted) {
        throw std::runtime_error("Filesystem not mounted");
    }

    try {
        MiniHSFS::SuperblockInfo info = MiniHSFS::LoadSuperblock();
        info.freeBlocks = disk.freeBlocksCount();
        info.lastMountTime = time(nullptr);
        int count = 0;
        for (int i = 1; i < inodeCount; i++) {
            if (!inodeTable[i].isUsed) {
                count++;
            }
        }
        info.lastWriteTime = (lastTimeWrite != -1) ? lastTimeWrite : info.lastWriteTime;
        info.systemSize = MiniHSFS::dataStartIndex;
        info.freeInodes = CountFreeInodes();
        autoSyncRunning = false;

        if (autoSyncThread.joinable()) {
            autoSyncThread.join();
        }
        SaveSuperblock(info);
        SaveInodeTable();
        SaveBTree();
        SynchronizeDisk();
        btreeCache.clear();
        inodeTable.clear();
        btreeLruList.clear();
        btreeLruMap.clear();
        inodeLruList.clear();
        inodeLruMap.clear();

        mounted = false;
    }
    catch (const std::exception& e) {
        throw std::runtime_error(std::string("Failed to unmount: ") + e.what());
    }

}

////////////////////////////Initialize System

void MiniHSFS::InitializeSuperblock() {
    SuperblockInfo info;
    std::memset(&info, 0, sizeof(info));

    const char* magicStr = "Tomas";
    std::memcpy(info.magic, magicStr, strlen(magicStr));

    info.version = 0x00010000;
    info.blockSize = disk.blockSize;
    info.inodeSize = inodeSize;
    info.systemSize = static_cast<uint32_t>(inodeBlocks) + btreeBlocks + superBlockBlocks + disk.getSystemBlocks();
    info.totalBlocks = disk.totalBlocks();
    info.freeBlocks = static_cast<uint32_t>(info.totalBlocks - (static_cast<uint32_t>(inodeBlocks) + btreeBlocks + superBlockBlocks + disk.getSystemBlocks()));
    info.totalInodes = inodeCount;
    info.freeInodes = inodeCount - 1;
    info.creationTime = time(nullptr);
    info.lastMountTime = info.creationTime;
    info.lastWriteTime = info.creationTime;
    info.state = 1;

    SaveSuperblock(info);
}

//void MiniHSFS::InitializeInodeTable() {
//    inodeTable.clear();
//    inodeTable.resize(inodeCount);
//}

void MiniHSFS::InitializeBTree() {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    rootNodeIndex = AllocateBTreeNode();
    if (rootNodeIndex == -1) {
        throw std::runtime_error("Failed to allocate root B-tree node");
    }

    BTreeNode rootNode(btreeOrder, true);
    btreeCache[rootNodeIndex] = rootNode;

    int currentNode = rootNodeIndex;
    int block = dataStartIndex;

    while (block < disk.totalBlocks()) {
        BTreeNode* node = &btreeCache[currentNode];

        if (node->keyCount == btreeOrder - 1) {
            int newNode = AllocateBTreeNode();
            if (newNode == -1) {
                throw std::runtime_error("No free B-tree nodes available");
            }

            BTreeNode newLeaf(btreeOrder, true);
            newLeaf.nextLeaf = node->nextLeaf;
            node->nextLeaf = newNode;

            SaveBTreeNode(currentNode, *node);
            currentNode = newNode;
            btreeCache[currentNode] = newLeaf;
            continue;
        }

        node->keys[node->keyCount] = block;
        node->values[node->keyCount] = 0;
        node->keyCount++;
        block++;
    }

    SaveBTreeNode(currentNode, btreeCache[currentNode]);

    if (currentNode != rootNodeIndex) {
        SaveBTreeNode(rootNodeIndex, btreeCache[rootNodeIndex]);
    }
}

///////////////////////////////B-Tree Operations

bool MiniHSFS::IsBTreeBlockFree(int index) {
    try {
        auto data = disk.readData(VirtualDisk::Extent{ static_cast<uint32_t>(btreeStartIndex + index), 1 });

        // Empty if all data is Zero
        return std::all_of(data.begin(), data.end(), [](char c) { return c == 0; });
    }
    catch (...) {
        std::cout << "Error XX : Can Not Find Free Blocks!" << std::endl;
        // if Faild empty also
        return true;
    }
}

int MiniHSFS::AllocateBTreeNode() {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    for (int i = 0; i < btreeBlocks; ++i) {
        if (IsBTreeBlockFree(i) && !disk.IsNew()) {

            // initial Block by zero data
            std::vector<char> zeroBuffer(disk.blockSize, 0);
            disk.writeData(
                zeroBuffer,
                VirtualDisk::Extent{ static_cast<uint32_t>(btreeStartIndex + i), 1 },
                "", true
            );
            return i;
        }
        else
        {
            std::vector<char> zeroBuffer(disk.blockSize, 0);
            disk.writeData(
                zeroBuffer,
                VirtualDisk::Extent{ static_cast<uint32_t>(btreeStartIndex + i), 1 },
                "", true
            );
            return i;
        }
    }

    return -1; // Not Blocks Empty
}

void MiniHSFS::FreeBTreeNode(int nodeIndex) {
    if (nodeIndex < 0 || nodeIndex >= btreeBlocks) return;
    BTreeNode emptyNode(btreeOrder);
    SaveBTreeNode(nodeIndex, emptyNode);
    btreeCache.erase(nodeIndex);
    std::vector<char> emptyBlock(disk.blockSize, 0);
    disk.writeData(emptyBlock, VirtualDisk::Extent{ static_cast<uint32_t>(btreeStartIndex + nodeIndex), 1 }, "", true);
}

std::pair<bool, int> MiniHSFS::BTreeFind(int nodeIndex, int key) {

    // Load the current node
    BTreeNode node = LoadBTreeNode(nodeIndex);

    // Constant to define the boundary between linear and binary search
    constexpr int binarySearchThreshold = 16;

    int pos = 0; //Initial position

    // Selecting a search algorithm based on node size
    if (node.keyCount > binarySearchThreshold) {
        // Improved binary search for large nodes
        int left = 0;
        int right = node.keyCount - 1;

        while (left <= right) {
            pos = left + (right - left) / 2;

            if (key == node.keys[pos]) {
                // We found the key
                return { true, node.isLeaf ? node.values[pos] : node.children[pos + 1] };
            }

            if (key < node.keys[pos]) {
                right = pos - 1;
            }
            else {
                left = pos + 1;
            }
        }
        pos = left; // The position where the key would have been inserted
    }
    else {
        // Improved linear search for small nodes
        while (pos < node.keyCount && key > node.keys[pos]) {
            ++pos;
        }

        // Check if we found the key
        if (pos < node.keyCount && key == node.keys[pos]) {
            return { true, node.isLeaf ? node.values[pos] : node.children[pos + 1] };
        }
    }

    // If we get here and don't find the key
    if (node.isLeaf) {
        return { false, -1 }; // Key not found
    }

    // Continue searching for the appropriate child node
    return BTreeFind(node.children[pos], key);
}

bool MiniHSFS::BTreeInsert(int nodeIndex, int key, int value) {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);
    if (value < 0) throw std::invalid_argument("B-tree value cannot be negative");
    BTreeNode node = LoadBTreeNode(nodeIndex);

    if (node.keyCount == btreeOrder - 1) {
        if (nodeIndex == rootNodeIndex) {
            BTreeNode newRoot(btreeOrder, false);
            int newRootIndex = AllocateBTreeNode();
            if (newRootIndex == -1) throw std::runtime_error("Failed to allocate new root node");
            newRoot.children[0] = rootNodeIndex;
            rootNodeIndex = newRootIndex;
            btreeCache[newRootIndex] = newRoot;
        }
        BTreeSplitChild(rootNodeIndex, nodeIndex, 0);
        return BTreeInsertNonFull(rootNodeIndex, key, value);
    }
    return BTreeInsertNonFull(nodeIndex, key, value);
}

bool MiniHSFS::BTreeInsertNonFull(int nodeIndex, int key, int value) {
    BTreeNode node = LoadBTreeNode(nodeIndex);
    int i = node.keyCount - 1;

    // Check if the core exists -> Just set the value and return true
    for (int j = 0; j < node.keyCount; ++j) {
        if (node.keys[j] == key) {
            node.values[j] = value;
            SaveBTreeNode(nodeIndex, node);
            return true;
        }
    }

    if (node.isLeaf) {
        // Insert the key into the correct position ->
        while (i >= 0 && key < node.keys[i]) {
            node.keys[i + 1] = node.keys[i];
            node.values[i + 1] = node.values[i];
            i--;
        }

        node.keys[i + 1] = key;
        node.values[i + 1] = value;
        node.keyCount++;
        SaveBTreeNode(nodeIndex, node);
        return true;
    }
    else {
        // Going down to the right child ->
        while (i >= 0 && key < node.keys[i]) i--;
        i++;

        BTreeNode child = LoadBTreeNode(node.children[i]);

        if (child.keyCount == btreeOrder - 1) {
            BTreeSplitChild(nodeIndex, node.children[i], i);
            // After splitting, the input location may change
            if (key > node.keys[i]) i++;
        }

        return BTreeInsertNonFull(node.children[i], key, value);
    }
}

void MiniHSFS::BTreeSplitChild(int parentIndex, int childIndex, int index) {
    BTreeNode parent = LoadBTreeNode(parentIndex);
    BTreeNode child = LoadBTreeNode(childIndex);
    BTreeNode newNode(btreeOrder, child.isLeaf);

    int newNodeIndex = AllocateBTreeNode();
    if (newNodeIndex == -1) throw std::runtime_error("No space for new B-tree node");

    int t = (btreeOrder - 1) / 2;
    newNode.keyCount = t;
    for (int j = 0; j < t; j++) {
        newNode.keys[j] = child.keys[j + t + 1];
        if (child.isLeaf) newNode.values[j] = child.values[j + t + 1];
    }

    if (!child.isLeaf) {
        for (int j = 0; j <= t; j++) {
            newNode.children[j] = child.children[j + t + 1];
        }
    }
    else {
        newNode.nextLeaf = child.nextLeaf;
        child.nextLeaf = newNodeIndex;
    }

    child.keyCount = t;

    for (int j = parent.keyCount; j > index; j--) {
        parent.children[j + 1] = parent.children[j];
        parent.keys[j] = parent.keys[j - 1];
    }
    parent.children[index + 1] = newNodeIndex;
    parent.keys[index] = child.keys[t];
    parent.keyCount++;

    SaveBTreeNode(parentIndex, parent);
    SaveBTreeNode(childIndex, child);
    SaveBTreeNode(newNodeIndex, newNode);
}

bool MiniHSFS::BTreeDelete(int nodeIndex, int key) {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);  // simultaneous protection

    try {
        BTreeNode node = LoadBTreeNode(nodeIndex);
        int idx = 0;

        // Find the key inside the node
        while (idx < node.keyCount && key > node.keys[idx]) {
            idx++;
        }

        // Case 1: The key is inside the current node
        if (idx < node.keyCount && node.keys[idx] == key) {
            bool success;
            if (node.isLeaf)
                success = BTreeDeleteFromLeaf(nodeIndex, idx);
            else
                success = BTreeDeleteFromNonLeaf(nodeIndex, idx);

            if (success) {
                node.isDirty = true;
                SaveBTreeNode(nodeIndex, node);
            }

            return success;
        }

        // Case 2: The key does not exist and this is a leaf node
        if (node.isLeaf) {
            return false;  // Key not found
        }

        // Case 3: The key is in one of the children
        bool atEnd = (idx == node.keyCount);
        int childIndex = node.children[atEnd ? idx - 1 : idx];

        // Download the target child
        BTreeNode child = LoadBTreeNode(childIndex);

        // If the child has fewer keys than the minimum, fill in
        if (child.keyCount < (btreeOrder / 2)) {
            BTreeFill(nodeIndex, idx); // May be combined or borrowed from neighbors

            // Reload node and child after modification
            node = LoadBTreeNode(nodeIndex);
            childIndex = node.children[atEnd ? idx - 1 : idx];
        }

        // Follow-up deletion within the child
        return BTreeDelete(childIndex, key);
    }
    catch (const std::exception& e) {
        std::cerr << "BTreeDelete Exception: " << e.what() << "\n";
        return false;
    }
}

bool MiniHSFS::BTreeMergeChildren(int parentIndex, int index) {
    try {
        BTreeNode parent = LoadBTreeNode(parentIndex);
        int leftIndex = parent.children[index];
        int rightIndex = parent.children[index + 1];

        BTreeNode left = LoadBTreeNode(leftIndex);
        BTreeNode right = LoadBTreeNode(rightIndex);

        // Add the parent key between the two nodes
        left.keys[left.keyCount] = parent.keys[index];
        if (left.isLeaf)
            left.values[left.keyCount] = right.values[0]; // Or keep it as designed
        left.keyCount++;

        // Copy the right child's keys
        for (int i = 0; i < right.keyCount; ++i) {
            left.keys[left.keyCount + i] = right.keys[i];
            if (left.isLeaf)
                left.values[left.keyCount + i] = right.values[i];
        }

        if (!left.isLeaf) {
            for (int i = 0; i <= right.keyCount; ++i)
                left.children[left.keyCount + i] = right.children[i];
        }
        else {
            // B+ Tree Support
            left.nextLeaf = right.nextLeaf;
        }

        left.keyCount += right.keyCount;

        // Transferring keys and children to the father
        for (int i = index + 1; i < parent.keyCount; ++i)
            parent.keys[i - 1] = parent.keys[i];
        for (int i = index + 2; i <= parent.keyCount; ++i)
            parent.children[i - 1] = parent.children[i];

        parent.keyCount--;

        SaveBTreeNode(leftIndex, left);
        SaveBTreeNode(parentIndex, parent);
        FreeBTreeNode(rightIndex);

        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Merge failed: " << e.what() << "\n";
        return false;
    }
}

void MiniHSFS::BTreeFill(int nodeIndex, int index) {
    BTreeNode node = LoadBTreeNode(nodeIndex);
    if (index > 0 && LoadBTreeNode(node.children[index - 1]).keyCount >= btreeOrder / 2) {
        BTreeBorrowFromLeft(nodeIndex, index);
    }
    else if (index < node.keyCount && LoadBTreeNode(node.children[index + 1]).keyCount >= btreeOrder / 2) {
        BTreeBorrowFromRight(nodeIndex, index);
    }
    else {
        BTreeMergeChildren(nodeIndex, index == node.keyCount ? index - 1 : index);
    }
}

void MiniHSFS::BTreeBorrowFromLeft(int nodeIndex, int index) {
    BTreeNode parent = LoadBTreeNode(nodeIndex);
    BTreeNode child = LoadBTreeNode(parent.children[index]);
    BTreeNode left = LoadBTreeNode(parent.children[index - 1]);

    for (int i = child.keyCount - 1; i >= 0; i--) {
        child.keys[i + 1] = child.keys[i];
        if (child.isLeaf) child.values[i + 1] = child.values[i];
    }
    if (!child.isLeaf) {
        for (int i = child.keyCount; i >= 0; i--) child.children[i + 1] = child.children[i];
        child.children[0] = left.children[left.keyCount];
    }

    child.keys[0] = parent.keys[index - 1];
    if (child.isLeaf) child.values[0] = left.values[left.keyCount - 1];
    parent.keys[index - 1] = left.keys[left.keyCount - 1];

    child.keyCount++;
    left.keyCount--;

    SaveBTreeNode(nodeIndex, parent);
    SaveBTreeNode(parent.children[index], child);
    SaveBTreeNode(parent.children[index - 1], left);
}

void MiniHSFS::BTreeBorrowFromRight(int nodeIndex, int index) {
    BTreeNode parent = LoadBTreeNode(nodeIndex);
    BTreeNode child = LoadBTreeNode(parent.children[index]);
    BTreeNode right = LoadBTreeNode(parent.children[index + 1]);

    child.keys[child.keyCount] = parent.keys[index];
    if (child.isLeaf) child.values[child.keyCount] = right.values[0];
    if (!child.isLeaf) child.children[child.keyCount + 1] = right.children[0];

    parent.keys[index] = right.keys[0];

    for (int i = 1; i < right.keyCount; i++) {
        right.keys[i - 1] = right.keys[i];
        if (right.isLeaf) right.values[i - 1] = right.values[i];
    }
    if (!right.isLeaf) {
        for (int i = 1; i <= right.keyCount; i++) right.children[i - 1] = right.children[i];
    }

    child.keyCount++;
    right.keyCount--;

    SaveBTreeNode(nodeIndex, parent);
    SaveBTreeNode(parent.children[index], child);
    SaveBTreeNode(parent.children[index + 1], right);
}

bool MiniHSFS::BTreeDeleteFromLeaf(int nodeIndex, int index) {
    try {
        BTreeNode node = LoadBTreeNode(nodeIndex);

        if (index < 0 || index >= node.keyCount) {
            throw std::out_of_range("Invalid index in BTreeDeleteFromLeaf");
        }

        for (int i = index + 1; i < node.keyCount; ++i) {
            node.keys[i - 1] = node.keys[i];
            node.values[i - 1] = node.values[i];  // No need for isLeaf
        }

        node.keyCount--;
        node.isDirty = true;
        SaveBTreeNode(nodeIndex, node);

        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Error in BTreeDeleteFromLeaf: " << e.what() << std::endl;
        return false;
    }
}

bool MiniHSFS::BTreeDeleteFromNonLeaf(int nodeIndex, int index) {
    try {
        BTreeNode node = LoadBTreeNode(nodeIndex);

        // Input validation
        if (index < 0 || index >= node.keyCount) {
            throw std::out_of_range("Invalid index in BTreeDeleteFromNonLeaf");
        }

        int key = node.keys[index];

        // Case 1: The left child has enough keys.
        BTreeNode leftChild = LoadBTreeNode(node.children[index]);
        if (leftChild.keyCount >= (btreeOrder + 1) / 2) {
            int predecessor = BTreeGetPredecessor(node.children[index]);
            node.keys[index] = predecessor;
            node.isDirty = true;
            SaveBTreeNode(nodeIndex, node);
            return BTreeDelete(node.children[index], predecessor);
        }

        // Case 2: The right child has enough keys.
        BTreeNode rightChild = LoadBTreeNode(node.children[index + 1]);
        if (rightChild.keyCount >= (btreeOrder + 1) / 2) {
            int successor = BTreeGetSuccessor(node.children[index + 1]);
            node.keys[index] = successor;
            node.isDirty = true;
            SaveBTreeNode(nodeIndex, node);
            return BTreeDelete(node.children[index + 1], successor);
        }

        // Case 3: Required Merger
        if (!BTreeMergeChildren(nodeIndex, index)) {
            throw std::runtime_error("Failed to merge children");
        }

        // Reload node after merge
        node = LoadBTreeNode(nodeIndex);
        return BTreeDelete(node.children[index], key);

    }
    catch (const std::exception& e) {
        std::cerr << "Error in BTreeDeleteFromNonLeaf: " << e.what() << std::endl;
        return false;
    }
}

int MiniHSFS::BTreeGetPredecessor(int nodeIndex) {
    BTreeNode node = LoadBTreeNode(nodeIndex);
    while (!node.isLeaf) node = LoadBTreeNode(node.children[node.keyCount]);
    return node.keys[node.keyCount - 1];
}

int MiniHSFS::BTreeGetSuccessor(int nodeIndex) {
    BTreeNode node = LoadBTreeNode(nodeIndex);
    while (!node.isLeaf) node = LoadBTreeNode(node.children[0]);
    return node.keys[0];
}

/////////////////////////////////Load and Save Tables

MiniHSFS::SuperblockInfo MiniHSFS::LoadSuperblock() {
    std::vector<char> data = disk.readData(
        VirtualDisk::Extent{ static_cast<uint32_t>(superBlockIndex), static_cast<uint32_t>(superBlockBlocks) });

    SuperblockInfo info;
    std::memcpy(&info, data.data(), sizeof(SuperblockInfo));

    return info;
}

void MiniHSFS::SaveSuperblock(const SuperblockInfo& info) {
    std::vector<char> data(superBlockBlocks * disk.blockSize, 0);

    std::memcpy(data.data(), &info, sizeof(SuperblockInfo));
    disk.writeData(data,
        VirtualDisk::Extent{ static_cast<uint32_t>(superBlockIndex), static_cast<uint32_t>(superBlockBlocks) }, "", false);

}

//void MiniHSFS::LoadInodeTable() {
//    std::lock_guard<std::recursive_mutex> lock(fsMutex);
//    try {
//        InitializeInodeTable();
//
//        std::vector<char> inode_data(inodeBlocks * disk.blockSize);
//
//        for (size_t block = 0; block < inodeBlocks; ++block) {
//            auto data = disk.readData(VirtualDisk::Extent(disk.getSystemBlocks() + static_cast<uint32_t>(MiniHSFS::superBlockBlocks) + static_cast<uint32_t>(block), 1));
//            std::copy(data.begin(), data.end(), inode_data.begin() + block * disk.blockSize);
//        }
//
//        for (size_t i = 0; i < inodeCount; ++i) {
//            DeserializeInode(inodeTable[i], &inode_data[i * inodeSize], inodeSize);
//        }
//    }
//    catch (const std::bad_alloc&) {
//        std::cerr << "Memory pressure detected during inode loading. Flushing cache!" << std::endl;
//        SaveBTree(); // Save BTree
//        // Clean Caches
//        inodeTable.clear();
//        inodeLruMap.clear();
//        inodeLruList.clear();
//
//        throw;
//    }
//}
//
//void MiniHSFS::SaveInodeTable() {
//    std::lock_guard<std::recursive_mutex> lock(fsMutex);
//    std::vector<char> inode_data(inodeBlocks * disk.blockSize, 0);
//
//    for (size_t i = 0; i < inodeCount; ++i) {
//        SerializeInode(inodeTable[i], &inode_data[i * inodeSize], inodeSize);
//    }
//
//    for (size_t block = 0; block < inodeBlocks; ++block) {
//        std::vector<char> block_data(inode_data.begin() + block * disk.blockSize,
//            inode_data.begin() + (block + 1) * disk.blockSize);
//        disk.writeData(block_data, VirtualDisk::Extent(disk.getSystemBlocks() + static_cast<uint32_t>(MiniHSFS::superBlockBlocks) + static_cast<uint32_t>(block), 1), "", true);
//    }
//}

void MiniHSFS::LoadBTree() {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    try {
        // Load root data from disk
        auto rootData = disk.readData(VirtualDisk::Extent{ static_cast<uint32_t>(btreeStartIndex + rootNodeIndex), 1 });

        // Use the correct btreeOrder in the configuration
        BTreeNode rootNode(btreeOrder);
        DeserializeBTreeNode(rootNode, rootData.data());

        // Store root in cache
        btreeCache[rootNodeIndex] = std::move(rootNode);
        TouchBTreeNode(rootNodeIndex);
    }
    catch (...) {
        // If the upload fails, rebuild the tree from scratch.
        InitializeBTree();
    }
}

void MiniHSFS::SaveBTree() {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);
    for (auto& entry : btreeCache) {
        if (entry.second.isDirty) {
            SaveBTreeNode(entry.first, entry.second);
            entry.second.isDirty = false;
        }
    }
}

MiniHSFS::BTreeNode MiniHSFS::LoadBTreeNode(int nodeIndex) {
    try {
        if (nodeIndex < 0 || nodeIndex >= btreeBlocks) {
            throw std::out_of_range("Invalid B-tree node index");
        }

        auto it = btreeCache.find(nodeIndex);
        if (it != btreeCache.end()) {
            it->second.accessCount++;
            TouchBTreeNode(nodeIndex);
            return it->second;
        }

        // Load node data from disk
        auto nodeData = disk.readData(
            VirtualDisk::Extent{ static_cast<uint32_t>(btreeStartIndex + nodeIndex), 1 });

        BTreeNode node(btreeOrder);
        DeserializeBTreeNode(node, nodeData.data());

        node.accessCount = 1;
        btreeCache[nodeIndex] = node;
        TouchBTreeNode(nodeIndex);

        btreeLoadCounter++;
        if (btreeLoadCounter >= 100) {
            for (auto& entry : btreeCache) {
                entry.second.accessCount /= 2;
            }
            btreeLoadCounter = 0;
        }

        return node;
    }
    catch (const std::bad_alloc&) {
        std::cerr << "!! Memory pressure detected during BTree node load. Clearing BTree cache.\n";
        btreeCache.clear();
        btreeLruMap.clear();
        btreeLruList.clear();
        throw;
    }
}

void MiniHSFS::SaveBTreeNode(int nodeIndex, const BTreeNode& node) {
    if (nodeIndex < 0 || nodeIndex >= btreeBlocks) {
        throw std::out_of_range("Invalid B-tree node index");
    }

    std::vector<char> buffer(disk.blockSize, 0);
    SerializeBTreeNode(node, buffer.data());

    disk.writeData(buffer,
        VirtualDisk::Extent{ static_cast<uint32_t>(btreeStartIndex + nodeIndex), 1 },
        "", true);

    // Update the cache without resetting the entire node
    auto it = btreeCache.find(nodeIndex);
    if (it != btreeCache.end()) {
        it->second.isDirty = false;
    }
    else {
        btreeCache[nodeIndex] = node;
        btreeCache[nodeIndex].isDirty = false;
    }

    TouchBTreeNode(nodeIndex);
}

/////////////////////////////File System Operations

int MiniHSFS::FindFile(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);
    if (!mounted) throw std::runtime_error("Filesystem not mounted");

    // Fast path for root directory
    if (path == "/") return 0;

    ValidatePath(path);  // Check Right Path
    const auto& components = SplitPath(path);
    if (components.empty()) return 0;

    int currentInode = 0;
    for (const auto& component : components) {
        if (!inodeTable[currentInode].isDirectory)
            return -1;

        auto it = inodeTable[currentInode].entries.find(component);
        if (it == inodeTable[currentInode].entries.end())
            return -1;

        currentInode = it->second;
    }

    return currentInode;
}

std::vector<std::string> MiniHSFS::SplitPath(const std::string& path) const {
    std::vector<std::string> components;
    if (path.empty() || path == "/") return components;

    const char* start = path.data() + 1; // Skip leading '/'
    const char* end = path.data() + path.size();

    while (start < end) {
        const char* slash = std::find(start, end, '/');
        components.emplace_back(start, slash);
        start = slash + (slash != end);
    }

    return components;
}

void MiniHSFS::ValidatePath(const std::string& path) {
    if (path.empty()) {
        throw std::invalid_argument("Path cannot be empty");
    }

    if (path.length() > maxPathLength) {
        throw std::invalid_argument("Path too long");
    }

    if (path[0] != '/') {
        throw std::invalid_argument("Path must be absolute");
    }
}

int MiniHSFS::PathToInode(const std::vector<std::string>& path) {
    int currentInode = 0; // Start at root

    for (const auto& component : path) {
        if (!inodeTable[currentInode].isDirectory) {
            return -1; // Not a directory
        }

        auto it = inodeTable[currentInode].entries.find(component);
        if (it == inodeTable[currentInode].entries.end()) {
            return -1; // Component not found
        }

        currentInode = it->second;
    }

    return currentInode;
}

/////////////////////////////Printing Operation

void MiniHSFS::PrintSuperblockInfo() {
    SuperblockInfo info = LoadSuperblock();

    auto printField = [](const std::string& label, const std::string& value) {
        std::cout << "\033[1m\033[34m" << label << ":\033[0m " << "\033[32m" << value << "\033[0m\n";
        };

    printField("Filesystem Magic", std::string(info.magic, strnlen(info.magic, sizeof(info.magic))));

    std::ostringstream version;
    version << (info.version >> 16) << "."
        << ((info.version >> 8) & 0xFF) << "."
        << (info.version & 0xFF);
    printField("Version", version.str());
    printField("System Blocks Total", std::to_string(info.systemSize));
    printField("Block Size", std::to_string(info.blockSize));
    printField("Inode Size", std::to_string(info.inodeSize));
    printField("Total Blocks", std::to_string(info.totalBlocks));
    printField("Free Blocks", std::to_string(info.freeBlocks));
    printField("Total Inodes", std::to_string(info.totalInodes));
    printField("Free Inodes", std::to_string(info.freeInodes));

    char buffer[26];
#ifdef _WIN32
    ctime_s(buffer, sizeof(buffer), &info.creationTime);
#else
    ctime_r(&info.creationTime, buffer);
#endif
    printField("Created", std::string(buffer));
#ifdef _WIN32
    ctime_s(buffer, sizeof(buffer), &info.lastMountTime);
#else
    ctime_r(&info.lastMountTime, buffer);
#endif
    printField("Last Mount", std::string(buffer));
#ifdef _WIN32
    ctime_s(buffer, sizeof(buffer), &info.lastWriteTime);
#else
    ctime_r(&info.lastWriteTime, buffer);
#endif
    printField("Last Write", std::string(buffer));

    printField("State", std::to_string(info.state));
}

void MiniHSFS::PrintBTreeStructure() {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    if (!mounted) {
        std::cout << "\033[1m\033[31mFilesystem not mounted\033[0m\n";
        return;
    }

    std::cout << "\n\033[1m\033[34mB-Tree Structure (Root: " << rootNodeIndex << ")\033[0m\n";
    std::cout << "\033[34m----------------------------------------\033[0m\n";

    // Map to track visited nodes
    std::unordered_set<int> visitedNodes;

    //Defining the NodeInfo structure inside a function
    struct NodeInfo {
        int index;
        int level;
        bool from_next_leaf;
    };

    std::deque<NodeInfo> nodes;
    nodes.push_back({ rootNodeIndex, 0, false });

    while (!nodes.empty()) {
        NodeInfo current = nodes.front();
        nodes.pop_front();

        // Skip if this node has already been visited
        if (visitedNodes.count(current.index)) {
            continue;
        }
        visitedNodes.insert(current.index);

        try {
            BTreeNode node = LoadBTreeNode(current.index);

            // Indents by level
            for (int i = 0; i < current.level; i++) {
                std::cout << (i == current.level - 1 ? "\033[90m|-- " : "\033[90m|   ");
            }

            // Node information
            std::cout << "\033[1m\033[36m[" << current.index << "] "
                << (node.isLeaf ? "\033[32mLeaf\033[0m" : "\033[33mNode\033[0m")
                << " (" << node.keyCount << " keys)\033[0m: ";

            // Print keys and values
            for (int i = 0; i < node.keyCount; i++) {
                std::cout << "\033[35m" << node.keys[i] << "\033[0m";
                if (node.isLeaf) {
                    std::cout << (node.values[i] ? "\033[92m(U)\033[0m" : "\033[90m(F)\033[0m");
                }
                if (i < node.keyCount - 1) std::cout << ", ";
            }

            // Print children's indicators for internal nodes
            if (!node.isLeaf) {
                std::cout << " \033[34m[Children: ";
                for (int i = 0; i <= node.keyCount; i++) {
                    if (node.children[i] != -1) {
                        std::cout << node.children[i];
                        if (i < node.keyCount) std::cout << ", ";
                    }
                }
                std::cout << "]\033[0m";
            }

            // Print the next sheet index if present
            if (node.isLeaf && node.nextLeaf != -1) {
                std::cout << " \033[90m-> Next: " << node.nextLeaf << "\033[0m";
            }

            std::cout << std::endl;

            // Add contract to waiting list
            if (!node.isLeaf && !current.from_next_leaf) {
                // For internal nodes: Add children
                for (int i = node.keyCount; i >= 0; i--) {
                    if (node.children[i] != -1) {
                        nodes.push_front({ node.children[i], current.level + 1, false });
                    }
                }
            }
            else if (node.isLeaf && node.nextLeaf != -1) {
                // For paper knots: Follow the chain
                nodes.push_back({ node.nextLeaf, current.level, true });
            }
        }
        catch (const std::exception& e) {
            std::cout << "\033[1m\033[31mError loading node " << current.index
                << ": " << e.what() << "\033[0m\n";
        }
    }

    std::cout << "\033[34m----------------------------------------\033[0m\n";
    std::cout << "Total nodes visited: " << visitedNodes.size() << std::endl;
}

/////////////////////////////Helper Function

int MiniHSFS::FindFreeBlock() {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    auto searchFreeBlock = [this]() -> int {
        int currentLeaf = rootNodeIndex;

        // Move to the first leaf node
        while (!btreeCache[currentLeaf].isLeaf) {
            currentLeaf = btreeCache[currentLeaf].children[0];
        }

        // Search paper nodes
        while (currentLeaf != -1) {
            BTreeNode& leaf = btreeCache[currentLeaf];

            for (int i = 0; i < leaf.keyCount; ++i) {
                if (leaf.values[i] == 0) {  //Free block
                    int block = leaf.keys[i];

                    // Check block scope and grouping preference
                    if (block >= dataStartIndex) {
                        // Prefer contiguous blocks (optimize for fragmentation)
                        if (i + 1 < leaf.keyCount && leaf.values[i + 1] == 0) {
                            return leaf.keys[i + 1];  // Return the next block to help with contiguous allocation
                        }
                        return block;
                    }
                }
            }
            currentLeaf = leaf.nextLeaf;  // Move to the next leaf node
        }
        return -1;  // No free blocks found
        };

    //First attempt
    int freeBlock = searchFreeBlock();
    if (freeBlock != -1) return freeBlock;

    // If there are no free blocks, perform defragmentation.
    DefragmentDisk();

    // Second attempt after defragmentation
    return searchFreeBlock();
}

void MiniHSFS::MarkBlockUsed(int blockIndex) {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    // Quick check of cache first
    auto rootIt = btreeCache.find(rootNodeIndex);
    if (rootIt != btreeCache.end()) {
        BTreeNode& rootNode = rootIt->second;

        // Fast binary search in the root node
        auto it = std::lower_bound(rootNode.keys, rootNode.keys + rootNode.keyCount, blockIndex);
        int pos = it - rootNode.keys;

        if (pos < rootNode.keyCount && rootNode.keys[pos] == blockIndex) {
            if (rootNode.values[pos] != 1) {
                rootNode.values[pos] = 1;
                rootNode.isDirty = true;
            }
            return;  // Updated successfully
        }
    }
    // If not in root, use global insertion
    if (!BTreeInsert(rootNodeIndex, blockIndex, 1)) {
        throw std::runtime_error("Failed to mark block as used");
    }
}

void MiniHSFS::MarkBlocksUsed(const VirtualDisk::Extent& extent) {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    // Helper functions
    auto logError = [](const std::string& msg) {
        std::cerr << "\n[ERROR] " << msg << std::endl;
        };

    auto showProgress = [](int processed, int total) {
        int percent = static_cast<int>((processed * 100.0) / total);
        std::cerr << "\rMarking blocks: " << percent << "% ("
            << processed << "/" << total << ")" << std::flush;
        };

    // Validate input
    if (extent.startBlock < dataStartIndex || extent.startBlock + extent.blockCount > disk.totalBlocks()) {
        std::ostringstream msg;
        msg << "Invalid block range [" << extent.startBlock << ", "
            << extent.startBlock + extent.blockCount << "]";
        logError(msg.str());
        throw std::invalid_argument(msg.str());
    }

    const int totalBlocks = extent.blockCount;
    int processed = 0;
    bool hasErrors = false;

    // Processing large batches
    constexpr int batchThreshold = 2;
    if (totalBlocks >= batchThreshold) {
        try {
            int currentBlock = extent.startBlock;
            int remaining = totalBlocks;
            int lastUsedNodeIndex = -1;
            BTreeNode lastUsedNode(btreeOrder);
            bool hasLastNode = false;

            while (remaining > 0) {
                // Find the appropriate node
                int nodeIndex = rootNodeIndex;
                BTreeNode node;

                if (hasLastNode && currentBlock > lastUsedNode.keys[0] &&
                    currentBlock < lastUsedNode.keys[lastUsedNode.keyCount - 1]) {
                    nodeIndex = lastUsedNodeIndex;
                    node = lastUsedNode;
                }
                else {
                    int cur = rootNodeIndex;
                    node = LoadBTreeNode(cur);

                    while (!node.isLeaf) {
                        int i = 0;
                        while (i < node.keyCount && currentBlock > node.keys[i]) i++;
                        cur = node.children[i];
                        node = LoadBTreeNode(cur);
                    }
                    nodeIndex = cur;
                }

                // Insert blocks into the node
                int available = (btreeOrder - 1) - node.keyCount;
                int blocksToInsert = (std::min)(remaining, available);

                for (int i = 0; i < blocksToInsert; ++i) {
                    int key = currentBlock;
                    int pos = 0;

                    // Find the right position
                    while (pos < node.keyCount && key > node.keys[pos]) pos++;

                    if (pos < node.keyCount&& node.keys[pos] == key) {
                        node.values[pos] = 1; // Update an existing block
                    }
                    else {
                        // Insert a new block
                        for (int j = node.keyCount; j > pos; --j) {
                            node.keys[j] = node.keys[j - 1];
                            node.values[j] = node.values[j - 1];
                        }
                        node.keys[pos] = key;
                        node.values[pos] = 1;
                        node.keyCount++;
                    }

                    currentBlock++;
                    remaining--;
                    processed++;
                    showProgress(processed, totalBlocks);
                }

                node.isDirty = true;
                SaveBTreeNode(nodeIndex, node);

                lastUsedNode = node;
                lastUsedNodeIndex = nodeIndex;
                hasLastNode = true;

                if (remaining > 0 && node.keyCount == btreeOrder - 1) {
                    if (node.nextLeaf == -1) break;
                }
            }
        }
        catch (const std::exception& e) {
            logError(std::string("Batch processing failed: ") + e.what());
            hasErrors = true;
        }
    }

    // Process the remaining blocks individually
    if (processed < totalBlocks) {
        for (uint32_t i = processed; i < totalBlocks; ++i) {
            try {
                BTreeInsert(rootNodeIndex, extent.startBlock + i, 1);
                processed++;
                showProgress(processed, totalBlocks);
            }
            catch (const std::exception& e) {
                logError("Failed to mark block " + std::to_string(extent.startBlock + i) +
                    ": " + e.what());
                hasErrors = true;
            }
        }
    }
    std::cout << "" << std::endl;
}

int MiniHSFS::GetInodeIndex(const Inode& inode) const {
    for (int i = 0; i < inodeTable.size(); ++i) {
        if (&inodeTable[i] == &inode) {
            return i;
        }
    }
    throw std::runtime_error("Inode not found in inodeTable");
}

bool MiniHSFS::FreeFileBlocks(Inode& inode) {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    if (!inode.isUsed || inode.firstBlock == -1 || inode.blocksUsed == 0)
        return true;  //No need to free

    try {
        for (int i = 0; i < inode.blocksUsed; ++i) {
            int block = inode.firstBlock + i;

            if (block >= 0 && block < Disk().totalBlocks()) {
                if (Disk().getBitmap()[block]) {
                    Disk().setBitmap(block, false);
                }

                BTreeDelete(rootNodeIndex, block);
            }
        }

        // Remove blocks from the disk
        disk.freeBlocks(VirtualDisk::Extent(inode.firstBlock, inode.blocksUsed));

        // Update the inode
        inode.firstBlock = -1;
        inode.blocksUsed = 0;
        inode.isDirty = true;
        UpdateInodeTimestamps(GetInodeIndex(inode), true);
        SaveInodeToDisk(GetInodeIndex(inode));

        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Error freeing file blocks: " << e.what() << "\n";
        return false;
    }
}

bool MiniHSFS::IsBlockUsed(int blockIndex) {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);
    auto result = BTreeFind(rootNodeIndex, blockIndex);
    return result.first && result.second == 1;
}

void MiniHSFS::ValidateInode(int inodeIndex, bool checkDirectory) {
    // Check the validity of the inode number
    if (inodeIndex < 0 || inodeIndex >= inodeCount) {
        throw std::out_of_range("Invalid inode index");
    }

    // Check if inode is in use
    if (!inodeTable[inodeIndex].isUsed) {
        throw std::runtime_error("Inode not in use");
    }

    // If checkDirectory is enabled, we check if the inode represents a directory.
    if (checkDirectory && !inodeTable[inodeIndex].isDirectory) {
        throw std::runtime_error("Not a directory");
    }
}

void MiniHSFS::UpdateInodeTimestamps(int inodeIndex, bool modify) {
    if (inodeIndex < 0 || inodeIndex >= inodeCount) return;

    time_t now = time(nullptr);
    inodeTable[inodeIndex].lastAccessed = now;

    if (modify) {
        inodeTable[inodeIndex].modificationTime = now;
    }
    else if (inodeTable[inodeIndex].creationTime == 0) {
        inodeTable[inodeIndex].creationTime = now;
        inodeTable[inodeIndex].modificationTime = now;
    }

    inodeTable[inodeIndex].isDirty = true;
}

////////////////////////////Calculation Operation

void MiniHSFS::calculatePercentage(double inodePercentage, double btreePercentage)
{
    this->inodePercentage = inodePercentage;
    this->btreePercentage = btreePercentage;
}

size_t MiniHSFS::calculateInodeCount() {
    if (inodePercentage > 1)
        return inodePercentage;

    size_t diskSizeBytes = disk.totalBlocks() * disk.blockSize;

    double inodeAreaSize = static_cast<double>(diskSizeBytes) * inodePercentage;
    size_t inodeCount = static_cast<size_t>(inodeAreaSize / inodeSize);

    return std::max<size_t>(inodeCount, 32);//Minimum 32 blocks
}

size_t MiniHSFS::calculateInodeBlocks() {
    return static_cast<size_t>(std::ceil(static_cast<double>(inodeCount * inodeSize) / disk.blockSize));
}

size_t MiniHSFS::calculateBTreeBlocks() {
    if (btreePercentage > 1)
        return btreePercentage;

    size_t totalBlocks = disk.totalBlocks();
    size_t suggested = static_cast<size_t>(std::ceil(totalBlocks * btreePercentage));
    return std::max<size_t>(suggested, 16); //Minimum 16 blocks
}

size_t MiniHSFS::calculateBTreeOrder() {
    return (disk.blockSize - (sizeof(bool) + sizeof(int) * 2)) / (sizeof(int) * 2);
}

//size_t MiniHSFS::Inode::calculateSerializedSize() const {
//    size_t total = sizeof(size) + sizeof(blocksUsed) + sizeof(firstBlock) +
//        sizeof(isDirectory) + sizeof(isUsed) +
//        sizeof(creationTime) + sizeof(modificationTime);
//
//    if (isDirectory) {
//        total += sizeof(uint16_t); // entry count
//        for (const auto& entry : entries) {
//            const std::string& name = entry.first;
//            total += sizeof(uint8_t) + name.size() + sizeof(int);
//        }
//    }
//
//    return total;
//}

//size_t MiniHSFS::CountFreeInodes() {
//    size_t count = 0;
//    for (const auto& inode : inodeTable) {
//        if (!inode.isUsed) {
//            count++;
//        }
//    }
//    return count;
//}

size_t MiniHSFS::getAvailableMemory() {
    return disk.getAvailableMemory();
}

//////////////////////////////Convertion Operation

//void MiniHSFS::SerializeInode(const Inode& inode, char* buffer, size_t bufferSize) {
//    size_t offset = 0;
//    auto write = [&](const void* data, size_t size) {
//        if (offset + size > bufferSize) throw std::runtime_error("Buffer overflow");
//        std::memcpy(buffer + offset, data, size);
//        offset += size;
//        };
//
//    write(&inode.size, sizeof(inode.size));
//    write(&inode.blocksUsed, sizeof(inode.blocksUsed));
//    write(&inode.firstBlock, sizeof(inode.firstBlock));
//    write(&inode.isDirectory, sizeof(inode.isDirectory));
//    write(&inode.isUsed, sizeof(inode.isUsed));
//    write(&inode.creationTime, sizeof(inode.creationTime));
//    write(&inode.modificationTime, sizeof(inode.modificationTime));
//
//    if (inode.isDirectory) {
//        uint16_t count = static_cast<uint16_t>(inode.entries.size());
//        write(&count, sizeof(count));
//        for (const auto& entry : inode.entries) {
//            const std::string& name = entry.first;
//            int id = entry.second;
//            uint8_t len = static_cast<uint8_t>(name.size());
//            write(&len, sizeof(len));
//            write(name.data(), len);
//            write(&id, sizeof(id));
//        }
//    }
//
//    if (offset < bufferSize) {
//        std::memset(buffer + offset, 0, bufferSize - offset);
//    }
//}
//
//void MiniHSFS::DeserializeInode(Inode& inode, const char* buffer, size_t bufferSize) {
//    size_t offset = 0;
//    auto read = [&](void* dest, size_t size) {
//        if (offset + size > bufferSize) throw std::runtime_error("Buffer underflow");
//        std::memcpy(dest, buffer + offset, size);
//        offset += size;
//        };
//
//    read(&inode.size, sizeof(inode.size));
//    read(&inode.blocksUsed, sizeof(inode.blocksUsed));
//    read(&inode.firstBlock, sizeof(inode.firstBlock));
//    read(&inode.isDirectory, sizeof(inode.isDirectory));
//    read(&inode.isUsed, sizeof(inode.isUsed));
//    read(&inode.creationTime, sizeof(inode.creationTime));
//    read(&inode.modificationTime, sizeof(inode.modificationTime));
//
//    inode.entries.clear();
//    if (inode.isDirectory) {
//        uint16_t count;
//        read(&count, sizeof(count));
//        for (uint16_t i = 0; i < count && offset < bufferSize; ++i) {
//            uint8_t len;
//            read(&len, sizeof(len));
//            char name[256] = {};
//            read(name, len);
//            int id;
//            read(&id, sizeof(id));
//            inode.entries[std::string(name, len)] = id;
//        }
//    }
//}

void MiniHSFS::SerializeBTreeNode(const BTreeNode& node, char* buffer) {
    size_t offset = 0;
    int order = node.order;

    if (node.keyCount < 0 || node.keyCount > order - 1) {
        throw std::runtime_error("SerializeBTreeNode: Invalid key_count value");
    }

    auto write = [&](const void* data, size_t size) {
        if (offset + size > disk.blockSize) throw std::runtime_error("SerializeBTreeNode: buffer overflow");
        std::memcpy(buffer + offset, data, size);
        offset += size;
        };

    write(&node.isLeaf, sizeof(bool));
    write(&node.keyCount, sizeof(int));
    write(&node.order, sizeof(int));
    write(node.keys, sizeof(int) * (order - 1));

    if (node.isLeaf) {
        write(node.values, sizeof(int) * (order - 1));
        write(&node.nextLeaf, sizeof(int));
    }
    else {
        write(node.children, sizeof(int) * order);
    }
}

void MiniHSFS::DeserializeBTreeNode(BTreeNode& node, const char* buffer) {
    size_t offset = 0;
    bool isLeaf;
    int key_count, order;

    auto read = [&](void* dest, size_t size) {
        if (offset + size > disk.blockSize) throw std::runtime_error("DeserializeBTreeNode: buffer underflow");
        std::memcpy(dest, buffer + offset, size);
        offset += size;
        };

    read(&isLeaf, sizeof(bool));
    read(&key_count, sizeof(int));
    read(&order, sizeof(int));

    if (key_count < 0 || key_count > order - 1) {
        throw std::runtime_error("DeserializeBTreeNode: Invalid key count");
    }

    node = BTreeNode(order, isLeaf);
    node.keyCount = key_count;

    read(node.keys, sizeof(int) * (order - 1));
    if (isLeaf) {
        read(node.values, sizeof(int) * (order - 1));
        read(&node.nextLeaf, sizeof(int));
    }
    else {
        read(node.children, sizeof(int) * order);
    }
}

//////////////////////////////Defragment Blocks

void MiniHSFS::DefragmentFileBlocks(int inodeIndex) {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);
    ValidateInode(inodeIndex);

    MiniHSFS::Inode& inode = inodeTable[inodeIndex];

    if (inode.isDirectory || inode.blocksUsed <= 1)
        return;

    // 1. Read data with proper encryption handling
    VirtualDisk::Extent oldExtent(inode.firstBlock, inode.blocksUsed);
    std::vector<char> fileData;

    // Read normally
    fileData = disk.readData(oldExtent);

    // 2. Free old blocks
    for (int i = 0; i < (int)oldExtent.blockCount; ++i) {
        BTreeDelete(rootNodeIndex, oldExtent.startBlock + i);
    }
    disk.freeBlocks(oldExtent);

    // 3. Allocate new contiguous blocks
    VirtualDisk::Extent newExtent = AllocateContiguousBlocks(inode.blocksUsed);
    if (newExtent.startBlock == -1) {
        throw std::runtime_error("Failed to allocate blocks during defragmentation");
    }

    // 4. Write data with proper encryption handling
    if (!disk.writeData(fileData, newExtent, "", false)) {
        throw std::runtime_error("Failed to write data during defragmentation");
    }

    // 5. Update inode information
    inode.firstBlock = newExtent.startBlock;
    inode.blocksUsed = newExtent.blockCount;
    inode.isDirty = true;
    UpdateInodeTimestamps(inodeIndex, true);
}

VirtualDisk::Extent MiniHSFS::AllocateContiguousBlocks(int blocksNeeded) {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    if (blocksNeeded <= 0) {
        throw std::invalid_argument("Block count must be positive");
    }

    // First: Try customizing directly
    try {

        VirtualDisk::Extent extent = disk.allocateBlocks(blocksNeeded);

        if (extent.blockCount > 100)
        {
            for (uint32_t i = 0; i < extent.blockCount; ++i) {
                MarkBlockUsed(extent.startBlock + i);
            }
        }
        else
            MarkBlocksUsed(extent);

        return extent;
    }
    catch (const VirtualDisk::DiskFullException&) {
        // If it fails, defragment and try again
        DefragmentDisk();

        // Try allocating again after defragmenting
        try {
            VirtualDisk::Extent extent = disk.allocateBlocks(blocksNeeded);
            for (uint32_t i = 0; i < extent.blockCount; ++i) {
                MarkBlockUsed(extent.startBlock + i);
            }
            return extent;
        }
        catch (const VirtualDisk::DiskFullException&) {
            VirtualDisk::Extent extent(-1, 0);
            return extent;
        }
    }
}

void MiniHSFS::SynchronizeDisk() {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);
    if (!mounted) throw std::runtime_error("Filesystem not mounted");

    // Save all temporary data
    SaveInodeTable();
    SaveBTree();

    // Sync virtual disk
    disk.syncToDisk();
}

//void MiniHSFS::DefragmentDisk() {
//    std::lock_guard<std::recursive_mutex> lock(fsMutex);
//
//    // 1. Collect information about files that need to be defragmented
//    std::vector<int> filesToDefrag;
//    for (int i = 0; i < inodeCount; ++i) {
//        if (inodeTable[i].isUsed &&
//            !inodeTable[i].isDirectory &&
//            inodeTable[i].blocksUsed > 1) {
//            filesToDefrag.push_back(i);
//        }
//    }
//
//    // 2. Sort files by fragment size (largest first)
//    std::sort(filesToDefrag.begin(), filesToDefrag.end(),
//        [this](int a, int b) {
//            return inodeTable[a].blocksUsed > inodeTable[b].blocksUsed;
//        });
//
//    // 3. Defragment each file
//    for (int inodeIndex : filesToDefrag) {
//        try {
//            DefragmentFileBlocks(inodeIndex);
//        }
//        catch (const std::exception& e) {
//            std::cerr << "Failed to defragment inode " << inodeIndex
//                << ": " << e.what() << std::endl;
//            continue;
//        }
//    }
//
//    // 4. Rebuild the free block map
//    RebuildFreeBlockList();
//}

void MiniHSFS::DefragmentDisk() {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    // 1. Collect information about files that need to be defragmented
    std::vector<int> filesToDefrag;
    for (int i = 0; i < inodeCount; ++i) {
        if (inodeTable[i].isUsed &&
            !inodeTable[i].isDirectory &&
            inodeTable[i].blocksUsed > 1) {
            filesToDefrag.push_back(i);
        }
    }

    // 2. Sort files by fragment size (largest first)
    std::sort(filesToDefrag.begin(), filesToDefrag.end(),
        [this](int a, int b) {
            return inodeTable[a].blocksUsed > inodeTable[b].blocksUsed;
        });

    // 3. Defragment each file and print progress
    int totalFiles = filesToDefrag.size();
    for (int i = 0; i < totalFiles; ++i) {
        int inodeIndex = filesToDefrag[i];
        try {
            DefragmentFileBlocks(inodeIndex);
        }
        catch (const std::exception& e) {
            std::cerr << "Failed to defragment inode " << inodeIndex
                << ": " << e.what() << std::endl;
            continue;
        }

        // Print progress percentage
        int percent = static_cast<int>((i + 1) * 100.0 / totalFiles);
        std::cout << "\rDefragmenting... " << percent << "% completed" << std::flush;
    }

    std::cout << std::endl << "Defragmentation completed." << std::endl;

    // 4. Rebuild the free block map
    RebuildFreeBlockList();
}

void MiniHSFS::RebuildFreeBlockList() {
    std::vector<int> freeBlocks;

    // Clear all blocks in the data range
    for (int block = dataStartIndex; block < disk.totalBlocks(); ++block) {
        if (!IsBlockUsed(block)) {
            freeBlocks.push_back(block);
        }
    }

    // Reconstruct the B-tree with free blocks
    InitializeBTree();
}

//void MiniHSFS::SaveInodeToDisk(int inodeIndex) {
//    std::vector<char> buffer(inodeSize, 0);
//    SerializeInode(inodeTable[inodeIndex], buffer.data(), inodeSize);
//
//    size_t blockIndex = inodeIndex * inodeSize / disk.blockSize;
//    size_t offset = (inodeIndex * inodeSize) % disk.blockSize;
//
//    auto diskBlock = disk.readData(
//        VirtualDisk::Extent(disk.getSystemBlocks() + static_cast<uint32_t>(superBlockBlocks) + static_cast<uint32_t>(blockIndex), 1));
//
//    
//    size_t bytesToCopy = (std::min)(buffer.size(), diskBlock.size() - offset);
//
//    std::memcpy(diskBlock.data() + offset, buffer.data(), bytesToCopy);
//
//    disk.writeData(diskBlock,
//        VirtualDisk::Extent(disk.getSystemBlocks() + static_cast<uint32_t>(superBlockBlocks) + static_cast<uint32_t>(blockIndex), 1), "", true);
//
//    inodeTable[inodeIndex].isDirty = false;
//}

void MiniHSFS::TouchBTreeNode(int index) {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    auto it = btreeCache.find(index);
    if (it != btreeCache.end()) {
        it->second.accessCount++; // Update usage
    }

    if (btreeLruMap.find(index) != btreeLruMap.end()) {
        btreeLruList.erase(btreeLruMap[index]);
    }
    btreeLruList.push_front(index);
    btreeLruMap[index] = btreeLruList.begin();

    if (btreeLruList.size() > std::max<size_t>(1000, static_cast<size_t>((getAvailableMemory() * 0.05) / sizeof(BTreeNode)))) {
        FreeLRUBTreeNode(); // Throw out the least used
    }
}

void MiniHSFS::FreeLRUBTreeNode() {
    if (btreeLruList.empty()) return;

    // Get the oldest node (last in LRU)
    int victimIndex = btreeLruList.back();
    btreeLruList.pop_back();
    btreeLruMap.erase(victimIndex);

    auto it = btreeCache.find(victimIndex);
    if (it != btreeCache.end()) {
        if (it->second.isDirty) {
            SaveBTreeNode(victimIndex, it->second);
        }
        btreeCache.erase(it);
    }
}

//Nowww