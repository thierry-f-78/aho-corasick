/* Copyright (c) 2023 Thierry FOURNIER (tfournier@arpalert.org) */

#include <stdlib.h>

#include "aho-corasick.h"

/* recurssive function which replace node pointer by another one.
 * this function must be called only during the tree creation and
 * never after backlinks created. Each node is encountered only
 * once. Chekc first if the replaced node is one of the children
 * node, to avoid browsing of all the tree if it is not required.
 */
static int node_replace(struct ac_node *node, struct ac_node *old, struct ac_node *new) {
	int i;
	for (i = node->first; i <= node->last; i++) {
		if (node->children[i - node->first] == old) {
			node->children[i - node->first] = new;
			return 1;
		}
	}
	for (i = node->first; i <= node->last; i++) {
		if (node->children[i - node->first] != NULL) {
			if (node_replace(node->children[i - node->first], old, new) == 1) {
				return 1;
			}
		}
	}
	return 0;
}

static inline
struct ac_node *node_growth(struct ac_root *root, struct ac_node *node, int slots)
{
	struct ac_node *new;

	new = realloc(node, sizeof(struct ac_node) + (slots * sizeof(struct ac_node *)));
	if (new == NULL)
		return NULL;
	if (new != node) {
		if (root->root == node)
			root->root = new;
		else
			node_replace(root->root, node, new);
	}
	return new;
}

/* compressed index add children */
static inline
struct ac_node *node_add_children(struct ac_root *root, struct ac_node *node, unsigned char c)
{
	unsigned char index;

	/* first case : array not initialized */
	if (node->last < node->first) {
		node = node_growth(root, node, 1);
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
		node = node_growth(root, node, node->last - c + 1);
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
		node = node_growth(root, node, c - node->first + 1);
		if (node == NULL)
			return NULL;
		/* reset from last slot + 1 for number of new slots */
		memset(&node->children[node->last - node->first + 1], 0, (c - node->last) * sizeof(struct ac_node *));
		node->last = c;
		index = c - node->first;
	}

	node->children[index] = calloc(sizeof(struct ac_node), 1);
	if (node->children[index] == NULL)
		return NULL;
	node->children[index]->first = 1;
	return node->children[index];
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
	root->root = calloc(sizeof(struct ac_node), 1);
	if (root->root == NULL)
		return 0;
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
