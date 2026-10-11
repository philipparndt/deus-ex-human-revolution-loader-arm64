// Guest blocks (see blocks.c).
#pragma once
#include "../gmem.h"

gaddr_t gblock_copy(gaddr_t block);
void gblock_release(gaddr_t block);
// Invoke a block that takes no arguments: invoke(block).
void gblock_invoke0(gaddr_t block);

extern gaddr_t (*block_objc_retain)(gaddr_t obj);
extern void (*block_objc_release)(gaddr_t obj);
