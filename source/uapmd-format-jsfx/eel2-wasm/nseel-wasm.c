// WebAssembly backend for EEL2: translates the portable bytecode of a compiled code handle into a wasm module.
#include <emscripten.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "WDL/eel2/ns-eel-int.h"
#include "WDL/denormal.h"

// glue_port.h defines these arrays non-static, and nseel-compiler.c already owns them.
#define _asm_generic1parm nseel_wasm_unused_generic1parm
#define _asm_generic2parm nseel_wasm_unused_generic2parm
#define _asm_generic3parm nseel_wasm_unused_generic3parm
#define _asm_generic1parm_retd nseel_wasm_unused_generic1parm_retd
#define _asm_generic2parm_retd nseel_wasm_unused_generic2parm_retd
#define _asm_generic2xparm_retd nseel_wasm_unused_generic2xparm_retd
#define _asm_generic3parm_retd nseel_wasm_unused_generic3parm_retd
#include "WDL/eel2/glue_port.h"

#ifdef NSEEL_WASM_DEBUG
#define WASM_FAIL(...) (fprintf(stderr, "[eel2-wasm] " __VA_ARGS__), fputc('\n', stderr), 0)
#else
#define WASM_FAIL(...) 0
#endif

#define NSEEL_WASM_STACK_SIZE 65536
#define NSEEL_WASM_MAX_BLOCK_BYTES (16 * 1024 * 1024)
#define NSEEL_WASM_RAM_ITEMS ((uint32_t) NSEEL_RAM_BLOCKS * NSEEL_RAM_ITEMSPERBLOCK)

// ---- byte buffer ---------------------------------------------------------------------

typedef struct
{
  unsigned char *data;
  int size, cap;
  int failed;
} WasmBuf;

static void wbReserve(WasmBuf *b, int extra)
{
  if (b->failed || b->size + extra <= b->cap) return;
  {
    int ncap = b->cap ? b->cap * 2 : 1024;
    unsigned char *n;
    while (ncap < b->size + extra) ncap *= 2;
    n = (unsigned char *) realloc(b->data, ncap);
    if (!n) { b->failed = 1; return; }
    b->data = n;
    b->cap = ncap;
  }
}
static void wbByte(WasmBuf *b, unsigned char c) { wbReserve(b, 1); if (!b->failed) b->data[b->size++] = c; }
static void wbBytes(WasmBuf *b, const void *p, int n) { wbReserve(b, n); if (!b->failed) { memcpy(b->data + b->size, p, n); b->size += n; } }
static void wbUleb(WasmBuf *b, uint32_t v) { do { unsigned char c = v & 0x7f; v >>= 7; if (v) c |= 0x80; wbByte(b, c); } while (v); }
static void wbSleb(WasmBuf *b, int64_t v)
{
  for (;;)
  {
    unsigned char c = v & 0x7f;
    v >>= 7;
    if ((v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40))) { wbByte(b, c); return; }
    wbByte(b, c | 0x80);
  }
}
static void wbF64(WasmBuf *b, double d) { wbBytes(b, &d, 8); }
static void wbName(WasmBuf *b, const char *s) { int n = (int) strlen(s); wbUleb(b, n); wbBytes(b, s, n); }
static void wbSection(WasmBuf *out, unsigned char id, WasmBuf *body)
{
  wbByte(out, id);
  wbUleb(out, body->size);
  wbBytes(out, body->data, body->size);
  if (body->failed) out->failed = 1;
}

// ---- wasm opcodes --------------------------------------------------------------------

enum
{
  W_BLOCK = 0x02, W_LOOP = 0x03, W_IF = 0x04, W_ELSE = 0x05, W_END = 0x0b, W_BR = 0x0c, W_BR_IF = 0x0d,
  W_RETURN = 0x0f, W_CALL = 0x10, W_DROP = 0x1a, W_SELECT = 0x1b,
  W_LOCAL_GET = 0x20, W_LOCAL_SET = 0x21, W_LOCAL_TEE = 0x22,
  W_I32_LOAD = 0x28, W_F64_LOAD = 0x2b, W_I32_STORE = 0x36, W_F64_STORE = 0x39,
  W_I32_CONST = 0x41, W_I64_CONST = 0x42, W_F64_CONST = 0x44,
  W_I32_EQZ = 0x45, W_I32_LT_S = 0x48, W_I32_LT_U = 0x49, W_I32_GT_S = 0x4a, W_I32_GT_U = 0x4b, W_I32_LE_S = 0x4c,
  W_F64_EQ = 0x61, W_F64_NE = 0x62, W_F64_LT = 0x63, W_F64_GT = 0x64, W_F64_GE = 0x66,
  W_I32_ADD = 0x6a, W_I32_SUB = 0x6b, W_I32_MUL = 0x6c, W_I32_AND = 0x71, W_I32_OR = 0x72,
  W_I32_SHL = 0x74, W_I32_SHR_S = 0x75, W_I32_SHR_U = 0x76,
  W_I64_REM_S = 0x81, W_I64_AND = 0x83, W_I64_OR = 0x84, W_I64_XOR = 0x85, W_I64_SHR_U = 0x88,
  W_F64_ABS = 0x99, W_F64_NEG = 0x9a, W_F64_ADD = 0xa0, W_F64_SUB = 0xa1, W_F64_MUL = 0xa2, W_F64_DIV = 0xa3,
  W_I32_WRAP_I64 = 0xa7, W_I64_EXTEND_I32_S = 0xac, W_F32_DEMOTE_F64 = 0xb6, W_F64_CONVERT_I32_S = 0xb7,
  W_F64_CONVERT_I64_S = 0xb9, W_F64_PROMOTE_F32 = 0xbb, W_I32_REINTERPRET_F32 = 0xbc, W_I64_REINTERPRET_F64 = 0xbd,
  W_F32_REINTERPRET_I32 = 0xbe, W_PREFIX_FC = 0xfc,
  W_TYPE_I32 = 0x7f, W_TYPE_F64 = 0x7c, W_TYPE_VOID = 0x40,
};

enum
{
  SIG_D_D, SIG_D_DD, SIG_P_PP, SIG_P_PPP, SIG_P_PPPP, SIG_D_PP, SIG_D_PPP, SIG_D_PPPP, SIG_ENTRY, SIG_CALLEE, SIG_COUNT
};

// ---- decoded program -----------------------------------------------------------------

typedef struct
{
  int offset;
  int op;
  INT_PTR imm[4];
  int target;     // instruction index of a jump target, -1 if none
  int callee;     // block index of an FCALL target, -1 if none
  int importIdx;  // import index of a called C function, -1 if none
  int fpIn;       // fp stack depth before the instruction, -1 if unreachable
  int loopEnd;    // for a loop header, the index of the last instruction of the loop, else -1
} WasmInst;

typedef struct
{
  const unsigned char *code;
  WasmInst *insts;
  int count, cap;
  int state;      // 0 = not analyzed, 1 = analyzing, 2 = done
  int maxFp;
  int retFp;
} WasmBlock;

typedef struct
{
  INT_PTR fn;
  int sig;
} WasmImport;

typedef struct
{
  WasmBlock *blocks;
  int blockCount, blockCap;
  WasmImport *imports;
  int importCount, importCap;
  int failed;
} WasmProgram;

