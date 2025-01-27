/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <string.h>

#include "buf_size.h"
#include "functions/array.h"
#include "functions/boolean.h"
#include "functions/both_libs.h"
#include "functions/build_target.h"
#include "functions/common.h"
#include "functions/compiler.h"
#include "functions/configuration_data.h"
#include "functions/custom_target.h"
#include "functions/dependency.h"
#include "functions/dict.h"
#include "functions/disabler.h"
#include "functions/environment.h"
#include "functions/external_program.h"
#include "functions/feature_opt.h"
#include "functions/file.h"
#include "functions/generator.h"
#include "functions/kernel.h"
#include "functions/machine.h"
#include "functions/meson.h"
#include "functions/modules.h"
#include "functions/modules/python.h"
#include "functions/number.h"
#include "functions/run_result.h"
#include "functions/source_configuration.h"
#include "functions/source_set.h"
#include "functions/string.h"
#include "functions/subproject.h"
#include "lang/interpreter.h"
#include "log.h"
#include "tracy.h"

// When true, disable functions with the .fuzz_unsafe attribute set to true.
// This is useful when running `muon internal eval` on randomly generated
// files, where you don't want to accidentally execute `run_command('rm',
// '-rf', '/')` for example
bool disable_fuzz_unsafe_functions = false;

// HACK: this is pretty terrible, but the least intrusive way to handle
// disablers in function arguments as they are currently implemented.  When
// interp_args sees a disabler, it sets this flag, and "fails".  In the
// function error handler we check this flag and don't raise an error if it is
// set but instead return disabler.
static bool disabler_among_args = false;
// HACK: we also need this for the is_disabler() function :(
bool disabler_among_args_immunity = false;

// HACK: This works like disabler_among_args kind of.  These opts should only
// ever be set by analyze_function().
static struct analyze_function_opts {
	bool do_analyze;
	bool pure_function;
	bool encountered_error;
	bool set_variable_special; // set to true if the function is set_variable()

	bool dump_signature; // used when dumping funciton signatures
} analyze_function_opts;

static bool
interp_args_interp_node(struct workspace *wk, uint32_t arg_node, obj *res)
{
	bool was_immune = disabler_among_args_immunity;
	disabler_among_args_immunity = false;

	if (!wk->interp_node(wk, arg_node, res)) {
		return false;
	}

	disabler_among_args_immunity = was_immune;
	return true;
}

static bool
next_arg(struct ast *ast, uint32_t *arg_node, uint32_t *kwarg_node, const char **kw, struct node **args)
{
	if (!*args || (*args)->type == node_empty) {
		return false;
	}

	assert((*args)->type == node_argument);

	if ((*args)->subtype == arg_kwarg) {
		*kw = get_node(ast, (*args)->l)->dat.s;
		*kwarg_node = (*args)->l;
		*arg_node = (*args)->r;
	} else {
		*kw = NULL;
		*arg_node = (*args)->l;
	}

	/* L("got arg %s:%s", *kw, node_to_s(*arg)); */

	if ((*args)->chflg & node_child_c) {
		*args = get_node(ast, (*args)->c);
	} else {
		*args = NULL;
	}

	return true;
}

struct function_signature {
	const char *name,
		   *posargs,
		   *varargs,
		   *optargs,
		   *kwargs,
		   *returns;
	bool is_method;

	const struct func_impl_name *impl;
};

struct {
	struct darr sigs;
} function_sig_dump;

static const char *
dump_type(struct workspace *wk, type_tag type)
{
	obj types = typechecking_type_to_arr(wk, type);
	obj typestr, sep = make_str(wk, "|");
	obj_array_join(wk, false, types, sep, &typestr);

	if (type & ARG_TYPE_ARRAY_OF) {
		obj_array_push(wk, types, make_strf(wk, "list[%s]", get_cstr(wk, typestr)));
		obj sorted;
		obj_array_sort(wk, NULL, types, obj_array_sort_by_str, &sorted);
		obj_array_join(wk, false, sorted, sep, &typestr);
	}

	return get_cstr(wk, typestr);
}

