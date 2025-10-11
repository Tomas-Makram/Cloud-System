#ifndef MINI_HSFS_H
#define MINI_HSFS_H

#include "VirtualDisk.h"
#include <vector>
#include <string>
#include <map>
#include <ctime>
#include <mutex>
#include <memory>
#include <algorithm>
#include <unordered_map>
#include <iostream>
#include <sstream>
#include <unordered_set>
#include <stdexcept>
#include <cstring>
#include <list>
#include <deque>

class MiniHSFS {


public:

    struct SuperblockInfo {
        char magic[8];                   //8bytes
        uint32_t version;               // 4 bytes
        uint32_t systemSize;           // 4 bytes
        uint32_t blockSize;           // 4 bytes
        size_t inodeSize;            // 4 bytes
        uint64_t totalBlocks;       // 8 bytes
        uint64_t freeBlocks;       // 8 bytes
        size_t totalInodes;       // 4 bytes
        size_t freeInodes;       // 4 bytes
        time_t creationTime;    // 8 bytes (Usually)
        time_t lastMountTime;  // 8 bytes
        time_t lastWriteTime; // 8 bytes
        uint32_t state;      // 4 bytes
    };

    // Inode structure
    //struct Inode {
    //    uint64_t size = 0;                      // 8 bytes
    //    int blocksUsed = 0;                    // 4 bytes
    //    int firstBlock = -1;                  // 4 bytes
    //    bool isDirectory = false;            // 1 byte 
    //    bool isUsed = false;                // 1 byte
    //    time_t creationTime = 0;           // 8 bytes
    //    time_t modificationTime = 0;      // 8 bytes
    //    bool isDirty = false;            // 1 byte  ( Has it been modified? )
    //    int accessCount = 0;            // 4 bytes
    //    time_t lastAccessed = 0;       //8 bytes Last used time
    //    bool isLoaded = false;        // 1 byte
    //    std::map<std::string, int> entries; //Not Constant 12*n

    //    size_t calculateSerializedSize() const;

    //};

    struct Inode {
        size_t size = 0;
        int blocksUsed = 0;
        int firstBlock = -1;
        bool isDirectory = false;
        bool isUsed = false;
        bool isDirty = false;
        time_t creationTime = 0;
        time_t modificationTime = 0;
        time_t lastAccessed = 0;
        std::unordered_map<std::string, int> entries; // For directories

        // دالة لاحتساب الحجم الفعلي للعقدة
        size_t actualSize() const {
            size_t baseSize = sizeof(size) + sizeof(blocksUsed) + sizeof(firstBlock) +
                sizeof(isDirectory) + sizeof(isUsed) +
                sizeof(creationTime) + sizeof(modificationTime) +
                sizeof(lastAccessed) + sizeof(isDirty);

            if (isDirectory) {
                baseSize += sizeof(size_t); // لحجم الـ unordered_map
                for (const auto& entry : entries) {
                    baseSize += entry.first.size() + sizeof(int);
                }
            }
            return baseSize;
        }

        // دالة مساعدة للتحقق من الصلاحية
        bool isValid() const {
            if (!isUsed) return true;

            // تحقق أساسي من القيم
            if (size < 0) return false;
            if (blocksUsed < 0) return false;
            if (creationTime <= 0) return false;
            if (modificationTime <= 0) return false;
            if (lastAccessed <= 0) return false;

            // تحقق من تناسق البيانات
            if (isDirectory) {
                // للمجلدات: التحقق من الإدخالات
                for (const auto& entry : entries) {
                    if (entry.first.empty() || entry.second < 0) {
                        return false;
                    }
                }
            }
            else {
                // للفايلات: التحقق من الكتل
                if (blocksUsed > 0 && firstBlock < 0) return false;
            }

            return true;
        }
    };

    int btreeOrder;

    struct BTreeNode {
        bool isLeaf;
        int keyCount;
        int* keys;
        union {
            int* children;
            int* values;
        };
        int nextLeaf;
        int accessCount;
        bool isDirty;
        int order; //We need to store btreeOrder in each node