static int immediateBytes(int op)
{
  const int P = (int) sizeof(INT_PTR), J = (int) sizeof(GLUE_JMP_TYPE), I = (int) sizeof(int);
  switch (op)
  {
    case EEL_BC_JMP_NC: case EEL_BC_JMP_IF_P1_Z: case EEL_BC_JMP_IF_P1_NZ:
    case EEL_BC_LOOP_LOADCNT: case EEL_BC_LOOP_END: case EEL_BC_WHILE_CHECK_RV:
      return J;
#if NSEEL_LOOPFUNC_SUPPORT_MAXLEN > 0
    case EEL_BC_WHILE_END: return J;
    case EEL_BC_WHILE_SETUP: return 0;
#else
    case EEL_BC_WHILE_END: return 0;
#endif
    case EEL_BC_MOV_FPTOP_DV: case EEL_BC_MOV_P1_DV: case EEL_BC_MOV_P2_DV: case EEL_BC_MOV_P3_DV:
    case EEL_BC__RESET_WTP: case EEL_BC_POP_VALUE_TO_ADDR: case EEL_BC_COPY_VALUE_AT_P1_TO_ADDR:
    case EEL_BC_POP_FPSTACK_TO_PTR: case EEL_BC_FCALL:
    case EEL_BC_CFUNC_1PDD: case EEL_BC_CFUNC_2PDD: case EEL_BC_CFUNC_2PDDS:
    case EEL_BC_USERSTACK_PEEK_TOP: case EEL_BC_USERSTACK_EXCH:
      return P;
    case EEL_BC_MOVE_STACK: case EEL_BC_STORE_P1_TO_STACK_AT_OFFS:
      return I;
    case EEL_BC_GMEGABUF:
    case EEL_BC_GENERIC1PARM: case EEL_BC_GENERIC2PARM: case EEL_BC_GENERIC3PARM:
    case EEL_BC_GENERIC1PARM_RETD: case EEL_BC_GENERIC2PARM_RETD: case EEL_BC_GENERIC3PARM_RETD:
      return 2 * P;
    case EEL_BC_GENERIC2XPARM_RETD:
    case EEL_BC_USERSTACK_PUSH: case EEL_BC_USERSTACK_POP: case EEL_BC_USERSTACK_POPFAST: case EEL_BC_USERSTACK_PEEK:
      return 3 * P;
    case EEL_BC_USERSTACK_PEEK_INT:
      return 4 * P;
    case EEL_BC_NOP: case EEL_BC_RET: case EEL_BC_PUSH_P1: case EEL_BC_PUSH_P1PTR_AS_VALUE:
    case EEL_BC_POP_P1: case EEL_BC_POP_P2: case EEL_BC_POP_P3:
    case EEL_BC_MOVE_STACKPTR_TO_P1: case EEL_BC_MOVE_STACKPTR_TO_P2: case EEL_BC_MOVE_STACKPTR_TO_P3:
    case EEL_BC_SET_P2_FROM_P1: case EEL_BC_SET_P3_FROM_P1:
    case EEL_BC_SET_P1_FROM_WTP: case EEL_BC_SET_P2_FROM_WTP: case EEL_BC_SET_P3_FROM_WTP:
    case EEL_BC_POP_FPSTACK_TOSTACK: case EEL_BC_PUSH_VAL_AT_P1_TO_FPSTACK: case EEL_BC_PUSH_VAL_AT_P2_TO_FPSTACK:
    case EEL_BC_PUSH_VAL_AT_P3_TO_FPSTACK: case EEL_BC_POP_FPSTACK_TO_WTP: case EEL_BC_SET_P1_Z: case EEL_BC_SET_P1_NZ:
    case EEL_BC_WHILE_BEGIN:
    case EEL_BC_BNOT: case EEL_BC_EQUAL: case EEL_BC_EQUAL_EXACT: case EEL_BC_NOTEQUAL: case EEL_BC_NOTEQUAL_EXACT:
    case EEL_BC_ABOVE: case EEL_BC_BELOWEQ:
    case EEL_BC_ADD: case EEL_BC_SUB: case EEL_BC_MUL: case EEL_BC_DIV: case EEL_BC_AND: case EEL_BC_OR:
    case EEL_BC_OR0: case EEL_BC_XOR:
    case EEL_BC_ADD_OP: case EEL_BC_SUB_OP: case EEL_BC_ADD_OP_FAST: case EEL_BC_SUB_OP_FAST:
    case EEL_BC_MUL_OP: case EEL_BC_DIV_OP: case EEL_BC_MUL_OP_FAST: case EEL_BC_DIV_OP_FAST:
    case EEL_BC_AND_OP: case EEL_BC_OR_OP: case EEL_BC_XOR_OP:
    case EEL_BC_UMINUS: case EEL_BC_ASSIGN: case EEL_BC_ASSIGN_FAST: case EEL_BC_ASSIGN_FAST_FROMFP:
    case EEL_BC_ASSIGN_FROMFP: case EEL_BC_MOD: case EEL_BC_MOD_OP: case EEL_BC_SHR: case EEL_BC_SHL:
    case EEL_BC_SQR: case EEL_BC_MIN: case EEL_BC_MAX: case EEL_BC_MIN_FP: case EEL_BC_MAX_FP:
    case EEL_BC_ABS: case EEL_BC_SIGN: case EEL_BC_INVSQRT: case EEL_BC_FXCH: case EEL_BC_POP_FPSTACK:
    case EEL_BC_BOOLTOFP: case EEL_BC_FPTOBOOL: case EEL_BC_FPTOBOOL_REV: case EEL_BC_MEGABUF:
      return 0;
  }
  return -1;
}

static int isJump(int op)
{
  return op == EEL_BC_JMP_NC || op == EEL_BC_JMP_IF_P1_Z || op == EEL_BC_JMP_IF_P1_NZ ||
         op == EEL_BC_LOOP_LOADCNT || op == EEL_BC_LOOP_END || op == EEL_BC_WHILE_CHECK_RV
#if NSEEL_LOOPFUNC_SUPPORT_MAXLEN > 0
         || op == EEL_BC_WHILE_END
#endif
         ;
}

static int addImport(WasmProgram *p, INT_PTR fn, int sig)
{
  int i;
  if (!fn) { p->failed = 1; return -1; }
  for (i = 0; i < p->importCount; i++)
    if (p->imports[i].fn == fn && p->imports[i].sig == sig) return i;
  if (p->importCount == p->importCap)
  {
    int ncap = p->importCap ? p->importCap * 2 : 16;
    WasmImport *n = (WasmImport *) realloc(p->imports, ncap * sizeof(WasmImport));
    if (!n) { p->failed = 1; return -1; }
    p->imports = n;
    p->importCap = ncap;
  }
  p->imports[p->importCount].fn = fn;
  p->imports[p->importCount].sig = sig;
  return p->importCount++;
}

static int findOrAddBlock(WasmProgram *p, const unsigned char *code)
{
  int i;
  for (i = 0; i < p->blockCount; i++)
    if (p->blocks[i].code == code) return i;
  if (p->blockCount == p->blockCap)
  {
    int ncap = p->blockCap ? p->blockCap * 2 : 8;
    WasmBlock *n = (WasmBlock *) realloc(p->blocks, ncap * sizeof(WasmBlock));
    if (!n) { p->failed = 1; return -1; }
    p->blocks = n;
    p->blockCap = ncap;
  }
  memset(&p->blocks[p->blockCount], 0, sizeof(WasmBlock));
  p->blocks[p->blockCount].code = code;
  return p->blockCount++;
}

static int instIndexAt(const WasmBlock *b, int offset)
{
  int lo = 0, hi = b->count - 1;
  while (lo <= hi)
  {
    const int mid = (lo + hi) / 2;
    if (b->insts[mid].offset == offset) return mid;
    if (b->insts[mid].offset < offset) lo = mid + 1;
    else hi = mid - 1;
  }
  return -1;
}

// Decodes one code block up to its terminating RET; FCALL targets are queued as further blocks.
static int decodeBlock(WasmProgram *p, int blockIdx)
{
  int offset = 0, i;
  for (;;)
  {
    WasmBlock *b = &p->blocks[blockIdx];
    WasmInst *in;
    int op, immBytes, k;
    if (offset > NSEEL_WASM_MAX_BLOCK_BYTES) return WASM_FAIL("block %d exceeds the size limit", blockIdx);
    if (b->count == b->cap)
    {
      int ncap = b->cap ? b->cap * 2 : 64;
      WasmInst *n = (WasmInst *) realloc(b->insts, ncap * sizeof(WasmInst));
      if (!n) return WASM_FAIL("out of memory");
      b->insts = n;
      b->cap = ncap;
    }
    memcpy(&op, b->code + offset, sizeof(EEL_BC_TYPE));
    immBytes = immediateBytes(op);
    if (immBytes < 0) return WASM_FAIL("block %d: unsupported opcode %d at offset %d", blockIdx, op, offset);
    in = &b->insts[b->count++];
    memset(in, 0, sizeof(*in));
    in->offset = offset;
    in->op = op;
    in->target = in->callee = in->importIdx = in->fpIn = in->loopEnd = -1;
    offset += (int) sizeof(EEL_BC_TYPE);
    if (isJump(op))
    {
      GLUE_JMP_TYPE rel;
      memcpy(&rel, b->code + offset, sizeof(rel));
      in->imm[0] = offset + (int) sizeof(GLUE_JMP_TYPE) + rel;
    }
    else if (op == EEL_BC_MOVE_STACK || op == EEL_BC_STORE_P1_TO_STACK_AT_OFFS)
    {
      int v;
      memcpy(&v, b->code + offset, sizeof(v));
      in->imm[0] = v;
    }
    else
    {
      for (k = 0; k * (int) sizeof(INT_PTR) < immBytes; k++)
        memcpy(&in->imm[k], b->code + offset + k * sizeof(INT_PTR), sizeof(INT_PTR));
    }
    offset += immBytes;
    if (op == EEL_BC_RET) break;
  }

  // p->blocks may move while FCALL targets are added, so always index through it
  {
    const int count = p->blocks[blockIdx].count;
    for (i = 0; i < count; i++)
    {
      WasmBlock *b = &p->blocks[blockIdx];
      WasmInst *in = &b->insts[i];
      if (isJump(in->op))
      {
        in->target = instIndexAt(b, (int) in->imm[0]);
        if (in->target < 0) return WASM_FAIL("block %d: jump at %d to %d is not an instruction", blockIdx, in->offset, (int) in->imm[0]);
      }
      switch (in->op)
      {
        case EEL_BC_FCALL:
          {
            const int callee = findOrAddBlock(p, (const unsigned char *) in->imm[0]);
            if (callee < 0) return 0;
            p->blocks[blockIdx].insts[i].callee = callee;
          }
        break;
        case EEL_BC_CFUNC_1PDD: in->importIdx = addImport(p, in->imm[0], SIG_D_D); break;
        case EEL_BC_CFUNC_2PDD: case EEL_BC_CFUNC_2PDDS: in->importIdx = addImport(p, in->imm[0], SIG_D_DD); break;
        case EEL_BC_MEGABUF: in->importIdx = addImport(p, (INT_PTR) &__NSEEL_RAMAlloc, SIG_P_PP); break;
        case EEL_BC_GMEGABUF: in->importIdx = addImport(p, (INT_PTR) &__NSEEL_RAMAllocGMEM, SIG_P_PP); break;
        case EEL_BC_GENERIC1PARM: in->importIdx = addImport(p, in->imm[1], SIG_P_PP); break;
        case EEL_BC_GENERIC2PARM: in->importIdx = addImport(p, in->imm[1], SIG_P_PPP); break;
        case EEL_BC_GENERIC3PARM: in->importIdx = addImport(p, in->imm[1], SIG_P_PPPP); break;
        case EEL_BC_GENERIC1PARM_RETD: in->importIdx = addImport(p, in->imm[1], SIG_D_PP); break;
        case EEL_BC_GENERIC2PARM_RETD: in->importIdx = addImport(p, in->imm[1], SIG_D_PPP); break;
        case EEL_BC_GENERIC2XPARM_RETD: in->importIdx = addImport(p, in->imm[2], SIG_D_PPPP); break;
        case EEL_BC_GENERIC3PARM_RETD: in->importIdx = addImport(p, in->imm[1], SIG_D_PPPP); break;
      }
      if (p->failed) return 0;
    }
  }
  return 1;
}

