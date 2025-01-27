/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include "functions/common.h"
#include "functions/array.h"
#include "lang/interpreter.h"
#include "log.h"

static bool
func_array_length(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	if (!interp_args(wk, args_node, NULL, NULL, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_number);
	set_obj_number(wk, *res, get_obj_array(wk, rcvr)->len);
	return true;
}

static bool
func_array_get(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_number }, ARG_TYPE_NULL };
	struct args_norm ao[] = { { tc_any }, ARG_TYPE_NULL };
	if (!interp_args(wk, args_node, an, ao, NULL)) {
		return false;
	}

	int64_t i = get_obj_number(wk, an[0].val);

	if (!bounds_adjust(wk, get_obj_array(wk, rcvr)->len, &i)) {
		if (ao[0].set) {
			*res = ao[0].val;
		} else {
			interp_error(wk, an[0].node, "index out of bounds");
			return false;
		}
	} else {
		obj_array_index(wk, rcvr, i, res);
	}

	return true;
}

struct array_contains_ctx {
	obj item;
	bool found;
};

static enum iteration_result
array_contains_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct array_contains_ctx *ctx = _ctx;

	if (get_obj_type(wk, val) == obj_array) {
		obj_array_foreach(wk, val, ctx, array_contains_iter);
		if (ctx->found) {
			return ir_done;
		}
	}

	if (obj_equal(wk, val, ctx->item)) {
		ctx->found = true;
		return ir_done;
	}

	return ir_cont;
}

static bool
func_array_contains(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { tc_any }, ARG_TYPE_NULL };
	if (!interp_args(wk, args_node, an, NULL, NULL)) {
		return false;
	}

	struct array_contains_ctx ctx = { .item = an[0].val };
	obj_array_foreach(wk, rcvr, &ctx, array_contains_iter);

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, ctx.found);
	return true;
}

static bool
func_array_delete(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { tc_number }, ARG_TYPE_NULL };
	if (!interp_args(wk, args_node, an, NULL, NULL)) {
		return false;
	}

	int64_t idx = get_obj_number(wk, an[0].val);
	if (!boundscheck(wk, an[0].node, get_obj_array(wk, rcvr)->len, &idx)) {
		return false;
	}

	obj_array_del(wk, rcvr, idx);
	return true;
}

const struct func_impl_name impl_tbl_array[] = {
	{ "length", func_array_length, tc_number, true },
	{ "get", func_array_get, tc_any, true },
	{ "contains", func_array_contains, tc_bool, true },
	{ NULL, NULL },
};

const struct func_impl_name impl_tbl_array_internal[] = {
	{ "length", func_array_length, tc_number, true },
	{ "get", func_array_get, tc_any, true },
	{ "contains", func_array_contains, tc_bool, true },
	{ "delete", func_array_delete, },
	{ NULL, NULL },
};
