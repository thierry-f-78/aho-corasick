/* Copyright (c) 2023 Thierry FOURNIER (tfournier@arpalert.org) */

#ifndef __AHO_CORASICK_H__
#define __AHO_CORASICK_H__

#include <string.h>

struct ac_node {
	struct ac_node *children[256]; /* array of childrens */
	struct ac_node *fail; /* fallback to this node if browsing fails */
	char *word;
};

struct ac_root {
	struct ac_node *root;
};

struct ac_search {
	char *text;
	size_t length;
	struct ac_root *root;
	struct ac_node *node;
	struct ac_node *fail_node;
	int i;
	int step;
	unsigned char c;
};

/* Init root node */
int ac_init_root(struct ac_root *root);

/* Insert word in the aho-corasick tree with length */
int ac_insert_wordl(struct ac_root *root, char *word, size_t len);

/* Insert word in the aho-corasick tree without length */
static inline int ac_insert_word(struct ac_root *root, char *word)
{
	return ac_insert_wordl(root, word, strlen(word));
}

/* Finalize aho-corasick index. Never insert words after calling this function */
int ac_finalize(struct ac_root *root);

/* Init search engine with multiple result and length */
const char *ac_search_firstl(struct ac_search *ac, struct ac_root *root, char *text, size_t length);

/* Init search engine with multiple result and no length */
static inline
const char *ac_search_first(struct ac_search *ac, struct ac_root *root, char *text) {
	return ac_search_firstl(ac, root, text, strlen(text));
}

/* Search next words */
const char *ac_search_next(struct ac_search *ac);

/* Simple search which return only first word, or NULL if none match. Wants word length */
const char *ac_searchl(struct ac_root *root, char *text, size_t length);

/* Simple search which return only first word, or NULL if none match. do not want word length */
static inline
const char *ac_search(struct ac_root *root, char *text)
{
	return ac_searchl(root, text, strlen(text));
}

#endif
