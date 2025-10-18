#include "MiniHSFS.h"

///////////////////////////////Start System

MiniHSFS::MiniHSFS(const std::string& path, uint32_t sizeMB, uint32_t blockSize)
    :disk(std::max<int>(1, static_cast<int>(std::ceil((double)sizeof(SuperblockInfo) / blockSize))), blockSize),
    mounted(false),
    initialized(false),
     btreeBlocks(0), btreeStartIndex(0), dataStartIndex(0), inodeBlocks(0), inodeCount(0) {

    // Format the virtual disk
    disk.Initialize(path, sizeMB);

    btreeOrder = static_cast<int>(CalculateBTreeOrder());

    // Root Formatting
    rootNode = BTreeNode(btreeOrder, true);

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
    inodeCount = CalculateInodeCount();
    inodeBlocks = CalculateInodeBlocks();
    btreeBlocks = static_cast<int>(CalculateBTreeBlocks());

    bool valid = ValidateBTreeConfiguration();   // التحقق من الكفاءة

    if (!valid) {
        std::cout << "Warning: B-Tree configuration may be inefficient!\n";
    }

    btreeStartIndex = static_cast<int>(inodeBlocks) + disk.getSystemBlocks() + superBlockBlocks;
    dataStartIndex = btreeStartIndex + btreeBlocks;

    try {
        if (disk.IsNew()) {
            InitializeSuperblock();

            InitializeBTree();
            disk.allocateBlocks(static_cast<uint32_t>(btreeBlocks ? btreeBlocks : 1));

            InitializeInodeTable();
            disk.allocateBlocks(static_cast<uint32_t>(inodeBlocks ? inodeBlocks : 1));

            SaveInodeTable();
            SaveBTree();
        }
        initialized = true;
        dataStartIndex = LoadSuperblock().dataStartIndex;
    }
    catch (...) {
        //FlushDirtyInodes();
        SaveBTree();
        std::cerr << "!!Memory pressure during initialization. Flushing all caches.\n";

        btreeCache.clear();
        btreeLruMap.clear();
        btreeLruList.clear();
        inodeTable.clear();
        throw;
    }
}

