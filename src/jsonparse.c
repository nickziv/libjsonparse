/*
 * This Source Code Form is subject to the terms of the Mozilla Public License,
 * v. 2.0. If a copy of the MPL was not distributed with this file, You can
 * obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2016, Joyent, Inc.
 */
#include "jsonparse_impl.h"

/*
 * Parsing Unicode Strings
 * =======================
 *
 * We want to parse a unicode string, which is essentially a superset of ASCII.
 * The way unicode works is as follows. A character is of variable width: 1 - 4
 * bytes.
 *
 * The 1 byte chars correspond to our beloved ASCII chars.
 * All 1 byte chars have the following format:
 *
 * 	0xxxxxxx
 *
 * The multi byte chars have this format.
 *
 * 	110xxxxx 10xxxxxx	
 * 	1110xxxx 10xxxxxx 10xxxxxx	
 * 	11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
 *
 * Notice that the first byte _always_ has a different pattern, while the
 * trailing bytes always have the same pattern.
 *
 * So we essentially have 5 primitive tokens: first_byte1, .. first_byte4, and
 * trailing_byte.
 *
 * These primitives are used to implement these higher level grammar nodes:
 *
 * 	1byter, 2byter, 3byter, 4byter
 *
 * And that's essentially all you need to parse plain UTF-8. However, to parse
 * a JSON string, we have to _not_ parse anything that needs to be escaped,
 * such as quotes, backslashes, and so forth. We also have to parse the escape
 * sequences themselves.
 *
 * Here is how many bytes will be needed for each data array for the Xbchar
 * token types:
 *
 * 	0xxxxxxx        128
 * 	110xxxxx        32
 * 	1110xxxx        16
 * 	11110xxx        8
 * 	10xxxxxx	64
 *
 * These arrays are automatically generated, using simple increments.
 */
static void
jsp_utf8_byte_tok(lp_grmr_t *g, char *nm, uint8_t byte_min, uint8_t byte_max)
{
	uint8_t num_elems = byte_max - byte_min;
	char *data = malloc(num_elems);
	int i = 0;
	uint8_t c = byte_min;
	while (c <= byte_max) {
		if (c != '\t' && c != '\\' && c != '\"' &&
		    c != '\n' && c != '\b' && c != '\f' &&
		    c != '\r') {
			data[i] = c;
			//printf("Making %x parsable for %s.\n", c, nm);
			i++;
		}
		c++;
	}
	lp_tok_t *byte = lp_create_tok(g, nm);
	lp_add_tok_op(byte, ROP_ANYOF, 8, num_elems, data);
	return;
}

/*
 * The 4 different kinds of leading bytes:
 */

static void
jsp_lbyte1_tok(lp_grmr_t *g)
{
	uint8_t byte_min = 0x00;
	uint8_t byte_max = 0x7F;
	jsp_utf8_byte_tok(g, "lbyte1", byte_min, byte_max);
}

static void
jsp_lbyte2_tok(lp_grmr_t *g)
{
	uint8_t byte_min = 0xC0;
	uint8_t byte_max = 0xDF;
	jsp_utf8_byte_tok(g, "lbyte2", byte_min, byte_max);
}

static void
jsp_lbyte3_tok(lp_grmr_t *g)
{
	uint8_t byte_min = 0xE0;
	uint8_t byte_max = 0xEF;
	jsp_utf8_byte_tok(g, "lbyte3", byte_min, byte_max);
}

static void
jsp_lbyte4_tok(lp_grmr_t *g)
{
	uint8_t byte_min = 0xF0;
	uint8_t byte_max = 0xF7;
	jsp_utf8_byte_tok(g, "lbyte4", byte_min, byte_max);
}

/* These are the trailing bytes */
static void
jsp_tbyte_tok(lp_grmr_t *g)
{
	uint8_t byte_min = 0x80;
	uint8_t byte_max = 0xBF;
	jsp_utf8_byte_tok(g, "tbyte", byte_min, byte_max);
	lp_create_grmr_node(g, "tbyte", "tbyte", PARSER);
}

