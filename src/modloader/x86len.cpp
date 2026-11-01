// x86len.cpp — see x86len.h.  A compact length-only decoder covering the
// opcodes that appear in real function prologues (push/mov/sub/lea, immediates,
// ModRM/SIB/disp, common SSE moves, jcc/call/jmp relatives).  Unknown opcodes
// return 0 so callers refuse rather than risk a wrong length.

#include "x86len.h"

namespace {

// Advance ``i`` past a ModRM byte plus any SIB/displacement.  Returns the
// /reg field via ``reg_field`` and false on truncation.
bool skip_modrm(const unsigned char* c, size_t max, size_t& i, int& reg_field) {
    if (i >= max) return false;
    unsigned char m = c[i++];
    reg_field = (m >> 3) & 7;
    unsigned char mod = m >> 6;
    unsigned char rm = m & 7;
    if (mod == 3) return true;

    bool disp32 = false;
    bool disp8 = false;
    if (rm == 4) {  // SIB
        if (i >= max) return false;
        unsigned char s = c[i++];
        unsigned char base = s & 7;
        if (mod == 0 && base == 5) disp32 = true;
    } else if (rm == 5 && mod == 0) {
        disp32 = true;
    }
    if (mod == 1) disp8 = true;
    if (mod == 2) disp32 = true;

    if (disp8) { if (i + 1 > max) return false; i += 1; }
    if (disp32) { if (i + 4 > max) return false; i += 4; }
    return true;
}

bool in_range(unsigned char v, unsigned char lo, unsigned char hi) {
    return v >= lo && v <= hi;
}

}  // namespace