        //Basic constructor
        BTreeNode(int btree_order, bool leaf = true)
            : isLeaf(leaf), keyCount(0), nextLeaf(-1),
            accessCount(0), isDirty(false), order(btree_order)
        {
            keys = new int[btree_order - 1];
            if (isLeaf)
                values = new int[btree_order - 1]();
            else
                children = new int[btree_order]();
            std::fill(keys, keys + btree_order - 1, -1);
        }

        //Default constructor(to bypass C2512 errors)
        BTreeNode() : isLeaf(true), keyCount(0), nextLeaf(-1),
            accessCount(0), isDirty(false), order(4)
        {
            keys = new int[order - 1];
            values = new int[order - 1]();
            std::fill(keys, keys + order - 1, -1);
        }

        ~BTreeNode() {
            delete[] keys;
            if (isLeaf)
                delete[] values;
            else
                delete[] children;
        }

        BTreeNode(const BTreeNode& other)
            : isLeaf(other.isLeaf), keyCount(other.keyCount),
            nextLeaf(other.nextLeaf), accessCount(other.accessCount),
            isDirty(other.isDirty), order(other.order)
        {
            keys = new int[order - 1];
            std::copy(other.keys, other.keys + order - 1, keys);
            if (isLeaf) {
                values = new int[order - 1];
                std::copy(other.values, other.values + order - 1, values);
            }
            else {
                children = new int[order];
                std::copy(other.children, other.children + order, children);
            }
        }

        BTreeNode& operator=(const BTreeNode& other) {
            if (this == &other) return *this;
            isLeaf = other.isLeaf;
            keyCount = other.keyCount;
            nextLeaf = other.nextLeaf;
            accessCount = other.accessCount;
            isDirty = other.isDirty;
            order = other.order;

            delete[] keys;
            keys = new int[order - 1];
            std::copy(other.keys, other.keys + order - 1, keys);

            if (isLeaf) {
                delete[] values;
                values = new int[order - 1];
                std::copy(other.values, other.values + order - 1, values);
            }
            else {
                delete[] children;
                children = new int[order];
                std::copy(other.children, other.children + order, children);
            }

            return *this;
        }
    };

    // Constants

    std::vector<Inode> inodeTable;
    std::map<int, BTreeNode> btreeCache;
    // B-tree node structure
    BTreeNode rootNode;
    std::recursive_mutex fsMutex;
    bool mounted = false;
    bool initialized = false;
    const int superBlockIndex = 0;
    int superBlockBlocks;
    size_t inodeCount;
    size_t inodeBlocks;
    int btreeBlocks;
    int btreeStartIndex;
    int dataStartIndex;
    time_t lastTimeWrite = -1;
    std::list<int> btreeLruList;
    std::unordered_map<int, std::list<int>::iterator> btreeLruMap;

    std::list<int> inodeLruList;
    std::unordered_map<int, std::list<int>::iterator> inodeLruMap;

    int btreeLoadCounter = 0;
    int inodeLoadCounter = 0;
    int rootNodeIndex = 0;



    const int maxFileNameLength = 255;
    const int maxPathLength = 4096;
    size_t inodeSize;
    size_t inodePercentage;
    size_t btreePercentage;

    //----------------------------------------------//
    
     // وفي القسم private أضف:
    struct FileInfo {
        int inodeIndex;
        uint32_t startBlock;
        uint32_t blockCount;
    };

    struct DataMoveOperation {
        int inodeIndex;
        uint32_t oldStartBlock;
        uint32_t newStartBlock;
        uint32_t blockCount;
        bool success;
    };

