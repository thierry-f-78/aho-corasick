/* Copyright (c) 2023 Thierry FOURNIER (tfournier@arpalert.org) */

#include <sys/time.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aho-corasick.h"

#define EXPECTED_NB_MATCH 2804

void dot_tree(FILE *dotfh, struct ac_node *n, char ch, struct ac_node *root) {
	int i;

	/* display node definition */
	fprintf(dotfh, "\"%p\" [label=\"%c", n, ch);
	if (n->match != 0) {
		fprintf(dotfh, ", match=%d\",color=green", n->match);
	} else {
		fprintf(dotfh, "\"");
	}
	fprintf(dotfh, "];\n");

	/* display fail link if different from root */
	if (n->fail != root) {
		fprintf(dotfh, "\"%p\" -> \"%p\" [label=\"\",color=red];\n", n, n->fail);
	}

	/* display children links */
	for (i=0;i<=255;i++) {
		if (n->children[i] != NULL) {
			fprintf(dotfh, "\"%p\" -> \"%p\" [label=\"%c\"];\n", n, n->children[i], i);
			dot_tree(dotfh, n->children[i], (char)i, root);
		}
	}
}

static inline size_t csz(struct ac_node *n) {
	size_t sz;
	int i;

	sz = sizeof(struct ac_node) + (n->last - n->first + 1) * sizeof(struct ac_node *);
	for (i = n->first; i <= n->last; i++) {
		if (n->children[i - n->first] != NULL) {
			sz += csz(n->children[i - n->first]);
		}
	}
	return sz;
}

void usage(char *name) {
	printf("usage: %s <command>\n", name);
	printf("\n");
	printf("commands:\n");
	printf("\n");
	printf(" - dot <data> [<out>]  Produce 'dot' diagram. <daat> is the data file, <out> is\n");
	printf("                       the .dot file. Use followinf command to create PDF:\n");
	printf("                       dot -Tpdf -o <dot>.pdf <dot>\n");
	printf("\n");
	printf(" - sz <data>           Compute total size of memory used to store tree.\n");
	printf("\n");
	printf(" - check <data> [<nm>] Load <data> file and check lookup for each word. <nm>\n");
	printf("                       is the expected number of match (%d for the\n", EXPECTED_NB_MATCH);
	printf("                       reference data file\n");
	printf("\n");
	//      12345678901234567890123456789012345678901234567890123456789012345678901234567890
	printf(" - lk <data> [<txt>]   Search <data> words in <txt>. Text are default for\n");
	printf("                       provided data file.\n");
	printf(" - bench <data> [<txt>] [<loop>]\n");
	printf("                       Run benchmarck with <data> as list of words, <txt> as\n");
	printf("                       match text (default provided) and <loop> as number of\n");
	printf("                       loops (default 10 000 000)\n");
}

