#include "posix.h"

#include <limits.h>
#include <string.h>

#include "filesystem.h"
#include "log.h"
#include "output.h"
#include "workspace.h"

static void
write_hdr(FILE *out, struct workspace *wk, struct project *main_proj)
{
	fprintf(
		out,
		"# This is the build file for project \"%s\"\n"
		"# It is autogenerated by the boson build system.\n"
		"\n"
		"ninja_required_version = 1.7.1\n"
		"\n"
		"# Rules for compiling.\n"
		"\n"
		"rule c_COMPILER\n"
		" command = cc $ARGS -MD -MQ $out -MF $DEPFILE -o $out -c $in\n"
		" deps = gcc\n"
		" depfile = $DEPFILE_UNQUOTED\n"
		" description = Compiling C object $out\n"
		"\n"
		"# Rules for linking.\n"
		"\n"
		"rule STATIC_LINKER\n"
		" command = rm -f $out && gcc-ar $LINK_ARGS $out $in\n"
		" description = Linking static target $out\n"
		"\n"
		"rule c_LINKER\n"
		" command = cc $ARGS -o $out $in $LINK_ARGS\n"
		" description = Linking target $out\n"
		"\n"
		"# Other rules\n"
		"\n"
		"rule CUSTOM_COMMAND\n"
		" command = $COMMAND\n"
		" description = $DESC\n"
		" restat = 1\n"
		"\n"
		"# Phony build target, always out of date\n"
		"\n"
		"build PHONY: phony \n"
		"\n"
		"# Build rules for targets\n",
		wk_str(wk, main_proj->cfg.name)
		);
}

static char *
path_without_slashes(const char *path)
{
	uint32_t i;
	static char buf[PATH_MAX + 1];

	strncpy(buf, path, PATH_MAX);
	for (i = 0; buf[i]; ++i) {
		if (buf[i] == '/') {
			buf[i] = '_';
		}
	}
	return buf;

}

struct write_tgt_iter_ctx {
	struct obj *tgt;
	FILE *out;
	uint32_t args_id;
	uint32_t object_names_id;
};

enum iteration_result
write_tgt_iter(struct workspace *wk, void *_ctx, uint32_t val_id)
{
	struct write_tgt_iter_ctx *ctx = _ctx;
	struct obj *src = get_obj(wk, val_id);
	assert(src->type == obj_file); // TODO
	const char *pws = path_without_slashes(wk_str(wk, src->dat.file));

	wk_strappf(wk, &ctx->object_names_id, "%s.p/%s.o ", wk_str(wk, ctx->tgt->dat.tgt.name), pws);

	fprintf(ctx->out,
		"build %s.p/%s.o: c_COMPILER %s\n"
		" DEPFILE = %s.p/%s.o.d\n"
		" DEPFILE_UNQUOTED = %s.p/%s.o.d\n"
		" ARGS = %s\n\n",
		wk_str(wk, ctx->tgt->dat.tgt.name), pws, wk_str(wk, src->dat.file),
		wk_str(wk, ctx->tgt->dat.tgt.name), pws,
		wk_str(wk, ctx->tgt->dat.tgt.name), pws,
		wk_str(wk, ctx->args_id)
		);

	return ir_cont;
}

enum iteration_result
make_args_iter(struct workspace *wk, void *_ctx, uint32_t val_id)
{
	struct write_tgt_iter_ctx *ctx = _ctx;
	struct obj *arg = get_obj(wk, val_id);

	assert(arg->type == obj_string); // TODO

	wk_strappf(wk, &ctx->args_id, "%s ", wk_str(wk, arg->dat.str));

	return ir_cont;
}

enum iteration_result
process_dep_args_iter(struct workspace *wk, void *_ctx, uint32_t val_id)
{
	struct write_tgt_iter_ctx *ctx = _ctx;
	struct obj *dep = get_obj(wk, val_id);

	if (dep->dat.dep.include_directories) {
		struct obj *inc = get_obj(wk, dep->dat.dep.include_directories);
		assert(inc->type == obj_file); // TODO

		wk_strappf(wk, &ctx->args_id, "-I%s ", wk_str(wk, inc->dat.file));
	}

	return ir_cont;
}

struct process_dep_links_iter_ctx {
	uint32_t *link_args_id;
	uint32_t *implicit_deps_id;
};

enum iteration_result
process_dep_links_iter_iter(struct workspace *wk, void *_ctx, uint32_t val_id)
{
	struct process_dep_links_iter_ctx *ctx = _ctx;

	struct obj *tgt = get_obj(wk, val_id);
	wk_strappf(wk, ctx->link_args_id, " %s", wk_str(wk, tgt->dat.tgt.build_name));
	wk_strappf(wk, ctx->implicit_deps_id, " %s", wk_str(wk, tgt->dat.tgt.build_name));

	return ir_cont;
}

