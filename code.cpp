#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

const int PAGE_SIZE = 4096;
const int MAX_KEYS = 60;
const int KEY_SIZE = 64;
const int VALUE_SIZE = 4;

struct NodeHeader {
    uint8_t is_leaf;
    uint16_t key_count;
};

struct InternalNode {
    NodeHeader header;
    char keys[MAX_KEYS][KEY_SIZE];
    uint32_t children[MAX_KEYS + 1];
};

struct LeafNode {
    NodeHeader header;
    char keys[MAX_KEYS][KEY_SIZE];
    int32_t values[MAX_KEYS];
    uint32_t next;
};

class BPTree {
private:
    int index_fd;
    int data_fd;
    void* index_map;
    size_t index_size;
    uint32_t root;
    uint32_t free_list;
    uint32_t node_count;

    std::string index_file;
    std::string data_file;

    void init_files() {
        index_fd = open(index_file.c_str(), O_RDWR | O_CREAT, 0644);
        data_fd = open(data_file.c_str(), O_RDWR | O_CREAT, 0644);

        if (index_fd < 0 || data_fd < 0) {
            perror("open");
            exit(1);
        }

        off_t idx_size = lseek(index_fd, 0, SEEK_END);
        off_t data_size = lseek(data_fd, 0, SEEK_END);

        if (idx_size == 0) {
            write(index_fd, "\0\0\0\0\0\0\0\0\0\0\0\0", 12);
            root = 0;
            free_list = 0;
            node_count = 1;
            index_size = PAGE_SIZE;
        } else {
            char header[12];
            lseek(index_fd, 0, SEEK_SET);
            read(index_fd, header, 12);
            root = *(uint32_t*)header;
            free_list = *(uint32_t*)(header + 4);
            node_count = *(uint32_t*)(header + 8);
            index_size = idx_size;
        }

        if (data_size == 0) {
            write(data_fd, "\0\0\0\0", 4);
        }

        index_map = mmap(NULL, index_size, PROT_READ | PROT_WRITE, MAP_SHARED, index_fd, 0);
        if (index_map == MAP_FAILED) {
            perror("mmap");
            exit(1);
        }
    }

    void write_header() {
        char* p = (char*)index_map;
        *(uint32_t*)p = root;
        *(uint32_t*)(p + 4) = free_list;
        *(uint32_t*)(p + 8) = node_count;
    }

    uint32_t alloc_node() {
        if (free_list) {
            uint32_t node = free_list;
            free_list = *(uint32_t*)((char*)index_map + PAGE_SIZE * (node + 1));
            write_header();
            return node;
        }
        uint32_t node = node_count++;
        write_header();

        if (index_size < (node + 2) * PAGE_SIZE) {
            munmap(index_map, index_size);
            index_size = (node + 2) * PAGE_SIZE;
            ftruncate(index_fd, index_size);
            index_map = mmap(NULL, index_size, PROT_READ | PROT_WRITE, MAP_SHARED, index_fd, 0);
        }
        return node;
    }

    void free_node(uint32_t node) {
        *(uint32_t*)((char*)index_map + PAGE_SIZE * (node + 1)) = free_list;
        free_list = node;
        write_header();
    }

    void* get_node(uint32_t node) {
        return (char*)index_map + PAGE_SIZE * (node + 1);
    }

    int key_compare(const char* k1, const char* k2) {
        return strcmp(k1, k2);
    }

    void insert_into_leaf(LeafNode* leaf, const char* key, int value) {
        int i = leaf->header.key_count - 1;
        while (i >= 0 && key_compare(key, leaf->keys[i]) < 0) {
            strcpy(leaf->keys[i + 1], leaf->keys[i]);
            leaf->values[i + 1] = leaf->values[i];
            i--;
        }
        strcpy(leaf->keys[i + 1], key);
        leaf->values[i + 1] = value;
        leaf->header.key_count++;
    }

    bool leaf_contains(LeafNode* leaf, const char* key, int value) {
        for (int i = 0; i < leaf->header.key_count; i++) {
            if (strcmp(leaf->keys[i], key) == 0 && leaf->values[i] == value) {
                return true;
            }
        }
        return false;
    }

    void split_leaf(uint32_t leaf_idx, LeafNode* leaf, char* split_key, uint32_t* new_leaf_idx) {
        *new_leaf_idx = alloc_node();
        LeafNode* new_leaf = (LeafNode*)get_node(*new_leaf_idx);
        memset(new_leaf, 0, PAGE_SIZE);
        new_leaf->header.is_leaf = 1;

        int mid = leaf->header.key_count / 2;
        new_leaf->header.key_count = leaf->header.key_count - mid;
        leaf->header.key_count = mid;

        for (int i = 0; i < new_leaf->header.key_count; i++) {
            strcpy(new_leaf->keys[i], leaf->keys[mid + i]);
            new_leaf->values[i] = leaf->values[mid + i];
        }

        new_leaf->next = leaf->next;
        leaf->next = *new_leaf_idx;

        strcpy(split_key, new_leaf->keys[0]);
    }