static int32_t
darr_sort_by_string(const void *a, const void *b, void *_ctx)
{
	return strcmp(*(const char **)a, *(const char **)b);
}

static void
dump_function_signature(struct workspace *wk,
	struct args_norm posargs[],
	struct args_norm optargs[],
	struct args_kw kwargs[])
{
	uint32_t i;

	struct function_signature *sig = darr_get(&function_sig_dump.sigs, function_sig_dump.sigs.len - 1);

	obj s;
	if (posargs) {
		s = make_str(wk, "");
		for (i = 0; posargs[i].type != ARG_TYPE_NULL; ++i) {
			if (posargs[i].type & ARG_TYPE_GLOB) {
				sig->varargs = get_cstr(wk, make_strf(wk, "    %s\n", dump_type(wk, posargs[i].type)));
				continue;
			}

			str_appf(wk, s, "    %s\n", dump_type(wk, posargs[i].type));
		}

		const char *ts = get_cstr(wk, s);
		if (*ts) {
			sig->posargs = ts;
		}
	}

	if (optargs) {
		s = make_str(wk, "");
		for (i = 0; optargs[i].type != ARG_TYPE_NULL; ++i) {
			str_appf(wk, s, "    %s\n", dump_type(wk, optargs[i].type));
		}
		sig->optargs = get_cstr(wk, s);
	}

	if (kwargs) {
		struct darr kwargs_list;
		darr_init(&kwargs_list, 8, sizeof(char *));

		for (i = 0; kwargs[i].key; ++i) {
			const char *v = get_cstr(wk, make_strf(wk, "    %s: %s\n", kwargs[i].key, dump_type(wk, kwargs[i].type)));
			darr_push(&kwargs_list, &v);
		}

		darr_sort(&kwargs_list, NULL, darr_sort_by_string);

		s = make_str(wk, "");
		for (i = 0; i < kwargs_list.len; ++i) {
			str_app(wk, s, *(const char **)darr_get(&kwargs_list, i));

		}
		sig->kwargs = get_cstr(wk, s);

		darr_destroy(&kwargs_list);
	}
}

static const char *
arity_to_s(struct args_norm positional_args[],
	struct args_norm optional_positional_args[],
	struct args_kw keyword_args[])
{
	static char buf[BUF_SIZE_2k + 1] = { 0 };

	uint32_t i, bufi = 0;

	bufi += snprintf(&buf[bufi], BUF_SIZE_2k - bufi, "(signature: ");

	if (positional_args) {
		bool glob = false;

		for (i = 0; positional_args[i].type != ARG_TYPE_NULL; ++i) {
			if (positional_args[i].type & ARG_TYPE_GLOB) {
				glob = true;
				break;
			}
		}

		if (i) {
			bufi += snprintf(&buf[bufi], BUF_SIZE_2k - bufi, "%d positional", i);
		}

		if (glob) {
			if (i) {
				bufi += snprintf(&buf[bufi], BUF_SIZE_2k - bufi, ", ");
			}
			bufi += snprintf(&buf[bufi], BUF_SIZE_2k - bufi, "varargs");
		}
	}

	if (optional_positional_args) {
		for (i = 0; optional_positional_args[i].type != ARG_TYPE_NULL; ++i) {
		}

		if (positional_args) {
			bufi += snprintf(&buf[bufi], BUF_SIZE_2k - bufi, ", ");
		}
		bufi += snprintf(&buf[bufi], BUF_SIZE_2k - bufi, "%d optional", i);
	}

	if (keyword_args) {
		for (i = 0; keyword_args[i].key; ++i) {
		}

		if (positional_args || optional_positional_args) {
			bufi += snprintf(&buf[bufi], BUF_SIZE_2k - bufi, ", ");
		}
		bufi += snprintf(&buf[bufi], BUF_SIZE_2k - bufi, "%d keyword", i);
	}