static void
jsp_1byter_gnode(lp_grmr_t *g)
{
	lp_create_grmr_node(g, "lbyte1", "lbyte1", PARSER);
	lp_create_grmr_node(g, "1byter", NULL, SEQUENCER);
	lp_add_child(g, "1byter", "lbyte1");
}

static void
jsp_2byter_gnode(lp_grmr_t *g)
{
	lp_create_grmr_node(g, "lbyte2", "lbyte2", PARSER);
	lp_create_grmr_node(g, "2byter", NULL, SEQUENCER);
	lp_add_child(g, "2byter", "lbyte2");
	lp_add_child(g, "2byter", "tbyte");
}

static void
jsp_3byter_gnode(lp_grmr_t *g)
{
	lp_create_grmr_node(g, "lbyte3", "lbyte3", PARSER);
	lp_create_grmr_node(g, "3byter", NULL, SEQUENCER);
	lp_add_child(g, "3byter", "lbyte3");
	lp_add_child(g, "3byter", "tbyte");
	lp_add_child(g, "3byter", "tbyte");
}

static void
jsp_4byter_gnode(lp_grmr_t *g)
{
	lp_create_grmr_node(g, "lbyte4", "lbyte4", PARSER);
	lp_create_grmr_node(g, "4byter", NULL, SEQUENCER);
	lp_add_child(g, "4byter", "lbyte4");
	lp_add_child(g, "4byter", "tbyte");
	lp_add_child(g, "4byter", "tbyte");
	lp_add_child(g, "4byter", "tbyte");
}

/*
 * In JSON the following chars may follow a backslash:
 *
 * 	\
 * 	"
 * 	t
 * 	n
 * 	b
 * 	f
 *      / (yeah I know)
 *      r
 *      u
 *
 * Almost none of these things can appear as a _literal_ char.  The slash and
 * the unicode-hex are exceptions, because we can also type a literal slash and
 * a literal unicode char (sans any of the above forbidden ones).
 */
