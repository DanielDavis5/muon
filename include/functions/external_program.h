/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_FUNCTIONS_EXTERNAL_PROGRAM_H
#define MUON_FUNCTIONS_EXTERNAL_PROGRAM_H
#include "functions/common.h"

void find_program_guess_version(struct workspace *wk, obj cmd_array, obj *ver);

extern const struct func_impl_name impl_tbl_external_program[5];
#endif
