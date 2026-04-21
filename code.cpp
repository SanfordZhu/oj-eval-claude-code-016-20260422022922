#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

const int PAGE_SIZE = 4096;
const int MAX_KEYS = 50;
const int KEY_SIZE = 64;

struct NodeHeader {
    uint8_t is_leaf;
    uint16_t key_count;
    uint32_t parent;
    uint32_t next;
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
};

class BPTree {
private:
    int index_fd;
    void* index_map;
    size_t index_size;
    uint32_t root;
    uint32_t free_list;
    uint32_t node_count;

    std::string index_file;

    void init_files() {
        index_fd = open(index_file.c_str(), O_RDWR | O_CREAT, 0644);

        if (index_fd < 0) {
            perror("open");
            exit(1);
        }

        off_t idx_size = lseek(index_fd, 0, SEEK_END);

        if (idx_size == 0) {
            char header[16] = {0};
            write(index_fd, header, 16);
            root = 0;
            free_list = 0;
            node_count = 1;
            index_size = PAGE_SIZE;
        } else {
            char header[16];
            lseek(index_fd, 0, SEEK_SET);
            read(index_fd, header, 16);
            root = *(uint32_t*)header;
            free_list = *(uint32_t*)(header + 4);
            node_count = *(uint32_t*)(header + 8);
            index_size = idx_size;
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
            char* node_ptr = (char*)index_map + PAGE_SIZE * (node + 1);
            free_list = *(uint32_t*)node_ptr;
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
            if (index_map == MAP_FAILED) {
                perror("mmap");
                exit(1);
            }
        }
        return node;
    }

    void free_node(uint32_t node) {
        char* node_ptr = (char*)index_map + PAGE_SIZE * (node + 1);
        *(uint32_t*)node_ptr = free_list;
        free_list = node;
        write_header();
    }

    void* get_node(uint32_t node) {
        if (node == 0 || node >= node_count) {
            return nullptr;
        }
        return (char*)index_map + PAGE_SIZE * (node + 1);
    }

    int key_compare(const char* k1, const char* k2) {
        return strcmp(k1, k2);
    }

    int find_pos_leaf(LeafNode* leaf, const char* key) {
        int pos = 0;
        while (pos < leaf->header.key_count && key_compare(key, leaf->keys[pos]) > 0) {
            pos++;
        }
        return pos;
    }

    int find_pos_internal(InternalNode* node, const char* key) {
        int pos = 0;
        while (pos < node->header.key_count && key_compare(key, node->keys[pos]) >= 0) {
            pos++;
        }
        return pos;
    }

    bool leaf_contains(LeafNode* leaf, const char* key, int value) {
        for (int i = 0; i < leaf->header.key_count; i++) {
            if (strcmp(leaf->keys[i], key) == 0 && leaf->values[i] == value) {
                return true;
            }
        }
        return false;
    }

    void insert_into_leaf(LeafNode* leaf, const char* key, int value) {
        int pos = find_pos_leaf(leaf, key);
        for (int i = leaf->header.key_count; i > pos; i--) {
            strcpy(leaf->keys[i], leaf->keys[i - 1]);
            leaf->values[i] = leaf->values[i - 1];
        }
        strcpy(leaf->keys[pos], key);
        leaf->values[pos] = value;
        leaf->header.key_count++;
    }

    void split_leaf(uint32_t leaf_idx, LeafNode* leaf, char* split_key, uint32_t* new_leaf_idx) {
        *new_leaf_idx = alloc_node();
        LeafNode* new_leaf = (LeafNode*)get_node(*new_leaf_idx);
        if (!new_leaf) return;
        memset(new_leaf, 0, PAGE_SIZE);
        new_leaf->header.is_leaf = 1;
        new_leaf->header.parent = leaf->header.parent;
        new_leaf->header.next = leaf->header.next;
        leaf->header.next = *new_leaf_idx;

        int mid = leaf->header.key_count / 2;
        while (mid > 0 && strcmp(leaf->keys[mid], leaf->keys[mid - 1]) == 0) {
            mid--;
        }

        new_leaf->header.key_count = leaf->header.key_count - mid;
        leaf->header.key_count = mid;

        for (int i = 0; i < new_leaf->header.key_count; i++) {
            strcpy(new_leaf->keys[i], leaf->keys[mid + i]);
            new_leaf->values[i] = leaf->values[mid + i];
        }

        strcpy(split_key, new_leaf->keys[0]);
    }

    void insert_into_internal(InternalNode* node, const char* key, uint32_t child) {
        int pos = find_pos_internal(node, key);
        for (int i = node->header.key_count; i > pos; i--) {
            strcpy(node->keys[i], node->keys[i - 1]);
            node->children[i + 1] = node->children[i];
        }
        strcpy(node->keys[pos], key);
        node->children[pos + 1] = child;
        node->header.key_count++;
    }

    void update_child_parent(uint32_t node_idx) {
        void* n = get_node(node_idx);
        if (!n) return;
        NodeHeader* h = (NodeHeader*)n;

        if (h->is_leaf) return;

        InternalNode* inode = (InternalNode*)n;
        for (int i = 0; i <= inode->header.key_count; i++) {
            void* child = get_node(inode->children[i]);
            if (child) {
                ((NodeHeader*)child)->parent = node_idx;
            }
        }
    }

