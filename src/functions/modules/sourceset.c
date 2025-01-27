/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include "functions/modules/sourceset.h"

static bool
func_module_sourceset_source_set(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	if (!interp_args(wk, args_node, NULL, NULL, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_source_set);
	struct obj_source_set *ss = get_obj_source_set(wk, *res);
	make_obj(wk, &ss->rules, obj_array);
	return true;
}

const struct func_impl_name impl_tbl_module_sourceset[] = {
	{ "source_set", func_module_sourceset_source_set, tc_source_set, },
	{ NULL, NULL },
};