static void
jsp_escape_gnode(lp_grmr_t *g)
{
	char *quote = "\\\"";
	lp_tok_t *esc_quote = lp_create_tok(g, "esc_quote");
	lp_add_tok_op(esc_quote, ROP_ONE, 8, 2, quote);
	lp_create_grmr_node(g, "esc_quote", "esc_quote", PARSER);

	char *backslash = "\\\\";
	lp_tok_t *esc_backslash = lp_create_tok(g, "esc_backslash");
	lp_add_tok_op(esc_backslash, ROP_ONE, 8, 2, backslash);
	lp_create_grmr_node(g, "esc_backslash", "esc_backslash", PARSER);

	char *tab = "\\t";
	lp_tok_t *esc_tab = lp_create_tok(g, "esc_tab");
	lp_add_tok_op(esc_tab, ROP_ONE, 8, 2, tab);
	lp_create_grmr_node(g, "esc_tab", "esc_tab", PARSER);

	char *nl = "\\n";
	lp_tok_t *esc_nl = lp_create_tok(g, "esc_nl");
	lp_add_tok_op(esc_nl, ROP_ONE, 8, 2, nl);
	lp_create_grmr_node(g, "esc_nl", "esc_nl", PARSER);

	char *bsp = "\\b";
	lp_tok_t *esc_bsp = lp_create_tok(g, "esc_bsp");
	lp_add_tok_op(esc_bsp, ROP_ONE, 8, 2, bsp);
	lp_create_grmr_node(g, "esc_bsp", "esc_bsp", PARSER);

	char *fmfd = "\\f";
	lp_tok_t *esc_fmfd = lp_create_tok(g, "esc_fmfd");
	lp_add_tok_op(esc_fmfd, ROP_ONE, 8, 2, fmfd);
	lp_create_grmr_node(g, "esc_fmfd", "esc_fmfd", PARSER);

	char *slash = "\\/";
	lp_tok_t *esc_slash = lp_create_tok(g, "esc_slash");
	lp_add_tok_op(esc_slash, ROP_ONE, 8, 2, slash);
	lp_create_grmr_node(g, "esc_slash", "esc_slash", PARSER);

	char *cr = "\\r";
	lp_tok_t *esc_cr = lp_create_tok(g, "esc_cr");
	lp_add_tok_op(esc_cr, ROP_ONE, 8, 2, cr);
	lp_create_grmr_node(g, "esc_cr", "esc_cr", PARSER);

	char *u = "\\u";
	char *hex_digits = "0123456789ABCDEF";
	lp_tok_t *esc_u = lp_create_tok(g, "esc_u");
	lp_add_tok_op(esc_u, ROP_ONE, 8, 2, u);
	lp_add_tok_op(esc_u, ROP_ANYOF, 8, 1, hex_digits);
	lp_add_tok_op(esc_u, ROP_ANYOF, 8, 1, hex_digits);
	lp_add_tok_op(esc_u, ROP_ANYOF, 8, 1, hex_digits);
	lp_add_tok_op(esc_u, ROP_ANYOF, 8, 1, hex_digits);
	lp_create_grmr_node(g, "esc_u", "esc_u", PARSER);

	lp_create_grmr_node(g, "escape_char", NULL, SPLITTER);
	lp_add_child(g, "escape_char", "esc_nl");
	lp_add_child(g, "escape_char", "esc_quote");
	lp_add_child(g, "escape_char", "esc_backslash");
	lp_add_child(g, "escape_char", "esc_slash");
	lp_add_child(g, "escape_char", "esc_u");
	lp_add_child(g, "escape_char", "esc_cr");
	lp_add_child(g, "escape_char", "esc_bsp");
	lp_add_child(g, "escape_char", "esc_fmfd");
}

static void
jsp_char_gnode(lp_grmr_t *g)
{
	/*
	 * The order in which we attempt the 5 types of characters does not
	 * matter because the first byte disambiguates everything. The only
	 * reasom we put 1byter and escape_char as the first 2 branches, is
	 * because we anticipate that those are two most common types of
	 * characters.
	 */
	lp_create_grmr_node(g, "char", NULL, SPLITTER);
	lp_add_child(g, "char", "1byter");
	lp_add_child(g, "char", "escape_char");
	lp_add_child(g, "char", "2byter");
	lp_add_child(g, "char", "3byter");
	lp_add_child(g, "char", "4byter");
}

/*
 * Here we implement our string of characters. We use recursion, but it is not
 * very intuitive, because of how libparse is implemented. Parse some char
 * repeatedly, one would think that all we have to do is this:
 *
 * 	char_loop: char char_loop
 *
 * However, this will simply keep trying to match a char until it no longer
 * can. As soon as an iteration of `char_loop` fails, we will start popping
 * back up the stack, and we will be back to where we started.
 *
 * What we actually want is this:
 *
 * 	char_loop: char char_next
 * 	char_next: char_loop || char
 *
 * `char_loop` is a SEQUENCER and `char_next` is a SPLITTER. This way, when
 * `char_loop` fails, we backtrack to the last call to `char_next`, and attempt
 * the other branch: `char`. The other branch will parse the last character,
 * and that will be that.
 */
static void
jsp_char_loop_gnode(lp_grmr_t *g)
{
	lp_create_grmr_node(g, "char_loop", NULL, SEQUENCER);
	lp_create_grmr_node(g, "char_next", NULL, SPLITTER);

	lp_add_child(g, "char_loop", "char");
	lp_add_child(g, "char_loop", "char_next");

	lp_add_child(g, "char_next", "char_loop");
	lp_add_child(g, "char_next", "char");
}

