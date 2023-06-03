/* Copyright (c) 2023 Thierry FOURNIER (tfournier@arpalert.org) */

#include <sys/mman.h>

#include <stdio.h>
#include <stdlib.h>

#include "aho-corasick.h"

/* useful only with mmap mapping */
#define MAP_BLOC_SZ (1024*1024)

#define NODESLOTS(__n) ((__n)->first > (__n)->last ? 0 : (__n)->last - (__n)->first + 1)
#define NODESZ(__n) (sizeof(struct ac_node) + (NODESLOTS(__n) * sizeof(struct ac_node *)))
#define NODENEXT(__n) ((struct ac_node *)((char *)(__n) + NODESZ(__n)))
#define NODEEND(__r) ((struct ac_node *)((__r)->data + (__r)->length))
#define NODEADDOFFSET(__n, __o) ((struct ac_node *)((char *)(__n) + (__o)))
#define NODESUBOFFSET(__n, __o) ((struct ac_node *)((char *)(__n) - (__o)))

static inline
void *ac_realloc(struct ac_root *root, size_t size) {
	size_t new_size;
	void *out;

	if (root->total >= size)
		return root->data;

	new_size = root->total;
	while (new_size < size)
		new_size += MAP_BLOC_SZ;

	out = mmap(NULL, new_size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
	if (out == MAP_FAILED)
		return NULL;

	/* use memcopy because the mmap block cannot override */
	memcpy(out, root->data, root->total);

	/* remove old mmap block */
	munmap(root->data, root->total);

	/* Update root totzl size */
	root->total = new_size;
	return out;
}

static
void node_move(char *new_bloc, struct ac_root *root,
               struct ac_node **track0, struct ac_node **track1) {
	int i;
	int sign; /* minus if true */
	unsigned long offset;
	struct ac_node *n;

	/* positive or negative offset ? pointeur is unsigned 64 bit
	 * value, for safety, offset must be be unsigned 64 bit value,
	 * so we must process sign outside the value.
	 */
	sign = new_bloc < root->data;

	/* Compute offset, apply it on the requested node. */
	if (sign) {
		offset = root->data - new_bloc;
		if (track0 != NULL && *track0 != NULL)
			*track0 = NODESUBOFFSET(*track0, offset);
		if (track1 != NULL && *track1 != NULL)
			*track1 = NODESUBOFFSET(*track1, offset);
	} else {
		offset = new_bloc - root->data;
		if (track0 != NULL && *track0 != NULL)
			*track0 = NODEADDOFFSET(*track0, offset);
		if (track1 != NULL && *track1 != NULL)
			*track1 = NODEADDOFFSET(*track1, offset);
	}

	/* Update root */
	root->data = new_bloc;
	root->root = (struct ac_node *)new_bloc;

	/* Browse each bloc and apply offset */
	for (n = root->root; n < NODEEND(root); n = NODENEXT(n)) {
		if (n->fail != NULL) {
			if (sign) {
				n->fail = NODESUBOFFSET(n->fail, offset);
			} else {
				n->fail = NODEADDOFFSET(n->fail, offset);
			}
		}
		for (i = n->first; i <= n->last; i++) {
			if (n->children[i - n->first] != NULL) {
				if (sign) {
					n->children[i - n->first] = NODESUBOFFSET(n->children[i - n->first], offset);
				} else {
					n->children[i - n->first] = NODEADDOFFSET(n->children[i - n->first], offset);
				}
			}
		}
	}
}



/* This function growth a node, but it can change all pointers.
 * changing memory locations. The growth pointer is returned.
 * the incoming pointer must not be used after calling this
 * function. Note the "track" variable accept a pointer and
 * the function apply shit on this pointer ios needed.
 */
static inline
struct ac_node *node_growth(struct ac_root *root, struct ac_node *node, int slots, struct ac_node **track)
{
	struct ac_node *n;
	size_t cur_slots;
	size_t sz;
	char *new_bloc;
	int i;

	/* Compute the size to add to main memory bloc */
	if (node == NULL) {
		sz = sizeof(struct ac_node) + (slots * sizeof(struct ac_node *));
	} else if (node->first > node->last) {
		sz = slots * sizeof(struct ac_node *);
	} else {
		cur_slots = node->last - node->first + 1;
		sz = (slots - cur_slots) * sizeof(struct ac_node *);
	}

	/* execute effective realloc and update bloc length */
	new_bloc = ac_realloc(root, root->length + sz);
	if (new_bloc == NULL)
		return NULL;

	/* If realloc move the memory bloc, we must update all the pointers of
	 * all childrens link. Note the fail links are not computed at this time.
	 * Note the tree root node is always the first node of the memory bloc.
	 */
	if (new_bloc != root->data)
		node_move(new_bloc, root, &node, track);

	/* Special case : init new node and return it. New
	 * node is always the last allocated space
	 */
	if (node == NULL) {
		root->length += sz;
		n = (struct ac_node *)(root->data + root->length - sz);
		memset((char *)n, 0, sz);
		return n;
	}

	/* Apply offset on all children links greater than the growth node
	 * and then perform effective move of the memory bloc
	 */
	for (n = root->root; n < NODEEND(root); n = NODENEXT(n)) {
		for (i = n->first; i <= n->last; i++) {
			if (n->children[i - n->first] > node) {
				n->children[i - n->first] = NODEADDOFFSET(n->children[i - n->first], sz);
			}
		}
	}
	if (track != NULL && *track > node)
		*track = NODEADDOFFSET(*track, sz);

	root->length += sz;
	memmove((char *)NODENEXT(node) + sz,
	        (char *)NODENEXT(node),
	        (char *)NODEEND(root) - (char *)NODENEXT(node) - sz);

	return node;
}

/* compressed index add children */
static inline
struct ac_node *node_add_children(struct ac_root *root, struct ac_node *node, unsigned char c)
{
	unsigned char index;
	struct ac_node *new;

	/* This function perform two allocation, the first one is new node without
	 * childrens and the second is a growth of existing children. Each one of
	 * these allocation could modify tree base or pointer links. To growth
	 * safely the children array of link, we must known its pointer of new
	 * node. If we growth without the pointer value, the memory zone which
	 * receive its value remains uninitialized, and the next growth operation
	 * could read uninitialized pointer (in reallity we dont care).
	 */
	new = node_growth(root, NULL, 0, &node);
	if (new == NULL)
		return NULL;
	new->first = 1;

	/* first case : array not initialized */
	if (node->last < node->first) {
		node = node_growth(root, node, 1, &new);
		if (node == NULL)
			return NULL;
		node->first = c;
		node->last = c;
		index = 0;
	}

	/* second case : allocated space contains slot for a new node */
	else if (c >= node->first && c <= node->last) {
		index = c - node->first;
	}

	/* third case : new node is lower than low boundary */
	else if (c < node->first) {
		node = node_growth(root, node, node->last - c + 1, &new);
		if (node == NULL)
			return NULL;
		/* move memory from 0 to new destination */
		memmove(&node->children[node->first - c],
		        node->children,
		        (node->last - node->first + 1) * sizeof(struct ac_node *));
		/* reset from 0 for number of new slots */
		memset(node->children, 0, (node->first - c) * sizeof(struct ac_node *));
		node->first = c;
		index = 0;
	}

	/* third case : new node is upper than high boundary */
	else {
		node = node_growth(root, node, c - node->first + 1, &new);
		if (node == NULL)
			return NULL;
		/* reset from last slot + 1 for number of new slots */
		memset(&node->children[node->last - node->first + 1], 0, (c - node->last) * sizeof(struct ac_node *));
		node->last = c;
		index = c - node->first;
	}

	/* Index new node */
	node->children[index] = new;
	return new;
}

/* compressed index get children. Note the condition is always false
 * when first > last.
 */
static inline
struct ac_node *node_get_children(struct ac_node *node, unsigned char c)
{
	if (c <= node->last && c >= node->first)
		return node->children[c - node->first];
	return NULL;
}

/* compressed index get or new children. create children if not found */
static inline
struct ac_node *node_get_or_new_children(struct ac_root *root, struct ac_node *node, unsigned char c)
{
	struct ac_node *n;

	n = node_get_children(node, c);
	if (n != NULL)
		return n;
	return node_add_children(root, node, c);
}

struct ac_node_browse {
	struct ac_node *node;
	int c;
	int end;
};

/* browsing function : get next */
static inline 
struct ac_node *node_browse_next(struct ac_node_browse *bn)
{
	struct ac_node *node;

	while (bn->c <= bn->end) {
		node = bn->node->children[bn->c];
		bn->c++;
		if (node != NULL)
			return node;
	}
	return NULL;
}

/* browsing function : get first */
static inline
struct ac_node *node_browse_first(struct ac_node_browse *bn, struct ac_node *node)
{
	bn->node = node;
	bn->c = 0;
	bn->end = node->last - node->first;
	return node_browse_next(bn);
}

/* Init root node */
int ac_init_root(struct ac_root *root)
{
	root->total = 0;
	root->data = NULL;
	root->data = ac_realloc(root, sizeof(struct ac_node));
	if (root->data == NULL)
		return 0;
	memset(root->data, 0, sizeof(struct ac_node));
	root->length = sizeof(struct ac_node);
	root->root = (struct ac_node *)root->data;
	root->root->first = 1;
	return 1;
}

/* Insert word in a tree */
int ac_insert_wordl(struct ac_root *root, char *word, size_t len)
{
	struct ac_node *node;
	int i;

	/* Index wod */
	node = root->root;
	for (i = 0; i < len; i++) {
		node = node_get_or_new_children(root, node, (unsigned char)word[i]);
		if (node == NULL)
			return -1;
	}

	/* Mark match */
	node->match = len;
	return 0;
}

struct fifo {
	struct fifo_node *first;
	struct fifo_node *last;
};

struct fifo_node {
	struct ac_node *node;
	struct fifo_node *next;
};

static inline
void fifo_init(struct fifo *fifo)
{
	fifo->first = NULL;
	fifo->last = NULL;
}

static inline
int fifo_push(struct fifo *fifo, struct ac_node *node)
{
	struct fifo_node *fn;

	fn = malloc(sizeof(struct fifo_node));
	if (fn == NULL)
		return -1;

	fn->node = node;
	fn->next = NULL;
	if (fifo->last != NULL) {
		fifo->last->next = fn;
	}
	fifo->last = fn;
	if (fifo->first == NULL) {
		fifo->first = fn;
	}

	return 0;
}

static inline
struct ac_node *fifo_pop(struct fifo *fifo)
{
	struct fifo_node *fn;
	struct ac_node *node;

	fn = fifo->first;
	if (fn == NULL)
		return NULL;
	node = fn->node;
	fifo->first = fn->next;
	if (fifo->last == fn)
		fifo->last = NULL;
	free(fn);
	return node;
}

/* compute failure link */
int ac_finalize(struct ac_root *root)
{
	struct ac_node *node;
	struct ac_node *child;
	struct ac_node *fail_node;
	struct ac_node_browse bn;
	int c;
	struct fifo fifo;
	char *new_bloc;

	/* Convert the mmap allocated bloc to malloc'ed memory
	 * bloc and free the mmap bloc. When mmap id freed,
	 * the memory is really free and retruned to the system.
	 */
	new_bloc = malloc(root->length);
	if (new_bloc == NULL)
		return -1;
	memcpy(new_bloc, root->data, root->length);
	munmap(root->data, root->total);
	node_move(new_bloc, root, NULL, NULL);

	/* winit fifo ill contains node waiting for processing */
	fifo_init(&fifo);

	/* Append all root's children to the process fifo */
	for (node = node_browse_first(&bn, root->root); node != NULL; node = node_browse_next(&bn)) {

		/* first level node always have root as fail link */
		node->fail = root->root;

		/* Append node at last item of the queue */
		if (fifo_push(&fifo, node) != 0)
			return -1;
	}

	/* browse queue and configure "fail" links */
	while (1) {

		/* Get first node of the queue */
		node = fifo_pop(&fifo);
		if (node == NULL)
			break;

		/* browse childrens of current node */
		for (c = 0; c < 256; c++) {

			child = node_get_children(node, c);
			if (child == NULL)
				continue;

			/* find fail link for this child */
			fail_node = node->fail;
			while (fail_node != NULL && node_get_children(fail_node, c) == NULL) {
				fail_node = fail_node->fail;
			}
			if (fail_node == NULL) {
				child->fail = root->root;
			} else {
				child->fail = node_get_children(fail_node, c);
			}

			/* append child to the fifo queue */
			if (fifo_push(&fifo, child) != 0)
				return -1;
		}
	}

	return 0;
}

#define AC_RESULT(__x, __y) ((struct ac_result){.word = (__x), .length = (__y)})

// Fonction pour rechercher des mots dans le texte à l'aide de l'arbre de recherche de motifs
struct ac_result ac_search_next(struct ac_search *ac)
{
	unsigned char c;
	register int i;
	register short match;

	/* load counter in stack variable. This increase speed avoid dereference on each loop */
	i = ac->i;

	/* continue function at last stop */
	switch (ac->step) {
	case 0: break;
	case 1: goto continue_step_1;
	case 2: goto continue_step_2;
	}

	for (i = 0; i < ac->length; i++) {
		c = (unsigned char)ac->text[i];
		while (ac->node != NULL && node_get_children(ac->node, c) == NULL) {
			ac->node = ac->node->fail;
		}
		if (ac->node == NULL) {
			ac->node = ac->root->root;
		} else {
			ac->node = node_get_children(ac->node, c);
			match = ac->node->match;
			if (match > 0) {
				ac->step = 1;
				ac->i = i;
				return AC_RESULT(&ac->text[i - match + 1], match);
			}
continue_step_1:
			ac->fail_node = ac->node->fail;
			/* Check if fail nodes match */
			while (ac->fail_node != NULL) {
				match = ac->fail_node->match;
				if (match > 0) {
					ac->step = 2;
					ac->i = i;
					return AC_RESULT(&ac->text[i - match + 1], match);
				}
continue_step_2:
				ac->fail_node = ac->fail_node->fail;
			}
		}
	}
	return AC_RESULT(NULL, 0);
}

struct ac_result ac_search_firstl(struct ac_search *ac, struct ac_root *root, char *text, size_t length)
{
	ac->text = text;
	ac->length = length;
	ac->root = root;
	ac->node = root->root;
	ac->step = 0;

	return ac_search_next(ac);
}

struct ac_result ac_searchl(struct ac_root *root, char *text, size_t length)
{
	struct ac_search ac;

	return ac_search_firstl(&ac, root, text, length);
}
