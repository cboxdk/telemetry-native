/*
 * cbox_telemetry — aggregated call tree.
 *
 * Samples are folded into a trie of call paths as they are taken, so a
 * 30-second profile costs the same memory as a 30-millisecond one. Each node
 * is (parent, frame) with a self-sample counter; the path from a node to the
 * root is the stack.
 *
 * Node 0 is always the root sentinel and holds no frame.
 */
#ifndef CBOX_STACKTREE_H
#define CBOX_STACKTREE_H

#include <stdbool.h>
#include <stdint.h>

#define CBOX_NODE_NONE UINT32_MAX
#define CBOX_NODE_ROOT 0u

typedef struct _cbox_node {
	uint32_t parent;
	uint32_t frame_id;
	uint32_t samples; /* samples whose innermost frame is this node */
} cbox_node;

typedef struct _cbox_stack_tree {
	cbox_node *nodes;
	uint32_t   count;
	uint32_t   capacity;

	uint32_t  *buckets; /* open addressing; 0 = empty, else node_id + 1 */
	uint32_t   bucket_mask;

	uint32_t   capacity_hits; /* times a node could not be created */
	uint64_t   total;   /* samples successfully recorded */
} cbox_stack_tree;

int  cbox_stacktree_init(cbox_stack_tree *tree, uint32_t capacity);
void cbox_stacktree_destroy(cbox_stack_tree *tree);
void cbox_stacktree_reset(cbox_stack_tree *tree);

/*
 * Find-or-create the child of `parent` for `frame_id`.
 * CBOX_NODE_NONE when the tree is full — the caller drops the sample.
 */
uint32_t cbox_stacktree_child(cbox_stack_tree *tree, uint32_t parent, uint32_t frame_id);

/* Attribute `weight` samples to `node` (the innermost frame of the stack). */
void cbox_stacktree_record(cbox_stack_tree *tree, uint32_t node, uint32_t weight);

#endif /* CBOX_STACKTREE_H */