// Returns {pops, pushes} of the fp stack for an instruction, or pops = -1 if unsupported.
static void fpEffect(const WasmProgram *p, const WasmInst *in, int *pops, int *pushes)
{
  *pops = 0;
  *pushes = 0;
  switch (in->op)
  {
    case EEL_BC_MOV_FPTOP_DV: case EEL_BC_PUSH_VAL_AT_P1_TO_FPSTACK: case EEL_BC_PUSH_VAL_AT_P2_TO_FPSTACK:
    case EEL_BC_PUSH_VAL_AT_P3_TO_FPSTACK: case EEL_BC_BOOLTOFP:
    case EEL_BC_GENERIC1PARM_RETD: case EEL_BC_GENERIC2PARM_RETD: case EEL_BC_GENERIC2XPARM_RETD:
    case EEL_BC_GENERIC3PARM_RETD:
      *pushes = 1;
    break;
    case EEL_BC_POP_FPSTACK_TO_PTR: case EEL_BC_POP_FPSTACK_TOSTACK: case EEL_BC_POP_FPSTACK_TO_WTP:
    case EEL_BC_POP_FPSTACK: case EEL_BC_LOOP_LOADCNT: case EEL_BC_ASSIGN_FAST_FROMFP: case EEL_BC_ASSIGN_FROMFP:
    case EEL_BC_FPTOBOOL: case EEL_BC_FPTOBOOL_REV: case EEL_BC_MEGABUF: case EEL_BC_GMEGABUF:
    case EEL_BC_CFUNC_2PDDS: case EEL_BC_USERSTACK_PEEK:
    case EEL_BC_ADD_OP: case EEL_BC_SUB_OP: case EEL_BC_ADD_OP_FAST: case EEL_BC_SUB_OP_FAST:
    case EEL_BC_MUL_OP: case EEL_BC_DIV_OP: case EEL_BC_MUL_OP_FAST: case EEL_BC_DIV_OP_FAST:
    case EEL_BC_AND_OP: case EEL_BC_OR_OP: case EEL_BC_XOR_OP: case EEL_BC_MOD_OP:
      *pops = 1;
    break;
    case EEL_BC_EQUAL: case EEL_BC_EQUAL_EXACT: case EEL_BC_NOTEQUAL: case EEL_BC_NOTEQUAL_EXACT:
    case EEL_BC_ABOVE: case EEL_BC_BELOWEQ:
      *pops = 2;
    break;
    case EEL_BC_ADD: case EEL_BC_SUB: case EEL_BC_MUL: case EEL_BC_DIV: case EEL_BC_AND: case EEL_BC_OR:
    case EEL_BC_XOR: case EEL_BC_MOD: case EEL_BC_SHR: case EEL_BC_SHL: case EEL_BC_MIN_FP: case EEL_BC_MAX_FP:
    case EEL_BC_CFUNC_2PDD:
      *pops = 2;
      *pushes = 1;
    break;
    case EEL_BC_OR0: case EEL_BC_UMINUS: case EEL_BC_SQR: case EEL_BC_ABS: case EEL_BC_SIGN: case EEL_BC_INVSQRT:
    case EEL_BC_CFUNC_1PDD:
      *pops = 1;
      *pushes = 1;
    break;
    case EEL_BC_FXCH:
      *pops = 2;
      *pushes = 2;
    break;
    case EEL_BC_FCALL:
      *pushes = p->blocks[in->callee].retFp;
    break;
  }
}

static int analyzeBlock(WasmProgram *p, int blockIdx);

// Computes fp stack depths with a worklist; every merge point must agree, as EEL2's code generator guarantees.
static int analyzeDepths(WasmProgram *p, int blockIdx)
{
  WasmBlock *b;
  int *work, nwork = 0, i, retFp = -1;
  // analyzing a callee can move p->blocks, so index through it
  for (i = 0; i < p->blocks[blockIdx].count; i++)
  {
    const WasmInst *in = &p->blocks[blockIdx].insts[i];
    if (in->op == EEL_BC_FCALL && !analyzeBlock(p, in->callee)) return 0;
  }
  b = &p->blocks[blockIdx];
  work = (int *) malloc(sizeof(int) * (b->count + 1));
  if (!work) return 0;
  b->insts[0].fpIn = 0;
  work[nwork++] = 0;
  b->maxFp = 0;
  while (nwork > 0)
  {
    const int idx = work[--nwork];
    WasmInst *in = &b->insts[idx];
    int pops, pushes, out, succ[2], nsucc = 0, s;
    fpEffect(p, in, &pops, &pushes);
    if (in->fpIn < pops)
    {
#ifdef NSEEL_WASM_DEBUG
      int k;
      for (k = idx > 6 ? idx - 6 : 0; k <= idx; k++)
      {
        const WasmInst *x = &b->insts[k];
        fprintf(stderr, "[eel2-wasm]   #%d off=%d op=%d fpIn=%d target=%d callee=%d\n", k, x->offset, x->op, x->fpIn, x->target, x->callee);
        if (x->callee >= 0)
        {
          const WasmBlock *c = &p->blocks[x->callee];
          int m;
          fprintf(stderr, "[eel2-wasm]     callee %d: %d insts retFp=%d\n", x->callee, c->count, c->retFp);
          for (m = c->count > 8 ? c->count - 8 : 0; m < c->count; m++)
            fprintf(stderr, "[eel2-wasm]       #%d off=%d op=%d fpIn=%d target=%d\n", m, c->insts[m].offset, c->insts[m].op, c->insts[m].fpIn, c->insts[m].target);
        }
      }
#endif
      free(work);
      return WASM_FAIL("block %d: fp stack underflow at %d (op %d)", blockIdx, in->offset, in->op);
    }
    out = in->fpIn - pops + pushes;
    if (out > b->maxFp) b->maxFp = out;
    if (in->fpIn > b->maxFp) b->maxFp = in->fpIn;
    if (in->op == EEL_BC_RET)
    {
      if (retFp >= 0 && retFp != in->fpIn) { free(work); return WASM_FAIL("block %d: inconsistent return depth", blockIdx); }
      retFp = in->fpIn;
      continue;
    }
    if (in->op != EEL_BC_JMP_NC) succ[nsucc++] = idx + 1;
    if (in->target >= 0) succ[nsucc++] = in->target;
    for (s = 0; s < nsucc; s++)
    {
      WasmInst *n;
      if (succ[s] >= b->count) { free(work); return WASM_FAIL("block %d: falls off the end", blockIdx); }
      n = &b->insts[succ[s]];
      if (n->fpIn < 0)
      {
        n->fpIn = out;
        work[nwork++] = succ[s];
      }
      else if (n->fpIn != out) { free(work); return WASM_FAIL("block %d: fp depth %d vs %d merging at %d", blockIdx, n->fpIn, out, n->offset); }
    }
  }
  free(work);
  if (retFp < 0 || retFp > 1) return WASM_FAIL("block %d: returns fp depth %d", blockIdx, retFp);
  b->retFp = retFp;
  return 1;
}

static int analyzeBlock(WasmProgram *p, int blockIdx)
{
  WasmBlock *b = &p->blocks[blockIdx];
  int i;
  if (b->state == 2) return 1;
  if (b->state == 1) return WASM_FAIL("block %d: recursive call", blockIdx);
  b->state = 1;
  if (!decodeBlock(p, blockIdx)) return 0;
  if (!analyzeDepths(p, blockIdx)) return 0;
  b = &p->blocks[blockIdx];
  // a backward jump closes a loop that starts at its target
  for (i = 0; i < b->count; i++)
  {
    WasmInst *in = &b->insts[i];
    if (in->target >= 0 && in->target <= i)
    {
      WasmInst *h = &b->insts[in->target];
      if (h->loopEnd >= 0 && h->loopEnd != i) return WASM_FAIL("block %d: two back edges to %d", blockIdx, in->target);
      h->loopEnd = i;
    }
  }
  b->state = 2;
  return 1;
}

// ---- emission ------------------------------------------------------------------------

typedef struct
{
  int kind;   // 0 = block ending at target, 1 = loop starting at target
  int target;
} WasmLabel;

typedef struct
{
  WasmBuf *b;
  WasmProgram *prog;
  WasmBlock *blk;
  int isEntry;
  int p1, p2, p3, wtp, sp, rt, t0, t1, d0, fp0;
  int funcBase;   // function index of the first block
  int filterFunc;
  WasmLabel *labels;
  int labelCount, labelCap;
  int failed;
} WasmEmit;

static void eOp(WasmEmit *e, unsigned char op) { wbByte(e->b, op); }
static void eGet(WasmEmit *e, int l) { wbByte(e->b, W_LOCAL_GET); wbUleb(e->b, l); }
static void eSet(WasmEmit *e, int l) { wbByte(e->b, W_LOCAL_SET); wbUleb(e->b, l); }
static void eTee(WasmEmit *e, int l) { wbByte(e->b, W_LOCAL_TEE); wbUleb(e->b, l); }
static void eI32(WasmEmit *e, INT_PTR v) { wbByte(e->b, W_I32_CONST); wbSleb(e->b, (int32_t) v); }
static void eF64(WasmEmit *e, double v) { wbByte(e->b, W_F64_CONST); wbF64(e->b, v); }
static void eLoadI32(WasmEmit *e, uint32_t off) { wbByte(e->b, W_I32_LOAD); wbUleb(e->b, 2); wbUleb(e->b, off); }
static void eStoreI32(WasmEmit *e, uint32_t off) { wbByte(e->b, W_I32_STORE); wbUleb(e->b, 2); wbUleb(e->b, off); }
static void eLoadF64(WasmEmit *e) { wbByte(e->b, W_F64_LOAD); wbUleb(e->b, 3); wbUleb(e->b, 0); }
static void eStoreF64(WasmEmit *e) { wbByte(e->b, W_F64_STORE); wbUleb(e->b, 3); wbUleb(e->b, 0); }
static void eCall(WasmEmit *e, int f) { wbByte(e->b, W_CALL); wbUleb(e->b, f); }
static void eTruncI32(WasmEmit *e) { wbByte(e->b, W_PREFIX_FC); wbUleb(e->b, 2); }
static void eTruncU32(WasmEmit *e) { wbByte(e->b, W_PREFIX_FC); wbUleb(e->b, 3); }
static void eTruncI64(WasmEmit *e) { wbByte(e->b, W_PREFIX_FC); wbUleb(e->b, 6); }
static void eAddLocal(WasmEmit *e, int l, int amt) { eGet(e, l); eI32(e, amt); eOp(e, W_I32_ADD); eSet(e, l); }
static int fpLocal(WasmEmit *e, int slot) { return e->fp0 + slot; }

