// Process-level guest runtime state (arguments, environment, loaded images).
#pragma once
#include "gmem.h"
#include "macho.h"

void runtime_set_process_args(int argc, gaddr_t argv, gaddr_t envp, gaddr_t apple);
void runtime_set_main_stack(gaddr_t lo, gaddr_t hi);
uint32_t runtime_argc(void);
gaddr_t runtime_argv(void);
gaddr_t runtime_envp(void);
gaddr_t runtime_apple(void);
gaddr_t runtime_program_vars(void);

// Load guest-side runtime libraries (e.g. libstdc++) that replace system dylibs.
void runtime_preload_guest_libs(struct image **list, int *count);
// Called after linking; records images for the dyld query APIs.
void runtime_images_loaded(struct image **list, int count);
int runtime_image_count(void);
struct image *runtime_image(int i);

// Run guest atexit handlers, flush guest stdio and exit the process.
_Noreturn void runtime_exit(int code);