	if (!positional_args && !optional_positional_args && !keyword_args) {
		bufi += snprintf(&buf[bufi], BUF_SIZE_2k - bufi, "0 arguments");
	}

	bufi += snprintf(&buf[bufi], BUF_SIZE_2k - bufi, ")");

	return buf;
}

struct typecheck_function_arg_ctx {
	uint32_t err_node;
	obj arr;
	type_tag type;
};

static enum iteration_result
typecheck_function_arg_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct typecheck_function_arg_ctx *ctx = _ctx;

	if (!typecheck(wk, ctx->err_node, val, ctx->type)) {
		return ir_err;
	}

	obj_array_push(wk, ctx->arr, val);

	return ir_cont;
}

static enum iteration_result
typecheck_function_arg_check_disabler_iter(struct workspace *wk, void *_ctx, obj val)
{
	bool *among = _ctx;

	if (val == disabler_id) {
		*among = true;
		return ir_done;
	}
	return ir_cont;
}

static bool
typecheck_function_arg(struct workspace *wk, uint32_t err_node, obj *val, type_tag type)
{
	if (!disabler_among_args_immunity) {
		if (*val == disabler_id) {
			disabler_among_args = true;
			return false;
		} else if (get_obj_type(wk, *val) == obj_array) {
			bool among = false;
			obj_array_foreach_flat(wk, *val, &among, typecheck_function_arg_check_disabler_iter);
			if (among) {
				disabler_among_args = true;
				return false;
			}
		}
	}

	bool array_of = false;
	if (type & ARG_TYPE_ARRAY_OF) {
		array_of = true;
		type &= ~ARG_TYPE_ARRAY_OF;
	}

	assert((type & obj_typechecking_type_tag) || type < obj_type_count);

	// If obj_file or tc_file is requested, and the arugment is an array of
	// length 1, try to unpack it.
	if (!array_of && (type == obj_file || (type & tc_file) == tc_file)) {
		if (get_obj_type(wk, *val) == obj_array
		    && get_obj_array(wk, *val)->len == 1) {
			obj i0;
			obj_array_index(wk, *val, 0, &i0);
			if (get_obj_type(wk, i0) == obj_file) {
				*val = i0;
			}
		} else if (get_obj_type(wk, *val) == obj_typeinfo
			   && (get_obj_typeinfo(wk, *val)->type & tc_array) == tc_array) {
			return true;
		}
	}

	if (!array_of) {
		return typecheck(wk, err_node, *val, type);
	}

	struct typecheck_function_arg_ctx ctx = {
		.err_node = err_node,
		.type = type,
	};
	make_obj(wk, &ctx.arr, obj_array);

	if (get_obj_type(wk, *val) == obj_array) {
		if (!obj_array_foreach_flat(wk, *val, &ctx, typecheck_function_arg_iter)) {
			return false;
		}
	} else if (get_obj_type(wk, *val) == obj_typeinfo
		   && (get_obj_typeinfo(wk, *val)->type & tc_array) == tc_array) {
		return true;
	} else {
		if (!typecheck_function_arg_iter(wk, &ctx, *val)) {
			return false;
		}
	}

	*val = ctx.arr;
	return true;
}

#define ARITY arity_to_s(positional_args, optional_positional_args, keyword_args)

static bool
process_kwarg(struct workspace *wk, uint32_t kwarg_node, uint32_t arg_node, struct args_kw *keyword_args, const char *kw, obj val)
{
	uint32_t i;
	for (i = 0; keyword_args[i].key; ++i) {
		if (strcmp(kw, keyword_args[i].key) == 0) {
			break;
		}
	}

	if (!keyword_args[i].key) {
		interp_error(wk, kwarg_node, "invalid kwarg: '%s'", kw);
		return false;
	}

	if (!typecheck_function_arg(wk, arg_node, &val, keyword_args[i].type)) {
		return false;
	} else if (keyword_args[i].set) {
		interp_error(wk, arg_node, "keyword argument '%s' set twice", keyword_args[i].key);
		return false;
	}

	keyword_args[i].val = val;
	keyword_args[i].node = kwarg_node;
	keyword_args[i].set = true;

	return true;
}

