


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>


#include "kvstore.h"


// Key, Value --> 
// Modify 



kvs_hash_t global_hash;


//Connection 
// 'C' + 'o' + 'n'
static int _hash(char *key, int size) {

	if (!key) return -1;

	int sum = 0;
	int i = 0;

	while (key[i] != 0) {
		sum += key[i];
		i ++;
	}

	return sum % size;

}
/* 只申请节点本身；key/value 由 _create_node 另外申请。 */
static hashnode_t *hash_node_alloc(kvs_hash_t *hash)
{
    if (hash == NULL) {
        return NULL;
    }

    return kvs_malloc(sizeof(hashnode_t));
}

/* 只释放节点本身；key/value 由调用方先释放。 */
static void hash_storge_free(kvs_hash_t *hash, hashnode_t *node)
{
    if (hash == NULL || node == NULL) {
        return;
    }

    kvs_free(node);
}
hashnode_t *_create_node(kvs_hash_t *hash, char *key, char *value) {

	hashnode_t *node =hash_node_alloc(hash);
	if (node==NULL) return NULL;
	
#if ENABLE_KEY_POINTER
	char *kcopy = kvs_malloc(strlen(key) + 1);
	if (kcopy == NULL){
		hash_storge_free(hash,node);
		return NULL;
	}
	memcpy(kcopy, key, strlen(key) + 1);

	node->key = kcopy;

	char *kvalue = kvs_malloc(strlen(value) + 1);
	if (kvalue == NULL) { 
		kvs_free(kcopy);
		hash_storge_free(hash,node);
		return NULL;
	}
	memcpy(kvalue,value, strlen(value) + 1);

	node->value = kvalue;
	
#else
	strncpy(node->key, key, MAX_KEY_LEN);
	strncpy(node->value, value, MAX_VALUE_LEN);
#endif
	node->next = NULL;

	return node;
}
static void _hash_node_free(kvs_hash_t *hash,hashnode_t *node){
	if(hash==NULL||node==NULL){
		return;
	}
#if ENABLE_KEY_POINTER
	kvs_free(node->key);
	kvs_free(node->value);
#endif
	hash_storge_free(hash,node);
}

int kvs_hash_create(kvs_hash_t *hash)
{
    if (hash == NULL) {
        return -1;
    }

    /* Hash 只负责申请桶表，具体后端由统一入口决定。 */
    hash->nodes = kvs_malloc(sizeof(hashnode_t *) * MAX_TABLE_SIZE);
    if (hash->nodes == NULL) {
        return -1;
    }

    memset(hash->nodes, 0, sizeof(hashnode_t *) * MAX_TABLE_SIZE);

    hash->max_slots = MAX_TABLE_SIZE;
    hash->count = 0;
    return 0;
}

// 
void kvs_hash_destory(kvs_hash_t *hash) {

	if (!hash) return;

	int i = 0;
	for (i = 0;i < hash->max_slots;i ++) {
		hashnode_t *node = hash->nodes[i];

		while (node != NULL) { // error

			hashnode_t *next = node->next;
			_hash_node_free(hash, node);
			node = next;
			
		}
		hash->nodes[i] = NULL;
	}

	kvs_free(hash->nodes);
	hash->nodes = NULL;
	hash->max_slots = 0;
	hash->count = 0;
}

// 5 + 2

// mp
int kvs_hash_set(kvs_hash_t *hash, char *key, char *value) {

	if (!hash || !key || !value) return -1;

	int idx = _hash(key, MAX_TABLE_SIZE);

	hashnode_t *node = hash->nodes[idx];
#if 1
	while (node != NULL) {
        if (strcmp(node->key, key) == 0) {
            char *new_value = kvs_malloc(strlen(value) + 1);
            if (new_value == NULL) {
                return -2;
            }

            strcpy(new_value, value);
            kvs_free(node->value);
            node->value = new_value;
            return 0;
        }
		node = node->next;
	}
#endif

	hashnode_t *new_node = _create_node(hash,key, value);
	if(new_node == NULL) return -1;
	new_node->next = hash->nodes[idx];
	hash->nodes[idx] = new_node;
	hash->count ++;

	return 0;
}