static void pushLabel(WasmEmit *e, int kind, int target)
{
  if (e->labelCount == e->labelCap)
  {
    int ncap = e->labelCap ? e->labelCap * 2 : 16;
    WasmLabel *n = (WasmLabel *) realloc(e->labels, ncap * sizeof(WasmLabel));
    if (!n) { e->failed = 1; return; }
    e->labels = n;
    e->labelCap = ncap;
  }
  e->labels[e->labelCount].kind = kind;
  e->labels[e->labelCount].target = target;
  e->labelCount++;
}

// Branches to an instruction index; extraDepth counts enclosing if/else constructs opened by the caller.
static void eBranch(WasmEmit *e, unsigned char op, int source, int target, int extraDepth)
{
  const int kind = target <= source ? 1 : 0;
  int i;
  for (i = e->labelCount - 1; i >= 0; i--)
  {
    const WasmLabel *l = &e->labels[i];
    if (l->target == target && l->kind == kind)
    {
      wbByte(e->b, op);
      wbUleb(e->b, (uint32_t) (e->labelCount - 1 - i + extraDepth));
      return;
    }
  }
  WASM_FAIL("no label for a jump to %d", target);
  e->failed = 1;
}

static void eBinaryI64(WasmEmit *e, int dst, int a, int bb, unsigned char op)
{
  eGet(e, a); eTruncI64(e);
  eGet(e, bb); eTruncI64(e);
  eOp(e, op);
  eOp(e, W_F64_CONVERT_I64_S);
  eSet(e, dst);
}

static void eOpAssign(WasmEmit *e, int top, unsigned char op, int filter)
{
  eGet(e, e->p2);
  eGet(e, e->p2); eLoadF64(e);
  eGet(e, top);
  eOp(e, op);
  if (filter) eCall(e, e->filterFunc);
  eStoreF64(e);
  eGet(e, e->p2); eSet(e, e->p1);
}

static void eOpAssignI64(WasmEmit *e, int top, unsigned char op)
{
  eGet(e, e->p2);
  eGet(e, e->p2); eLoadF64(e); eTruncI64(e);
  eGet(e, top); eTruncI64(e);
  eOp(e, op);
  eOp(e, W_F64_CONVERT_I64_S);
  eStoreF64(e);
  eGet(e, e->p2); eSet(e, e->p1);
}

static void eUserStackFix(WasmEmit *e, INT_PTR mask, INT_PTR orv)
{
  eI32(e, mask); eOp(e, W_I32_AND);
  eI32(e, orv); eOp(e, W_I32_OR);
}

