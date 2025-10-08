#ifndef MINI_HSFS_H
#define MINI_HSFS_H

#include "VirtualDisk.h"
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <sstream>
#include <cstring>
#include <vector>
#include <string>
#include <memory>
#include <ctime>
#include <mutex>
#include <deque>
#include <list>
#include <map>


class MiniHSFS {


public:

    //Inode Information
    struct inodeInfo {
        std::vector<uint8_t> Password;
        std::string UserName = "";
        std::string Email = "";
        size_t TotalSize = 0;
        size_t Usage = 0;
    };

    //Inode structure
    struct Inode {
        size_t size = 0;                        // 8 bytes
        int blocksUsed = 0;                    // 4 bytes
        int firstBlock = -1;                  // 4 bytes
        bool isDirectory = false;            // 1 byte
        bool isUsed = false;                // 1 byte
        bool isDirty = false;              // 1 byte ( Has it been modified? )
        time_t creationTime = 0;          // 8 bytes
        time_t modificationTime = 0;     // 8 bytes
        time_t lastAccessed = 0;        // 8 bytes
        inodeInfo inodeInfo;           // Inode Count
    
        std::unordered_map<std::string, int> entries; // For directories
    
        // Calculate the actual size of the node
        size_t actualSize() const {
            size_t baseSize = sizeof(size) + sizeof(blocksUsed) + sizeof(firstBlock) +
                sizeof(isDirectory) + sizeof(isUsed) +
                sizeof(creationTime) + sizeof(modificationTime) +
                sizeof(lastAccessed) + sizeof(isDirty);
    
            if (isDirectory) {
                baseSize += sizeof(size_t); // For the size of the unordered_map
                for (const auto& entry : entries) {
                    baseSize += entry.first.size() + sizeof(int);
                }
            }
            return baseSize;
        }
    
        // Helper function to check validity
        bool isValid() const {
            if (!isUsed) return true;
    
            //Basic value verification
            if (size < 0) return false;
            if (blocksUsed < 0) return false;
            if (creationTime <= 0) return false;
            if (modificationTime <= 0) return false;
            if (lastAccessed <= 0) return false;
    
            // Check data consistency
            if (isDirectory) {
                // For folders: Check entries
                for (const auto& entry : entries) {
                    if (entry.first.empty() || entry.second < 0) {
                        return false;
                    }
                }
            }
            else {
                //For files: Check blocks
                if (blocksUsed > 0 && firstBlock < 0) return false;
            }
    
            return true;
        }
    };

    // Constants
    std::vector<Inode> inodeTable;
    std::recursive_mutex fsMutex;
    size_t inodeSize; // Size of Inode
    bool mounted = false;
    bool initialized = false;
    int dataStartIndex;
    time_t lastTimeWrite = -1;
    int rootNodeIndex = 0;
    const int maxFileNameLength = 255;
    const int maxPathLength = 4096;
    const int countAddExtraInode = 10;


    MiniHSFS(const std::string& path, uint32_t sizeMB, uint32_t blockSize);
    ~MiniHSFS();
    VirtualDisk& Disk();

    VirtualDisk::Extent AllocateContiguousBlocks(int blocksNeeded);
    int AllocateInode(bool isDirectory = false);
    void FreeInode(int inodeIndex);

    // Filesystem operations
    void Initialize();
    void Mount(size_t inodeSize = 512);
    void Unmount();
    void SaveInodeToDisk(int inodeIndex);

    // File operations
    int FindFile(const std::string& path);
    int FindFreeBlock();
    bool FreeFileBlocks(Inode& inode);
    
    // Directory operations
    void MarkBlockUsed(int blockIndex);


    int PathToInode(const std::vector<std::string>& path);
    std::vector<std::string> SplitPath(const std::string& path)const;
    void ValidatePath(const std::string& path); // Check Validate Path
    void UpdateInodeTimestamps(int inodeIndex, bool modify = true); // To Update Time after any Edit Parent Directory or File
    bool ValidateEntry(const std::string& name); //Check the name Entry (Dir or file)

    // Utility functions
    void PrintBTreeStructure();
    void PrintSuperblockInfo();

    // B-tree operations
    bool BTreeDelete(int nodeIndex, int key);


private:

    //SuperBlock Structue Using To Get Info Super Block
    struct SuperblockInfo {
        char magic[8];                   // 8 bytes (Name File System)
        uint32_t version;               // 4 bytes
        uint32_t systemSize;           // 4 bytes
        uint32_t blockSize;           // 4 bytes
        size_t inodeSize;            // 4 bytes
        uint64_t totalBlocks;       // 8 bytes
        uint64_t freeBlocks;       // 8 bytes
        size_t totalInodes;       // 4 bytes
        size_t dataStartIndex;   // 4 bytes
        size_t freeInodes;      // 4 bytes
        time_t creationTime;   // 8 bytes
        time_t lastMountTime; // 8 bytes
        time_t lastWriteTime;// 8 bytes
        uint32_t state;     // 4 bytes
    };

