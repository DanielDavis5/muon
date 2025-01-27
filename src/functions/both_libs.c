/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-FileCopyrightText: illiliti <illiliti@dimension.sh>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include "buf_size.h"
#include "functions/both_libs.h"
#include "functions/build_target.h"
#include "functions/common.h"
#include "lang/interpreter.h"
#include "log.h"

static bool
func_both_libs_get_shared_lib(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	if (!interp_args(wk, args_node, NULL, NULL, NULL)) {
		return false;
	}

	*res = get_obj_both_libs(wk, rcvr)->dynamic_lib;
	return true;
}

static bool
func_both_libs_get_static_lib(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	if (!interp_args(wk, args_node, NULL, NULL, NULL)) {
		return false;
	}

	*res = get_obj_both_libs(wk, rcvr)->static_lib;
	return true;
}

static obj
both_libs_rcvr_transform(struct workspace *wk, obj rcvr)
{
	return get_obj_both_libs(wk, rcvr)->dynamic_lib;
}

void
both_libs_build_impl_tbl(void)
{
	uint32_t i;
	for (i = 0; impl_tbl_build_target[i].name; ++i) {
		struct func_impl_name tmp = impl_tbl_build_target[i];
		tmp.rcvr_transform = both_libs_rcvr_transform;
		impl_tbl_both_libs[i] = tmp;
	}
}

struct func_impl_name impl_tbl_both_libs[] = {
	[ARRAY_LEN(impl_tbl_build_target) - 1] =
	{ "get_shared_lib", func_both_libs_get_shared_lib, tc_build_target },
	{ "get_static_lib", func_both_libs_get_static_lib, tc_build_target },
	{ NULL, NULL },
};