/*
 * Here we actually parse the string. There are three kinds of strings: those
 * with 0 characters, 1 character, and more than 1 character.
 *
 * 	empty_string: quote quote 
 * 	singleton_string: quote char quote 
 * 	regular_string: quote char_loop quote
 * 	string: regular_string || singleton_string || empty_string
 */
static void
jsp_string_gnode(lp_grmr_t *g)
{
	char *q = "\"";
	lp_tok_t *quote = lp_create_tok(g, "quote");
	lp_add_tok_op(quote, ROP_ONE, 8, 1, q);

	lp_create_grmr_node(g, "quote", "quote", PARSER);
	lp_create_grmr_node(g, "empty_string", NULL, SEQUENCER);
	lp_create_grmr_node(g, "singleton_string", NULL, SEQUENCER);
	lp_create_grmr_node(g, "regular_string", NULL, SEQUENCER);
	lp_create_grmr_node(g, "string", NULL, SPLITTER);

	lp_add_child(g, "empty_string", "quote");
	lp_add_child(g, "empty_string", "quote");

	lp_add_child(g, "singleton_string", "quote");
	lp_add_child(g, "singleton_string", "char");
	lp_add_child(g, "singleton_string", "quote");

	lp_add_child(g, "regular_string", "quote");
	lp_add_child(g, "regular_string", "char_loop");
	lp_add_child(g, "regular_string", "quote");

	lp_add_child(g, "string", "regular_string");
	lp_add_child(g, "string", "singleton_string");
	lp_add_child(g, "string", "empty_string");
}

static void
jsp_string(lp_grmr_t *g)
{
	/*
	 * We define the tokens for parsing each byte type.
	 */
	jsp_tbyte_tok(g);
	jsp_lbyte1_tok(g);
	jsp_lbyte2_tok(g);
	jsp_lbyte3_tok(g);
	jsp_lbyte4_tok(g);

	/*
	 * We define each variable width char.
	 */
	jsp_1byter_gnode(g);
	jsp_2byter_gnode(g);
	jsp_3byter_gnode(g);
	jsp_4byter_gnode(g);

	/*
	 * We define the escape sequences
	 */
	jsp_escape_gnode(g);

	/*
	 * We define the character parser.
	 */
	jsp_char_gnode(g);

	/*
	 * We define a repetition of characters.
	 */
	jsp_char_loop_gnode(g);

	/*
	 * Finally, we define a string.
	 */
	jsp_string_gnode(g);
}

static void
jsp_decimal_sci_gnode(lp_grmr_t *g)
{
	char *digits = "0123456789";
	char *neg = "-";
	char *plusmin = "+-";
	char *dot = ".";
	char *e = "eE";
	lp_tok_t *decimal = lp_create_tok(g, "decimal_sci");
	lp_add_tok_op(decimal, ROP_ZERO_ONE_PLUS, 8, 10, neg);
	lp_add_tok_op(decimal, ROP_ANYOF_ONE_PLUS, 8, 10, digits);
	lp_add_tok_op(decimal, ROP_ONE, 8, 1, dot);
	lp_add_tok_op(decimal, ROP_ANYOF_ONE_PLUS, 8, 10, digits);
	lp_add_tok_op(decimal, ROP_ANYOF, 8, 2, e);
	lp_add_tok_op(decimal, ROP_ANYOF_ZERO_ONE, 8, 2, plusmin);
	lp_add_tok_op(decimal, ROP_ANYOF_ONE_PLUS, 8, 10, digits);
}

static void
jsp_decimal_gnode(lp_grmr_t *g)
{
	char *digits = "0123456789";
	char *neg = "-";
	char *dot = ".";
	lp_tok_t *decimal = lp_create_tok(g, "decimal");
	lp_add_tok_op(decimal, ROP_ZERO_ONE_PLUS, 8, 10, neg);
	lp_add_tok_op(decimal, ROP_ANYOF_ONE_PLUS, 8, 10, digits);
	lp_add_tok_op(decimal, ROP_ONE, 8, 1, dot);
	lp_add_tok_op(decimal, ROP_ANYOF_ONE_PLUS, 8, 10, digits);
}

