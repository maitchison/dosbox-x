/*
 *  fury: a WebAssembly backend for the dynrec core (2026-09).
 *
 *  Each translated block becomes one wasm function () -> i32 (the
 *  BlockReturnDynRec), built at run time and instantiated by JS with the
 *  main module's memory and table imported. The decoder's "host registers"
 *  are i32 locals; helper calls are call_indirect through the shared table
 *  with signatures captured from the C function types; the decoder's
 *  forward branches are structured if/end pairs; exception handlers hang
 *  off one outer block selected by a branch index; a jump to another block
 *  is a tail call to that block's table index, which is what cache.xstart
 *  holds here. The cache buffer receives only instruction bytes; the
 *  module around them is assembled when the block closes.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include <stdint.h>
#include <stdio.h>
#include <vector>
#include <type_traits>

// capabilities: no flags-invalidation call patching yet (the simpler
// helpers have the same wasm signatures, so this can come later), no
// direct regs/segs addressing (the generic memory forms are used)
#define DRC_PTR_SIZE_IM uint32_t
#define DRC_CALL_CONV /* nothing */
#define DRC_FC /* nothing */

typedef uint8_t HostReg;

// the locals of a block function; all i32
enum {
	FC_RETOP = 0,     // return values of helpers, and the block's return code
	FC_ADDR = 1,
	FC_OP1 = 2,
	FC_OP2 = 3,
	FC_OP3 = 4,
	FC_TMP_BA1 = 5,
	FC_TMP_BA2 = 6,
	WL_P2 = 7,        // helper parameters 2 and 3 (0 and 1 are FC_OP1/FC_OP2,
	WL_P3 = 8,        // as in every backend: raw calls rely on it)
	WL_BRIDX = 9,     // which exception handler a long branch selected
	WL_TMP = 10,      // the decoder's scratch register
	WL_COUNT = 11
};
#define TEMP_REG_DRC WL_TMP

static INLINE HostReg wasm_param_local(Bitu param) {
	switch (param) {
		case 0: return FC_OP1;
		case 1: return FC_OP2;
		case 2: return WL_P2;
		default: return WL_P3;
	}
}

// wasm opcodes used here
enum {
	W_UNREACHABLE = 0x00, W_BLOCK = 0x02, W_IF = 0x04, W_END = 0x0b,
	W_BR = 0x0c, W_BR_IF = 0x0d, W_RETURN = 0x0f,
	W_CALL_INDIRECT = 0x11, W_RETURN_CALL_INDIRECT = 0x13, W_DROP = 0x1a,
	W_LOCAL_GET = 0x20, W_LOCAL_SET = 0x21, W_LOCAL_TEE = 0x22,
	W_I32_LOAD = 0x28, W_I32_LOAD8_S = 0x2c, W_I32_LOAD8_U = 0x2d,
	W_I32_LOAD16_S = 0x2e, W_I32_LOAD16_U = 0x2f,
	W_I32_STORE = 0x36, W_I32_STORE8 = 0x3a, W_I32_STORE16 = 0x3b,
	W_I32_CONST = 0x41,
	W_I32_EQZ = 0x45, W_I32_EQ = 0x46, W_I32_LE_S = 0x4c,
	W_I32_ADD = 0x6a, W_I32_SUB = 0x6b, W_I32_AND = 0x71, W_I32_OR = 0x72,
	W_I32_SHL = 0x74, W_I32_SHR_S = 0x75, W_I32_SHR_U = 0x76,
	W_I32_EXTEND8_S = 0xc0, W_I32_EXTEND16_S = 0xc1,
	W_BLOCKTYPE_VOID = 0x40
};

// ---- per-block state -------------------------------------------------