struct process_kwarg_dict_ctx {
	uint32_t kwarg_node;
	uint32_t arg_node;
	struct args_kw *keyword_args;
};

static enum iteration_result
process_kwarg_dict_iter(struct workspace *wk, void *_ctx, obj key, obj val)
{
	struct process_kwarg_dict_ctx *ctx = _ctx;

	if (!process_kwarg(wk, ctx->kwarg_node, ctx->arg_node, ctx->keyword_args, get_cstr(wk, key), val)) {
		return ir_err;
	}

	return ir_cont;
}

static bool obj_tainted_by_typeinfo(struct workspace *wk, obj o);

static enum iteration_result
obj_tainted_by_typeinfo_dict_iter(struct workspace *wk, void *_ctx, obj k, obj v)
{
	if (obj_tainted_by_typeinfo(wk, k)
	    || obj_tainted_by_typeinfo(wk, v)) {
		return ir_err;
	}

	return ir_cont;
}

static enum iteration_result
obj_tainted_by_typeinfo_array_iter(struct workspace *wk, void *_ctx, obj v)
{
	if (obj_tainted_by_typeinfo(wk, v)) {
		return ir_err;
	}

	return ir_cont;
}

static bool
obj_tainted_by_typeinfo(struct workspace *wk, obj o)
{
	if (!o) {
		return true;
	}

	switch (get_obj_type(wk, o)) {
	case obj_typeinfo:
		return true;
	case obj_array:
		return !obj_array_foreach(wk, o, NULL, obj_tainted_by_typeinfo_array_iter);
	case obj_dict:
		return !obj_dict_foreach(wk, o, NULL, obj_tainted_by_typeinfo_dict_iter);
	default:
		return false;
	}
}

bool
interp_args(struct workspace *wk, uint32_t args_node,
	struct args_norm positional_args[],
	struct args_norm optional_positional_args[],
	struct args_kw keyword_args[])
{
	if (analyze_function_opts.dump_signature) {
		dump_function_signature(wk, positional_args, optional_positional_args, keyword_args);
		return false;
	}

	const char *kw;
	uint32_t arg_node, kwarg_node;
	uint32_t i, stage;
	struct args_norm *an[2] = { positional_args, optional_positional_args };
	struct node *args = get_node(wk->ast, args_node);