char * kvs_hash_get(kvs_hash_t *hash, char *key) {

	if (!hash || !key) return NULL;

	int idx = _hash(key, MAX_TABLE_SIZE);

	hashnode_t *node = hash->nodes[idx];

	while (node != NULL) {

		if (strcmp(node->key, key) == 0) {
			return node->value;
		}

		node = node->next;
	}

	return NULL;

}


int kvs_hash_mod(kvs_hash_t *hash, char *key, char *value) {

	if (!hash || !key) return -1;

	int idx = _hash(key, MAX_TABLE_SIZE);

	hashnode_t *node = hash->nodes[idx];

	while (node != NULL) {

		if (strcmp(node->key, key) == 0) {
			break;
		}

		node = node->next;
	}

	if (node == NULL) {
		return 1;
	}

	// node --> 


	char *kvalue = kvs_malloc(strlen(value) + 1);
	if (kvalue == NULL) return -2;
	memcpy(kvalue, value, strlen(value) + 1);
	kvs_free(node->value);
	node->value = kvalue;
	
	return 0;
}

int kvs_hash_count(kvs_hash_t *hash) {
	return hash->count;
}

int kvs_hash_del(kvs_hash_t *hash, char *key) {
	if (!hash || !key) return -2;

	int idx = _hash(key, MAX_TABLE_SIZE);

	hashnode_t *head = hash->nodes[idx];
	if (head == NULL) return -1; // noexist
	// head node
	if (strcmp(head->key, key) == 0) {
		hash->nodes[idx] = head->next;
		_hash_node_free(hash, head);
		hash->count --;
		
		return 0;
	}

	hashnode_t *cur = head;
	while (cur->next != NULL) {
		if (strcmp(cur->next->key, key) == 0) break; // search node
		cur = cur->next;
	}

	if (cur->next == NULL) {
		
		return -1;
	}
	hashnode_t *tmp = cur->next;
	cur->next = tmp->next;

	_hash_node_free(hash, tmp);
	
	hash->count --;

	return 0;
}


int kvs_hash_exist(kvs_hash_t *hash, char *key) {

	char *value = kvs_hash_get(hash, key);
	if (!value) return 1;

	return 0;
	
}
int kvs_hash_foreach(
    kvs_hash_t *hash,
    kvs_visit_handler visitor,
    void *context
) {
    if (hash == NULL ||
        hash->nodes == NULL ||
        visitor == NULL) {
        return -1;
    }

    for (int i = 0; i < hash->max_slots; i++) {
        hashnode_t *node = hash->nodes[i];

        while (node != NULL) {
            if (node->key != NULL &&
                node->value != NULL) {

                int ret = visitor(
                    node->key,
                    node->value,
                    context
                );

                if (ret < 0) {
                    return -1;
                }
            }

            node = node->next;
        }
    }

    return 0;
}


#if 0
int main() {

	 kvs_hash_create(&hash);

	kvs_hash_set(&hash, "Teacher1", "King");
	kvs_hash_set(&hash, "Teacher2", "Darren");
	kvs_hash_set(&hash, "Teacher3", "Mark");
	kvs_hash_set(&hash, "Teacher4", "Vico");
	kvs_hash_set(&hash, "Teacher5", "Nick");

	char *value1 = kvs_hash_get(&hash, "Teacher1");
	printf("Teacher1 : %s\n", value1);

	int ret = kvs_hash_mod(&hash, "Teacher1", "King1");
	printf("mode Teacher1 ret : %d\n", ret);
	
	char *value2 = kvs_hash_get(&hash, "Teacher1");
	printf("Teacher2 : %s\n", value1);

	ret = kvs_hash_del(&hash, "Teacher1");
	printf("delete Teacher1 ret : %d\n", ret);

	ret = kvs_hash_exist(&hash, "Teacher1");
	printf("Exist Teacher1 ret : %d\n", ret);

	kvs_hash_destory(&hash);

	return 0;  
}

#endif