struct WasmSig { uint8_t nparams; bool ret; };
static std::vector<WasmSig> wasm_sigs;   // signatures this block calls; type index = 1 + position
static int wasm_open_ifs = 0;            // structured ifs currently open in the body
static int wasm_long_branches = 0;       // long branches created (handler ids)
static bool wasm_h_closed = false;       // the outer handler block has been ended
static bool wasm_handler_open = false;   // an exception handler's if is open
static CacheBlockDynRec* wasm_closing_block = nullptr;
static uint8_t* wasm_body_end = nullptr;
static uint32_t wasm_blocks_made = 0;

static void wasm_reset_block_state(void) {
	wasm_sigs.clear();
	wasm_open_ifs = 0;
	wasm_long_branches = 0;
	wasm_h_closed = false;
	wasm_handler_open = false;
}

// ---- encoding ----------------------------------------------------------

static INLINE void wasm_uleb(uint32_t v) {
	do { uint8_t c = v & 0x7f; v >>= 7; if (v) c |= 0x80; cache_addb(c); } while (v);
}

static INLINE void wasm_sleb(int32_t v) {
	bool more = true;
	while (more) {
		uint8_t c = v & 0x7f;
		v >>= 7;
		if ((v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40))) more = false; else c |= 0x80;
		cache_addb(c);
	}
}

// a 5-byte i32.const whose immediate can be patched later
static INLINE void wasm_i32_const_fixed(uint32_t v, uint8_t* pos) {
	pos[0] = 0x80 | (v & 0x7f);
	pos[1] = 0x80 | ((v >> 7) & 0x7f);
	pos[2] = 0x80 | ((v >> 14) & 0x7f);
	pos[3] = 0x80 | ((v >> 21) & 0x7f);
	pos[4] = ((v >> 28) & 0x0f) | ((v & 0x80000000u) ? 0x70 : 0x00);
}

static INLINE void wasm_i32_const(uint32_t v) { cache_addb(W_I32_CONST); wasm_sleb((int32_t)v); }
static INLINE void wasm_get(HostReg r) { cache_addb(W_LOCAL_GET); wasm_uleb(r); }
static INLINE void wasm_set(HostReg r) { cache_addb(W_LOCAL_SET); wasm_uleb(r); }
static INLINE void wasm_tee(HostReg r) { cache_addb(W_LOCAL_TEE); wasm_uleb(r); }
static INLINE void wasm_memarg(uint8_t align) { wasm_uleb(align); wasm_uleb(0); }
static INLINE void wasm_load32(void) { cache_addb(W_I32_LOAD); wasm_memarg(2); }
static INLINE void wasm_load16u(void) { cache_addb(W_I32_LOAD16_U); wasm_memarg(1); }
static INLINE void wasm_load8u(void) { cache_addb(W_I32_LOAD8_U); wasm_memarg(0); }
static INLINE void wasm_store32(void) { cache_addb(W_I32_STORE); wasm_memarg(2); }
static INLINE void wasm_store16(void) { cache_addb(W_I32_STORE16); wasm_memarg(1); }
static INLINE void wasm_store8(void) { cache_addb(W_I32_STORE8); wasm_memarg(0); }
static INLINE void wasm_addr(const void* p) { wasm_i32_const((uint32_t)(uintptr_t)p); }

static uint32_t wasm_sig_index(uint8_t nparams, bool ret) {
	for (size_t i = 0; i < wasm_sigs.size(); i++)
		if (wasm_sigs[i].nparams == nparams && wasm_sigs[i].ret == ret) return (uint32_t)(i + 1);
	wasm_sigs.push_back(WasmSig{nparams, ret});
	return (uint32_t)wasm_sigs.size();
}

// the wasm signature of a C helper, from its type
template <typename T> struct WasmFnSig;
template <typename R, typename... A> struct WasmFnSig<R (*)(A...)> {
	static const uint8_t nparams = (uint8_t)sizeof...(A);
	static const bool ret = !std::is_void<R>::value;
	static_assert(sizeof...(A) <= 4, "dynrec helper with more than 4 parameters");
};
template <typename R, typename... A> struct WasmFnSig<R (&)(A...)> : WasmFnSig<R (*)(A...)> {};
template <typename R, typename... A> struct WasmFnSig<R (A...)> : WasmFnSig<R (*)(A...)> {};