    void insert_into_parent(uint32_t parent, uint32_t left, const char* key, uint32_t right) {
        InternalNode* p = (InternalNode*)get_node(parent);

        if (p->header.key_count < MAX_KEYS) {
            int i = p->header.key_count - 1;
            while (i >= 0 && key_compare(key, p->keys[i]) < 0) {
                strcpy(p->keys[i + 1], p->keys[i]);
                p->children[i + 2] = p->children[i + 1];
                i--;
            }
            strcpy(p->keys[i + 1], key);
            p->children[i + 2] = right;
            p->header.key_count++;
        } else {
            split_internal(parent, left, key, right);
        }
    }

    void split_internal(uint32_t node_idx, uint32_t left_child, const char* new_key, uint32_t right_child) {
        InternalNode* node = (InternalNode*)get_node(node_idx);

        uint32_t new_idx = alloc_node();
        InternalNode* new_node = (InternalNode*)get_node(new_idx);
        memset(new_node, 0, PAGE_SIZE);

        char temp_keys[MAX_KEYS + 1][KEY_SIZE];
        uint32_t temp_children[MAX_KEYS + 2];

        for (int i = 0; i < node->header.key_count; i++) {
            strcpy(temp_keys[i], node->keys[i]);
            temp_children[i] = node->children[i];
        }
        temp_children[node->header.key_count] = node->children[node->header.key_count];

        int pos = 0;
        while (pos < node->header.key_count && key_compare(new_key, temp_keys[pos]) >= 0) {
            pos++;
        }

        for (int i = node->header.key_count; i > pos; i--) {
            strcpy(temp_keys[i], temp_keys[i - 1]);
            temp_children[i + 1] = temp_children[i];
        }
        strcpy(temp_keys[pos], new_key);
        temp_children[pos] = left_child;
        temp_children[pos + 1] = right_child;

        int mid = MAX_KEYS / 2;
        char split_key[KEY_SIZE];
        strcpy(split_key, temp_keys[mid]);

        node->header.key_count = mid;
        for (int i = 0; i < mid; i++) {
            strcpy(node->keys[i], temp_keys[i]);
            node->children[i] = temp_children[i];
        }
        node->children[mid] = temp_children[mid];

        new_node->header.key_count = MAX_KEYS - mid;
        for (int i = 0; i < new_node->header.key_count; i++) {
            strcpy(new_node->keys[i], temp_keys[mid + 1 + i]);
            new_node->children[i] = temp_children[mid + 1 + i];
        }
        new_node->children[new_node->header.key_count] = temp_children[MAX_KEYS + 1];

        if (node_idx == root) {
            uint32_t new_root = alloc_node();
            InternalNode* nr = (InternalNode*)get_node(new_root);
            memset(nr, 0, PAGE_SIZE);
            nr->header.is_leaf = 0;
            nr->header.key_count = 1;
            strcpy(nr->keys[0], split_key);
            nr->children[0] = node_idx;
            nr->children[1] = new_idx;
            root = new_root;
            write_header();
        } else {
            uint32_t parent = find_parent(root, node_idx);
            if (parent != (uint32_t)-1) {
                insert_into_parent(parent, node_idx, split_key, new_idx);
            }
        }
    }

    uint32_t find_parent(uint32_t current, uint32_t target) {
        void* node = get_node(current);
        NodeHeader* h = (NodeHeader*)node;

        if (h->is_leaf) {
            return (uint32_t)-1;
        }

        InternalNode* inode = (InternalNode*)node;
        for (int i = 0; i <= inode->header.key_count; i++) {
            if (inode->children[i] == target) {
                return current;
            }
            if (i < inode->header.key_count) {
                uint32_t child = inode->children[i];
                void* child_node = get_node(child);
                NodeHeader* ch = (NodeHeader*)child_node;
                if (!ch->is_leaf) {
                    uint32_t res = find_parent(child, target);
                    if (res != (uint32_t)-1) return res;
                }
            }
        }
        return (uint32_t)-1;
    }

