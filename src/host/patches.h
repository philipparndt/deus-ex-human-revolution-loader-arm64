// Fixes for bugs in the game's own code (see patches.c).
#pragma once
#include "macho.h"

void patches_apply(struct image *exe);