// ---- registers ---------------------------------------------------------

// move a full register from reg_src to reg_dst
static void gen_mov_regs(HostReg reg_dst, HostReg reg_src) {
	if (reg_dst == reg_src) return;
	wasm_get(reg_src); wasm_set(reg_dst);
}

// move a 32bit constant value into dest_reg
static void gen_mov_dword_to_reg_imm(HostReg dest_reg, uint32_t imm) {
	wasm_i32_const(imm); wasm_set(dest_reg);
}

// move a 32bit (dword==true) or 16bit (dword==false) value from memory into dest_reg
// 16bit moves may destroy the upper 16bit of the destination register
static void gen_mov_word_to_reg(HostReg dest_reg, void* data, bool dword) {
	wasm_addr(data);
	if (dword) wasm_load32(); else wasm_load16u();
	wasm_set(dest_reg);
}

// move a 16bit constant value into dest_reg
// the upper 16bit of the destination register may be destroyed
static void INLINE gen_mov_word_to_reg_imm(HostReg dest_reg, uint16_t imm) {
	wasm_i32_const(imm); wasm_set(dest_reg);
}

// move 32bit (dword==true) or 16bit (dword==false) of a register into memory
static void gen_mov_word_from_reg(HostReg src_reg, void* dest, bool dword) {
	wasm_addr(dest); wasm_get(src_reg);
	if (dword) wasm_store32(); else wasm_store16();
}

// move an 8bit value from memory into dest_reg
// the upper 24bit of the destination register can be destroyed
// this function does not use FC_OP1/FC_OP2 as dest_reg as these
// registers might not be directly byte-accessible on some architectures
static void gen_mov_byte_to_reg_low(HostReg dest_reg, void* data) {
	wasm_addr(data); wasm_load8u(); wasm_set(dest_reg);
}

// move an 8bit value from memory into dest_reg
// the upper 24bit of the destination register can be destroyed
// this function can use FC_OP1/FC_OP2 as dest_reg which are
// not directly byte-accessible on some architectures
static void INLINE gen_mov_byte_to_reg_low_canuseword(HostReg dest_reg, void* data) {
	gen_mov_byte_to_reg_low(dest_reg, data);
}

// move an 8bit constant value into dest_reg
// the upper 24bit of the destination register can be destroyed
static void gen_mov_byte_to_reg_low_imm(HostReg dest_reg, uint8_t imm) {
	wasm_i32_const(imm); wasm_set(dest_reg);
}

static void INLINE gen_mov_byte_to_reg_low_imm_canuseword(HostReg dest_reg, uint8_t imm) {
	gen_mov_byte_to_reg_low_imm(dest_reg, imm);
}

// move the lowest 8bit of a register into memory
static void gen_mov_byte_from_reg_low(HostReg src_reg, void* dest) {
	wasm_addr(dest); wasm_get(src_reg); wasm_store8();
}

// convert an 8bit word to a 32bit dword
// the register is zero-extended (sign==false) or sign-extended (sign==true)
static void gen_extend_byte(bool sign, HostReg reg) {
	wasm_get(reg);
	if (sign) cache_addb(W_I32_EXTEND8_S);
	else { wasm_i32_const(0xff); cache_addb(W_I32_AND); }
	wasm_set(reg);
}

// convert a 16bit word to a 32bit dword
static void gen_extend_word(bool sign, HostReg reg) {
	wasm_get(reg);
	if (sign) cache_addb(W_I32_EXTEND16_S);
	else { wasm_i32_const(0xffff); cache_addb(W_I32_AND); }
	wasm_set(reg);
}

// add a 32bit value from memory to a full register
static void gen_add(HostReg reg, void* op) {
	wasm_get(reg); wasm_addr(op); wasm_load32(); cache_addb(W_I32_ADD); wasm_set(reg);
}

