#include "posix.h"

#include <string.h>

#include "coerce.h"
#include "error.h"
#include "functions/build_target.h"
#include "functions/common.h"
#include "functions/generator.h"
#include "lang/interpreter.h"
#include "log.h"
#include "platform/path.h"

bool
tgt_src_to_object_path(struct workspace *wk, const struct obj_build_target *tgt, obj src_file, bool relative, char res[PATH_MAX])
{
	obj src = *get_obj_file(wk, src_file);

	char private_path_rel[PATH_MAX], rel[PATH_MAX];
	const char *base, *private_path = get_cstr(wk, tgt->private_path);

	if (relative) {
		if (!path_relative_to(private_path_rel, PATH_MAX, wk->build_root, private_path)) {
			return false;
		}

		private_path = private_path_rel;
	}

	if (path_is_subpath(get_cstr(wk, tgt->build_dir), get_cstr(wk, src))) {
		// file is a generated source
		base = get_cstr(wk, tgt->build_dir);
	} else if (path_is_subpath(get_cstr(wk, tgt->cwd), get_cstr(wk, src))) {
		// file is in target cwd
		base = get_cstr(wk, tgt->cwd);
	} else if (path_is_subpath(wk->source_root, get_cstr(wk, src))) {
		// file is in source root
		base = wk->source_root;
	} else {
		// outside the source root
		base = NULL;
	}

	if (base) {
		if (!path_relative_to(rel, PATH_MAX, base, get_cstr(wk, src))) {
			return false;
		}
	} else {
		strncpy(rel, get_cstr(wk, src), PATH_MAX);
		uint32_t i;
		for (i = 0; rel[i]; ++i) {
			if (rel[i] == '/') {
				rel[i] = '_';
			}
		}
	}

	if (!path_join(res, PATH_MAX, private_path, rel)) {
		return false;
	} else if (!path_add_suffix(res, PATH_MAX, ".o")) {
		return false;
	}

	return true;
}

static bool
func_build_target_name(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	if (!interp_args(wk, args_node, NULL, NULL, NULL)) {
		return false;
	}

	*res = get_obj_build_target(wk, rcvr)->name;
	return true;
}

static bool
func_build_target_full_path(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	if (!interp_args(wk, args_node, NULL, NULL, NULL)) {
		return false;
	}

	struct obj_build_target *tgt = get_obj_build_target(wk, rcvr);

	*res = tgt->build_path;
	return true;
}

struct build_target_extract_objects_ctx {
	uint32_t err_node;
	struct obj_build_target *tgt;
	obj tgt_id;
	obj *res;
};

static enum iteration_result
build_target_extract_objects_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct build_target_extract_objects_ctx *ctx = _ctx;
	obj file;
	enum obj_type t = get_obj_type(wk, val);

	if (!typecheck(wk, ctx->err_node, val,
		tc_file | tc_string | tc_custom_target | tc_generated_list)) {
		return false;
	}

	switch (t) {
	case obj_string: {
		if (!coerce_string_to_file(wk, get_cstr(wk, ctx->tgt->cwd), val, &file)) {
			return ir_err;
		}
		break;
	}
	case obj_file:
		file = val;
		break;
	case obj_custom_target: {
		struct obj_custom_target *tgt = get_obj_custom_target(wk, val);
		if (!obj_array_flatten_one(wk, tgt->output, &file)) {
			interp_error(wk, ctx->err_node, "cannot coerce custom_target with multiple outputs to file");
			return ir_err;
		}
		break;
	}
	case obj_generated_list: {
		obj res;
		if (!generated_list_process_for_target(wk, ctx->err_node, val, ctx->tgt_id, false, &res)) {
			return ir_err;
		}

		if (!obj_array_foreach(wk, res, ctx, build_target_extract_objects_iter)) {
			return ir_err;
		}
		return ir_cont;
	}
	default:
		UNREACHABLE_RETURN;
	}

	if (!obj_array_in(wk, ctx->tgt->src, file)) {
		interp_error(wk, ctx->err_node, "%o is not in target sources (%o)", file, ctx->tgt->src);
		return ir_err;
	}

	enum compiler_language l;
	if (!filename_to_compiler_language(get_file_path(wk, file), &l)) {
		return ir_cont;
	}

	switch (l) {
	case compiler_language_cpp_hdr:
	case compiler_language_c_hdr:
		return ir_cont;
	case compiler_language_c_obj:
		obj_array_push(wk, *ctx->res, file);
		return ir_cont;
	case compiler_language_c:
	case compiler_language_cpp:
	case compiler_language_assembly:
	case compiler_language_llvm_ir:
		break;
	case compiler_language_objc:
	case compiler_language_count:
		assert(false && "unreachable");
		break;
	}

	char dest_path[PATH_MAX];
	if (!tgt_src_to_object_path(wk, ctx->tgt, file, false, dest_path)) {
		return ir_err;
	}

	obj new_file;
	make_obj(wk, &new_file, obj_file);
	*get_obj_file(wk, new_file) = make_str(wk, dest_path);
	obj_array_push(wk, *ctx->res, new_file);
	return ir_cont;
}

