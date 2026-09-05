// fury: the leftovers the dynrec decoder handed to the normal core one
// instruction at a time (rdtsc, the debug-register moves), as helpers.
// Each returns nonzero when the instruction raised an exception, which
// the decoder turns into its exception path with dyn_check_exception.
static void fury_rdtsc(void) {
	int64_t tsc = CPU_RDTSC();
	reg_edx = (uint32_t)(tsc >> 32);
	reg_eax = (uint32_t)(tsc & 0xffffffff);
}

static Bitu fury_mov_rd_drx(Bitu rm) {          // 0F 21: MOV Rd,DRx
	Bitu which = (rm >> 3) & 7;
	uint32_t val = 0;
	if (CPU_READ_DRX(which, val)) return 1;
	reg_32(rm & 7) = val;
	return 0;
}

static Bitu fury_mov_drx_rd(Bitu rm) {          // 0F 23: MOV DRx,Rd
	Bitu which = (rm >> 3) & 7;
	if (CPU_WRITE_DRX(which, reg_32(rm & 7))) return 1;
	return 0;
}

static void dyn_fury_mov_drx(bool write) {
	dyn_get_modrm();
	Bitu modrm = (decode.modrm.mod << 6) | (decode.modrm.reg << 3) | decode.modrm.rm;
	gen_load_param_imm(modrm | 0xc0, 0);
	if (write) gen_call_function_setup(fury_mov_drx_rd, 1);
	else gen_call_function_setup(fury_mov_rd_drx, 1);
	dyn_check_exception(FC_RETOP);
}