// add a 32bit constant value to a full register
static void gen_add_imm(HostReg reg, uint32_t imm) {
	if (!imm) return;
	wasm_get(reg); wasm_i32_const(imm); cache_addb(W_I32_ADD); wasm_set(reg);
}

// and a 32bit constant value with a full register
static void gen_and_imm(HostReg reg, uint32_t imm) {
	wasm_get(reg); wasm_i32_const(imm); cache_addb(W_I32_AND); wasm_set(reg);
}

// shift right a register by an 8-bit constant
static void gen_shr_imm(HostReg reg, uint8_t imm) {
	if (!imm) return;
	wasm_get(reg); wasm_i32_const(imm); cache_addb(W_I32_SHR_U); wasm_set(reg);
}

// move a 32bit constant value into memory
static void gen_mov_direct_dword(void* dest, uint32_t imm) {
	wasm_addr(dest); wasm_i32_const(imm); wasm_store32();
}

// move an address into memory
static void INLINE gen_mov_direct_ptr(void* dest, DRC_PTR_SIZE_IM imm) {
	gen_mov_direct_dword(dest, (uint32_t)imm);
}

// add a 32bit (dword==true) or 16bit (dword==false) constant value to a memory value
static void gen_add_direct_word(void* dest, uint32_t imm, bool dword) {
	if (!imm) return;
	wasm_addr(dest); wasm_addr(dest);
	if (dword) wasm_load32(); else wasm_load16u();
	wasm_i32_const(imm); cache_addb(W_I32_ADD);
	if (dword) wasm_store32(); else wasm_store16();
}

// add an 8bit constant value to a dword memory value
static void gen_add_direct_byte(void* dest, int8_t imm) {
	gen_add_direct_word(dest, (uint32_t)(int32_t)imm, true);
}

// subtract a 32bit (dword==true) or 16bit (dword==false) constant value from a memory value
static void gen_sub_direct_word(void* dest, uint32_t imm, bool dword) {
	if (!imm) return;
	wasm_addr(dest); wasm_addr(dest);
	if (dword) wasm_load32(); else wasm_load16u();
	wasm_i32_const(imm); cache_addb(W_I32_SUB);
	if (dword) wasm_store32(); else wasm_store16();
}

// subtract an 8bit constant value from a dword memory value
static void gen_sub_direct_byte(void* dest, int8_t imm) {
	gen_sub_direct_word(dest, (uint32_t)(int32_t)imm, true);
}

// effective address calculation, destination is dest_reg
// scale_reg is scaled by scale (scale_reg*(2^scale)) and
// added to dest_reg, then the immediate value is added
static INLINE void gen_lea(HostReg dest_reg, HostReg scale_reg, Bitu scale, Bits imm) {
	wasm_get(dest_reg);
	wasm_get(scale_reg);
	if (scale) { wasm_i32_const((uint32_t)scale); cache_addb(W_I32_SHL); }
	cache_addb(W_I32_ADD);
	if (imm) { wasm_i32_const((uint32_t)imm); cache_addb(W_I32_ADD); }
	wasm_set(dest_reg);
}

// effective address calculation, destination is dest_reg
// dest_reg is scaled by scale (dest_reg*(2^scale)),
// then the immediate value is added
static INLINE void gen_lea(HostReg dest_reg, Bitu scale, Bits imm) {
	if (!scale && !imm) return;
	wasm_get(dest_reg);
	if (scale) { wasm_i32_const((uint32_t)scale); cache_addb(W_I32_SHL); }
	if (imm) { wasm_i32_const((uint32_t)imm); cache_addb(W_I32_ADD); }
	wasm_set(dest_reg);
}

// ---- calls -------------------------------------------------------------