int main(int argc, char *argv[]) {
	struct ac_root root;
	char *filename;
	char *dotfile = NULL;
	FILE *dotfh = NULL;
	FILE *file;
	char buffer[1024];
	size_t len;
	struct ac_search ac;
	struct ac_result res;
	struct timeval tv_start;
	struct timeval tv_stop;
	int ok;
	double e;
	double s;
	int nb_matchs;
	int do_sz = 0;
	int do_check = 0;
	int do_lookup = 0;
	int do_bench = 0;
	int nmatch = -1;
	char *text = "hello etc/postgresql/pg_hba.conf world, this is a yaml_emit foo bar test.";
	unsigned int n_loops = 10000000;

	/* First arg is command */
	if (argc <= 1) {
		usage(argv[0]);
		exit(1);
	}
	if (strcmp(argv[1], "dot") == 0) {
		if (argc < 3 || argc > 4) {
			usage(argv[0]);
			exit(1);
		}
		filename = argv[2];
		if (argc == 4) {
			dotfile = argv[3];
			dotfh = fopen(dotfile, "w");
			if (dotfh == NULL) {
				fprintf(stderr, "Can't open output dot file '%s': %s\n", dotfile, strerror(errno));
				exit(1);
			}
		} else {
			dotfh = stderr;
		}
	} else if (strcmp(argv[1], "sz") == 0) {
		if (argc != 3) {
			usage(argv[0]);
			exit(1);
		}
		do_sz = 1;
		filename = argv[2];
	} else if (strcmp(argv[1], "check") == 0) {
		if (argc < 3 || argc > 4) {
			usage(argv[0]);
			exit(1);
		}
		do_check = 1;
		filename = argv[2];
		if (argc == 4) {
			nmatch = atoi(argv[3]);
		}
	} else if (strcmp(argv[1], "lk") == 0) {
		if (argc < 3 || argc > 4) {
			usage(argv[0]);
			exit(1);
		}
		do_lookup = 1;
		filename = argv[2];
		if (argc >= 4) {
			text = argv[3];
		}
	} else if (strcmp(argv[1], "bench") == 0) {
		if (argc < 3 || argc > 5) {
			usage(argv[0]);
			exit(1);
		}
		do_bench = 1;
		filename = argv[2];
		if (argc >= 4) {
			text = argv[3];
		}
		if (argc >= 5) {
			n_loops = atoi(argv[4]);
		}
	} else {
		usage(argv[0]);
		exit(1);
	}

	/* create tree root */
	if (!ac_init_root(&root)) {
		fprintf(stderr, "out of memory error\n");
		exit(1);
	}

	/* load word from datafile */
	file = fopen(filename, "r");
	if (file == NULL) {
		fprintf(stderr, "Can't open input data file '%s': %s\n", filename, strerror(errno));
		exit(1);
	}
	while (fgets(buffer, 1024, file)) {
		len = strlen(buffer);
		if (len > 0 && buffer[len-1] == '\n') {
			buffer[len-1] = '\0';
		}
		ac_insert_word(&root, buffer);
	}
	fclose(file);

	/* finalize aho-corasick tree - compute backlinks */
	ac_finalize(&root);

	/* Display size used by the tree */
	if (do_sz) {
		printf("data size: %zu\n", csz(root.root));
		exit(0);
	}

	/* digraph - display dot data */
	if (dotfh != NULL) {
		fprintf(dotfh, "digraph ER {\n");
		dot_tree(dotfh, root.root, '-', root.root);
		fprintf(dotfh, "}\n");
		fclose(dotfh);
		exit(0);
	}

	/* check lookup of all words in the input list */
	if (do_check) {
		nb_matchs = 0;
		file = fopen(filename, "r");
		if (file == NULL) {
			fprintf(stderr, "Can't open input data file '%s': %s\n", filename, strerror(errno));
			exit(1);
		}
		while (fgets(buffer, 1024, file)) {
			len = strlen(buffer);
			if (len > 0 && buffer[len-1] == '\n') {
				buffer[len-1] = '\0';
			}
			ok = 0;
			for (res = ac_search_first(&ac, &root, buffer); res.word != NULL; res = ac_search_next(&ac)) {
				nb_matchs++;
				if (strlen(buffer) == res.length && strncmp(res.word, buffer, res.length) == 0) {
					ok = 1;
				}
			}
			if (ok == 0) {
				fprintf(stderr, "Word <%s> not found\n", buffer);
				exit(1);
			}
		}
		fclose(file);
		if (nmatch != -1 && nb_matchs != nmatch) {
			fprintf(stderr, "Expect %d match, got %d\n", nmatch, nb_matchs);
			exit(1);
		}
		printf("ok\n");
		exit(0);
	}

	/* Perform test lookup */
	if (do_lookup) {
		for (res = ac_search_first(&ac, &root, text); res.word != NULL; res = ac_search_next(&ac)) {
			printf("%.*s\n", (int)res.length, res.word);
		}
		exit(0);
	}

	/* Perform benchmark */
	if (do_bench) {
		gettimeofday(&tv_start, NULL);
		for (len = 0; len < n_loops; len++) {
			for (res = ac_search_first(&ac, &root, text); res.word != NULL; res = ac_search_next(&ac));
		}
		gettimeofday(&tv_stop, NULL);

		s = (double)tv_start.tv_sec + ((double)tv_start.tv_usec / (double)n_loops);
		e = (double)tv_stop.tv_sec + ((double)tv_stop.tv_usec / (double)n_loops);
		fprintf(stderr, "%u / (%f - %f) = %f\n", n_loops, e, s, (double)n_loops / (e - s));
		exit(0);
	}

	exit(1);
}