static void emitInst(WasmEmit *e, int idx)
{
  const WasmInst *in = &e->blk->insts[idx];
  const int d = in->fpIn;
  const int top = fpLocal(e, d - 1), top2 = fpLocal(e, d - 2), push = fpLocal(e, d);
  const int importBase = 0;
  if (d < 0) return; // unreachable
  switch (in->op)
  {
    case EEL_BC_NOP: break;
    case EEL_BC_RET:
      if (!e->isEntry)
      {
        eGet(e, e->p1); eGet(e, e->p2); eGet(e, e->p3); eGet(e, e->wtp);
        if (e->blk->retFp) eGet(e, top); else eF64(e, 0.0);
      }
      eOp(e, W_RETURN);
    break;
    case EEL_BC_JMP_NC: eBranch(e, W_BR, idx, in->target, 0); break;
    case EEL_BC_JMP_IF_P1_Z: eGet(e, e->p1); eOp(e, W_I32_EQZ); eBranch(e, W_BR_IF, idx, in->target, 0); break;
    case EEL_BC_JMP_IF_P1_NZ: eGet(e, e->p1); eBranch(e, W_BR_IF, idx, in->target, 0); break;
    case EEL_BC_MOV_FPTOP_DV: eI32(e, in->imm[0]); eLoadF64(e); eSet(e, push); break;
    case EEL_BC_MOV_P1_DV: eI32(e, in->imm[0]); eSet(e, e->p1); break;
    case EEL_BC_MOV_P2_DV: eI32(e, in->imm[0]); eSet(e, e->p2); break;
    case EEL_BC_MOV_P3_DV: eI32(e, in->imm[0]); eSet(e, e->p3); break;
    case EEL_BC__RESET_WTP: eI32(e, in->imm[0]); eSet(e, e->wtp); break;
    case EEL_BC_PUSH_P1: eAddLocal(e, e->sp, -8); eGet(e, e->sp); eGet(e, e->p1); eStoreI32(e, 0); break;
    case EEL_BC_PUSH_P1PTR_AS_VALUE: eAddLocal(e, e->sp, -8); eGet(e, e->sp); eGet(e, e->p1); eLoadF64(e); eStoreF64(e); break;
    case EEL_BC_POP_P1: eGet(e, e->sp); eLoadI32(e, 0); eSet(e, e->p1); eAddLocal(e, e->sp, 8); break;
    case EEL_BC_POP_P2: eGet(e, e->sp); eLoadI32(e, 0); eSet(e, e->p2); eAddLocal(e, e->sp, 8); break;
    case EEL_BC_POP_P3: eGet(e, e->sp); eLoadI32(e, 0); eSet(e, e->p3); eAddLocal(e, e->sp, 8); break;
    case EEL_BC_POP_VALUE_TO_ADDR: eI32(e, in->imm[0]); eGet(e, e->sp); eLoadF64(e); eStoreF64(e); eAddLocal(e, e->sp, 8); break;
    case EEL_BC_MOVE_STACK: eAddLocal(e, e->sp, (int) in->imm[0]); break;
    case EEL_BC_STORE_P1_TO_STACK_AT_OFFS: eGet(e, e->sp); eI32(e, in->imm[0]); eOp(e, W_I32_ADD); eGet(e, e->p1); eStoreI32(e, 0); break;
    case EEL_BC_MOVE_STACKPTR_TO_P1: eGet(e, e->sp); eSet(e, e->p1); break;
    case EEL_BC_MOVE_STACKPTR_TO_P2: eGet(e, e->sp); eSet(e, e->p2); break;
    case EEL_BC_MOVE_STACKPTR_TO_P3: eGet(e, e->sp); eSet(e, e->p3); break;
    case EEL_BC_SET_P2_FROM_P1: eGet(e, e->p1); eSet(e, e->p2); break;
    case EEL_BC_SET_P3_FROM_P1: eGet(e, e->p1); eSet(e, e->p3); break;
    case EEL_BC_COPY_VALUE_AT_P1_TO_ADDR: eI32(e, in->imm[0]); eGet(e, e->p1); eLoadF64(e); eStoreF64(e); break;
    case EEL_BC_SET_P1_FROM_WTP: eGet(e, e->wtp); eSet(e, e->p1); break;
    case EEL_BC_SET_P2_FROM_WTP: eGet(e, e->wtp); eSet(e, e->p2); break;
    case EEL_BC_SET_P3_FROM_WTP: eGet(e, e->wtp); eSet(e, e->p3); break;
    case EEL_BC_POP_FPSTACK_TO_PTR: eI32(e, in->imm[0]); eGet(e, top); eStoreF64(e); break;
    case EEL_BC_POP_FPSTACK_TOSTACK: eAddLocal(e, e->sp, -8); eGet(e, e->sp); eGet(e, top); eStoreF64(e); break;
    case EEL_BC_PUSH_VAL_AT_P1_TO_FPSTACK: eGet(e, e->p1); eLoadF64(e); eSet(e, push); break;
    case EEL_BC_PUSH_VAL_AT_P2_TO_FPSTACK: eGet(e, e->p2); eLoadF64(e); eSet(e, push); break;
    case EEL_BC_PUSH_VAL_AT_P3_TO_FPSTACK: eGet(e, e->p3); eLoadF64(e); eSet(e, push); break;
    case EEL_BC_POP_FPSTACK_TO_WTP: eGet(e, e->wtp); eGet(e, top); eStoreF64(e); eAddLocal(e, e->wtp, 8); break;
    case EEL_BC_SET_P1_Z: eI32(e, 0); eSet(e, e->p1); break;
    case EEL_BC_SET_P1_NZ: eI32(e, 1); eSet(e, e->p1); break;

    case EEL_BC_LOOP_LOADCNT:
      eGet(e, top); eTruncI32(e); eSet(e, e->t0);
      eAddLocal(e, e->sp, -8); eGet(e, e->sp); eGet(e, e->t0); eStoreI32(e, 0);
      eGet(e, e->t0); eI32(e, 1); eOp(e, W_I32_LT_S);
      eOp(e, W_IF); eOp(e, W_TYPE_VOID);
        eAddLocal(e, e->sp, 8);
        eBranch(e, W_BR, idx, in->target, 1);
      eOp(e, W_END);
#if NSEEL_LOOPFUNC_SUPPORT_MAXLEN > 0
      eGet(e, e->t0); eI32(e, NSEEL_LOOPFUNC_SUPPORT_MAXLEN); eOp(e, W_I32_GT_S);
      eOp(e, W_IF); eOp(e, W_TYPE_VOID);
        eGet(e, e->sp); eI32(e, NSEEL_LOOPFUNC_SUPPORT_MAXLEN); eStoreI32(e, 0);
      eOp(e, W_END);
#endif
      eAddLocal(e, e->sp, -8); eGet(e, e->sp); eGet(e, e->wtp); eStoreI32(e, 0);
    break;
    case EEL_BC_LOOP_END:
      eGet(e, e->sp); eLoadI32(e, 0); eSet(e, e->wtp);
      eGet(e, e->sp);
      eGet(e, e->sp); eLoadI32(e, 8); eI32(e, 1); eOp(e, W_I32_SUB); eTee(e, e->t0);
      eStoreI32(e, 8);
      eGet(e, e->t0); eI32(e, 0); eOp(e, W_I32_LE_S);
      eOp(e, W_IF); eOp(e, W_TYPE_VOID);
        eAddLocal(e, e->sp, 16);
      eOp(e, W_ELSE);
        eBranch(e, W_BR, idx, in->target, 1);
      eOp(e, W_END);
    break;
#if NSEEL_LOOPFUNC_SUPPORT_MAXLEN > 0
    case EEL_BC_WHILE_SETUP: eAddLocal(e, e->sp, -8); eGet(e, e->sp); eI32(e, NSEEL_LOOPFUNC_SUPPORT_MAXLEN); eStoreI32(e, 0); break;
#endif
    case EEL_BC_WHILE_BEGIN: eAddLocal(e, e->sp, -8); eGet(e, e->sp); eGet(e, e->wtp); eStoreI32(e, 0); break;
    case EEL_BC_WHILE_END:
      eGet(e, e->sp); eLoadI32(e, 0); eSet(e, e->wtp);
      eAddLocal(e, e->sp, 8);
#if NSEEL_LOOPFUNC_SUPPORT_MAXLEN > 0
      eGet(e, e->sp);
      eGet(e, e->sp); eLoadI32(e, 0); eI32(e, 1); eOp(e, W_I32_SUB); eTee(e, e->t0);
      eStoreI32(e, 0);
      eGet(e, e->t0); eI32(e, 0); eOp(e, W_I32_LE_S);
      eOp(e, W_IF); eOp(e, W_TYPE_VOID);
        eAddLocal(e, e->sp, 8);
        eBranch(e, W_BR, idx, in->target, 1);
      eOp(e, W_END);
#endif
    break;
    case EEL_BC_WHILE_CHECK_RV:
      eGet(e, e->p1);
      eOp(e, W_IF); eOp(e, W_TYPE_VOID);
        eBranch(e, W_BR, idx, in->target, 1);
#if NSEEL_LOOPFUNC_SUPPORT_MAXLEN > 0
      eOp(e, W_ELSE);
        eAddLocal(e, e->sp, 8);
#endif
      eOp(e, W_END);
    break;

    case EEL_BC_BNOT: eGet(e, e->p1); eOp(e, W_I32_EQZ); eSet(e, e->p1); break;
    case EEL_BC_EQUAL: eGet(e, top); eGet(e, top2); eOp(e, W_F64_SUB); eOp(e, W_F64_ABS); eF64(e, NSEEL_CLOSEFACTOR); eOp(e, W_F64_LT); eSet(e, e->p1); break;
    case EEL_BC_EQUAL_EXACT: eGet(e, top); eGet(e, top2); eOp(e, W_F64_EQ); eSet(e, e->p1); break;
    case EEL_BC_NOTEQUAL: eGet(e, top); eGet(e, top2); eOp(e, W_F64_SUB); eOp(e, W_F64_ABS); eF64(e, NSEEL_CLOSEFACTOR); eOp(e, W_F64_GE); eSet(e, e->p1); break;
    case EEL_BC_NOTEQUAL_EXACT: eGet(e, top); eGet(e, top2); eOp(e, W_F64_NE); eSet(e, e->p1); break;
    case EEL_BC_ABOVE: eGet(e, top); eGet(e, top2); eOp(e, W_F64_LT); eSet(e, e->p1); break;
    case EEL_BC_BELOWEQ: eGet(e, top); eGet(e, top2); eOp(e, W_F64_GE); eSet(e, e->p1); break;

    case EEL_BC_ADD: eGet(e, top2); eGet(e, top); eOp(e, W_F64_ADD); eSet(e, top2); break;
    case EEL_BC_SUB: eGet(e, top2); eGet(e, top); eOp(e, W_F64_SUB); eSet(e, top2); break;
    case EEL_BC_MUL: eGet(e, top2); eGet(e, top); eOp(e, W_F64_MUL); eSet(e, top2); break;
    case EEL_BC_DIV: eGet(e, top2); eGet(e, top); eOp(e, W_F64_DIV); eSet(e, top2); break;
    case EEL_BC_AND: eBinaryI64(e, top2, top, top2, W_I64_AND); break;
    case EEL_BC_OR: eBinaryI64(e, top2, top, top2, W_I64_OR); break;
    case EEL_BC_XOR: eBinaryI64(e, top2, top, top2, W_I64_XOR); break;
    case EEL_BC_OR0: eGet(e, top); eTruncI64(e); eOp(e, W_F64_CONVERT_I64_S); eSet(e, top); break;

    case EEL_BC_ADD_OP: eOpAssign(e, top, W_F64_ADD, 1); break;
    case EEL_BC_SUB_OP: eOpAssign(e, top, W_F64_SUB, 1); break;
    case EEL_BC_MUL_OP: eOpAssign(e, top, W_F64_MUL, 1); break;
    case EEL_BC_DIV_OP: eOpAssign(e, top, W_F64_DIV, 1); break;
    case EEL_BC_ADD_OP_FAST: eOpAssign(e, top, W_F64_ADD, 0); break;
    case EEL_BC_SUB_OP_FAST: eOpAssign(e, top, W_F64_SUB, 0); break;
    case EEL_BC_MUL_OP_FAST: eOpAssign(e, top, W_F64_MUL, 0); break;
    case EEL_BC_DIV_OP_FAST: eOpAssign(e, top, W_F64_DIV, 0); break;
    case EEL_BC_AND_OP: eOpAssignI64(e, top, W_I64_AND); break;
    case EEL_BC_OR_OP: eOpAssignI64(e, top, W_I64_OR); break;
    case EEL_BC_XOR_OP: eOpAssignI64(e, top, W_I64_XOR); break;
    case EEL_BC_UMINUS: eGet(e, top); eOp(e, W_F64_NEG); eSet(e, top); break;

    case EEL_BC_ASSIGN: eGet(e, e->p2); eGet(e, e->p1); eLoadF64(e); eCall(e, e->filterFunc); eStoreF64(e); eGet(e, e->p2); eSet(e, e->p1); break;
    case EEL_BC_ASSIGN_FAST: eGet(e, e->p2); eGet(e, e->p1); eLoadF64(e); eStoreF64(e); eGet(e, e->p2); eSet(e, e->p1); break;
    case EEL_BC_ASSIGN_FAST_FROMFP: eGet(e, e->p2); eGet(e, top); eStoreF64(e); eGet(e, e->p2); eSet(e, e->p1); break;
    case EEL_BC_ASSIGN_FROMFP: eGet(e, e->p2); eGet(e, top); eCall(e, e->filterFunc); eStoreF64(e); eGet(e, e->p2); eSet(e, e->p1); break;

    case EEL_BC_MOD:
      eGet(e, top); eOp(e, W_F64_ABS); eTruncI32(e); eSet(e, e->t0);
      eGet(e, e->t0);
      eOp(e, W_IF); eOp(e, W_TYPE_F64);
        eGet(e, top2); eOp(e, W_F64_ABS); eTruncI64(e);
        eGet(e, e->t0); eOp(e, W_I64_EXTEND_I32_S);
        eOp(e, W_I64_REM_S); eOp(e, W_F64_CONVERT_I64_S);
      eOp(e, W_ELSE);
        eF64(e, 0.0);
      eOp(e, W_END);
      eSet(e, top2);
    break;
    case EEL_BC_MOD_OP:
      eGet(e, top); eOp(e, W_F64_ABS); eTruncI32(e); eSet(e, e->t0);
      eGet(e, e->p2);
      eGet(e, e->t0);
      eOp(e, W_IF); eOp(e, W_TYPE_F64);
        eGet(e, e->p2); eLoadF64(e); eOp(e, W_F64_ABS); eTruncI64(e);
        eGet(e, e->t0); eOp(e, W_I64_EXTEND_I32_S);
        eOp(e, W_I64_REM_S); eOp(e, W_F64_CONVERT_I64_S);
      eOp(e, W_ELSE);
        eF64(e, 0.0);
      eOp(e, W_END);
      eStoreF64(e);
      eGet(e, e->p2); eSet(e, e->p1);
    break;
    case EEL_BC_SHR: eGet(e, top2); eTruncI32(e); eGet(e, top); eTruncI32(e); eOp(e, W_I32_SHR_S); eOp(e, W_F64_CONVERT_I32_S); eSet(e, top2); break;
    case EEL_BC_SHL: eGet(e, top2); eTruncI32(e); eGet(e, top); eTruncI32(e); eOp(e, W_I32_SHL); eOp(e, W_F64_CONVERT_I32_S); eSet(e, top2); break;
    case EEL_BC_SQR: eGet(e, top); eGet(e, top); eOp(e, W_F64_MUL); eSet(e, top); break;
    case EEL_BC_MIN: eGet(e, e->p2); eGet(e, e->p1); eGet(e, e->p1); eLoadF64(e); eGet(e, e->p2); eLoadF64(e); eOp(e, W_F64_GT); eOp(e, W_SELECT); eSet(e, e->p1); break;
    case EEL_BC_MAX: eGet(e, e->p2); eGet(e, e->p1); eGet(e, e->p1); eLoadF64(e); eGet(e, e->p2); eLoadF64(e); eOp(e, W_F64_LT); eOp(e, W_SELECT); eSet(e, e->p1); break;
    case EEL_BC_MIN_FP: eGet(e, top); eGet(e, top2); eGet(e, top); eGet(e, top2); eOp(e, W_F64_LT); eOp(e, W_SELECT); eSet(e, top2); break;
    case EEL_BC_MAX_FP: eGet(e, top); eGet(e, top2); eGet(e, top); eGet(e, top2); eOp(e, W_F64_GT); eOp(e, W_SELECT); eSet(e, top2); break;
    case EEL_BC_ABS: eGet(e, top); eOp(e, W_F64_ABS); eSet(e, top); break;
    case EEL_BC_SIGN:
      eF64(e, -1.0);
      eF64(e, 1.0); eGet(e, top); eGet(e, top); eF64(e, 0.0); eOp(e, W_F64_GT); eOp(e, W_SELECT);
      eGet(e, top); eF64(e, 0.0); eOp(e, W_F64_LT); eOp(e, W_SELECT);
      eSet(e, top);
    break;
    case EEL_BC_INVSQRT:
      eI32(e, 0x5f3759df);
      eGet(e, top); eOp(e, W_F32_DEMOTE_F64); eOp(e, W_I32_REINTERPRET_F32); eI32(e, 1); eOp(e, W_I32_SHR_S);
      eOp(e, W_I32_SUB); eOp(e, W_F32_REINTERPRET_I32); eOp(e, W_F64_PROMOTE_F32); eSet(e, e->d0);
      eGet(e, e->d0);
      eF64(e, 1.5);
      eGet(e, top); eF64(e, 0.5); eOp(e, W_F64_MUL); eGet(e, e->d0); eOp(e, W_F64_MUL); eGet(e, e->d0); eOp(e, W_F64_MUL);
      eOp(e, W_F64_SUB); eOp(e, W_F64_MUL);
      eSet(e, top);
    break;
    case EEL_BC_FXCH: eGet(e, top); eSet(e, e->d0); eGet(e, top2); eSet(e, top); eGet(e, e->d0); eSet(e, top2); break;
    case EEL_BC_POP_FPSTACK: break;

    case EEL_BC_FCALL:
      {
        const WasmBlock *callee = &e->prog->blocks[in->callee];
        eGet(e, e->p1); eGet(e, e->p2); eGet(e, e->p3); eGet(e, e->wtp);
        eGet(e, e->sp); eI32(e, 8); eOp(e, W_I32_SUB);
        eGet(e, e->rt);
        eCall(e, e->funcBase + in->callee);
        if (callee->retFp) eSet(e, push); else eOp(e, W_DROP);
        eSet(e, e->wtp); eSet(e, e->p3); eSet(e, e->p2); eSet(e, e->p1);
      }
    break;
    case EEL_BC_BOOLTOFP: eF64(e, 1.0); eF64(e, 0.0); eGet(e, e->p1); eOp(e, W_SELECT); eSet(e, push); break;
    case EEL_BC_FPTOBOOL: eGet(e, top); eOp(e, W_F64_ABS); eF64(e, NSEEL_CLOSEFACTOR); eOp(e, W_F64_GE); eSet(e, e->p1); break;
    case EEL_BC_FPTOBOOL_REV: eGet(e, top); eOp(e, W_F64_ABS); eF64(e, NSEEL_CLOSEFACTOR); eOp(e, W_F64_LT); eSet(e, e->p1); break;

    case EEL_BC_CFUNC_1PDD: eGet(e, top); eCall(e, importBase + in->importIdx); eSet(e, top); break;
    case EEL_BC_CFUNC_2PDD: eGet(e, top2); eGet(e, top); eCall(e, importBase + in->importIdx); eSet(e, top2); break;
    case EEL_BC_CFUNC_2PDDS:
      eGet(e, e->p2);
      eGet(e, e->p2); eLoadF64(e); eGet(e, top); eCall(e, importBase + in->importIdx);
      eStoreF64(e);
      eGet(e, e->p2); eSet(e, e->p1);
    break;

    case EEL_BC_MEGABUF:
      eGet(e, top); eF64(e, NSEEL_CLOSEFACTOR); eOp(e, W_F64_ADD); eTruncU32(e); eSet(e, e->t0);
      eGet(e, e->t0); eI32(e, (int32_t) NSEEL_WASM_RAM_ITEMS); eOp(e, W_I32_LT_U);
      eOp(e, W_IF); eOp(e, W_TYPE_I32);
        eGet(e, e->rt); eGet(e, e->t0); eI32(e, NSEEL_RAM_ITEMSPERBLOCK_LOG2); eOp(e, W_I32_SHR_U);
        eI32(e, 2); eOp(e, W_I32_SHL); eOp(e, W_I32_ADD); eLoadI32(e, 0);
      eOp(e, W_ELSE);
        eI32(e, 0);
      eOp(e, W_END);
      eTee(e, e->t1);
      eOp(e, W_IF); eOp(e, W_TYPE_I32);
        eGet(e, e->t1); eGet(e, e->t0); eI32(e, NSEEL_RAM_ITEMSPERBLOCK - 1); eOp(e, W_I32_AND);
        eI32(e, 3); eOp(e, W_I32_SHL); eOp(e, W_I32_ADD);
      eOp(e, W_ELSE);
        eGet(e, e->rt); eGet(e, e->t0); eCall(e, importBase + in->importIdx);
      eOp(e, W_END);
      eSet(e, e->p1);
    break;
    case EEL_BC_GMEGABUF:
      eI32(e, in->imm[0]);
      eGet(e, top); eF64(e, NSEEL_CLOSEFACTOR); eOp(e, W_F64_ADD); eTruncI32(e);
      eCall(e, importBase + in->importIdx);
      eSet(e, e->p1);
    break;

    case EEL_BC_GENERIC1PARM: eI32(e, in->imm[0]); eGet(e, e->p1); eCall(e, importBase + in->importIdx); eSet(e, e->p1); break;
    case EEL_BC_GENERIC2PARM: eI32(e, in->imm[0]); eGet(e, e->p2); eGet(e, e->p1); eCall(e, importBase + in->importIdx); eSet(e, e->p1); break;
    case EEL_BC_GENERIC3PARM: eI32(e, in->imm[0]); eGet(e, e->p3); eGet(e, e->p2); eGet(e, e->p1); eCall(e, importBase + in->importIdx); eSet(e, e->p1); break;
    case EEL_BC_GENERIC1PARM_RETD: eI32(e, in->imm[0]); eGet(e, e->p1); eCall(e, importBase + in->importIdx); eSet(e, push); break;
    case EEL_BC_GENERIC2PARM_RETD: eI32(e, in->imm[0]); eGet(e, e->p2); eGet(e, e->p1); eCall(e, importBase + in->importIdx); eSet(e, push); break;
    case EEL_BC_GENERIC2XPARM_RETD: eI32(e, in->imm[0]); eI32(e, in->imm[1]); eGet(e, e->p2); eGet(e, e->p1); eCall(e, importBase + in->importIdx); eSet(e, push); break;
    case EEL_BC_GENERIC3PARM_RETD: eI32(e, in->imm[0]); eGet(e, e->p3); eGet(e, e->p2); eGet(e, e->p1); eCall(e, importBase + in->importIdx); eSet(e, push); break;

    case EEL_BC_USERSTACK_PUSH:
      eI32(e, in->imm[0]); eLoadI32(e, 0); eI32(e, 8); eOp(e, W_I32_ADD); eUserStackFix(e, in->imm[1], in->imm[2]); eSet(e, e->t0);
      eI32(e, in->imm[0]); eGet(e, e->t0); eStoreI32(e, 0);
      eGet(e, e->t0); eGet(e, e->p1); eLoadF64(e); eStoreF64(e);
    break;
    case EEL_BC_USERSTACK_POP:
      eI32(e, in->imm[0]); eLoadI32(e, 0); eSet(e, e->t0);
      eGet(e, e->p1); eGet(e, e->t0); eLoadF64(e); eStoreF64(e);
      eI32(e, in->imm[0]); eGet(e, e->t0); eI32(e, 8); eOp(e, W_I32_SUB); eUserStackFix(e, in->imm[1], in->imm[2]); eStoreI32(e, 0);
    break;
    case EEL_BC_USERSTACK_POPFAST:
      eI32(e, in->imm[0]); eLoadI32(e, 0); eSet(e, e->t0);
      eGet(e, e->t0); eSet(e, e->p1);
      eI32(e, in->imm[0]); eGet(e, e->t0); eI32(e, 8); eOp(e, W_I32_SUB); eUserStackFix(e, in->imm[1], in->imm[2]); eStoreI32(e, 0);
    break;
    case EEL_BC_USERSTACK_PEEK:
      eI32(e, in->imm[0]); eLoadI32(e, 0);
      eGet(e, top); eTruncI32(e); eI32(e, 8); eOp(e, W_I32_MUL); eOp(e, W_I32_SUB);
      eUserStackFix(e, in->imm[1], in->imm[2]);
      eSet(e, e->p1);
    break;
    case EEL_BC_USERSTACK_PEEK_INT:
      eI32(e, in->imm[0]); eLoadI32(e, 0); eI32(e, in->imm[1]); eOp(e, W_I32_SUB);
      eUserStackFix(e, in->imm[2], in->imm[3]);
      eSet(e, e->p1);
    break;
    case EEL_BC_USERSTACK_PEEK_TOP: eI32(e, in->imm[0]); eLoadI32(e, 0); eSet(e, e->p1); break;
    case EEL_BC_USERSTACK_EXCH:
      eI32(e, in->imm[0]); eLoadI32(e, 0); eSet(e, e->t0);
      eGet(e, e->t0); eLoadF64(e); eSet(e, e->d0);
      eGet(e, e->t0); eGet(e, e->p1); eLoadF64(e); eStoreF64(e);
      eGet(e, e->p1); eGet(e, e->d0); eStoreF64(e);
    break;

    default:
      WASM_FAIL("no emitter for opcode %d", in->op);
      e->failed = 1;
    break;
  }
}

