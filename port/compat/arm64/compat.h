/* ARM64 (Apple Silicon): the port's SSE code goes through sse2neon (MIT licence,
 * https://github.com/DLTcollab/sse2neon), and the spin-wait hint becomes ARM's yield.
 * Force-included by the Makefile when ARCH=arm64. */
#pragma once
#if defined(__aarch64__) || defined(__arm64__)
#define __builtin_ia32_pause() __asm__ __volatile__("yield")
#endif