void MiniHSFS::Mount(size_t inodeSize) {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    this->inodeSize = inodeSize;

    if (mounted) {
        throw std::runtime_error("Filesystem already mounted");
    }

    if (!initialized) {
        Initialize();
    }
    try {

        LoadInodeTable();
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

        //Save All Updates
        SaveSuperblock(info);
        SaveInodeTable();
        SaveBTree();

        // Sync virtual disk
        disk.syncToDisk();

        btreeCache.clear();
        inodeTable.clear();
        btreeLruList.clear();
        btreeLruMap.clear();

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
    info.dataStartIndex = dataStartIndex;
    info.freeInodes = inodeCount - 1;
    info.creationTime = time(nullptr);
    info.lastMountTime = info.creationTime;
    info.lastWriteTime = info.creationTime;
    info.state = 1;

    SaveSuperblock(info);
}

void MiniHSFS::InitializeInodeTable() {

    inodeTable.clear();
    inodeTable.resize(inodeCount);
    inodeBitmap.assign(inodeCount, false);

    //Create root directory
    inodeTable[0].isUsed = true;
    inodeTable[0].isDirectory = true;
    time_t now = time(nullptr);
    inodeTable[0].creationTime = inodeTable[0].modificationTime = inodeTable[0].lastAccessed = now;
    inodeBitmap[0] = true;

}

int MiniHSFS::InitializeInode(int index, bool isDirectory) {
    if (index < 0 || static_cast<size_t>(index) >= inodeTable.size()) {
        throw std::out_of_range("Invalid inode index: " + std::to_string(index));
    }

    // Complete new configuration with default values
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

    // Update SuperBlock 
    UpdateSuperblockForDynamicInodes();

    return index;
}

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

void MiniHSFS::UpdateSuperblockForDynamicInodes() {
    SuperblockInfo info = LoadSuperblock();
    info.inodeSize = inodeSize;
    info.totalInodes = static_cast<uint32_t>(inodeTable.size());
    info.freeInodes = static_cast<uint32_t>(CountFreeInodes());
    info.lastWriteTime = time(nullptr);
    info.dataStartIndex = dataStartIndex;

    // Update the system size to reflect the expansion of the inodes area
    info.systemSize = static_cast<uint32_t>(disk.getSystemBlocks() + superBlockBlocks + inodeBlocks + btreeBlocks);

    SaveSuperblock(info);
}

void MiniHSFS::LoadInodeTable() {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    SuperblockInfo sb = LoadSuperblock();
    inodeSize = sb.inodeSize;

    inodeCount = sb.totalInodes;               // The only source of volume
    inodeBlocks = CalculateBlocksForNewInodes(inodeCount); // Calculate how many blocks we need to read
    inodeTable.assign(inodeCount, Inode{});

    // Read all blocks of the iode table
    size_t bytes = inodeBlocks * disk.blockSize;
    std::vector<char> buf(bytes, 0);
    for (size_t b = 0; b < inodeBlocks; ++b) {
        auto data = disk.readData(
            VirtualDisk::Extent(disk.getSystemBlocks() + static_cast<uint32_t>(superBlockBlocks) + static_cast<uint32_t>(b), 1));
        std::copy(data.begin(), data.end(), buf.begin() + b * disk.blockSize);
    }

    // Decode each inode
    for (size_t i = 0; i < inodeCount; ++i) {
        if (DeserializeInode(inodeTable[i], buf.data() + i * inodeSize, inodeSize) == 0) {
            inodeTable[i] = Inode(); // if faild reset inode
        }
    }

    RebuildInodeBitmap(); // Build bitmap from isUsed after loading
}

void MiniHSFS::SaveInodeTable() {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    size_t requiredBlocks = CalculateBlocksForNewInodes(inodeTable.size());
    if (requiredBlocks > inodeBlocks) {
        size_t add = requiredBlocks - inodeBlocks;
        VirtualDisk::Extent ext(disk.getSystemBlocks() + static_cast<uint32_t>(superBlockBlocks) + static_cast<uint32_t>(inodeBlocks), static_cast<uint32_t>(add));
        disk.allocateBlocks(ext.blockCount);

        std::vector<char> zero(disk.blockSize, 0);
        for (size_t i = 0; i < add; ++i)
            disk.writeData(zero, VirtualDisk::Extent(ext.startBlock + static_cast<uint32_t>(i), 1), "", true);

        inodeBlocks = requiredBlocks;
        UpdateSuperblockForDynamicInodes();
    }

    std::vector<char> big(inodeBlocks * disk.blockSize, 0);
    size_t ok = 0;
    for (size_t i = 0; i < inodeTable.size(); ++i) {
        size_t off = i * inodeSize;
        if (off + inodeSize > big.size())
            throw std::runtime_error("SaveInodeTable: inode area too small");

        /////////////////////////////////////////////////////////////////////////////////////////////////////////////
        if (SerializeInode(inodeTable[i], big.data() + off, inodeSize) == 0)
            throw std::runtime_error("SaveInodeTable: serialize failed for inode " + std::to_string(i));
        ////////////////////////////////////////////////////////////////////////////////////////////////////////////

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

int MiniHSFS::AllocateInode(bool isDirectory) {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    // First attempt: Use an existing free node
    if (!freeInodesList.empty()) {
        int idx = freeInodesList.front();
        freeInodesList.erase(freeInodesList.begin());
        return InitializeInode(idx, isDirectory);
    }

    // Second attempt: Find an unused node
    for (size_t i = 1; i < inodeTable.size(); ++i) {
        if (!inodeTable[i].isUsed) {
            return InitializeInode(static_cast<int>(i), isDirectory);
        }
    }

    // Third attempt: Expanding using Defragmentation

    if (!DefragmentAndExtendInodes(countAddExtraInode)) { // Add 10 nodes at once
        throw std::runtime_error("Cannot allocate inode - no space even after defrag");
    }

    // Search again after expansion
    for (size_t i = 1; i < inodeTable.size(); ++i) {
        if (!inodeTable[i].isUsed) {
            return InitializeInode(static_cast<int>(i), isDirectory);
        }
    }

    throw std::runtime_error("Failed to allocate inode after expansion");
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

std::string MiniHSFS::ValidatePath(const std::string& path) {
    if (path.empty()) {
        throw std::invalid_argument("Path cannot be empty");
    }

    if (path.length() > maxPathLength) {
        throw std::invalid_argument("Path too long");
    }

    if (path[0] != '/') {
        throw std::invalid_argument("Path must be absolute");
    }

    std::vector<std::string> paths = SplitPath(path);
    std::string newPath;

    for (const std::string& item : paths) {
        if (!item.empty())
            newPath += "/" + item;
    }
    if (paths.size() == 0)
        newPath = "/";
    return newPath;
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

bool MiniHSFS::ValidateEntry(const std::string& name) {
    if (name.empty() || name.size() > maxFileNameLength) {
        return false;
    }

    // It cannot start or end with a space
    if (name.front() == ' ' || name.back() == ' ') {
        return false;
    }

    // Forbidden symbols
    static const std::string invalidChars = "\\/:*?\"<>|";

    for (char c : name) {
        if (invalidChars.find(c) != std::string::npos) {
            return false;
        }
    }

    return true; // Valid name
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

std::vector<MiniHSFS::FileInfo> MiniHSFS::FindFilesInRange(uint32_t startBlock, uint32_t endBlock) {
    std::vector<FileInfo> files;

    for (size_t i = 1; i < inodeTable.size(); ++i) {
        if (inodeTable[i].isUsed && !inodeTable[i].isDirectory && inodeTable[i].blocksUsed > 0) {
            uint32_t fileStart = inodeTable[i].firstBlock;
            uint32_t fileEnd = fileStart + inodeTable[i].blocksUsed - 1;

            if (fileStart >= startBlock && fileStart < endBlock) {
                FileInfo info;
                info.inodeIndex = static_cast<int>(i);
                info.startBlock = fileStart;
                info.blockCount = inodeTable[i].blocksUsed;
                files.push_back(info);
            }
        }
    }

    // Sort by start block
    std::sort(files.begin(), files.end(),
        [](const FileInfo& a, const FileInfo& b) { return a.startBlock < b.startBlock; });

    return files;
}

uint32_t MiniHSFS::FindFreeSpaceAtEnd(size_t requiredBlocks) {
    uint32_t totalBlocks = static_cast<uint32_t>(disk.totalBlocks());
    uint32_t freeBlocksFound = 0;
    uint32_t startBlock = 0;

    // Search from end to start
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
    if (extent.startBlock < static_cast<uint32_t>(dataStartIndex) || extent.startBlock + extent.blockCount > disk.totalBlocks()) {
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
        for (uint32_t i = processed; i < static_cast<uint32_t>(totalBlocks); ++i) {
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

bool MiniHSFS::MoveFileBlocks(int inodeIndex, uint32_t oldStart, uint32_t newStart, uint32_t blockCount) {
    try {
        // Read data
        std::vector<char> fileData = disk.readData(VirtualDisk::Extent(oldStart, blockCount));

        // Writing data to the new location
        disk.writeData(fileData, VirtualDisk::Extent(newStart, blockCount), "", true);

        // Update the Inode
        inodeTable[inodeIndex].firstBlock = newStart;
        inodeTable[inodeIndex].isDirty = true;

        // Free up old space
        std::vector<char> zeroBlock(disk.blockSize, 0);
        for (uint32_t i = 0; i < blockCount; ++i) {
            disk.writeData(zeroBlock, VirtualDisk::Extent(oldStart + i, 1), "", true);
        }

        // Update the B-tree
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

void MiniHSFS::RollbackMoves(const std::vector<DataMoveOperation>& moves) {
    std::cout << "Rolling back " << moves.size() << " file moves..." << std::endl;

    for (const auto& op : moves) {
        if (op.success) {
            //Return the file to its original location
            MoveFileBlocks(op.inodeIndex, op.newStartBlock, op.oldStartBlock, op.blockCount);
        }
    }
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

void MiniHSFS::FreeInode(int index) {
    if (index <= 0 || static_cast<size_t>(index) >= inodeTable.size()) return;

    // Edit blocks first
    if (inodeTable[index].isUsed) {
        if (!inodeTable[index].isDirectory) {
            FreeFileBlocks(inodeTable[index]);
        }
        else {
            inodeTable[index].entries.clear();
        }
    }

    // Reset to safe default values
    inodeTable[index] = Inode();
    inodeTable[index].isUsed = false;

    if (inodeBitmap.size() > static_cast<size_t>(index)) {
        inodeBitmap[index] = false;
    }

    RebuildFreeInodesList();
    UpdateSuperblockForDynamicInodes();
    SaveInodeToDisk(index); // Save changes to disk
}

bool MiniHSFS::IsBlockUsed(int blockIndex) {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);
    auto result = BTreeFind(rootNodeIndex, blockIndex);
    return result.first && result.second == 1;
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

size_t MiniHSFS::CalculateInodeCount() {

    return static_cast<size_t>(static_cast<double>(disk.totalBlocks() * disk.blockSize) * .0025 / inodeSize);
}

size_t MiniHSFS::CalculateInodeBlocks() {
    return static_cast<size_t>(std::ceil(static_cast<double>(inodeCount * inodeSize) / disk.blockSize));
}

size_t MiniHSFS::CalculateBTreeOrder() {
    // حساب أساسي لـ Order بناءً على حجم الكتلة
    size_t basicOrder = (disk.blockSize - (sizeof(bool) + sizeof(int) * 2)) / (sizeof(int) * 2);

    // تحسين الـ Order بناءً على حجم القرص ونوع الاستخدام
    size_t totalBlocks = disk.totalBlocks();

    // لـ SSDs أو أقراص سريعة: order أعلى لتحسين الأداء
    // لـ HDDs أو أقراص بطيئة: order أقل لتقليل عمليات القراءة
    if (totalBlocks > 1000000) { // أقراص كبيرة
        // تخفيض الـ Order قليلاً للكفاءة في الذاكرة
        basicOrder = static_cast<size_t>(basicOrder * 0.8);
    }
    else if (totalBlocks < 10000) { // أقراص صغيرة
        // زيادة الـ Order لتحسين الأداء
        basicOrder = static_cast<size_t>(basicOrder * 1.2);
    }

    // حدود أمان للـ Order
    const size_t MIN_ORDER = 4;
    const size_t MAX_ORDER = 512;

    return (std::max)(MIN_ORDER, (std::min)(basicOrder, MAX_ORDER));
}

size_t MiniHSFS::CalculateBTreeBlocks() {

    size_t totalBlocks = disk.totalBlocks();
    size_t btreeOrder = CalculateBTreeOrder();

    // حساب المساحة المطلوبة بناءً على عدة عوامل
    double calculatedPercentage = CalculateAdaptiveBTreePercentage(totalBlocks, btreeOrder);

    size_t suggested = static_cast<size_t>(std::ceil(totalBlocks * calculatedPercentage));

    // الحد الأدنى يختلف حسب حجم القرص
    size_t minBlocks = CalculateMinimumBTreeBlocks(totalBlocks);

    return (std::max)(suggested, minBlocks);
}

// Helper function: Calculate adaptive percentage
double MiniHSFS::CalculateAdaptiveBTreePercentage(size_t totalBlocks, size_t btreeOrder) {

    // Fixed base ratio
    double basePercentage = 0.05; // 5% as a basic percentage, for example

    // Adjust the ratio based on the disk size
    if (totalBlocks >= 500000) { //Very large discs
        basePercentage = 0.03; // 3% for large discs
    }
    else if (totalBlocks >= 100000) { // Large discs
        basePercentage = 0.04; // 4% for large discs
    }
    else if (totalBlocks <= 10000) { // Small tablets
        basePercentage = 0.08; // 8% for small tablets
    }

    std::cout << "basePercentage = " << basePercentage << '\n';

    // Modify based on the order
    double orderFactor = 1.0;
    if (btreeOrder > 100) {
        orderFactor = 0.7;// Order higher = smaller tree
    }
    else if (btreeOrder < 20) {
        orderFactor = 1.3;// Order lower = larger tree
    }

    double adaptivePercentage = basePercentage * orderFactor;

    // حدود أمان للنسبة
    const double minPercentaeg = 0.01;  // 1% minimum
    const double maxPercentaeg = 0.20; // 20% maximum

    return (std::max)(minPercentaeg, (std::min)(adaptivePercentage, maxPercentaeg));
}

// Helper function: Calculate minimum blocks
size_t MiniHSFS::CalculateMinimumBTreeBlocks(size_t totalBlocks) {
    if (totalBlocks < 1000) {
        return 8;  //For very small tablets
    }
    else if (totalBlocks < 10000) {
        return 16; //For small tablets
    }
    else if (totalBlocks < 100000) {
        return 32; //For medium tablets
    }
    else {
        return 64; //For large discs
    }
}

// Additional function: Check the efficiency of the distribution
bool MiniHSFS::ValidateBTreeConfiguration() {
    size_t btreeBlocks = CalculateBTreeBlocks();
    size_t btreeOrder = CalculateBTreeOrder();
    size_t totalBlocks = disk.totalBlocks();

    // Maximum tree capacity
    size_t maxCapacity = CalculateBTreeMaxCapacity(btreeBlocks, btreeOrder);

    // The tree must accommodate at least 150% of the expected blocks.
    size_t expectedDataBlocks = totalBlocks - dataStartIndex;

    std::cout << "B-Tree Configuration Validation:\n";
    std::cout << "  - Total Blocks: " << totalBlocks << "\n";
    std::cout << "  - B-Tree Blocks: " << btreeBlocks << "\n";
    std::cout << "  - B-Tree Order: " << btreeOrder << "\n";
    std::cout << "  - Max Capacity: " << maxCapacity << " blocks\n";
    std::cout << "  - Expected Data: " << expectedDataBlocks << " blocks\n";
    std::cout << "  - Efficiency: " << (maxCapacity * 100 / expectedDataBlocks) << "%\n";

    return maxCapacity >= expectedDataBlocks * 1.2; //20% safety margin
}

// Helper function: Calculate the maximum tree capacity
size_t MiniHSFS::CalculateBTreeMaxCapacity(size_t btreeBlocks, size_t btreeOrder) {
    
    // In a B+ tree, each leaf node has an (order - 1) key
    size_t keysPerLeaf = btreeOrder - 1;

    //Assume 50% of blocks are for leaf nodes
    size_t leafNodes = btreeBlocks / 2;

    return leafNodes * keysPerLeaf;
}

size_t MiniHSFS::CountFreeInodes() {
    size_t count = 0;
    for (size_t i = 0; i < inodeTable.size(); ++i) {
        if (!inodeTable[i].isUsed) {
            count++;
        }
    }
    return count;
}

size_t MiniHSFS::getAvailableMemory() {
    return disk.getAvailableMemory();
}

uint32_t MiniHSFS::CalculateChecksum(const char* data, size_t length) {
 
    if (data == nullptr || length == 0) {
        return 0;
    }
    uint32_t checksum = 0;
    for (size_t i = 0; i < length; ++i) {
        checksum = (checksum << 4) ^ (checksum >> 28) ^ static_cast<uint8_t>(data[i]);
    }
    return checksum;
}

size_t MiniHSFS::CalculateBlocksForNewInodes(size_t inodeCount) {
    if (inodeCount == 0) return 0;

    size_t totalBytes = inodeCount * inodeSize;
    size_t blocksNeeded = (totalBytes + disk.blockSize - 1) / disk.blockSize;

    return blocksNeeded;
}

//////////////////////////////Convertion Operation

size_t MiniHSFS::SerializeInode(const Inode& inode, char* buffer, size_t bufferSize) {
    if (bufferSize < inodeSize) {
        std::cerr << "Buffer too small for inode serialization. Needed: "
            << inodeSize << ", Got: " << bufferSize << std::endl;
        return 0;
    }

    std::memset(buffer, 0, bufferSize);
    size_t offset = 0;

    // ---- Basic fields ----
    std::memcpy(buffer + offset, &inode.size, sizeof(inode.size));                 offset += sizeof(inode.size);
    std::memcpy(buffer + offset, &inode.blocksUsed, sizeof(inode.blocksUsed));     offset += sizeof(inode.blocksUsed);
    std::memcpy(buffer + offset, &inode.firstBlock, sizeof(inode.firstBlock));     offset += sizeof(inode.firstBlock);

    uint8_t flags = 0;
    if (inode.isDirectory) flags |= 0x01;
    if (inode.isUsed)      flags |= 0x02;
    if (inode.isDirty)     flags |= 0x04;
    std::memcpy(buffer + offset, &flags, sizeof(flags));                           offset += sizeof(flags);

    time_t c = inode.creationTime > 0 ? inode.creationTime : time(nullptr);
    time_t m = inode.modificationTime > 0 ? inode.modificationTime : c;
    time_t a = inode.lastAccessed > 0 ? inode.lastAccessed : c;

    std::memcpy(buffer + offset, &c, sizeof(c));                                   offset += sizeof(c);
    std::memcpy(buffer + offset, &m, sizeof(m));                                   offset += sizeof(m);
    std::memcpy(buffer + offset, &a, sizeof(a));                                   offset += sizeof(a);

    // ---- Helpers ----
    auto writeString = [&](const std::string& str) {
        uint16_t len = static_cast<uint16_t>(str.size());
        if (offset + sizeof(len) + len > bufferSize) return false;
        std::memcpy(buffer + offset, &len, sizeof(len));   offset += sizeof(len);
        if (len > 0) {
            std::memcpy(buffer + offset, str.data(), len);
            offset += len;
        }
        return true;
        };

    auto writeVector = [&](const std::vector<uint8_t>& vec) {
        uint16_t len = static_cast<uint16_t>(vec.size());
        if (offset + sizeof(len) + len > bufferSize) return false;
        std::memcpy(buffer + offset, &len, sizeof(len)); offset += sizeof(len);
        if (len > 0) {
            std::memcpy(buffer + offset, vec.data(), len);
            offset += len;
        }
        return true;
        };

    // ---- inodeInfo ----
    if (!writeVector(inode.inodeInfo.Password)) return 0;
    if (!writeString(inode.inodeInfo.UserName)) return 0;
    if (!writeString(inode.inodeInfo.Email))    return 0;

    std::memcpy(buffer + offset, &inode.inodeInfo.TotalSize, sizeof(inode.inodeInfo.TotalSize));
    offset += sizeof(inode.inodeInfo.TotalSize);

    std::memcpy(buffer + offset, &inode.inodeInfo.Usage, sizeof(inode.inodeInfo.Usage));
    offset += sizeof(inode.inodeInfo.Usage);

    // ---- Directory entries ----
    if (inode.isDirectory && inode.isUsed) {
        uint32_t count = static_cast<uint32_t>(inode.entries.size());
        if (offset + sizeof(count) > bufferSize) count = 0;
        if (count) {
            std::memcpy(buffer + offset, &count, sizeof(count));                   offset += sizeof(count);
            for (const auto& kv : inode.entries) {
                uint16_t nameLen = static_cast<uint16_t>(kv.first.size());
                size_t need = sizeof(nameLen) + nameLen + sizeof(int);
                if (offset + need > bufferSize) break;

                std::memcpy(buffer + offset, &nameLen, sizeof(nameLen));           offset += sizeof(nameLen);
                if (nameLen) {
                    std::memcpy(buffer + offset, kv.first.data(), nameLen);        offset += nameLen;
                }
                std::memcpy(buffer + offset, &kv.second, sizeof(kv.second));       offset += sizeof(kv.second);
            }
        }
    }

    // ---- checksum ----
    uint32_t checksum = CalculateChecksum(buffer, offset);
    if (offset + sizeof(checksum) <= bufferSize) {
        std::memcpy(buffer + offset, &checksum, sizeof(checksum));
        offset += sizeof(checksum);
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
        // ---- Basic fields ----
        std::memcpy(&inode.size, buffer + offset, sizeof(inode.size));        offset += sizeof(inode.size);
        std::memcpy(&inode.blocksUsed, buffer + offset, sizeof(inode.blocksUsed));  offset += sizeof(inode.blocksUsed);
        std::memcpy(&inode.firstBlock, buffer + offset, sizeof(inode.firstBlock));  offset += sizeof(inode.firstBlock);

        uint8_t flags = 0;
        std::memcpy(&flags, buffer + offset, sizeof(flags));                  offset += sizeof(flags);
        inode.isDirectory = (flags & 0x01) != 0;
        inode.isUsed = (flags & 0x02) != 0;
        inode.isDirty = (flags & 0x04) != 0;

        std::memcpy(&inode.creationTime, buffer + offset, sizeof(inode.creationTime));    offset += sizeof(inode.creationTime);
        std::memcpy(&inode.modificationTime, buffer + offset, sizeof(inode.modificationTime)); offset += sizeof(inode.modificationTime);
        std::memcpy(&inode.lastAccessed, buffer + offset, sizeof(inode.lastAccessed));    offset += sizeof(inode.lastAccessed);

        time_t now = time(nullptr);
        if (inode.creationTime <= 0 || inode.creationTime > now + 3600)        inode.creationTime = now;
        if (inode.modificationTime <= 0 || inode.modificationTime > now + 3600)inode.modificationTime = inode.creationTime;
        if (inode.lastAccessed <= 0 || inode.lastAccessed > now + 3600)        inode.lastAccessed = inode.creationTime;

        // ---- Helpers ----
        auto readString = [&](std::string& out) {
            if (offset + sizeof(uint16_t) > bufferSize) return false;
            uint16_t len = 0;
            std::memcpy(&len, buffer + offset, sizeof(len)); offset += sizeof(len);
            if (offset + len > bufferSize) return false;
            out.assign(buffer + offset, buffer + offset + len);
            offset += len;
            return true;
            };

        auto readVector = [&](std::vector<uint8_t>& vec) {
            if (offset + sizeof(uint16_t) > bufferSize) return false;
            uint16_t len = 0;
            std::memcpy(&len, buffer + offset, sizeof(len)); offset += sizeof(len);
            if (offset + len > bufferSize) return false;
            vec.assign(reinterpret_cast<const uint8_t*>(buffer + offset),
                reinterpret_cast<const uint8_t*>(buffer + offset + len));
            offset += len;
            return true;
            };

        // ---- inodeInfo ----
        if (!readVector(inode.inodeInfo.Password)) return 0;
        if (!readString(inode.inodeInfo.UserName)) return 0;
        if (!readString(inode.inodeInfo.Email))    return 0;

        if (offset + sizeof(inode.inodeInfo.TotalSize) > bufferSize) return 0;
        std::memcpy(&inode.inodeInfo.TotalSize, buffer + offset, sizeof(inode.inodeInfo.TotalSize));
        offset += sizeof(inode.inodeInfo.TotalSize);

        if (offset + sizeof(inode.inodeInfo.Usage) > bufferSize) return 0;
        std::memcpy(&inode.inodeInfo.Usage, buffer + offset, sizeof(inode.inodeInfo.Usage));
        offset += sizeof(inode.inodeInfo.Usage);

        // ---- Directory entries ----
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

                    if (child > 0) inode.entries.emplace(std::move(name), child);
                }
            }
        }

        // ---- checksum ----
        if (offset + sizeof(uint32_t) <= bufferSize) {
            uint32_t stored = 0;
            std::memcpy(&stored, buffer + offset, sizeof(stored));
            uint32_t calc = CalculateChecksum(copy.data(), offset);
            if (stored != calc) {
                std::cerr << "Checksum mismatch in inode — treating as invalid.\n";
                inode.isDirty = true;
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

bool MiniHSFS::DefragmentAndExtendInodes(size_t extraInodes) {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);
    std::cout << "Defragmenting to add " << extraInodes << " inodes..." << std::endl;

    // Calculate requirements
    size_t currentInodes = inodeTable.size();
    size_t newTotalInodes = currentInodes + extraInodes;
    size_t currentBlocks = inodeBlocks;
    size_t neededBlocks = CalculateBlocksForNewInodes(newTotalInodes);

    std::cout << "Current: " << currentInodes << " inodes, " << currentBlocks << " blocks" << std::endl;
    std::cout << "Needed: " << newTotalInodes << " inodes, " << neededBlocks << " blocks" << std::endl;

    if (neededBlocks <= currentBlocks) {
        std::cout << "No additional blocks needed, just extending in-memory table" << std::endl;
        // Only expand the table in memory
        inodeTable.resize(newTotalInodes);
        inodeBitmap.resize(newTotalInodes, false);
        inodeCount = newTotalInodes;

        // Super Block Update
        UpdateSuperblockForDynamicInodes();

        SaveInodeTable();

        return true;
    }

    size_t additionalBlocks = neededBlocks - currentBlocks;
    std::cout << "Need " << additionalBlocks << " additional blocks for inodes" << std::endl;

    // Determine the expansion location
    uint32_t expansionStart = disk.getSystemBlocks() + static_cast<uint32_t>(superBlockBlocks) + static_cast<uint32_t>(inodeBlocks);

    // Find the files that need to be transferred.
    auto filesToMove = FindFilesInRange(expansionStart, expansionStart + static_cast<uint32_t>(additionalBlocks));

    if (filesToMove.empty()) {
        std::cout << "No files need moving, can expand directly" << std::endl;
        return ExpandInodeAreaDirect(additionalBlocks, newTotalInodes);
    }

    std::cout << "Need to move " << filesToMove.size() << " files" << std::endl;

    // Search for free space at the end of the disk
    size_t totalMoveSize = 0;
    for (const auto& file : filesToMove) {
        totalMoveSize += file.blockCount;
    }

    uint32_t freeSpaceStart = FindFreeSpaceAtEnd(additionalBlocks + totalMoveSize);
    if (freeSpaceStart == 0) {
        std::cout << "Not enough free space for defragmentation" << std::endl;
        return false;
    }

    // Transfer files
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

    // Expansion after freeing up space
    if (!ExpandInodeAreaDirect(additionalBlocks, newTotalInodes)) {
        std::cerr << "Failed to expand inode area after moving files" << std::endl;
        RollbackMoves(moveOperations);
        return false;
    }

    std::cout << "Successfully added " << extraInodes << " inodes after defragmentation" << std::endl;
    return true;
}

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

void MiniHSFS::DefragmentDisk() {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    // Collect information about files that need to be defragmented
    std::vector<int> filesToDefrag;
    for (int i = 0; i < inodeCount; ++i) {
        if (inodeTable[i].isUsed &&
            !inodeTable[i].isDirectory &&
            inodeTable[i].blocksUsed > 1) {
            filesToDefrag.push_back(i);
        }
    }

    // Sort files by fragment size (largest first)
    std::sort(filesToDefrag.begin(), filesToDefrag.end(),
        [this](int a, int b) {
            return inodeTable[a].blocksUsed > inodeTable[b].blocksUsed;
        });

    // Defragment each file and print progress
    int totalFiles = static_cast<int>(filesToDefrag.size());
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

    // Rebuild the free block map
    RebuildFreeBlockList();
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

bool MiniHSFS::ExpandInodeAreaByInodes(size_t extraInodes) {
    if (extraInodes == 0) return true;

    size_t oldCount = inodeTable.size();
    size_t newCount = oldCount + extraInodes;

    size_t oldBlocks = CalculateBlocksForNewInodes(oldCount);
    size_t newBlocks = CalculateBlocksForNewInodes(newCount);

    if (newBlocks > inodeBlocks) {
        size_t addBlocks = newBlocks - inodeBlocks;
        VirtualDisk::Extent ext(disk.getSystemBlocks() + static_cast<uint32_t>(superBlockBlocks) + static_cast<uint32_t>(inodeBlocks), static_cast<uint32_t>(addBlocks));
        disk.allocateBlocks(ext.blockCount);

        std::vector<char> zero(disk.blockSize, 0);
        for (size_t i = 0; i < addBlocks; ++i)
            disk.writeData(zero, VirtualDisk::Extent(ext.startBlock + static_cast<uint32_t>(i), 1), "", true);

        inodeBlocks = newBlocks;
    }

    // Enlarge structures in memory
    inodeTable.resize(newCount);
    inodeBitmap.resize(newCount, false);
    inodeCount = newCount;

    UpdateSuperblockForDynamicInodes();
    return true;
}

bool MiniHSFS::ExpandInodeAreaDirect(size_t additionalBlocks, size_t newTotalInodes) {
    try {
        // Reserve additional blocks
        VirtualDisk::Extent extent(disk.getSystemBlocks() + static_cast<uint32_t>(superBlockBlocks) + static_cast<uint32_t>(inodeBlocks), static_cast<uint32_t>(additionalBlocks));
        disk.allocateBlocks(extent.blockCount);

        // Initialize new blocks
        std::vector<char> zeroBlock(disk.blockSize, 0);
        for (size_t i = 0; i < additionalBlocks; ++i) {
            disk.writeData(zeroBlock,
                VirtualDisk::Extent(extent.startBlock + static_cast<uint32_t>(i), 1), "", true);
        }

        // Update internal variables
        inodeBlocks += additionalBlocks;
        dataStartIndex += static_cast<int>(additionalBlocks);

        // Expand the table in memory
        inodeTable.resize(newTotalInodes);
        inodeBitmap.resize(newTotalInodes, false);
        inodeCount = newTotalInodes;

        // Super Block Update
        UpdateSuperblockForDynamicInodes();

        return true;

    }
    catch (const std::exception& e) {
        std::cerr << "Error expanding inode area: " << e.what() << std::endl;
        return false;
    }
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

void MiniHSFS::RebuildFreeInodesList() {
    freeInodesList.clear();

    for (size_t i = 1; i < inodeTable.size(); ++i) {
        if (!inodeTable[i].isUsed) {
            freeInodesList.push_back(static_cast<int>(i));
        }
    }

    // Update the pointer to the next free node
    nextFreeInode = freeInodesList.empty() ? inodeTable.size() : freeInodesList.front();
}

void MiniHSFS::RebuildInodeBitmap() {
    inodeBitmap.clear();
    inodeBitmap.resize(inodeTable.size());
    for (size_t i = 0; i < inodeTable.size(); ++i) {
        inodeBitmap[i] = inodeTable[i].isUsed;
    }
    nextFreeInode = 0;
}

void MiniHSFS::SaveInodeToDisk(int inodeIndex) {
    std::lock_guard<std::recursive_mutex> lock(fsMutex);

    if (inodeIndex < 0 || static_cast<size_t>(inodeIndex) >= inodeTable.size())
        throw std::out_of_range("SaveInodeToDisk: invalid inode index " + std::to_string(inodeIndex));

    // Additional verification: Ensure the inode is valid before saving
    if (!inodeTable[inodeIndex].isValid()) {
        throw std::runtime_error("SaveInodeToDisk: inode " + std::to_string(inodeIndex) + " is invalid");
    }

    // Prepare a fixed inode buffer of inodeSize
    std::vector<char> buf(inodeSize, 0);
    if (SerializeInode(inodeTable[inodeIndex], buf.data(), inodeSize) == 0)
        throw std::runtime_error("SaveInodeToDisk: serialize failed for inode " + std::to_string(inodeIndex));

    // Calculate its position within the inodes area (distance from the beginning of the inodes area)
    const size_t absByte = static_cast<size_t>(inodeIndex) * inodeSize;
    size_t       startBlockRel = absByte / disk.blockSize;
    size_t       offsetInBlock = absByte % disk.blockSize;

    const size_t bytesToWrite = inodeSize;
    size_t       blocksNeeded = (offsetInBlock + bytesToWrite + disk.blockSize - 1) / disk.blockSize;

    // If outside the current space -> expand
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

        // Read the block, modify the part for this inode, and then write
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