static void emitRange(WasmEmit *e, int start, int last, int ownsHeader);

// Opens a block for every forward target owned by this region, then walks it, recursing into nested loops.
static void emitRange(WasmEmit *e, int start, int last, int ownsHeader)
{
  WasmBlock *blk = e->blk;
  int *targets, ntargets = 0, i, j;
  const int baseLabels = e->labelCount;
  targets = (int *) malloc(sizeof(int) * (blk->count + 1));
  if (!targets) { e->failed = 1; return; }
  for (i = start; i <= last; i++)
  {
    const WasmInst *in = &blk->insts[i];
    int t = in->target, nested = 0, k;
    if (t < 0 || t <= i || t > last) continue;
    // a forward target inside a nested loop is that loop's business
    for (k = start; k <= last && !nested; k++)
    {
      const WasmInst *h = &blk->insts[k];
      if (k == start && ownsHeader) continue;
      if (h->loopEnd >= 0 && k < t && t <= h->loopEnd) nested = 1;
    }
    if (nested) continue;
    for (k = 0; k < ntargets; k++) if (targets[k] == t) break;
    if (k == ntargets) targets[ntargets++] = t;
  }
  // larger targets are outer blocks
  for (i = 0; i < ntargets; i++)
    for (j = i + 1; j < ntargets; j++)
      if (targets[j] > targets[i]) { int tmp = targets[i]; targets[i] = targets[j]; targets[j] = tmp; }
  for (i = 0; i < ntargets; i++)
  {
    eOp(e, W_BLOCK); eOp(e, W_TYPE_VOID);
    pushLabel(e, 0, targets[i]);
  }
  free(targets);

  i = start;
  while (i <= last && !e->failed)
  {
    while (e->labelCount > baseLabels && e->labels[e->labelCount - 1].kind == 0 && e->labels[e->labelCount - 1].target == i)
    {
      eOp(e, W_END);
      e->labelCount--;
    }
    if (blk->insts[i].loopEnd >= 0 && !(i == start && ownsHeader))
    {
      const int end = blk->insts[i].loopEnd;
      if (end > last) { WASM_FAIL("loop %d..%d escapes its region", i, end); e->failed = 1; return; }
      eOp(e, W_LOOP); eOp(e, W_TYPE_VOID);
      pushLabel(e, 1, i);
      emitRange(e, i, end, 1);
      eOp(e, W_END);
      e->labelCount--;
      i = end + 1;
      continue;
    }
    emitInst(e, i);
    i++;
  }
  if (e->labelCount != baseLabels) { WASM_FAIL("unbalanced blocks in %d..%d", start, last); e->failed = 1; }
}

