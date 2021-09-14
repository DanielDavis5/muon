#ifndef MUON_FUNCTIONS_DEFAULT_DEPENDENCY_H
#define MUON_FUNCTIONS_DEFAULT_DEPENDENCY_H
#include "functions/common.h"

bool func_dependency(struct workspace *wk, uint32_t rcvr, uint32_t args_node, uint32_t *obj);
bool func_declare_dependency(struct workspace *wk, uint32_t _, uint32_t args_node, uint32_t *obj);
#endif
