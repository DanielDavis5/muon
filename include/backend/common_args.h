/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_BACKEND_COMMON_ARGS_H
#define MUON_BACKEND_COMMON_ARGS_H

#include "lang/workspace.h"

void get_std_args(struct workspace *wk, const struct project *proj,
	const struct obj_build_target *tgt, obj args_id, enum compiler_language lang, enum compiler_type t);
void get_option_compile_args(struct workspace *wk, const struct project *proj,
	const struct obj_build_target *tgt, obj args_id, enum compiler_language lang);
void get_option_link_args(struct workspace *wk, const struct project *proj,
	const struct obj_build_target *tgt, obj args_id, enum compiler_language lang);

bool setup_compiler_args(struct workspace *wk, const struct obj_build_target *tgt,
	const struct project *proj, obj include_dirs, obj dep_args, obj *joined_args);

bool build_target_args(struct workspace *wk, const struct project *proj,
	const struct obj_build_target *tgt, obj *joined_args);

struct setup_linker_args_ctx {
	enum linker_type linker;
	enum compiler_language link_lang;
	struct build_dep *args;
};

void setup_linker_args(struct workspace *wk, const struct project *proj,
	const struct obj_build_target *tgt, struct setup_linker_args_ctx *ctx);
void setup_compiler_args_includes(struct workspace *wk, obj compiler, obj include_dirs, obj args, bool relativize);

void relativize_paths(struct workspace *wk, obj arr, bool relativize_strings, obj *res);
void relativize_path(struct workspace *wk, obj path, bool relativize_strings, obj *res);
void relativize_path_push(struct workspace *wk, obj path, obj arr);
#endif