static void emitFunctionBody(WasmBuf *code, WasmProgram *prog, int blockIdx, int funcBase, int filterFunc)
{
  WasmBuf body = { 0 };
  WasmEmit e;
  WasmBlock *blk = &prog->blocks[blockIdx];
  const int isEntry = blockIdx == 0;
  memset(&e, 0, sizeof(e));
  e.b = &body;
  e.prog = prog;
  e.blk = blk;
  e.isEntry = isEntry;
  e.funcBase = funcBase;
  e.filterFunc = filterFunc;
  if (isEntry)
  {
    // params: wtp, rt, sp; locals: p1, p2, p3, t0, t1 (i32), d0 (f64), fp slots (f64)
    e.wtp = 0; e.rt = 1; e.sp = 2; e.p1 = 3; e.p2 = 4; e.p3 = 5; e.t0 = 6; e.t1 = 7; e.d0 = 8; e.fp0 = 9;
  }
  else
  {
    // params: p1, p2, p3, wtp, sp, rt; locals: t0, t1 (i32), d0 (f64), fp slots (f64)
    e.p1 = 0; e.p2 = 1; e.p3 = 2; e.wtp = 3; e.sp = 4; e.rt = 5; e.t0 = 6; e.t1 = 7; e.d0 = 8; e.fp0 = 9;
  }
  wbUleb(&body, 2);
  wbUleb(&body, isEntry ? 5 : 2); wbByte(&body, W_TYPE_I32);
  wbUleb(&body, 1 + blk->maxFp); wbByte(&body, W_TYPE_F64);

  // a call with sp == 0 only makes V8 compile the function; real calls always have a stack
  eGet(&e, e.sp); eOp(&e, W_I32_EQZ);
  eOp(&e, W_IF); eOp(&e, W_TYPE_VOID);
  if (!isEntry) { eI32(&e, 0); eI32(&e, 0); eI32(&e, 0); eI32(&e, 0); eF64(&e, 0.0); }
  eOp(&e, W_RETURN);
  eOp(&e, W_END);
  emitRange(&e, 0, blk->count - 1, 0);
  // the final RET returns; this keeps validation happy for the fallthrough
  if (!isEntry)
  {
    eI32(&e, 0); eI32(&e, 0); eI32(&e, 0); eI32(&e, 0); eF64(&e, 0.0);
  }
  eOp(&e, W_END);

  if (e.failed || body.failed) code->failed = 1;
  wbUleb(code, body.size);
  wbBytes(code, body.data, body.size);
  free(body.data);
  free(e.labels);
}

static void emitType(WasmBuf *b, int sig)
{
  static const unsigned char I = W_TYPE_I32, D = W_TYPE_F64;
  wbByte(b, 0x60);
  switch (sig)
  {
    case SIG_D_D: wbUleb(b, 1); wbByte(b, D); wbUleb(b, 1); wbByte(b, D); break;
    case SIG_D_DD: wbUleb(b, 2); wbByte(b, D); wbByte(b, D); wbUleb(b, 1); wbByte(b, D); break;
    case SIG_P_PP: wbUleb(b, 2); wbByte(b, I); wbByte(b, I); wbUleb(b, 1); wbByte(b, I); break;
    case SIG_P_PPP: wbUleb(b, 3); wbByte(b, I); wbByte(b, I); wbByte(b, I); wbUleb(b, 1); wbByte(b, I); break;
    case SIG_P_PPPP: wbUleb(b, 4); wbByte(b, I); wbByte(b, I); wbByte(b, I); wbByte(b, I); wbUleb(b, 1); wbByte(b, I); break;
    case SIG_D_PP: wbUleb(b, 2); wbByte(b, I); wbByte(b, I); wbUleb(b, 1); wbByte(b, D); break;
    case SIG_D_PPP: wbUleb(b, 3); wbByte(b, I); wbByte(b, I); wbByte(b, I); wbUleb(b, 1); wbByte(b, D); break;
    case SIG_D_PPPP: wbUleb(b, 4); wbByte(b, I); wbByte(b, I); wbByte(b, I); wbByte(b, I); wbUleb(b, 1); wbByte(b, D); break;
    case SIG_ENTRY: wbUleb(b, 3); wbByte(b, I); wbByte(b, I); wbByte(b, I); wbUleb(b, 0); break;
    case SIG_CALLEE:
      wbUleb(b, 6); wbByte(b, I); wbByte(b, I); wbByte(b, I); wbByte(b, I); wbByte(b, I); wbByte(b, I);
      wbUleb(b, 5); wbByte(b, I); wbByte(b, I); wbByte(b, I); wbByte(b, I); wbByte(b, D);
    break;
  }
}

static int buildModule(WasmProgram *prog, WasmBuf *out)
{
  static const unsigned char header[8] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
  WasmBuf sec = { 0 };
  const int filterFunc = prog->importCount, funcBase = prog->importCount + 1;
  int i;

  wbBytes(out, header, 8);

  wbUleb(&sec, SIG_COUNT);
  for (i = 0; i < SIG_COUNT; i++) emitType(&sec, i);
  wbSection(out, 1, &sec); sec.size = 0;

  wbUleb(&sec, prog->importCount + 1);
  for (i = 0; i < prog->importCount; i++)
  {
    char name[16];
    snprintf(name, sizeof(name), "%d", i);
    wbName(&sec, "f"); wbName(&sec, name);
    wbByte(&sec, 0x00); wbUleb(&sec, prog->imports[i].sig);
  }
  wbName(&sec, "env"); wbName(&sec, "memory");
  wbByte(&sec, 0x02); wbByte(&sec, 0x03); wbUleb(&sec, 1); wbUleb(&sec, 65536);
  wbSection(out, 2, &sec); sec.size = 0;

  wbUleb(&sec, 1 + prog->blockCount);
  wbUleb(&sec, SIG_D_D);
  for (i = 0; i < prog->blockCount; i++) wbUleb(&sec, i == 0 ? SIG_ENTRY : SIG_CALLEE);
  wbSection(out, 3, &sec); sec.size = 0;

  wbUleb(&sec, 1 + prog->blockCount);
  wbName(&sec, "entry"); wbByte(&sec, 0x00); wbUleb(&sec, funcBase);
  wbName(&sec, "filter"); wbByte(&sec, 0x00); wbUleb(&sec, filterFunc);
  for (i = 1; i < prog->blockCount; i++)
  {
    char name[16];
    snprintf(name, sizeof(name), "c%d", i);
    wbName(&sec, name); wbByte(&sec, 0x00); wbUleb(&sec, funcBase + i);
  }
  wbSection(out, 7, &sec); sec.size = 0;

  wbUleb(&sec, 1 + prog->blockCount);
  {
    // denormal_filter_double2: keeps x unless its exponent says denormal, zero, inf or nan
    WasmBuf f = { 0 };
    wbUleb(&f, 0);
    wbByte(&f, W_LOCAL_GET); wbUleb(&f, 0);
    wbByte(&f, W_F64_CONST); wbF64(&f, 0.0);
    wbByte(&f, W_LOCAL_GET); wbUleb(&f, 0);
    wbByte(&f, W_I64_REINTERPRET_F64);
    wbByte(&f, W_I64_CONST); wbSleb(&f, 32);
    wbByte(&f, W_I64_SHR_U);
    wbByte(&f, W_I32_WRAP_I64);
    wbByte(&f, W_I32_CONST); wbSleb(&f, 0x100000);
    wbByte(&f, W_I32_ADD);
    wbByte(&f, W_I32_CONST); wbSleb(&f, 0x7ff00000);
    wbByte(&f, W_I32_AND);
    wbByte(&f, W_I32_CONST); wbSleb(&f, 0x100000);
    wbByte(&f, W_I32_GT_U);
    wbByte(&f, W_SELECT);
    wbByte(&f, W_END);
    wbUleb(&sec, f.size);
    wbBytes(&sec, f.data, f.size);
    free(f.data);
  }
  for (i = 0; i < prog->blockCount; i++)
    emitFunctionBody(&sec, prog, i, funcBase, filterFunc);
  wbSection(out, 10, &sec);
  free(sec.data);
  return !out->failed && !sec.failed;
}