static void
jsp_wholenum_sci_gnode(lp_grmr_t *g)
{
	char *digits = "0123456789";
	char *neg = "-";
	char *plusmin = "+-";
	char *e = "eE";
	lp_tok_t *wholenum = lp_create_tok(g, "wholenum_sci");
	lp_add_tok_op(wholenum, ROP_ZERO_ONE_PLUS, 8, 10, neg);
	lp_add_tok_op(wholenum, ROP_ANYOF_ONE_PLUS, 8, 10, digits);
	lp_add_tok_op(wholenum, ROP_ANYOF, 8, 2, e);
	lp_add_tok_op(wholenum, ROP_ANYOF_ZERO_ONE, 8, 2, plusmin);
	lp_add_tok_op(wholenum, ROP_ANYOF_ONE_PLUS, 8, 10, digits);
}

static void
jsp_wholenum_gnode(lp_grmr_t *g)
{
	char *digits = "0123456789";
	char *neg = "-";
	lp_tok_t *wholenum = lp_create_tok(g, "wholenum");
	lp_add_tok_op(wholenum, ROP_ZERO_ONE, 8, 1, neg);
	lp_add_tok_op(wholenum, ROP_ANYOF_ONE_PLUS, 8, 10, digits);
}

static void
jsp_number_gnode(lp_grmr_t *g)
{
	lp_create_grmr_node(g, "wholenum_sci", "wholenum_sci", PARSER);
	lp_create_grmr_node(g, "decimal_sci", "decimal_sci", PARSER);
	lp_create_grmr_node(g, "wholenum", "wholenum", PARSER);
	lp_create_grmr_node(g, "decimal", "decimal", PARSER);

	lp_create_grmr_node(g, "number", NULL, SPLITTER);

	lp_add_child(g, "number", "decimal_sci");
	lp_add_child(g, "number", "wholenum_sci");
	lp_add_child(g, "number", "decimal");
	lp_add_child(g, "number", "wholenum");
}

static void
jsp_number(lp_grmr_t *g)
{
	jsp_decimal_sci_gnode(g);
	jsp_wholenum_sci_gnode(g);
	jsp_decimal_gnode(g);
	jsp_wholenum_gnode(g);
	jsp_number_gnode(g);
}

static void
jsp_bool(lp_grmr_t *g)
{
	char *t = "true";
	char *f = "false";
	lp_tok_t *T = lp_create_tok(g, "true");
	lp_tok_t *F = lp_create_tok(g, "false");
	lp_add_tok_op(T, ROP_ONE, 32, 1, t);
	lp_add_tok_op(F, ROP_ONE, 40, 1, f);
	lp_create_grmr_node(g, "true", "true", PARSER);
	lp_create_grmr_node(g, "false", "false", PARSER);

	lp_create_grmr_node(g, "bool", NULL, SPLITTER);
	lp_add_child(g, "bool", "false");
	lp_add_child(g, "bool", "true");
}

static void
jsp_null(lp_grmr_t *g)
{
	char *n = "null";
	lp_tok_t *null = lp_create_tok(g, "null");
	lp_add_tok_op(null, ROP_ONE, 32, 1, n);
	lp_create_grmr_node(g, "null", "null", PARSER);

}

static void
jsp_value(lp_grmr_t *g)
{
	/*
	 * We create some gnodes (object and arra), that don't yet exist. This
	 * is because their definitions are recursive with the definition of
	 * the 'value' gnode.  This is the only way to deal with the
	 * chicken-or-egg problem.
	 */
	lp_create_grmr_node(g, "object", NULL, SPLITTER);
	lp_create_grmr_node(g, "array", NULL, SPLITTER);

	lp_create_grmr_node(g, "value", NULL, SPLITTER);

	lp_add_child(g, "value", "object");
	lp_add_child(g, "value", "array");
	lp_add_child(g, "value", "string");
	lp_add_child(g, "value", "bool");
	lp_add_child(g, "value", "null");
	lp_add_child(g, "value", "number");
}