	for (stage = 0; stage < 2; ++stage) {
		if (!an[stage]) {
			continue;
		}

		for (i = 0; an[stage][i].type != ARG_TYPE_NULL; ++i) {
			if (an[stage][i].type & ARG_TYPE_GLOB) {
				assert(stage == 0 && "glob args must not be optional");
				assert(!optional_positional_args && "glob args cannot be followed by optional args");
				assert(an[stage][i + 1].type == ARG_TYPE_NULL && "glob args must come last");
				assert(!(an[stage][i].type & ARG_TYPE_ARRAY_OF) && "glob args are implicitly ARG_TYPE_ARRAY_OF");

				an[stage][i].type &= ~ARG_TYPE_GLOB;

				bool set_arg_node = false;

				make_obj(wk, &an[stage][i].val, obj_array);
				an[stage][i].set = true;

				while (next_arg(wk->ast, &arg_node, &kwarg_node, &kw, &args)) {
					if (kw) {
						goto kwargs;
					}

					if (!set_arg_node) {
						an[stage][i].node = arg_node;
						set_arg_node = true;
					}

					obj val;
					if (!interp_args_interp_node(wk, arg_node, &val)) {
						return false;
					}

					// If we get an array, but that isn't a valid type here, flatten it.
					if ((get_obj_type(wk, val) == obj_array
					     || (get_obj_type(wk, val) == obj_typeinfo
						 && (get_obj_typeinfo(wk, val)->type & tc_array) == tc_array))
					    && !(an[stage][i].type == tc_any
						 || an[stage][i].type == obj_array
						 || (an[stage][i].type & tc_array) == tc_array)
					    ) {
						if (get_obj_type(wk, val) == obj_typeinfo) {
							// TODO typecheck subtype
							obj_array_push(wk, an[stage][i].val, val);
						} else {
							if (!typecheck_function_arg(wk, arg_node, &val, ARG_TYPE_ARRAY_OF | an[stage][i].type)) {
								return false;
							}

							obj_array_extend_nodup(wk, an[stage][i].val, val);
						}
					} else {
						if (!typecheck_function_arg(wk, arg_node, &val, an[stage][i].type)) {
							return false;
						}
						obj_array_push(wk, an[stage][i].val, val);
					}
				}

				if (!set_arg_node) {
					an[stage][i].node = args_node;
				}
				continue;
			}

			if (!next_arg(wk->ast, &arg_node, &kwarg_node, &kw, &args)) {
				if (stage == 0) { // required
					interp_error(wk, args_node, "missing arguments %s", ARITY);
					return false;
				} else if (stage == 1) { // optional
					goto end;
				}
			}

			if (kw) {
				if (stage == 0) {
					interp_error(wk, kwarg_node, "unexpected kwarg before required arguments %s", ARITY);
					return false;
				}

				goto kwargs;
			}

			if (!interp_args_interp_node(wk, arg_node, &an[stage][i].val)) {
				return false;
			}

			if (!typecheck_function_arg(wk, arg_node, &an[stage][i].val, an[stage][i].type)) {
				return false;
			}

			an[stage][i].node = arg_node;
			an[stage][i].set = true;
		}
	}

	if (keyword_args) {
		while (next_arg(wk->ast, &arg_node, &kwarg_node, &kw, &args)) {
			goto process_kwarg;
kwargs:
			if (!keyword_args) {
				interp_error(wk, args_node, "this function does not accept kwargs %s", ARITY);
				return false;
			}
process_kwarg:
			if (!kw) {
				interp_error(wk, arg_node, "non-kwarg after kwargs %s", ARITY);
				return false;
			}

			obj val;
			if (!interp_args_interp_node(wk, arg_node, &val)) {
				return false;
			}

			if (strcmp(kw, "kwargs") == 0) {
				if (!typecheck(wk, arg_node, val, obj_dict)) {
					return false;
				}

				struct process_kwarg_dict_ctx ctx = {
					.kwarg_node = kwarg_node,
					.arg_node = arg_node,
					.keyword_args = keyword_args
				};

				if (get_obj_type(wk, val) != obj_typeinfo) {
					if (!obj_dict_foreach(wk, val, &ctx, process_kwarg_dict_iter)) {
						return false;
					}
				}
			} else {
				if (!process_kwarg(wk, kwarg_node, arg_node, keyword_args, kw, val)) {
					return false;
				}
			}
		}

		for (i = 0; keyword_args[i].key; ++i) {
			if (keyword_args[i].required && !keyword_args[i].set) {
				interp_error(wk, args_node, "missing required kwarg: %s", keyword_args[i].key);
				return false;
			}
		}
	} else if (next_arg(wk->ast, &arg_node, &kwarg_node, &kw, &args)) {
		if (kw) {
			interp_error(wk, kwarg_node, "this function does not accept kwargs %s", ARITY);
		} else {
			interp_error(wk, arg_node, "too many arguments %s", ARITY);
		}

		return false;
	}

end:
	if (analyze_function_opts.do_analyze) {
		bool typeinfo_among_args = false;

		for (stage = 0; stage < 2; ++stage) {
			if (!an[stage]) {
				continue;
			}

			for (i = 0; an[stage][i].type != ARG_TYPE_NULL; ++i) {
				if (!an[stage][i].set) {
					continue;
				}

				if (analyze_function_opts.set_variable_special && stage == 0 && i == 1) {
					// allow set_variable() to be called
					// even if its second argument is
					// impure
					continue;
				}

				if (obj_tainted_by_typeinfo(wk, an[stage][i].val)) {
					typeinfo_among_args = true;
					break;
				}
			}
		}

		if (!typeinfo_among_args && keyword_args) {
			for (i = 0; keyword_args[i].key; ++i) {
				if (!keyword_args[i].set) {
					continue;
				}

				if (obj_tainted_by_typeinfo(wk, keyword_args[i].val)) {
					typeinfo_among_args = true;
					break;
				}
			}
		}

		if (typeinfo_among_args) {
			analyze_function_opts.pure_function = false;
		}

		if (analyze_function_opts.pure_function) {
			return true;
		}

		analyze_function_opts.encountered_error = false;
		//
		// if we are analyzing arguments only return false to halt the
		// function
		return false;
	}