    // File Info Structure, Inode File Information Using in Defragmentation Inodes Table 
    struct FileInfo {
        int inodeIndex;
        uint32_t startBlock;
        uint32_t blockCount;
    };

    // Data Info Structure, Inode File Data Information Using in Defragmentation Data Inode
    struct DataMoveOperation {
        int inodeIndex;
        uint32_t oldStartBlock;
        uint32_t newStartBlock;
        uint32_t blockCount;
        bool success;
    };

    //B-Tree Structure, Using To Control in free or not Inodes
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

    std::unordered_map<int, std::list<int>::iterator> btreeLruMap;
    std::vector<int> freeBTreeBlocksCache;
    std::map<int, BTreeNode> btreeCache;
    std::vector<int> freeInodesList;
    std::vector<bool> inodeBitmap;   //To track used/free nodes
    const int superBlockIndex = 0;  // First Index Have data SuperBlock 
    std::list<int> btreeLruList;   //
    size_t inodeAreaSize = 0;     //Current size of the contract space
    int btreeLoadCounter = 0;    //
    size_t nextFreeInode = 1;   // To speed up the search for a free node
    size_t freeBlocks = 0;     //Number of free blocks
    int superBlockBlocks;   // SuperBlock Blocks Count
    int btreeStartIndex;   // First Index of Block Data of B-Tree Inodes 
    size_t inodeBlocks;   // Inode Blocks Count
    BTreeNode rootNode;  // Root Inode Directory (Child && Entries)
    size_t inodeCount;  //Inode Count
    VirtualDisk disk;  // Object from VirtualDisk File
    int btreeBlocks;  // B-Tree Blocks Count
    int btreeOrder;  // B-Tree Order

    //Inode Operations
    int GetInodeIndex(const Inode& inode) const;

    //Btree Operations
    int AllocateBTreeNode();
    bool BTreeInsert(int nodeIndex, int key, int value);
    void FreeBTreeNode(int nodeIndex);
    bool IsBTreeBlockFree(int index);
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

    // Initializations
    void InitializeSuperblock();
    void InitializeInodeTable();
    int InitializeInode(int index, bool isDirectory);
    void InitializeBTree();

    //Calculations
    size_t CalculateInodeCount();
    size_t CalculateInodeBlocks();
    size_t CalculateBTreeBlocks();
    size_t CalculateBTreeOrder();
    double CalculateAdaptiveBTreePercentage(size_t totalBlocks, size_t btreeOrder);
    size_t CalculateMinimumBTreeBlocks(size_t totalBlocks);
    bool ValidateBTreeConfiguration();
    size_t CalculateBTreeMaxCapacity(size_t btreeBlocks, size_t btreeOrder);
    size_t CountFreeInodes();
    size_t getAvailableMemory();
    uint32_t CalculateChecksum(const char* data, size_t length);
    size_t CalculateBlocksForNewInodes(size_t inodeCount);

    //Loader
    BTreeNode LoadBTreeNode(int nodeIndex);
    SuperblockInfo LoadSuperblock();
    void LoadInodeTable();
    void LoadBTree();

    //Saver
    void SaveBTreeNode(int nodeIndex, const BTreeNode& node);
    void SaveSuperblock(const SuperblockInfo& info);
    void SaveInodeTable();
    void SaveBTree();

    // Directory operations
    void MarkBlocksUsed(const VirtualDisk::Extent& extent);

    //Updater
    void UpdateSuperblockForDynamicInodes();

    // B-tree operations
    bool IsBlockUsed(int blockIndex);

    // Helper functions
    void ValidateInode(int inodeIndex, bool checkDirectory = false);

    // Serialization && Deserialization
    size_t SerializeInode(const Inode& inode, char* buffer, size_t bufferSize);
    size_t DeserializeInode(Inode& inode, const char* buffer, size_t bufferSize);
    void SerializeBTreeNode(const BTreeNode& node, char* buffer);
    void DeserializeBTreeNode(BTreeNode& node, const char* buffer);

    //Defragmentations
    bool DefragmentAndExtendInodes(size_t extraInodes);
    void DefragmentFileBlocks(int inodeIndex);
    void DefragmentDisk();
    bool ExpandInodeAreaDirect(size_t additionalBlocks, size_t newTotalInodes);
    bool ExpandInodeAreaByInodes(size_t extraInodes);
    void RollbackMoves(const std::vector<DataMoveOperation>& moves);
    bool MoveFileBlocks(int inodeIndex, uint32_t oldStart, uint32_t newStart, uint32_t blockCount);
    std::vector<FileInfo> FindFilesInRange(uint32_t startBlock, uint32_t endBlock);
    uint32_t FindFreeSpaceAtEnd(size_t requiredBlocks);

    //Rebuldations
    void RebuildFreeBlockList();
    void RebuildFreeInodesList();
    void RebuildInodeBitmap();

    void TouchBTreeNode(int index);
    void FreeLRUBTreeNode();

};

#endif // MINI_HSFS_H