static void
jsp_array(lp_grmr_t *g)
{
	char *arr_open = "[";
	char *arr_close = "]";
	char *comma = ",";
	char *ws = " \t\n";

	lp_tok_t *aop = lp_create_tok(g, "arr_open");
	lp_tok_t *acl = lp_create_tok(g, "arr_close");
	lp_tok_t *com = lp_create_tok(g, "comma");
	lp_tok_t *tws = lp_create_tok(g, "ws");
	lp_add_tok_op(aop, ROP_ONE, 8, 1, arr_open);
	lp_add_tok_op(acl, ROP_ONE, 8, 1, arr_close);
	lp_add_tok_op(com, ROP_ONE, 8, 1, comma);
	lp_add_tok_op(tws, ROP_ANYOF_ZERO_ONE_PLUS, 8, 3, ws);

	lp_create_grmr_node(g, "arr_open", "arr_open", PARSER);
	lp_create_grmr_node(g, "arr_close", "arr_close", PARSER);
	lp_create_grmr_node(g, "comma", "comma", PARSER);
	lp_create_grmr_node(g, "ws", "ws", PARSER);
	lp_create_grmr_node(g, "value_loop", NULL, SEQUENCER);
	lp_create_grmr_node(g, "value_next", NULL, SPLITTER);

	lp_create_grmr_node(g, "empty_array", NULL, SEQUENCER);
	lp_create_grmr_node(g, "singleton_array", NULL, SEQUENCER);
	lp_create_grmr_node(g, "regular_array", NULL, SEQUENCER);

	lp_add_child(g, "value_loop", "value");
	lp_add_child(g, "value_loop", "ws");
	lp_add_child(g, "value_loop", "comma");
	lp_add_child(g, "value_loop", "ws");
	lp_add_child(g, "value_loop", "value_next");
	lp_add_child(g, "value_next", "value_loop");
	lp_add_child(g, "value_next", "value");

	lp_add_child(g, "regular_array", "arr_open");
	lp_add_child(g, "regular_array", "ws");
	lp_add_child(g, "regular_array", "value_loop");
	lp_add_child(g, "regular_array", "ws");
	lp_add_child(g, "regular_array", "arr_close");

	lp_add_child(g, "singleton_array", "arr_open");
	lp_add_child(g, "singleton_array", "ws");
	lp_add_child(g, "singleton_array", "value");
	lp_add_child(g, "singleton_array", "ws");
	lp_add_child(g, "singleton_array", "arr_close");

	lp_add_child(g, "empty_array", "arr_open");
	lp_add_child(g, "singleton_array", "ws");
	lp_add_child(g, "empty_array", "arr_close");

	lp_add_child(g, "array", "empty_array");
	lp_add_child(g, "array", "singleton_array");
	lp_add_child(g, "array", "regular_array");
}