	return true;
}

const struct func_impl_name *kernel_func_tbl[language_mode_count] = {
	impl_tbl_kernel,
	impl_tbl_kernel_internal,
	impl_tbl_kernel_opts,
};

const struct func_impl_name *func_tbl[obj_type_count][language_mode_count] = {
	[obj_meson] = { impl_tbl_meson, },
	[obj_subproject] = { impl_tbl_subproject },
	[obj_number] = { impl_tbl_number, impl_tbl_number, },
	[obj_dependency] = { impl_tbl_dependency },
	[obj_machine] = { impl_tbl_machine, impl_tbl_machine },
	[obj_compiler] = { impl_tbl_compiler },
	[obj_feature_opt] = { impl_tbl_feature_opt },
	[obj_run_result] = { impl_tbl_run_result, impl_tbl_run_result },
	[obj_string] = { impl_tbl_string, impl_tbl_string },
	[obj_dict] = { impl_tbl_dict, impl_tbl_dict },
	[obj_external_program] = { impl_tbl_external_program, impl_tbl_external_program },
	[obj_python_installation] = { impl_tbl_python_installation, impl_tbl_python_installation },
	[obj_configuration_data] = { impl_tbl_configuration_data, impl_tbl_configuration_data },
	[obj_custom_target] = { impl_tbl_custom_target },
	[obj_file] = { impl_tbl_file, impl_tbl_file },
	[obj_bool] = { impl_tbl_boolean, impl_tbl_boolean },
	[obj_array] = { impl_tbl_array, impl_tbl_array_internal },
	[obj_build_target] = { impl_tbl_build_target },
	[obj_environment] = { impl_tbl_environment, impl_tbl_environment },
	[obj_disabler] = { impl_tbl_disabler, impl_tbl_disabler },
	[obj_generator] = { impl_tbl_generator, },
	[obj_both_libs] = { impl_tbl_both_libs, },
	[obj_source_set] = { impl_tbl_source_set, },
	[obj_source_configuration] = { impl_tbl_source_configuration, },
	[obj_module] = { impl_tbl_module, }
};

void
build_func_impl_tables(void)
{
	both_libs_build_impl_tbl();
	python_build_impl_tbl();
}

const struct func_impl_name *
func_lookup(const struct func_impl_name *impl_tbl, const char *name)
{
	uint32_t i;
	for (i = 0; impl_tbl[i].name; ++i) {
		if (strcmp(impl_tbl[i].name, name) == 0) {
			return &impl_tbl[i];
		}
	}

	return NULL;
}

const char *
func_name_str(bool have_rcvr, enum obj_type rcvr_type, const char *name)
{
	static char buf[256];
	if (have_rcvr) {
		snprintf(buf, 256, "method %s.%s()", obj_type_to_s(rcvr_type), name);
	} else {
		snprintf(buf, 256, "function %s()", name);
	}

	return buf;
}