// the call itself: params already in WL_P0..; returns the position of the
// patchable function index immediate
template <typename T> static DRC_PTR_SIZE_IM wasm_emit_call(const T func, Bitu paramcount) {
	typedef WasmFnSig<T> S;
	// the table entry's real signature is what call_indirect checks, so
	// pass exactly the helper's parameters, whatever the decoder loaded
	if (paramcount > S::nparams) E_Exit("wasm dynrec: %u params loaded for a %u-param helper",
		(unsigned)paramcount, (unsigned)S::nparams);
	for (Bitu i = 0; i < S::nparams; i++) wasm_get(wasm_param_local(i));
	cache_addb(W_I32_CONST);
	DRC_PTR_SIZE_IM pos = (DRC_PTR_SIZE_IM)(uintptr_t)cache.pos;
	wasm_i32_const_fixed((uint32_t)(uintptr_t)func, cache.pos);
	cache.pos += 5;
	cache_addb(W_CALL_INDIRECT);
	wasm_uleb(wasm_sig_index(S::nparams, S::ret));
	wasm_uleb(0);   // table 0
	if (S::ret) wasm_set(FC_RETOP);
	return pos;
}

// generate a call to a parameterless function
template <typename T> static void INLINE gen_call_function_raw(const T func) {
	wasm_emit_call(func, 0);
}

// generate a call to a function with paramcount parameters
// note: the parameters are loaded in the architecture specific way
// using the gen_load_param_ functions below
template <typename T> static DRC_PTR_SIZE_IM INLINE gen_call_function_setup(const T func, Bitu paramcount, bool fastcall = false) {
	(void)fastcall;
	return wasm_emit_call(func, paramcount);
}

// load an immediate value as param'th function parameter
static void INLINE gen_load_param_imm(Bitu imm, Bitu param) {
	wasm_i32_const((uint32_t)imm); wasm_set(wasm_param_local(param));
}

// load an address as param'th function parameter
static void INLINE gen_load_param_addr(DRC_PTR_SIZE_IM addr, Bitu param) {
	wasm_i32_const((uint32_t)addr); wasm_set(wasm_param_local(param));
}

// load a host-register as param'th function parameter
static void INLINE gen_load_param_reg(Bitu reg, Bitu param) {
	gen_mov_regs(wasm_param_local(param), (HostReg)reg);
}

// load a value from memory as param'th function parameter
static void INLINE gen_load_param_mem(Bitu mem, Bitu param) {
	wasm_i32_const((uint32_t)mem); wasm_load32(); wasm_set(wasm_param_local(param));
}

// ---- control flow ------------------------------------------------------

// jump to an address pointed at by ptr, offset is in imm: here the table
// index of the next block, entered by a tail call
static void gen_jmp_ptr(void* ptr, Bits imm = 0) {
	wasm_addr(ptr);
	wasm_load32();                                    // the target block
	cache_addb(W_I32_LOAD); wasm_uleb(2); wasm_uleb((uint32_t)imm);   // its xstart: a table index
	cache_addb(W_RETURN_CALL_INDIRECT);
	wasm_uleb(0);   // type 0: () -> i32
	wasm_uleb(0);   // table 0
}

// short forward branch over the code up to the fill: the code between
// runs only when the condition does NOT branch
static DRC_PTR_SIZE_IM gen_create_branch_on_zero(HostReg reg, bool dword) {
	wasm_get(reg);
	if (!dword) { wasm_i32_const(0xffff); cache_addb(W_I32_AND); }
	cache_addb(W_IF); cache_addb(W_BLOCKTYPE_VOID);
	wasm_open_ifs++;
	return 0;
}

static DRC_PTR_SIZE_IM gen_create_branch_on_nonzero(HostReg reg, bool dword) {
	wasm_get(reg);
	if (!dword) { wasm_i32_const(0xffff); cache_addb(W_I32_AND); }
	cache_addb(W_I32_EQZ);
	cache_addb(W_IF); cache_addb(W_BLOCKTYPE_VOID);
	wasm_open_ifs++;
	return 0;
}

// calculate relative offset and fill it into the location pointed to by data
static void INLINE gen_fill_branch(DRC_PTR_SIZE_IM data) {
	(void)data;
	if (wasm_open_ifs <= 0) E_Exit("wasm dynrec: fill_branch with no open if");
	cache_addb(W_END);
	wasm_open_ifs--;
}