    uint32_t find_leaf(const char* key) {
        if (node_count == 1) return 0;

        uint32_t node = root;
        while (true) {
            void* n = get_node(node);
            NodeHeader* h = (NodeHeader*)n;
            if (h->is_leaf) return node;

            InternalNode* inode = (InternalNode*)n;
            int i = 0;
            while (i < inode->header.key_count && key_compare(key, inode->keys[i]) >= 0) {
                i++;
            }
            node = inode->children[i];
        }
    }

public:
    BPTree(const std::string& idx_file, const std::string& dat_file)
        : index_file(idx_file), data_file(dat_file) {
        init_files();
    }

    ~BPTree() {
        if (index_map) munmap(index_map, index_size);
        if (index_fd >= 0) close(index_fd);
        if (data_fd >= 0) close(data_fd);
    }

    void insert(const std::string& key, int value) {
        if (node_count == 1) {
            root = alloc_node();
            LeafNode* leaf = (LeafNode*)get_node(root);
            memset(leaf, 0, PAGE_SIZE);
            leaf->header.is_leaf = 1;
            leaf->header.key_count = 0;
            leaf->next = 0;
            write_header();
        }

        uint32_t leaf_idx = find_leaf(key.c_str());
        LeafNode* leaf = (LeafNode*)get_node(leaf_idx);

        if (leaf_contains(leaf, key.c_str(), value)) {
            return;
        }

        if (leaf->header.key_count < MAX_KEYS) {
            insert_into_leaf(leaf, key.c_str(), value);
        } else {
            char split_key[KEY_SIZE];
            uint32_t new_leaf_idx;
            split_leaf(leaf_idx, leaf, split_key, &new_leaf_idx);

            LeafNode* new_leaf = (LeafNode*)get_node(new_leaf_idx);
            if (key_compare(key.c_str(), split_key) < 0) {
                insert_into_leaf(leaf, key.c_str(), value);
            } else {
                insert_into_leaf(new_leaf, key.c_str(), value);
            }

            if (leaf_idx == root) {
                uint32_t new_root = alloc_node();
                InternalNode* nr = (InternalNode*)get_node(new_root);
                memset(nr, 0, PAGE_SIZE);
                nr->header.is_leaf = 0;
                nr->header.key_count = 1;
                strcpy(nr->keys[0], split_key);
                nr->children[0] = leaf_idx;
                nr->children[1] = new_leaf_idx;
                root = new_root;
                write_header();
            } else {
                uint32_t parent = find_parent(root, leaf_idx);
                if (parent != (uint32_t)-1) {
                    insert_into_parent(parent, leaf_idx, split_key, new_leaf_idx);
                }
            }
        }
    }

    void remove(const std::string& key, int value) {
        if (node_count == 1) return;

        uint32_t leaf_idx = find_leaf(key.c_str());
        LeafNode* leaf = (LeafNode*)get_node(leaf_idx);

        int found = -1;
        for (int i = 0; i < leaf->header.key_count; i++) {
            if (strcmp(leaf->keys[i], key.c_str()) == 0 && leaf->values[i] == value) {
                found = i;
                break;
            }
        }

        if (found == -1) return;

        for (int i = found; i < leaf->header.key_count - 1; i++) {
            strcpy(leaf->keys[i], leaf->keys[i + 1]);
            leaf->values[i] = leaf->values[i + 1];
        }
        leaf->header.key_count--;
    }

    std::vector<int> find(const std::string& key) {
        std::vector<int> result;
        if (node_count == 1) return result;

        uint32_t leaf_idx = find_leaf(key.c_str());
        LeafNode* leaf = (LeafNode*)get_node(leaf_idx);

        for (int i = 0; i < leaf->header.key_count; i++) {
            if (strcmp(leaf->keys[i], key.c_str()) == 0) {
                result.push_back(leaf->values[i]);
            }
        }

        std::sort(result.begin(), result.end());
        return result;
    }
};

int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    BPTree tree("bptree.idx", "bptree.dat");

    int n;
    std::cin >> n;

    for (int i = 0; i < n; i++) {
        std::string cmd;
        std::cin >> cmd;

        if (cmd == "insert") {
            std::string key;
            int value;
            std::cin >> key >> value;
            tree.insert(key, value);
        } else if (cmd == "delete") {
            std::string key;
            int value;
            std::cin >> key >> value;
            tree.remove(key, value);
        } else if (cmd == "find") {
            std::string key;
            std::cin >> key;
            std::vector<int> result = tree.find(key);
            if (result.empty()) {
                std::cout << "null\n";
            } else {
                for (size_t i = 0; i < result.size(); i++) {
                    if (i > 0) std::cout << " ";
                    std::cout << result[i];
                }
                std::cout << "\n";
            }
        }
    }

    return 0;
}