bool
builtin_run(struct workspace *wk, bool have_rcvr, obj rcvr_id, uint32_t node_id, obj *res)
{
	const char *name;

	enum obj_type rcvr_type = 0;
	uint32_t args_node, name_node;
	struct node *n = get_node(wk->ast, node_id);
	const struct func_impl_name *impl_tbl;

	if (have_rcvr && !rcvr_id) {
		interp_error(wk, n->r, "tried to call function on null");
		return false;
	}

	if (have_rcvr) {
		name_node = n->r;
		args_node = n->c;
		rcvr_type = get_obj_type(wk, rcvr_id);
		impl_tbl = func_tbl[rcvr_type][wk->lang_mode];
	} else {
		assert(n->chflg & node_child_l);
		name_node = n->l;
		args_node = n->r;
		impl_tbl = kernel_func_tbl[wk->lang_mode];
	}

	const struct func_impl_name *fi;
	name = get_node(wk->ast, name_node)->dat.s;

	if (have_rcvr && rcvr_type == obj_module) {
		struct obj_module *m = get_obj_module(wk, rcvr_id);
		enum module mod = m->module;

		if (!m->found && strcmp(name, "found") != 0) {
			interp_error(wk, name_node, "invalid attempt to use not-found module");
			return false;
		} else if (!(fi = module_func_lookup(wk, name, mod))) {
			if (!m->has_impl) {
				interp_error(wk, name_node, "module '%s' is unimplemented,\n"
					"  If you would like to make your build files portable to muon, use"
					" `import('%s', required: false)`, and then check"
					" the .found() method before use."
					, module_names[mod]
					, module_names[mod]
					);
				return false;
			} else {
				interp_error(wk, name_node, "%s not found in module %s", func_name_str(false, 0, name), module_names[mod]);
				return false;
			}
		}
	} else {
		if (!impl_tbl) {
			interp_error(wk, name_node, "%s not found", func_name_str(true, rcvr_type, name));
			return false;
		}

		if (!(fi = func_lookup(impl_tbl, name))) {
			if (rcvr_type == obj_disabler) {
				*res = disabler_id;
				return true;
			}

			interp_error(wk, name_node, "%s not found", func_name_str(have_rcvr, rcvr_type, name));
			return false;
		}
	}

	if (fi->fuzz_unsafe && disable_fuzz_unsafe_functions) {
		interp_error(wk, name_node, "%s is disabled", func_name_str(have_rcvr, rcvr_type, name));
		return false;
	}

	if (have_rcvr && fi->rcvr_transform) {
		rcvr_id = fi->rcvr_transform(wk, rcvr_id);
	}

	TracyCZoneC(tctx_func, 0xff5000, true);
#ifdef TRACY_ENABLE
	const char *func_name = func_name_str(have_rcvr, rcvr_type, name);
	TracyCZoneName(tctx_func, func_name, strlen(func_name));
#endif

	bool func_res = fi->func(wk, rcvr_id, args_node, res);
	TracyCZoneEnd(tctx_func);

	if (!func_res) {
		if (disabler_among_args) {
			*res = disabler_id;
			disabler_among_args = false;
			return true;
		} else {
			interp_error(wk, name_node, "in %s", func_name_str(have_rcvr, rcvr_type, name));
			return false;
		}
	}
	return true;
}

bool
analyze_function(struct workspace *wk, const struct func_impl_name *fi, uint32_t args_node, obj rcvr, obj *res, bool *was_pure)
{
	struct analyze_function_opts old_opts = analyze_function_opts;
	*res = 0;

	bool pure = fi->pure;

	if (rcvr && obj_tainted_by_typeinfo(wk, rcvr)) {
		pure = false;
	}

	if (!rcvr && strcmp(fi->name, "set_variable") == 0) {
		analyze_function_opts.set_variable_special = true;
	}

	analyze_function_opts.do_analyze = true;
	// pure_function can be set to false even if it was true in the case
	// that any of its arguments are of type obj_typeinfo
	analyze_function_opts.pure_function = pure;
	analyze_function_opts.encountered_error = true;

	bool func_ret = fi->func(wk, rcvr, args_node, res);

	pure = analyze_function_opts.pure_function;
	bool ok = !analyze_function_opts.encountered_error;

	analyze_function_opts = old_opts;

	*was_pure = pure;

	if (pure) {
		return func_ret;
	} else {
		return ok;
	}
}