// a long branch to an exception handler emitted at the end of the block:
// remember which one in WL_BRIDX and leave the body through the outer block
static DRC_PTR_SIZE_IM wasm_long_branch(void) {
	cache_addb(W_IF); cache_addb(W_BLOCKTYPE_VOID);
	int id = wasm_long_branches++;
	wasm_i32_const((uint32_t)id); wasm_set(WL_BRIDX);
	cache_addb(W_BR); wasm_uleb((uint32_t)(wasm_open_ifs + 1));
	cache_addb(W_END);
	return (DRC_PTR_SIZE_IM)id;
}

// compare 32bit-register against zero and jump if value less/equal than zero
static DRC_PTR_SIZE_IM gen_create_branch_long_leqzero(HostReg reg) {
	wasm_get(reg); wasm_i32_const(0); cache_addb(W_I32_LE_S);
	return wasm_long_branch();
}

// for isdword==false the lowest 8bit of the register are tested
static DRC_PTR_SIZE_IM gen_create_branch_long_nonzero(HostReg reg, bool isdword) {
	wasm_get(reg);
	if (!isdword) { wasm_i32_const(0xff); cache_addb(W_I32_AND); }
	return wasm_long_branch();
}

// the handler for long branch 'data' starts here
static void INLINE gen_fill_branch_long(DRC_PTR_SIZE_IM data) {
	if (wasm_open_ifs) E_Exit("wasm dynrec: handler while an if is open");
	if (!wasm_h_closed) { cache_addb(W_END); wasm_h_closed = true; }
	if (wasm_handler_open) cache_addb(W_END);
	wasm_get(WL_BRIDX); wasm_i32_const((uint32_t)data); cache_addb(W_I32_EQ);
	cache_addb(W_IF); cache_addb(W_BLOCKTYPE_VOID);
	wasm_handler_open = true;
}

// nothing to generate: blocks are entered through fury_wasm_runcode
static void gen_run_code(void) { }

// return from a function: the return code sits in FC_RETOP
static void gen_return_function(void) {
	wasm_get(FC_RETOP); cache_addb(W_RETURN);
}

// ---- block assembly ----------------------------------------------------

extern "C" int fury_jit_instantiate(const uint8_t* bytes, int len);
extern "C" void fury_jit_last_error(char* buf, int len);

namespace {
struct WasmBuf {
	std::vector<uint8_t> b;
	void u8(uint8_t v) { b.push_back(v); }
	void uleb(uint32_t v) { do { uint8_t c = v & 0x7f; v >>= 7; if (v) c |= 0x80; b.push_back(c); } while (v); }
	void str(const char* s) { uint32_t n = 0; while (s[n]) n++; uleb(n); for (uint32_t i = 0; i < n; i++) u8((uint8_t)s[i]); }
	void bytes(const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); }
	void section(uint8_t id, const WasmBuf& body) { u8(id); uleb((uint32_t)body.b.size()); bytes(body.b.data(), body.b.size()); }
};
}