size_t x86_instruction_length(const unsigned char* c, size_t max, int* has_rel) {
    if (has_rel) *has_rel = 0;
    if (!c || max == 0) return 0;

    size_t i = 0;
    bool opsize16 = false;
    for (;;) {
        if (i >= max) return 0;
        unsigned char b = c[i];
        if (b == 0x66) { opsize16 = true; i++; continue; }
        if (b == 0x67) return 0;  // 16-bit addressing: refuse
        if (b == 0xF0 || b == 0xF2 || b == 0xF3 ||
            b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 ||
            b == 0x64 || b == 0x65) {
            i++;
            continue;
        }
        break;
    }
    if (i >= max) return 0;

    unsigned char op = c[i++];
    bool has_modrm = false;
    size_t imm = 0;
    bool rel = false;
    size_t rel_size = 0;
    int reg = -1;

    if (in_range(op, 0x00, 0x03) || in_range(op, 0x08, 0x0B) ||
        in_range(op, 0x10, 0x13) || in_range(op, 0x18, 0x1B) ||
        in_range(op, 0x20, 0x23) || in_range(op, 0x28, 0x2B) ||
        in_range(op, 0x30, 0x33) || in_range(op, 0x38, 0x3B) ||
        op == 0x62 || op == 0x63 ||
        op == 0x84 || op == 0x85 || op == 0x86 || op == 0x87 ||
        op == 0x88 || op == 0x89 || op == 0x8A || op == 0x8B ||
        op == 0x8C || op == 0x8D || op == 0x8E || op == 0x8F ||
        op == 0xC0 || op == 0xC1 || op == 0xD0 || op == 0xD1 ||
        op == 0xD2 || op == 0xD3 ||
        in_range(op, 0xD8, 0xDF) || op == 0xFE || op == 0xFF) {
        has_modrm = true;
    } else if (op == 0x6B || op == 0x80 || op == 0x83 || op == 0xC6) {
        has_modrm = true; imm = 1;
    } else if (op == 0x69 || op == 0x81 || op == 0xC7) {
        has_modrm = true; imm = opsize16 ? 2 : 4;
    } else if (in_range(op, 0x04, 0x04) || op == 0x0C || op == 0x14 || op == 0x1C ||
               op == 0x24 || op == 0x2C || op == 0x34 || op == 0x3C) {
        imm = 1;
    } else if (op == 0x05 || op == 0x0D || op == 0x15 || op == 0x1D ||
               op == 0x25 || op == 0x2D || op == 0x35 || op == 0x3D) {
        imm = opsize16 ? 2 : 4;
    } else if (op == 0x68) {
        imm = opsize16 ? 2 : 4;
    } else if (op == 0x6A) {
        imm = 1;
    } else if (in_range(op, 0x40, 0x4F) || in_range(op, 0x50, 0x5F) ||
               in_range(op, 0x90, 0x97) || op == 0x98 || op == 0x99 ||
               op == 0x9C || op == 0x9D || op == 0x9E || op == 0x9F ||
               op == 0x60 || op == 0x61 || op == 0x9B ||
               op == 0x27 || op == 0x2F || op == 0x37 || op == 0x3F ||
               op == 0xC3 || op == 0xC9 || op == 0xCC || op == 0xCE || op == 0xCF ||
               op == 0xF4 || op == 0xF5 || op == 0xF8 || op == 0xF9 ||
               op == 0xFA || op == 0xFB || op == 0xFC || op == 0xFD) {
        // no operands
    } else if (op == 0xCD) {
        imm = 1;
    } else if (op == 0xC2 || op == 0xCA) {
        imm = 2;
    } else if (op == 0xC8) {
        imm = 3;  // enter imm16, imm8
    } else if (in_range(op, 0xA0, 0xA3)) {
        imm = opsize16 ? 2 : 4;  // moffs
    } else if (op == 0xA8) {
        imm = 1;
    } else if (op == 0xA9) {
        imm = opsize16 ? 2 : 4;
    } else if (in_range(op, 0xB0, 0xB7)) {
        imm = 1;
    } else if (in_range(op, 0xB8, 0xBF)) {
        imm = opsize16 ? 2 : 4;
    } else if (op == 0xE4 || op == 0xE5 || op == 0xE6 || op == 0xE7) {
        imm = 1;
    } else if (in_range(op, 0x70, 0x7F) || op == 0xE0 || op == 0xE1 ||
               op == 0xE2 || op == 0xE3 || op == 0xEB) {
        rel = true; rel_size = 1;
    } else if (op == 0xE8 || op == 0xE9) {
        rel = true; rel_size = opsize16 ? 2 : 4;
    } else if (op == 0x0F) {
        if (i >= max) return 0;
        unsigned char op2 = c[i++];
        if (in_range(op2, 0x80, 0x8F)) {
            rel = true; rel_size = opsize16 ? 2 : 4;
        } else if (in_range(op2, 0xC8, 0xCF)) {
            // bswap reg: no operands
        } else if (op2 == 0x05 || op2 == 0x0B || op2 == 0x30 || op2 == 0x31 ||
                   op2 == 0x32 || op2 == 0x33 || op2 == 0x34 || op2 == 0x35 ||
                   op2 == 0x77 || op2 == 0xA2 || op2 == 0xAA ||
                   op2 == 0x06 || op2 == 0x07 || op2 == 0x08 || op2 == 0x09 ||
                   op2 == 0x0E || op2 == 0x37) {
            // no operands
        } else if (op2 == 0x38) {
            if (i >= max) return 0;
            i++;  // 0F 38 xx: modrm only
            has_modrm = true;
        } else if (op2 == 0x3A) {
            if (i >= max) return 0;
            i++;  // 0F 3A xx: modrm + imm8
            has_modrm = true; imm = 1;
        } else if (op2 == 0x70 || op2 == 0x71 || op2 == 0x72 || op2 == 0x73 ||
                   op2 == 0xA4 || op2 == 0xAC || op2 == 0xBA ||
                   op2 == 0xC2 || op2 == 0xC4 || op2 == 0xC5 || op2 == 0xC6) {
            has_modrm = true; imm = 1;
        } else {
            // the large majority of remaining two-byte opcodes take a ModRM
            has_modrm = true;
        }
    } else {
        return 0;  // unknown: refuse
    }

    if (has_modrm) {
        if (!skip_modrm(c, max, i, reg)) return 0;
        if (op == 0xF6 && (reg == 0 || reg == 1)) imm = 1;
        if (op == 0xF7 && (reg == 0 || reg == 1)) imm = opsize16 ? 2 : 4;
    }
    if (imm) {
        if (i + imm > max) return 0;
        i += imm;
    }
    if (rel) {
        if (i + rel_size > max) return 0;
        if (has_rel) *has_rel = 1;
        i += rel_size;
    }
    return i;
}
