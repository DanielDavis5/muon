/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_ERROR_H
#define MUON_ERROR_H

#include "compat.h"

#include <assert.h>
#include <stdarg.h>

#include "log.h"
#include "platform/filesystem.h"

#define UNREACHABLE assert(false && "unreachable")
#define UNREACHABLE_RETURN do { assert(false && "unreachable"); return 0; } while (0)

enum error_diagnostic_store_replay_opts {
	error_diagnostic_store_replay_errors_only = 1 << 0,
	error_diagnostic_store_replay_include_sources = 1 << 1,
	error_diagnostic_store_replay_werror = 1 << 2,
};

void error_unrecoverable(const char *fmt, ...) MUON_ATTR_FORMAT(printf, 1, 2);
void error_message(struct source *src, uint32_t line, uint32_t col, enum log_level lvl, const char *msg);
void error_messagev(struct source *src, uint32_t line, uint32_t col, enum log_level lvl, const char *fmt, va_list args);
void error_messagef(struct source *src, uint32_t line, uint32_t col, enum log_level lvl, const char *fmt, ...)
MUON_ATTR_FORMAT(printf, 5, 6);

void error_diagnostic_store_init(void);
void error_diagnostic_store_replay(enum error_diagnostic_store_replay_opts opts, bool *saw_error);
void error_diagnostic_store_push(uint32_t src_idx, uint32_t line, uint32_t col,
	enum log_level lvl, const char *msg);
uint32_t error_diagnostic_store_push_src(struct source *src);
void list_line_range(struct source *src, uint32_t lno, uint32_t list_amt);
#endif