// wrap the instruction bytes [start,end) into a module and instantiate it;
// returns the table index of the block function
static uint32_t wasm_finalize(const uint8_t* start, const uint8_t* end) {
	WasmBuf m;
	m.u8(0); m.u8('a'); m.u8('s'); m.u8('m'); m.u8(1); m.u8(0); m.u8(0); m.u8(0);
	WasmBuf t;
	t.uleb((uint32_t)(1 + wasm_sigs.size()));
	t.u8(0x60); t.uleb(0); t.uleb(1); t.u8(0x7f);              // type 0: () -> i32
	for (size_t i = 0; i < wasm_sigs.size(); i++) {
		t.u8(0x60); t.uleb(wasm_sigs[i].nparams);
		for (uint8_t p = 0; p < wasm_sigs[i].nparams; p++) t.u8(0x7f);
		if (wasm_sigs[i].ret) { t.uleb(1); t.u8(0x7f); } else t.uleb(0);
	}
	m.section(1, t);
	WasmBuf im;
	im.uleb(2);
	im.str("env"); im.str("memory"); im.u8(0x02); im.u8(0x00); im.uleb(0);
	im.str("env"); im.str("table");  im.u8(0x01); im.u8(0x70); im.u8(0x00); im.uleb(0);
	m.section(2, im);
	WasmBuf f; f.uleb(1); f.uleb(0); m.section(3, f);
	WasmBuf e; e.uleb(1); e.str("f"); e.u8(0x00); e.uleb(0); m.section(7, e);
	WasmBuf body;
	body.uleb(1); body.uleb(WL_COUNT); body.u8(0x7f);           // locals: WL_COUNT x i32
	body.u8(W_BLOCK); body.u8(W_BLOCKTYPE_VOID);                // the outer handler block
	body.bytes(start, (size_t)(end - start));
	if (!wasm_h_closed) body.u8(W_END);
	if (wasm_handler_open) body.u8(W_END);
	body.u8(W_UNREACHABLE);
	body.u8(W_END);
	WasmBuf c; c.uleb(1); c.uleb((uint32_t)body.b.size()); c.bytes(body.b.data(), body.b.size());
	m.section(10, c);
	int idx = fury_jit_instantiate(m.b.data(), (int)m.b.size());
	if (!idx) {
		char msg[512];
		fury_jit_last_error(msg, sizeof(msg));
		FILE* f = fopen("jitfail.bin", "wb");
		if (f) { fwrite(m.b.data(), 1, m.b.size(), f); fclose(f); }
		E_Exit("wasm dynrec: instantiate failed (%u bytes, %u sigs, block %u): %s",
			(unsigned)m.b.size(), (unsigned)wasm_sigs.size(), (unsigned)wasm_blocks_made, msg);
	}
	// the trail for the first blocks: their modules, and where they went
	if (wasm_blocks_made < 6) {
		char name[32];
		snprintf(name, sizeof(name), "jit%u.bin", (unsigned)wasm_blocks_made);
		FILE* f = fopen(name, "wb");
		if (f) { fwrite(m.b.data(), 1, m.b.size(), f); fclose(f); }
	}
	{
		FILE* f = fopen("jit.txt", wasm_blocks_made ? "a" : "w");
		if (f) {
			fprintf(f, "block %u: table %d, %u bytes, %u sigs, %d long branches, body %u bytes\n",
				(unsigned)wasm_blocks_made, idx, (unsigned)m.b.size(), (unsigned)wasm_sigs.size(),
				wasm_long_branches, (unsigned)(end - start));
			fclose(f);
		}
	}
	wasm_blocks_made++;
	return (uint32_t)idx;
}

// called just before cache_closeblock: the body ends here
static void cache_block_before_close(void) {
	if (wasm_open_ifs) E_Exit("wasm dynrec: block closing with %d ifs open", wasm_open_ifs);
	wasm_closing_block = cache.block.active;
	wasm_body_end = cache.pos;
}

// called after cache_closeblock: build the module, and make xstart the
// table index that runcode and block links use
static void cache_block_closing(uint8_t* block_start, Bitu block_size) {
	(void)block_size;
	uint32_t idx = wasm_finalize(block_start, wasm_body_end);
	wasm_closing_block->cache.xstart = (uint8_t*)(uintptr_t)idx;
	wasm_reset_block_state();
}

// finalize a block generated outside the decoder (the link blocks)
static uint32_t wasm_finalize_raw(uint8_t* start, uint8_t* end) {
	uint32_t idx = wasm_finalize(start, end);
	wasm_reset_block_state();
	return idx;
}

// enter a block: xstart is a table index
static BlockReturnDynRec fury_wasm_runcode(uint8_t* xstart) {
	typedef BlockReturnDynRec (*block_fn)(void);
	return ((block_fn)(uintptr_t)xstart)();
}
