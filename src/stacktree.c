#include "stacktree.h"

#include <stdlib.h>
#include <string.h>

static uint32_t cbox_bucket_count(uint32_t capacity)
{
	uint32_t buckets = 16;

	while (buckets < capacity * 2u && buckets < (1u << 30)) {
		buckets <<= 1;
	}

	return buckets;
}

static inline uint32_t cbox_edge_hash(uint32_t parent, uint32_t frame_id)
{
	/* Two rounds of a 32-bit mixer over the packed edge. */
	uint32_t hash = parent * 2654435761u ^ (frame_id + 0x9e3779b9u + (parent << 6) + (parent >> 2));

	hash ^= hash >> 16;
	hash *= 0x7feb352du;
	hash ^= hash >> 15;

	return hash;
}

static void cbox_stacktree_seed_root(cbox_stack_tree *tree)
{
	tree->nodes[0].parent = CBOX_NODE_NONE;
	tree->nodes[0].frame_id = UINT32_MAX;
	tree->nodes[0].samples = 0;
	tree->count = 1;
}

int cbox_stacktree_init(cbox_stack_tree *tree, uint32_t capacity)
{
	uint32_t buckets;

	memset(tree, 0, sizeof(*tree));

	if (capacity < 2) {
		return -1;
	}

	buckets = cbox_bucket_count(capacity);

	tree->nodes = (cbox_node *) calloc(capacity, sizeof(cbox_node));
	tree->buckets = (uint32_t *) calloc(buckets, sizeof(uint32_t));

	if (tree->nodes == NULL || tree->buckets == NULL) {
		cbox_stacktree_destroy(tree);
		return -1;
	}

	tree->capacity = capacity;
	tree->bucket_mask = buckets - 1;
	cbox_stacktree_seed_root(tree);

	return 0;
}

void cbox_stacktree_destroy(cbox_stack_tree *tree)
{
	free(tree->nodes);
	free(tree->buckets);
	memset(tree, 0, sizeof(*tree));
}

void cbox_stacktree_reset(cbox_stack_tree *tree)
{
	if (tree->nodes == NULL) {
		return;
	}

	if (tree->count > 1) {
		memset(tree->buckets, 0, ((size_t) tree->bucket_mask + 1) * sizeof(uint32_t));
	}

	cbox_stacktree_seed_root(tree);
	tree->dropped = 0;
	tree->total = 0;
}

uint32_t cbox_stacktree_child(cbox_stack_tree *tree, uint32_t parent, uint32_t frame_id)
{
	uint32_t hash, slot, id;

	if (tree->nodes == NULL || parent >= tree->count) {
		return CBOX_NODE_NONE;
	}

	hash = cbox_edge_hash(parent, frame_id);
	slot = hash & tree->bucket_mask;

	while (tree->buckets[slot] != 0) {
		id = tree->buckets[slot] - 1;

		if (tree->nodes[id].parent == parent && tree->nodes[id].frame_id == frame_id) {
			return id;
		}

		slot = (slot + 1) & tree->bucket_mask;
	}

	if (tree->count >= tree->capacity) {
		return CBOX_NODE_NONE;
	}

	id = tree->count++;
	tree->nodes[id].parent = parent;
	tree->nodes[id].frame_id = frame_id;
	tree->nodes[id].samples = 0;
	tree->buckets[slot] = id + 1;

	return id;
}

void cbox_stacktree_record(cbox_stack_tree *tree, uint32_t node, uint32_t weight)
{
	if (tree->nodes == NULL || node >= tree->count || weight == 0) {
		return;
	}

	tree->nodes[node].samples += weight;
	tree->total += weight;
}