    // في MiniHSFS.h أضف هذه الدوال في القسم public:
    bool DefragmentAndExtendInodes(size_t extraInodes);
    std::vector<FileInfo> FindFilesInRange(uint32_t startBlock, uint32_t endBlock);
    bool MoveFileBlocks(int inodeIndex, uint32_t oldStart, uint32_t newStart, uint32_t blockCount);
    uint32_t FindFreeSpaceAtEnd(size_t requiredBlocks);
    bool ExpandInodeAreaDirect(size_t additionalBlocks);
    void RollbackMoves(const std::vector<DataMoveOperation>& moves);

   
    bool autoDefrag = true;   // إعادة التنظيم التلقائية

    // دوال إدارة العقد الديناميكية
    void ValidateInodeTable();
    int AllocateInode(bool isDirectory = false);
    int InitializeInode(int index, bool isDirectory);
    void FreeInode(int inodeIndex);
    void DefragmentInodes();
    size_t GetAvailableInodes();
    bool CanCreateMoreInodes();
    void ExpandInodeArea(size_t additionalSize);
    void AutoDefragmentInodes();

    // متغيرات العقد الديناميكية
    std::vector<int> freeInodesList;
    bool dynamicInodes;
    size_t nextInodeIndex;
    size_t maxPossibleInodes;

    // دوال مساعدة للعقد الديناميكية
    void ExtendInodeTable(size_t additionalInodes = 0);
    void CompactInodeTable();
    void RebuildFreeInodesList();
    size_t CalculateMaxPossibleInodes();

    // في قسم private أضف:
    std::vector<bool> inodeBitmap; // لتتبع العقد المستخدمة/الحرة
    size_t nextFreeInode = 1;      // لتسريع البحث عن عقدة حرة
    size_t inodeAreaSize = 0;      // الحجم الحالي لمساحة العقد

    // أضف هذه الدوال:
    void ExtendInodeArea(size_t additionalInodes = 0);
    void UpdateSuperblockForDynamicInodes();
    bool CanAllocateInodes(size_t count);
    void RebuildInodeBitmap();
    void FreeBlock(uint32_t blockIndex);
    bool AllocateContiguousRegion(uint32_t start, size_t count);
    std::vector<std::tuple<int, uint32_t, uint32_t>> CollectFileExtents();
    void MoveInodeExtent(int inodeIndex, uint32_t srcStart, uint32_t dstStart, uint32_t count);
    bool DefragmentDataAreaToFreePrefix(size_t neededBlocks);
    bool ExpandInodeAreaInPlace(size_t addInodes);
    size_t BlocksForInodes(size_t inodeCount);
    uint32_t dataStartBlock = 0; // بداية منطقة البيانات
    size_t freeBlocks = 0;       // عدد البلوكات الحرة
    bool ReserveContiguousBlocksAtDataStart(size_t count);
    void ResetInode(int idx, bool isDir);
    bool TryExtendInodeTable();
    void UpdateFileBlocks(int inodeIndex, const std::vector<int>& newBlocks);
    int FindFreeBlockFrom(size_t startBlock);

    std::vector<int> GetFileBlocks(int inodeIndex);
    size_t SerializeInode(const Inode& inode, char* buffer, size_t bufferSize);
    size_t DeserializeInode(Inode& inode, const char* buffer, size_t bufferSize);
    bool CheckInodeSpace(size_t additionalInodes);
    uint32_t CalculateChecksum(const char* data, size_t length);
    void ValidateAndRepairInode(int inodeIndex);
    bool ExpandInodeAreaDirect(size_t additionalBlocks, size_t newTotalInodes);
    // في MiniHSFS.h أضف هذا المتغير في القسم private:
    bool is_validating = false;
    void ValidateAndRepairInodes();
    int FindParentInode(int inodeIndex);
    bool ExpandInodeAreaByInodes(size_t extraInodes);
    //----------------------------------------------//

    std::atomic<bool> autoSyncRunning = false;
    std::thread autoSyncThread;

    //Config currentConfig;
    std::condition_variable auto_sync_cv;
    std::mutex auto_sync_mutex;


    MiniHSFS(const std::string& path, uint32_t sizeMB, uint32_t blockSize);
    ~MiniHSFS();
    VirtualDisk& Disk();

