/*
 * This Source Code Form is subject to the terms of the Mozilla Public License,
 * v. 2.0. If a copy of the MPL was not distributed with this file, You can
 * obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2016, Joyent, Inc.
 */
typedef enum jsp_type {
	INTEGER,
	FLOAT,
	STRING,
	OBJECT,
	ARRAY,
	NUL,
	BOOL
} jsp_type_t;

typedef struct jsp_ast jsp_ast_t;
typedef struct jsp_walk jsp_walk_t;
jsp_ast_t *jsp_parse(char *in, size_t sz);
/* TODO not implemented */
jsp_walk_t *jsp_create_walker();
void jsp_destroy_walker(jsp_walk_t *);
int jsp_walk_member(jsp_ast_t *a, jsp_walk_t *w, char *key);
size_t jsp_value_size(jsp_ast_t *a, jsp_walk_t *w);
jsp_type_t jsp_value_type(jsp_ast_t *a, jsp_walk_t *w);
int jsp_value_str(jsp_ast_t *a, jsp_walk_t *w, char *);
int jsp_value_int(jsp_ast_t *a, jsp_walk_t *w, uint64_t *);
int jsp_value_float(jsp_ast_t *a, jsp_walk_t *w, double *);