bool
build_target_extract_objects(struct workspace *wk, obj rcvr, uint32_t err_node, obj *res, obj arr)
{
	make_obj(wk, res, obj_array);

	struct build_target_extract_objects_ctx ctx = {
		.err_node = err_node,
		.res = res,
		.tgt = get_obj_build_target(wk, rcvr),
		.tgt_id = rcvr,
	};

	return obj_array_foreach_flat(wk, arr, &ctx, build_target_extract_objects_iter);
}

static bool
func_build_target_extract_objects(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { ARG_TYPE_GLOB | tc_string | tc_file | tc_custom_target | tc_generated_list }, ARG_TYPE_NULL };
	if (!interp_args(wk, args_node, an, NULL, NULL)) {
		return false;
	}

	return build_target_extract_objects(wk, rcvr, an[0].node, res, an[0].val);
}

static enum iteration_result
build_target_extract_all_objects_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct build_target_extract_objects_ctx *ctx = _ctx;

	return build_target_extract_objects_iter(wk, ctx, val);
}

bool
build_target_extract_all_objects(struct workspace *wk, uint32_t err_node, obj rcvr, obj *res)
{
	make_obj(wk, res, obj_array);

	struct build_target_extract_objects_ctx ctx = {
		.err_node = err_node,
		.res = res,
		.tgt = get_obj_build_target(wk, rcvr),
		.tgt_id = rcvr,
	};

	return obj_array_foreach_flat(wk, ctx.tgt->src, &ctx, build_target_extract_all_objects_iter);
}

static bool
func_build_target_extract_all_objects(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	enum kwargs {
		kw_recursive,
	};
	struct args_kw akw[] = {
		[kw_recursive] = { "recursive", obj_bool },
		0
	};
	if (!interp_args(wk, args_node, NULL, NULL, akw)) {
		return false;
	}

	if (akw[kw_recursive].set && !get_obj_bool(wk, akw[kw_recursive].val)) {
		interp_error(wk, akw[kw_recursive].node, "non-recursive extract_all_objects not supported");
		return false;
	}

	return build_target_extract_all_objects(wk, args_node, rcvr, res);
}

static bool
func_build_target_private_dir_include(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	if (!interp_args(wk, args_node, NULL, NULL, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_include_directory);
	struct obj_include_directory *inc = get_obj_include_directory(wk, *res);

	inc->path = get_obj_build_target(wk, rcvr)->private_path;
	return true;
}

static bool
func_build_target_found(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	if (!interp_args(wk, args_node, NULL, NULL, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, true);
	return true;
}

const struct func_impl_name impl_tbl_build_target[] = {
	{ "extract_all_objects", func_build_target_extract_all_objects, tc_array },
	{ "extract_objects", func_build_target_extract_objects, tc_array },
	{ "found", func_build_target_found, tc_bool },
	{ "full_path", func_build_target_full_path, tc_string },
	{ "name", func_build_target_name, tc_string },
	{ "path", func_build_target_full_path, tc_string },
	{ "private_dir_include", func_build_target_private_dir_include, tc_string },
	{ NULL, NULL },
};
