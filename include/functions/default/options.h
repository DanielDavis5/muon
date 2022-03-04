#ifndef MUON_FUNCTIONS_DEFAULT_OPTIONS_H
#define MUON_FUNCTIONS_DEFAULT_OPTIONS_H

#include "lang/workspace.h"

bool func_option(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res);
bool func_get_option(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res);
void get_option(struct workspace *wk, const struct project *proj, const char *name, obj *res);
bool check_invalid_option_overrides(struct workspace *wk);
bool check_invalid_subproject_option(struct workspace *wk);
bool set_builtin_options(struct workspace *wk);

enum wrap_mode {
	wrap_mode_nopromote,
	wrap_mode_nodownload,
	wrap_mode_nofallback,
	wrap_mode_forcefallback,
};
enum wrap_mode get_option_wrap_mode(struct workspace *wk);

bool parse_and_set_cmdline_option(struct workspace *wk, char *lhs);
bool parse_and_set_default_options(struct workspace *wk, uint32_t err_node, obj arr, obj project_name, bool is_subproject);
#endif
