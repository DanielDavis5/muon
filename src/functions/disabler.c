/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include "functions/common.h"
#include "functions/disabler.h"
#include "lang/interpreter.h"
#include "log.h"

static bool
func_disabler_found(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	if (!interp_args(wk, args_node, NULL, NULL, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, false);
	return true;
}

const struct func_impl_name impl_tbl_disabler[] = {
	{ "found", func_disabler_found, tc_bool },
	{ NULL, NULL },
};