// ---- runtime -------------------------------------------------------------------------

typedef struct
{
  unsigned char *bytes;
  int size;
  INT_PTR *imports;
  int importCount;
  unsigned int id;
} NseelWasmCode;

typedef struct
{
  unsigned int id;
  int fn;
} NseelWasmThreadEntry;

typedef void (*NseelWasmEntry)(INT_PTR wtp, INT_PTR rt, INT_PTR sp);

static unsigned int nseel_wasm_next_id = 1;
static pthread_mutex_t nseel_wasm_freed_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned int *nseel_wasm_freed_ids;
static int nseel_wasm_freed_count, nseel_wasm_freed_cap;
static __thread NseelWasmThreadEntry *tl_entries;
static __thread int tl_count, tl_cap;
static __thread unsigned char *tl_stack;

EM_JS_DEPS(nseel_wasm, "$addFunction,$removeFunction");

EM_JS(void, nseel_wasm_js_release, (int fn), {
  removeFunction(fn);
});

// Drops this thread's instances of freed handles; skipped rather than waited for if the list is busy.
static void purgeFreedOnThisThread(void)
{
  int i, k, n = 0;
  if (pthread_mutex_trylock(&nseel_wasm_freed_mutex) != 0) return;
  for (i = 0; i < tl_count; i++)
  {
    int freed = 0;
    for (k = 0; k < nseel_wasm_freed_count && !freed; k++)
      freed = nseel_wasm_freed_ids[k] == tl_entries[i].id;
    if (freed) nseel_wasm_js_release(tl_entries[i].fn);
    else tl_entries[n++] = tl_entries[i];
  }
  tl_count = n;
  pthread_mutex_unlock(&nseel_wasm_freed_mutex);
}

EM_JS(int, nseel_wasm_js_validate, (const unsigned char *bytes, int size), {
  return WebAssembly.validate(HEAPU8.slice(bytes, bytes + size)) ? 1 : 0;
});

// Compiles and instantiates on the calling thread and returns the entry's index in this thread's table.
EM_JS(int, nseel_wasm_js_instantiate, (const unsigned char *bytes, int size, const int *imports, int importCount), {
  try {
    var mod = new WebAssembly.Module(HEAPU8.slice(bytes, bytes + size));
    var funcs = {};
    for (var i = 0; i < importCount; i++)
      funcs[String(i)] = wasmTable.get(HEAP32[(imports >> 2) + i]);
    var inst = null;
    // a callback whose C type differs from its EEL call site would trap in the interpreter when called; do the same
    for (var attempt = 0; !inst; attempt++) {
      try {
        inst = new WebAssembly.Instance(mod, { env: { memory: wasmMemory }, f: funcs });
      } catch (e) {
        var m = /Import #([0-9]+)/.exec(String(e));
        if (!(e instanceof WebAssembly.LinkError) || !m || attempt >= importCount) throw e;
        funcs[m[1]] = function() { throw new WebAssembly.RuntimeError('EEL2 callback signature mismatch'); };
      }
    }
    // V8 compiles wasm lazily on first call; do it now rather than on the audio path
    for (var name in inst.exports) {
      var fn = inst.exports[name];
      if (name === 'filter') fn(0.0);
      else if (name === 'entry') fn(0, 0, 0);
      else fn(0, 0, 0, 0, 0, 0);
    }
    out('[eel2-wasm] running ' + size + ' bytes of translated JSFX code on this thread');
    return addFunction(inst.exports.entry, 'viii');
  } catch (e) {
    err('[eel2-wasm] instantiate failed: ' + e);
    return 0;
  }
});

static void freeProgram(WasmProgram *p)
{
  int i;
  for (i = 0; i < p->blockCount; i++) free(p->blocks[i].insts);
  free(p->blocks);
  free(p->imports);
}

int NSEEL_code_wasm_generate(NSEEL_CODEHANDLE code)
{
  codeHandleType *h = (codeHandleType *) code;
  WasmProgram prog;
  WasmBuf out = { 0 };
  NseelWasmCode *w;
  int ok, i;
  if (!h || !h->code) return 0;
  if (__atomic_load_n(&h->wasm, __ATOMIC_ACQUIRE)) return 1;

  memset(&prog, 0, sizeof(prog));
  ok = findOrAddBlock(&prog, (const unsigned char *) h->code) == 0;
  // callees are appended while analyzing, so iterate by index
  for (i = 0; ok && i < prog.blockCount; i++)
    ok = analyzeBlock(&prog, i) && !prog.failed;
  if (ok) ok = buildModule(&prog, &out);
  if (ok) ok = nseel_wasm_js_validate(out.data, out.size) || WASM_FAIL("generated module does not validate");

  w = ok ? (NseelWasmCode *) calloc(1, sizeof(NseelWasmCode)) : NULL;
  if (w && prog.importCount > 0)
  {
    w->imports = (INT_PTR *) malloc(sizeof(INT_PTR) * prog.importCount);
    if (!w->imports) { free(w); w = NULL; }
  }
  if (w)
  {
    for (i = 0; i < prog.importCount; i++) w->imports[i] = prog.imports[i].fn;
    w->importCount = prog.importCount;
    w->bytes = out.data;
    w->size = out.size;
    w->id = __atomic_fetch_add(&nseel_wasm_next_id, 1, __ATOMIC_RELAXED);
    out.data = NULL;
    __atomic_store_n(&h->wasm, (void *) w, __ATOMIC_RELEASE);
  }
  free(out.data);
  freeProgram(&prog);
  return w != NULL;
}

int NSEEL_code_wasm_instantiate(NSEEL_CODEHANDLE code)
{
  codeHandleType *h = (codeHandleType *) code;
  NseelWasmCode *w = h ? (NseelWasmCode *) __atomic_load_n(&h->wasm, __ATOMIC_ACQUIRE) : NULL;
  int i, fn;
  if (!w) return 0;
  purgeFreedOnThisThread();
  for (i = 0; i < tl_count; i++)
    if (tl_entries[i].id == w->id) return 1;
  if (!tl_stack)
  {
    tl_stack = (unsigned char *) malloc(NSEEL_WASM_STACK_SIZE);
    if (!tl_stack) return 0;
  }
  if (tl_count == tl_cap)
  {
    int ncap = tl_cap ? tl_cap * 2 : 16;
    NseelWasmThreadEntry *n = (NseelWasmThreadEntry *) realloc(tl_entries, ncap * sizeof(NseelWasmThreadEntry));
    if (!n) return 0;
    tl_entries = n;
    tl_cap = ncap;
  }
  fn = nseel_wasm_js_instantiate(w->bytes, w->size, (const int *) w->imports, w->importCount);
  if (!fn)
  {
    return 0;
  }
  tl_entries[tl_count].id = w->id;
  tl_entries[tl_count].fn = fn;
  tl_count++;
  return 1;
}

int nseel_wasm_execute(codeHandleType *h)
{
  NseelWasmCode *w = (NseelWasmCode *) __atomic_load_n(&h->wasm, __ATOMIC_ACQUIRE);
  int i;
  if (!w) return 0;
  for (i = 0; i < tl_count; i++)
  {
    if (tl_entries[i].id == w->id)
    {
      NseelWasmEntry entry = (NseelWasmEntry) (INT_PTR) tl_entries[i].fn;
      entry((INT_PTR) h->workTable, (INT_PTR) h->ramPtr, (INT_PTR) (tl_stack + NSEEL_WASM_STACK_SIZE));
      return 1;
    }
  }
  return 0;
}

void nseel_wasm_free(codeHandleType *h)
{
  NseelWasmCode *w = (NseelWasmCode *) __atomic_exchange_n(&h->wasm, NULL, __ATOMIC_ACQ_REL);
  if (!w) return;
  // threads that instantiated it release their copies the next time they instantiate anything
  pthread_mutex_lock(&nseel_wasm_freed_mutex);
  if (nseel_wasm_freed_count == nseel_wasm_freed_cap)
  {
    const int ncap = nseel_wasm_freed_cap ? nseel_wasm_freed_cap * 2 : 64;
    unsigned int *n = (unsigned int *) realloc(nseel_wasm_freed_ids, ncap * sizeof(unsigned int));
    if (n) { nseel_wasm_freed_ids = n; nseel_wasm_freed_cap = ncap; }
  }
  if (nseel_wasm_freed_count < nseel_wasm_freed_cap) nseel_wasm_freed_ids[nseel_wasm_freed_count++] = w->id;
  pthread_mutex_unlock(&nseel_wasm_freed_mutex);
  free(w->bytes);
  free(w->imports);
  free(w);
}