enum iteration_result
process_dep_links_iter(struct workspace *wk, void *_ctx, uint32_t val_id)
{
	struct obj *dep = get_obj(wk, val_id);

	if (dep->dat.dep.link_with) {
		if (!obj_array_foreach(wk, dep->dat.dep.link_with, _ctx, process_dep_links_iter_iter)) {
			return false;
		}
	}

	return ir_cont;
}

struct write_tgt_ctx {
	FILE *out;
	struct project *proj;
};

static enum iteration_result
write_tgt(struct workspace *wk, void *_ctx, uint32_t tgt_id)
{
	FILE *out = ((struct write_tgt_ctx *)_ctx)->out;
	struct project *proj = ((struct write_tgt_ctx *)_ctx)->proj;

	struct obj *tgt = get_obj(wk, tgt_id);
	LOG_I(log_out, "writing rules for target '%s'", wk_str(wk, tgt->dat.tgt.name));

	struct write_tgt_iter_ctx ctx = { .tgt = tgt, .out = out };

	{ /* arguments */
		ctx.args_id = wk_str_pushf(wk, "-I%s.p -I%s ", wk_str(wk, tgt->dat.tgt.name), wk_str(wk, proj->cwd));

		if (tgt->dat.tgt.include_directories) {
			struct obj *inc = get_obj(wk, tgt->dat.tgt.include_directories);
			assert(inc->type == obj_file); // TODO

			wk_strappf(wk, &ctx.args_id, "-I%s ", wk_str(wk, inc->dat.file));
		}

		{ /* dep includes */
			if (tgt->dat.tgt.deps) {
				if (!obj_array_foreach(wk, tgt->dat.tgt.deps, &ctx, process_dep_args_iter)) {
					return false;
				}
			}
		}

		if (!obj_array_foreach(wk, proj->cfg.args, &ctx, make_args_iter)) {
			return false;
		}

		if (tgt->dat.tgt.c_args) {
			if (!obj_array_foreach(wk, tgt->dat.tgt.c_args, &ctx, make_args_iter)) {
				return false;
			}
		}
	}

	{ /* obj names */
		ctx.object_names_id = wk_str_push(wk, "");
		if (!obj_array_foreach(wk, tgt->dat.tgt.src, &ctx, write_tgt_iter)) {
			return false;
		}
	}

	{ /* target */
		const char *rule;
		uint32_t link_args_id, implicit_deps_id = wk_str_push(wk, "");

		switch (tgt->dat.tgt.type) {
		case tgt_executable:
			rule = "c_LINKER";
			link_args_id = wk_str_push(wk, "-Wl,--as-needed -Wl,--no-undefined");

			{ /* dep links */
				struct process_dep_links_iter_ctx ctx = {
					.link_args_id = &link_args_id,
					.implicit_deps_id = &implicit_deps_id
				};

				if (tgt->dat.tgt.deps) {
					if (!obj_array_foreach(wk, tgt->dat.tgt.deps, &ctx, process_dep_links_iter)) {
						return false;
					}
				}
			}

			wk_str(wk, link_args_id);

			break;
		case tgt_library:
			rule = "STATIC_LINKER";
			link_args_id = wk_str_push(wk, "csrD");
			break;
		}

		fprintf(out, "build %s: %s %s | %s",
			wk_str(wk, tgt->dat.tgt.build_name),
			rule,
			wk_str(wk, ctx.object_names_id),
			wk_str(wk, implicit_deps_id)
			);
		fprintf(out, "\n LINK_ARGS = %s\n\n", wk_str(wk, link_args_id));
	}

	return true;
}

static bool
write_project(FILE *out, struct workspace *wk, struct project *proj)
{
	struct write_tgt_ctx ctx = { .out = out, .proj = proj };

	if (!obj_array_foreach(wk, proj->targets, &ctx, write_tgt)) {
		return false;
	}

	return true;
}

static FILE *
setup_outdir(const char *dir)
{
	if (!fs_mkdir_p(dir)) {
		return false;
	}

	char path[PATH_MAX + 1] = { 0 };
	snprintf(path, PATH_MAX, "%s/%s", dir, "build.ninja");

	FILE *out;
	if (!(out = fs_fopen(path, "w"))) {
		return NULL;
	}

	return out;
}

bool
output_build(struct workspace *wk, const char *dir)
{
	FILE *out;
	if (!(out = setup_outdir(dir))) {
		return false;
	}

	write_hdr(out, wk, darr_get(&wk->projects, 0));

	uint32_t i;
	for (i = 0; i < wk->projects.len; ++i) {
		if (!write_project(out, wk, darr_get(&wk->projects, i))) {
			return false;
		}
	}

	if (!fs_fclose(out)) {
		return false;
	}

	return true;
}