static void
jsp_object(lp_grmr_t *g)
{
	char *obj_open = "{";
	char *obj_close = "}";
	char *colon = ":";

	lp_tok_t *oo = lp_create_tok(g, "obj_open");
	lp_tok_t *oc = lp_create_tok(g, "obj_close");
	lp_tok_t *col = lp_create_tok(g, "colon");

	lp_add_tok_op(oo, ROP_ONE, 8, 1, obj_open);
	lp_add_tok_op(oc, ROP_ONE, 8, 1, obj_close);
	lp_add_tok_op(col, ROP_ONE, 8, 1, colon);

	lp_create_grmr_node(g, "obj_open", "obj_open", PARSER);
	lp_create_grmr_node(g, "obj_close", "obj_close", PARSER);
	lp_create_grmr_node(g, "colon", "colon", PARSER);

	lp_create_grmr_node(g, "kvp", NULL, SEQUENCER);
	lp_create_grmr_node(g, "kvp_loop", NULL, SEQUENCER);
	lp_create_grmr_node(g, "kvp_next", NULL, SPLITTER);

	lp_create_grmr_node(g, "singleton_object", NULL, SEQUENCER);
	lp_create_grmr_node(g, "empty_object", NULL, SEQUENCER);
	lp_create_grmr_node(g, "regular_object", NULL, SEQUENCER);

	lp_add_child(g, "kvp", "string");
	lp_add_child(g, "kvp", "ws");
	lp_add_child(g, "kvp", "colon");
	lp_add_child(g, "kvp", "ws");
	lp_add_child(g, "kvp", "value");

	lp_add_child(g, "kvp_loop", "kvp");
	lp_add_child(g, "kvp_loop", "ws");
	lp_add_child(g, "kvp_loop", "comma");
	lp_add_child(g, "kvp_loop", "ws");
	lp_add_child(g, "kvp_loop", "kvp_next");

	lp_add_child(g, "kvp_next", "kvp_loop");
	lp_add_child(g, "kvp_next", "kvp");

	lp_add_child(g, "singleton_object", "obj_open");
	lp_add_child(g, "singleton_object", "ws");
	lp_add_child(g, "singleton_object", "kvp");
	lp_add_child(g, "singleton_object", "ws");
	lp_add_child(g, "singleton_object", "obj_close");

	lp_add_child(g, "empty_object", "obj_open");
	lp_add_child(g, "empty_object", "ws");
	lp_add_child(g, "empty_object", "obj_close");

	lp_add_child(g, "regular_object", "obj_open");
	lp_add_child(g, "regular_object", "ws");
	lp_add_child(g, "regular_object", "kvp_loop");
	lp_add_child(g, "regular_object", "ws");
	lp_add_child(g, "regular_object", "obj_close");

	lp_add_child(g, "object", "empty_object");
	lp_add_child(g, "object", "regular_object");
	lp_add_child(g, "object", "singleton_object");

	/*
	 * Any json input is just an object in the end.
	 */
	lp_root_grmr_node(g, "object");
}

static lp_grmr_t *
jsp_make_grammar()
{
	lp_grmr_t *g = lp_create_grammar("json");
	/*
	 * Note that these functions must be called in this order, because of
	 * the chicken and the egg.
	 */
	jsp_string(g);
	jsp_number(g);
	jsp_bool(g);
	jsp_null(g);
	jsp_value(g);
	jsp_array(g);
	jsp_object(g);
	return (g);
}

static lp_grmr_t *grammar;
static int init = 0;

jsp_ast_t *
jsp_parse(char *in, size_t sz)
{
	if (!init) {
		grammar = jsp_make_grammar();
		init = 1;
	}
	if (in == NULL || sz == 0) {
		return (NULL);
	}
	sz *= 8; /* transform size to bits */
	lp_ast_t *ast = lp_create_ast();
	jsp_ast_t *jast = malloc(sizeof (jsp_ast_t));
	jast->jspa_tree = ast;
	lp_dump_grmr(grammar);
	lp_run_grammar(grammar, ast, in, sz);
	lp_map_cc(ast, "key:val", "kvp", "string", "value");
	lp_map_pd(ast, "obj:key", "object", "key");
	lp_finish_run(ast);
	return (jast);
}

/*
 * This function will try to get to the object that's referred to by `key`. If
 * it finds a key with this value, it will return 0. Otherwise, -1. To get to a
 * deeply nested key, you must start from the root and work your down, much how
 * you would walk a chain of nested objects (foo.bar.baz).
 *
 * Once the key is found `w` will contain a reference to this key's value, as
 * well as its exact starting and ending location within the input provided to
 * the parse function above.
 */
int
jsp_walk_member(jsp_ast_t *a, jsp_walk_t *w, char *key)
{

}