    void split_internal(uint32_t node_idx, const char* new_key, uint32_t new_child) {
        InternalNode* node = (InternalNode*)get_node(node_idx);
        if (!node) return;

        uint32_t new_idx = alloc_node();
        InternalNode* new_node = (InternalNode*)get_node(new_idx);
        if (!new_node) return;
        memset(new_node, 0, PAGE_SIZE);
        new_node->header.is_leaf = 0;
        new_node->header.parent = node->header.parent;

        char temp_keys[MAX_KEYS + 1][KEY_SIZE];
        uint32_t temp_children[MAX_KEYS + 2];

        for (int i = 0; i < node->header.key_count; i++) {
            strcpy(temp_keys[i], node->keys[i]);
            temp_children[i] = node->children[i];
        }
        temp_children[node->header.key_count] = node->children[node->header.key_count];

        int pos = find_pos_internal(node, new_key);
        for (int i = node->header.key_count; i > pos; i--) {
            strcpy(temp_keys[i], temp_keys[i - 1]);
            temp_children[i + 1] = temp_children[i];
        }
        strcpy(temp_keys[pos], new_key);
        temp_children[pos] = node->children[pos];
        temp_children[pos + 1] = new_child;

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

        update_child_parent(node_idx);
        update_child_parent(new_idx);

        if (node_idx == root) {
            uint32_t new_root = alloc_node();
            InternalNode* nr = (InternalNode*)get_node(new_root);
            if (!nr) return;
            memset(nr, 0, PAGE_SIZE);
            nr->header.is_leaf = 0;
            nr->header.key_count = 1;
            nr->header.parent = 0;
            strcpy(nr->keys[0], split_key);
            nr->children[0] = node_idx;
            nr->children[1] = new_idx;
            node->header.parent = new_root;
            new_node->header.parent = new_root;
            root = new_root;
            write_header();
        } else {
            uint32_t parent = node->header.parent;
            InternalNode* p = (InternalNode*)get_node(parent);
            if (!p) return;
            if (p->header.key_count < MAX_KEYS) {
                insert_into_internal(p, split_key, new_idx);
                new_node->header.parent = parent;
            } else {
                split_internal(parent, split_key, new_idx);
            }
        }
    }

    uint32_t find_leaf(const char* key) {
        if (node_count == 1) return 0;

        uint32_t node = root;
        while (true) {
            void* n = get_node(node);
            if (!n) return 0;
            NodeHeader* h = (NodeHeader*)n;
            if (h->is_leaf) return node;

            InternalNode* inode = (InternalNode*)n;
            int pos = find_pos_internal(inode, key);
            node = inode->children[pos];
        }
    }

public:
    BPTree(const std::string& idx_file)
        : index_file(idx_file) {
        init_files();
    }

    ~BPTree() {
        if (index_map) munmap(index_map, index_size);
        if (index_fd >= 0) close(index_fd);
    }

    void insert(const std::string& key, int value) {
        if (node_count == 1) {
            root = alloc_node();
            LeafNode* leaf = (LeafNode*)get_node(root);
            if (!leaf) return;
            memset(leaf, 0, PAGE_SIZE);
            leaf->header.is_leaf = 1;
            leaf->header.key_count = 0;
            leaf->header.parent = 0;
            leaf->header.next = 0;
            write_header();
        }

        uint32_t leaf_idx = find_leaf(key.c_str());
        LeafNode* leaf = (LeafNode*)get_node(leaf_idx);
        if (!leaf) return;

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
            if (!new_leaf) return;
            if (key_compare(key.c_str(), split_key) < 0) {
                insert_into_leaf(leaf, key.c_str(), value);
            } else {
                insert_into_leaf(new_leaf, key.c_str(), value);
            }

            if (leaf_idx == root) {
                uint32_t new_root = alloc_node();
                InternalNode* nr = (InternalNode*)get_node(new_root);
                if (!nr) return;
                memset(nr, 0, PAGE_SIZE);
                nr->header.is_leaf = 0;
                nr->header.key_count = 1;
                nr->header.parent = 0;
                strcpy(nr->keys[0], split_key);
                nr->children[0] = leaf_idx;
                nr->children[1] = new_leaf_idx;
                leaf->header.parent = new_root;
                new_leaf->header.parent = new_root;
                root = new_root;
                write_header();
            } else {
                uint32_t parent = leaf->header.parent;
                InternalNode* p = (InternalNode*)get_node(parent);
                if (!p) return;
                if (p->header.key_count < MAX_KEYS) {
                    insert_into_internal(p, split_key, new_leaf_idx);
                    new_leaf->header.parent = parent;
                } else {
                    split_internal(parent, split_key, new_leaf_idx);
                }
            }
        }
    }

    void remove(const std::string& key, int value) {
        if (node_count == 1) return;

        uint32_t leaf_idx = find_leaf(key.c_str());
        LeafNode* leaf = (LeafNode*)get_node(leaf_idx);
        if (!leaf) return;

        for (int i = 0; i < leaf->header.key_count; i++) {
            if (strcmp(leaf->keys[i], key.c_str()) == 0 && leaf->values[i] == value) {
                for (int j = i; j < leaf->header.key_count - 1; j++) {
                    strcpy(leaf->keys[j], leaf->keys[j + 1]);
                    leaf->values[j] = leaf->values[j + 1];
                }
                leaf->header.key_count--;
                return;
            }
        }
    }

    std::vector<int> find(const std::string& key) {
        std::vector<int> result;
        if (node_count == 1) return result;

        uint32_t leaf_idx = find_leaf(key.c_str());
        LeafNode* leaf = (LeafNode*)get_node(leaf_idx);
        if (!leaf) return result;

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

    BPTree tree("bptree.idx");

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
                std::cout << "null
";
            } else {
                for (size_t i = 0; i < result.size(); i++) {
                    if (i > 0) std::cout << " ";
                    std::cout << result[i];
                }
                std::cout << "
";
            }
        }
    }

    return 0;
}
