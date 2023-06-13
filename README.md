Aho-Corasick multiple string search algorithm
=============================================

This is an implementation of Aho-Corasick algorithm written in C. The library is
fully functionnaly, anyway its first commits are base for an article about
memory saving [Comment optimiser la mémoire consommée en C](https://www.arpalert.org/memory_hunter.html).
The library doesn't have any dependencies.

The `test/test.c` file contains usage example.

Usage
-----

- Create root struct with `ac_init_root()`
- Insert many words int hte tree with `ac_insert_word()`
- Finalize tree to calculate Aho-Corasick failure links and free some memory with `ac_finalize()`
- Search all matching words in a text with `ac_search_first()` and `ac_search_next`

```C
struct ac_root root;
struct ac_search ac;
struct ac_result res;

ac_init_root(&root);
ac_insert_word(&root, "text");
ac_insert_word(&root, "word 1");
ac_insert_word(&root, "word 2");
ac_finalize(&root);
for (res = ac_search_first(&ac, &root, "does my text contains word 1 string ?");
     res.word != NULL;
     res = ac_search_next(&ac)) {
	printf("word <%.*s> match !\n", (int)res.length, res.word);
}
```
