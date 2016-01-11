/*
 *  * This Source Code Form is subject to the terms of the Mozilla Public License,
 *   * v. 2.0. If a copy of the MPL was not distributed with this file, You can
 *    * obtain one at http://mozilla.org/MPL/2.0/.
 *     */

/*
 *  * Copyright (c) 2015, Joyent, Inc.
 *   */
#include <umem.h>
#include <stdint.h>
#include <unistd.h>
#include <strings.h>
#include <graph.h>
#include <parse.h>
#include "jsonparse.h"

struct jsp_ast {
	lp_ast_t *jspa_tree;
};

struct jsp_walk {
	jsp_ast_t *jspw_tree;
	lp_ast_node_t *jspw_cur_key;
};
