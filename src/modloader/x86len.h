/* x86len.h — minimal x86 (32-bit) instruction-length decoder for the hook engine.
 *
 * This is mod-loader tooling, not game reconstruction: given the bytes at a
 * candidate hook site it reports the length of one instruction, so the loader
 * can steal whole instructions into a trampoline instead of blindly copying a
 * fixed five bytes (which can split an instruction when the game was built by a
 * different compiler).  It deliberately refuses anything it does not fully
 * understand, so a hook is never installed on an undecodable prologue.
 */
#ifndef CDEMOD_X86LEN_H
#define CDEMOD_X86LEN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Length in bytes of the instruction at ``code`` (at most ``max`` bytes
 * available).  Returns 0 if it cannot be decoded.  If ``has_rel`` is non-null it
 * is set to 1 when the instruction encodes a position-dependent relative
 * branch/call (which must be relocated or refused). */
size_t x86_instruction_length(const unsigned char* code, size_t max, int* has_rel);

#ifdef __cplusplus
}
#endif

#endif /* CDEMOD_X86LEN_H */