    bool IsBTreeBlockFree(int index);


    void calculatePercentage(double inodePercentage = 0.015, double btreePercentage = 0.01);
    size_t calculateInodeCount();
    size_t calculateInodeBlocks();
    size_t calculateBTreeBlocks();
    size_t calculateBTreeOrder();
    size_t CountFreeInodes();
    size_t getAvailableMemory();

    // Filesystem operations
    void Initialize();
    void Mount(size_t inodePercentage = 0, size_t btreePercentage = 0, size_t inodeSize = 512);
    void Unmount();
    bool IsMounted() { return mounted; }

    // File operations
    int FindFile(const std::string& path);
    bool FreeFileBlocks(Inode& inode);

    // Directory operations
    void MarkBlockUsed(int blockIndex);

    void MarkBlocksUsed(const VirtualDisk::Extent& extent);


    int PathToInode(const std::vector<std::string>& path);
    std::vector<std::string> SplitPath(const std::string& path)const;
    void ValidatePath(const std::string& path);
    void UpdateInodeTimestamps(int inodeIndex, bool modify = true);

    // Utility functions
    void PrintBTreeStructure();
    void PrintSuperblockInfo();

    // B-tree operations
    bool BTreeInsert(int nodeIndex, int key, int value);
    bool BTreeDelete(int nodeIndex, int key);

    int FindFreeBlock();

    VirtualDisk::Extent AllocateContiguousBlocks(int blocksNeeded);
    void SynchronizeDisk();
    void FreeLRUBTreeNode();

    int GetInodeIndex(const Inode& inode) const;
    //size_t Tooo(size_t plaintext);

private:
    VirtualDisk disk;
    
    std::vector<int> freeBTreeBlocksCache;

    // Private members    
    BTreeNode LoadBTreeNode(int nodeIndex);
    void SaveBTreeNode(int nodeIndex, const BTreeNode& node);
    int AllocateBTreeNode();
//    bool IsBTreeBlockEmpty(int index);
    void FreeBTreeNode(int nodeIndex);
    std::pair<bool, int> BTreeFind(int nodeIndex, int key);
    void BTreeSplitChild(int parentIndex, int childIndex, int index);
    bool BTreeMergeChildren(int nodeIndex, int index);
    bool BTreeInsertNonFull(int nodeIndex, int key, int value);
    int BTreeGetSuccessor(int nodeIndex);
    int BTreeGetPredecessor(int nodeIndex);
    bool BTreeDeleteFromNonLeaf(int nodeIndex, int index);
    bool BTreeDeleteFromLeaf(int nodeIndex, int index);
    void BTreeBorrowFromRight(int nodeIndex, int index);
    void BTreeBorrowFromLeft(int nodeIndex, int index);
    void BTreeFill(int nodeIndex, int index);

    // Private methods
    void InitializeSuperblock();
    void InitializeInodeTable();
    void InitializeBTree();

    void LoadInodeTable();
    void SaveInodeTable();

    void LoadBTree();
    void SaveBTree();

    SuperblockInfo LoadSuperblock();
    void SaveSuperblock(const SuperblockInfo& info);

    // B-tree operations
    bool IsBlockUsed(int blockIndex);

    // Helper functions
    void ValidateInode(int inodeIndex, bool checkDirectory = false);

    // Serialization
    //void SerializeInode(const Inode& inode, char* buffer, size_t bufferSize);
    //void DeserializeInode(Inode& inode, const char* buffer, size_t bufferSize);
    void SerializeBTreeNode(const BTreeNode& node, char* buffer);
    void DeserializeBTreeNode(BTreeNode& node, const char* buffer);


    void DefragmentFileBlocks(int inodeIndex);
    void RebuildFreeBlockList();
    void DefragmentDisk();


    void SaveInodeToDisk(int inodeIndex);
    void TouchBTreeNode(int index);
};

#endif // MINI_HSFS_H