static int32_t
function_sig_sort(const void *a, const void *b, void *_ctx)
{
	const struct function_signature *sa = a, *sb = b;

	if ((sa->is_method && sb->is_method) || (!sa->is_method && !sb->is_method)) {
		return strcmp(sa->name, sb->name);
	} else if (sa->is_method) {
		return 1;
	} else {
		return -1;
	}
}

void
dump_function_signatures(struct workspace *wk)
{
	analyze_function_opts.dump_signature = true;

	darr_init(&function_sig_dump.sigs, 64, sizeof(struct function_signature));
	struct function_signature *sig, empty = { 0 };

	uint32_t i;
	for (i = 0; kernel_func_tbl[wk->lang_mode][i].name; ++i) {
		sig = darr_get(&function_sig_dump.sigs, darr_push(&function_sig_dump.sigs, &empty));
		sig->impl = &kernel_func_tbl[wk->lang_mode][i];
		sig->name = kernel_func_tbl[wk->lang_mode][i].name;
		sig->returns = typechecking_type_to_s(wk, kernel_func_tbl[wk->lang_mode][i].return_type);
		kernel_func_tbl[wk->lang_mode][i].func(wk, 0, 0, 0);
	}

	{
		enum obj_type t;
		for (t = 0; t < obj_type_count; ++t) {
			if (!func_tbl[t][wk->lang_mode]) {
				continue;
			}

			for (i = 0; func_tbl[t][wk->lang_mode][i].name; ++i) {
				sig = darr_get(&function_sig_dump.sigs, darr_push(&function_sig_dump.sigs, &empty));
				sig->impl = &func_tbl[t][wk->lang_mode][i];
				sig->is_method = true;
				sig->name = get_cstr(wk, make_strf(wk, "%s.%s", obj_type_to_s(t), func_tbl[t][wk->lang_mode][i].name));
				sig->returns = typechecking_type_to_s(wk, func_tbl[t][wk->lang_mode][i].return_type);
				func_tbl[t][wk->lang_mode][i].func(wk, 0, 0, 0);
			}
		}
	}

	for (i = 0; i < module_count; ++i) {
		if (!module_func_tbl[i][wk->lang_mode]) {
			continue;
		}

		uint32_t j;
		for (j = 0; module_func_tbl[i][wk->lang_mode][j].name; ++j) {
			sig = darr_get(&function_sig_dump.sigs, darr_push(&function_sig_dump.sigs, &empty));
			sig->impl = &module_func_tbl[i][wk->lang_mode][j];
			sig->is_method = true;
			sig->name = get_cstr(wk, make_strf(wk, "import('%s').%s", module_names[i], module_func_tbl[i][wk->lang_mode][j].name));
			sig->returns = typechecking_type_to_s(wk, module_func_tbl[i][wk->lang_mode][j].return_type);
			module_func_tbl[i][wk->lang_mode][j].func(wk, 0, 0, 0);
		}
	}


	darr_sort(&function_sig_dump.sigs, NULL, function_sig_sort);

	for (i = 0; i < function_sig_dump.sigs.len; ++i) {
		sig = darr_get(&function_sig_dump.sigs, i);

		if (sig->impl->extension) {
			printf("extension:");
		}

		printf("%s\n", sig->name);
		if (sig->posargs) {
			printf("  posargs:\n%s", sig->posargs);
		}
		if (sig->varargs) {
			printf("  varargs:\n%s", sig->varargs);
		}
		if (sig->optargs) {
			printf("  optargs:\n%s", sig->optargs);
		}
		if (sig->kwargs) {
			printf("  kwargs:\n%s", sig->kwargs);
		}
		printf("  returns:\n    %s\n", sig->returns);
	}

	darr_destroy(&function_sig_dump.sigs);
}
