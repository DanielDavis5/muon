#include "ninja.h"
#include "parser.h"
#include "log.h"
#include "interpreter.h"
#include "options.h"

#define _XOPEN_SOURCE 700

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define PATH_MAX 4096

static void
write_header(FILE *file)
{
	fprintf(file, "# This file is generated by boson %s\n", VERSION);
	fprintf(file, "# Do not edit by hand\n");

	fprintf(file, "\nninja_required_version = 1.9\n\n");
}

static void
write_compiler(FILE *file)
{
	// TODO check if the compiler given through CC is a valid executable
	char *cc = getenv("CC");
	if (!cc) {
		cc = "cc";
	}

	char cmd[PATH_MAX] = {0};
	snprintf(cmd, sizeof(cmd), "%s > /dev/null 2>&1", cc);

	int rc = system(cmd);
	if (WEXITSTATUS(rc) == 127) {
		fatal("'%s' is not a valid compiler", cc);
	}

	fprintf(file, "cc = %s\n\n", cc);

}

static void
write_cflags(FILE *file, struct context *ctx)
{
	fprintf(file, "cflags =");

	struct options *options = ctx->options;

	switch(options->compiler.c_std) {
	case STD_C89:
		fprintf(file, " -std=c89");
		break;
	case STD_C99:
		fprintf(file, " -std=c99");
		break;
	case STD_C11:
		fprintf(file, " -std=c11");
		break;
	case STD_C17:
		fprintf(file, " -std=c17");
		break;
	case STD_C18:
		fprintf(file, " -std=c18");
		break;
	case STD_C2X:
		fprintf(file, " -std=c2x");
		break;
	default:
		break;
	}


	if (options->core.warning_level >= 1) {
		fprintf(file, " -Wall");
	}
	if (options->core.warning_level >= 2) {
		fprintf(file, " -Wextra");
	}
	if (options->core.warning_level == 3) {
		fprintf(file, " -Wpedantic");
	}

	if (options->core.werror) {
		fprintf(file, " -Werror");
	}

	for (size_t i = 0; i < ctx->project_arguments.n; ++i) {
		fprintf(file, " %s", ctx->project_arguments.data[i]);
	}

	fprintf(file, "\n\n");
}

static void
write_rules(FILE *file)
{
	fprintf(file, "rule cc\n");
	fprintf(file, " command = $cc -MD -MF $out.d $cflags -c $includes -o "
		"$out $in\n");
	fprintf(file, " depfile = $out.d\n");
	fprintf(file, " deps = gcc\n\n");
	fprintf(file, "rule ld\n");
	fprintf(file, " command = $cc $ldflags -o $out $in\n\n");
}

static void
write_targets(FILE *file, struct context *ctx)
{
	struct build_target *target = NULL;
	for(size_t i = 0; i < ctx->build.n; ++i) {
		target = ctx->build.targets[i];

		char includes[PATH_MAX] = {0};
		for (size_t j = 0; j < target->include.n; ++j) {
			int r = snprintf(includes + strlen(includes),
					sizeof(includes) - strlen(includes),
					" -I%s", target->include.paths[j]);
			if (r < 0) {
				fatal("failed to compose inclues");
			}
		}


		char **objs = calloc(1, sizeof(char*));
		size_t n_objs = 0;

		for (size_t j = 0; j < target->source.n; ++j) {
			char *path = target->source.files[j];
			char *obj = strdup(path);
			char *ext = strrchr(obj, '.');
			strcpy(ext, ".o");

			n_objs += 1;
			objs = realloc(objs, n_objs * sizeof(char*));
			objs[n_objs - 1] = obj;

			fprintf(file, "build %s: cc ../%s\n", obj, path);
			fprintf(file, " includes =%s\n\n", includes);
		}

		fprintf(file, "build %s: ld", target->name.data);
		for (size_t j = 0; j < n_objs; ++j) {
			fprintf(file, " %s", objs[j]);
			free(objs[j]);
		}
		free(objs);

		fprintf(file, "\n\n");
	}
}

int
emit_ninja(struct context *ctx, const char *build_dir)
{
	if (mkdir(build_dir, 0775) == -1) {
		report("Build directory already configured");
		return 1;
	}
	info("Build dir: %s", build_dir);

	char ninja_path[PATH_MAX] = {0};
	snprintf(ninja_path, sizeof(ninja_path), "%s/build.ninja", build_dir);

	FILE *ninja = fopen(ninja_path, "w");
	if (!ninja) {
		report("failed to create build.ninja");
		return 1;
	}

	write_header(ninja);
	write_compiler(ninja);
	write_cflags(ninja, ctx);
	write_rules(ninja);
	write_targets(ninja, ctx);

	fclose(ninja);
	return 0;
}
