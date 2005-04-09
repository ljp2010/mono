/*
 * mini-codegen.c: Arch independent code generation functionality
 *
 * (C) 2003 Ximian, Inc.
 */

#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/mman.h>

#include <mono/metadata/appdomain.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/threads.h>
#include <mono/metadata/profiler-private.h>
#include <mono/utils/mono-math.h>

#include "mini.h"
#include "trace.h"
#include "inssel.h"
#include "mini-arch.h"

#define DEBUG(a) if (cfg->verbose_level > 1) a

const char * const amd64_desc [OP_LAST];
static const char*const * ins_spec = amd64_desc;

#define use_fpstack MONO_ARCH_USE_FPSTACK

/*
 * returns the offset used by spillvar. It allocates a new
 * spill variable if necessary. 
 */
static int
mono_spillvar_offset (MonoCompile *cfg, int spillvar)
{
	MonoSpillInfo **si, *info;
	int i = 0;

	si = &cfg->spill_info; 
	
	while (i <= spillvar) {

		if (!*si) {
			*si = info = mono_mempool_alloc (cfg->mempool, sizeof (MonoSpillInfo));
			info->next = NULL;
			cfg->stack_offset += sizeof (gpointer);
			info->offset = - cfg->stack_offset;
		}

		if (i == spillvar)
			return (*si)->offset;

		i++;
		si = &(*si)->next;
	}

	g_assert_not_reached ();
	return 0;
}

/*
 * returns the offset used by spillvar. It allocates a new
 * spill float variable if necessary. 
 * (same as mono_spillvar_offset but for float)
 */
static int
mono_spillvar_offset_float (MonoCompile *cfg, int spillvar)
{
	MonoSpillInfo **si, *info;
	int i = 0;

	si = &cfg->spill_info_float; 
	
	while (i <= spillvar) {

		if (!*si) {
			*si = info = mono_mempool_alloc (cfg->mempool, sizeof (MonoSpillInfo));
			info->next = NULL;
			cfg->stack_offset += sizeof (double);
			info->offset = - cfg->stack_offset;
		}

		if (i == spillvar)
			return (*si)->offset;

		i++;
		si = &(*si)->next;
	}

	g_assert_not_reached ();
	return 0;
}

/*
 * Creates a store for spilled floating point items
 */
static MonoInst*
create_spilled_store_float (MonoCompile *cfg, int spill, int reg, MonoInst *ins)
{
	MonoInst *store;
	MONO_INST_NEW (cfg, store, OP_STORER8_MEMBASE_REG);
	store->sreg1 = reg;
	store->inst_destbasereg = MONO_ARCH_BASEREG;
	store->inst_offset = mono_spillvar_offset_float (cfg, spill);

	DEBUG (g_print ("SPILLED FLOAT STORE (%d at 0x%08lx(%%sp)) (from %d)\n", spill, (long)store->inst_offset, reg));
	return store;
}

/*
 * Creates a load for spilled floating point items 
 */
static MonoInst*
create_spilled_load_float (MonoCompile *cfg, int spill, int reg, MonoInst *ins)
{
	MonoInst *load;
	MONO_INST_NEW (cfg, load, OP_LOADR8_SPILL_MEMBASE);
	load->dreg = reg;
	load->inst_basereg = MONO_ARCH_BASEREG;
	load->inst_offset = mono_spillvar_offset_float (cfg, spill);

	DEBUG (g_print ("SPILLED FLOAT LOAD (%d at 0x%08lx(%%sp)) (from %d)\n", spill, (long)load->inst_offset, reg));
	return load;
}

#define is_hard_ireg(r) ((r) >= 0 && (r) < MONO_MAX_IREGS)
#define is_hard_freg(r) ((r) >= 0 && (r) < MONO_MAX_FREGS)
#define is_global_ireg(r) (is_hard_ireg ((r)) && (MONO_ARCH_CALLEE_SAVED_REGS & (1 << (r))))
#define is_local_ireg(r) (is_hard_ireg ((r)) && (MONO_ARCH_CALLEE_REGS & (1 << (r))))
#define is_global_freg(r) (is_hard_freg ((r)) && (MONO_ARCH_CALLEE_SAVED_FREGS & (1 << (r))))
#define is_local_freg(r) (is_hard_ireg ((r)) && (MONO_ARCH_CALLEE_FREGS & (1 << (r))))
#define ireg_is_freeable(r) is_local_ireg ((r))
#define freg_is_freeable(r) is_hard_freg ((r))

#define reg_is_freeable(r,fp) ((fp) ? freg_is_freeable ((r)) : ireg_is_freeable ((r)))
#define is_hard_reg(r,fp) ((fp) ? ((r) < MONO_MAX_FREGS) : ((r) < MONO_MAX_IREGS))
#define is_soft_reg(r,fp) (!is_hard_reg((r),(fp)))
#define rassign(cfg,reg,fp) ((fp) ? (cfg)->rs->fassign [(reg)] : (cfg)->rs->iassign [(reg)])
#define sreg1_is_fp(ins) (ins_spec [(ins)->opcode] [MONO_INST_SRC1] == 'f')
#define sreg2_is_fp(ins) (ins_spec [(ins)->opcode] [MONO_INST_SRC2] == 'f')
#define dreg_is_fp(ins)  (ins_spec [(ins)->opcode] [MONO_INST_DEST] == 'f')

typedef struct {
	int born_in;
	int killed_in;
	int last_use;
	int prev_use;
	int flags;		/* used to track fp spill/load */
} RegTrack;

static void
print_ins (int i, MonoInst *ins)
{
	const char *spec = ins_spec [ins->opcode];
	g_print ("\t%-2d %s", i, mono_inst_name (ins->opcode));
	if (!spec)
		g_error ("Unknown opcode: %s\n", mono_inst_name (ins->opcode));
	if (spec [MONO_INST_DEST]) {
		gboolean fp = (spec [MONO_INST_DEST] == 'f');
		if (is_soft_reg (ins->dreg, fp))
			g_print (" R%d <-", ins->dreg);
		else
			g_print (" %s <-", mono_arch_regname_full (ins->dreg, fp));
	}
	if (spec [MONO_INST_SRC1]) {
		gboolean fp = (spec [MONO_INST_SRC1] == 'f');
		if (is_soft_reg (ins->sreg1, fp))
			g_print (" R%d", ins->sreg1);
		else
			g_print (" %s", mono_arch_regname_full (ins->sreg1, fp));
	}
	if (spec [MONO_INST_SRC2]) {
		gboolean fp = (spec [MONO_INST_SRC2] == 'f');
		if (is_soft_reg (ins->sreg2, fp))
			g_print (" R%d", ins->sreg2);
		else
			g_print (" %s", mono_arch_regname_full (ins->sreg2, fp));
	}
	if (spec [MONO_INST_CLOB])
		g_print (" clobbers: %c", spec [MONO_INST_CLOB]);
	g_print ("\n");
}

static void
print_regtrack (RegTrack *t, int num)
{
	int i;
	char buf [32];
	const char *r;
	
	for (i = 0; i < num; ++i) {
		if (!t [i].born_in)
			continue;
		if (i >= MONO_MAX_IREGS) {
			g_snprintf (buf, sizeof(buf), "R%d", i);
			r = buf;
		} else
			r = mono_arch_regname (i);
		g_print ("liveness: %s [%d - %d]\n", r, t [i].born_in, t[i].last_use);
	}
}

typedef struct InstList InstList;

struct InstList {
	InstList *prev;
	InstList *next;
	MonoInst *data;
};

static inline InstList*
inst_list_prepend (MonoMemPool *pool, InstList *list, MonoInst *data)
{
	InstList *item = mono_mempool_alloc (pool, sizeof (InstList));
	item->data = data;
	item->prev = NULL;
	item->next = list;
	if (list)
		list->prev = item;
	return item;
}

/*
 * Force the spilling of the variable in the symbolic register 'reg'.
 */
static int
get_register_force_spilling (MonoCompile *cfg, InstList *item, MonoInst *ins, int reg, gboolean fp)
{
	MonoInst *load;
	int i, sel, spill;
	int *assign, *symbolic;

	if (fp) {
		assign = cfg->rs->fassign;
		symbolic = cfg->rs->fsymbolic;
	}
	else {
		assign = cfg->rs->iassign;
		symbolic = cfg->rs->isymbolic;
	}	
	
	sel = assign [reg];
	/*i = cfg->rs->isymbolic [sel];
	g_assert (i == reg);*/
	i = reg;
	spill = ++cfg->spill_count;
	assign [i] = -spill - 1;
	if (fp)
		mono_regstate_free_float (cfg->rs, sel);
	else
		mono_regstate_free_int (cfg->rs, sel);
	/* we need to create a spill var and insert a load to sel after the current instruction */
	if (fp)
		MONO_INST_NEW (cfg, load, OP_LOADR8_MEMBASE);
	else
		MONO_INST_NEW (cfg, load, OP_LOAD_MEMBASE);
	load->dreg = sel;
	load->inst_basereg = MONO_ARCH_BASEREG;
	load->inst_offset = mono_spillvar_offset (cfg, spill);
	if (item->prev) {
		while (ins->next != item->prev->data)
			ins = ins->next;
	}
	load->next = ins->next;
	ins->next = load;
	DEBUG (g_print ("SPILLED LOAD (%d at 0x%08lx(%%ebp)) R%d (freed %s)\n", spill, (long)load->inst_offset, i, mono_arch_regname_full (sel, fp)));
	if (fp)
		i = mono_regstate_alloc_float (cfg->rs, 1 << sel);
	else
		i = mono_regstate_alloc_int (cfg->rs, 1 << sel);
	g_assert (i == sel);

	return sel;
}

static int
get_register_spilling (MonoCompile *cfg, InstList *item, MonoInst *ins, guint32 regmask, int reg, gboolean fp)
{
	MonoInst *load;
	int i, sel, spill;
	int *assign, *symbolic;

	if (fp) {
		assign = cfg->rs->fassign;
		symbolic = cfg->rs->fsymbolic;
	}
	else {
		assign = cfg->rs->iassign;
		symbolic = cfg->rs->isymbolic;
	}

	DEBUG (g_print ("\tstart regmask to assign R%d: 0x%08x (R%d <- R%d R%d)\n", reg, regmask, ins->dreg, ins->sreg1, ins->sreg2));
	/* exclude the registers in the current instruction */
	if ((sreg1_is_fp (ins) == fp) && (reg != ins->sreg1) && (reg_is_freeable (ins->sreg1, fp) || (is_soft_reg (ins->sreg1, fp) && rassign (cfg, ins->sreg1, fp) >= 0))) {
		if (is_soft_reg (ins->sreg1, fp))
			regmask &= ~ (1 << rassign (cfg, ins->sreg1, fp));
		else
			regmask &= ~ (1 << ins->sreg1);
		DEBUG (g_print ("\t\texcluding sreg1 %s\n", mono_arch_regname_full (ins->sreg1, fp)));
	}
	if ((sreg2_is_fp (ins) == fp) && (reg != ins->sreg2) && (reg_is_freeable (ins->sreg2, fp) || (is_soft_reg (ins->sreg2, fp) && rassign (cfg, ins->sreg2, fp) >= 0))) {
		if (is_soft_reg (ins->sreg2, fp))
			regmask &= ~ (1 << rassign (cfg, ins->sreg2, fp));
		else
			regmask &= ~ (1 << ins->sreg2);
		DEBUG (g_print ("\t\texcluding sreg2 %s %d\n", mono_arch_regname_full (ins->sreg2, fp), ins->sreg2));
	}
	if ((dreg_is_fp (ins) == fp) && (reg != ins->dreg) && reg_is_freeable (ins->dreg, fp)) {
		regmask &= ~ (1 << ins->dreg);
		DEBUG (g_print ("\t\texcluding dreg %s\n", mono_arch_regname_full (ins->dreg, fp)));
	}

	DEBUG (g_print ("\t\tavailable regmask: 0x%08x\n", regmask));
	g_assert (regmask); /* need at least a register we can free */
	sel = -1;
	/* we should track prev_use and spill the register that's farther */
	if (fp) {
		for (i = 0; i < MONO_MAX_FREGS; ++i) {
			if (regmask & (1 << i)) {
				sel = i;
				DEBUG (g_print ("\t\tselected register %s has assignment %d\n", mono_arch_fregname (sel), cfg->rs->fsymbolic [sel]));
				break;
			}
		}

		i = cfg->rs->fsymbolic [sel];
		spill = ++cfg->spill_count;
		cfg->rs->fassign [i] = -spill - 1;
		mono_regstate_free_float (cfg->rs, sel);
	}
	else {
		for (i = 0; i < MONO_MAX_IREGS; ++i) {
			if (regmask & (1 << i)) {
				sel = i;
				DEBUG (g_print ("\t\tselected register %s has assignment %d\n", mono_arch_regname (sel), cfg->rs->isymbolic [sel]));
				break;
			}
		}

		i = cfg->rs->isymbolic [sel];
		spill = ++cfg->spill_count;
		cfg->rs->iassign [i] = -spill - 1;
		mono_regstate_free_int (cfg->rs, sel);
	}

	/* we need to create a spill var and insert a load to sel after the current instruction */
	MONO_INST_NEW (cfg, load, fp ? OP_LOADR8_MEMBASE : OP_LOAD_MEMBASE);
	load->dreg = sel;
	load->inst_basereg = MONO_ARCH_BASEREG;
	load->inst_offset = mono_spillvar_offset (cfg, spill);
	if (item->prev) {
		while (ins->next != item->prev->data)
			ins = ins->next;
	}
	load->next = ins->next;
	ins->next = load;
	DEBUG (g_print ("\tSPILLED LOAD (%d at 0x%08lx(%%ebp)) R%d (freed %s)\n", spill, (long)load->inst_offset, i, mono_arch_regname_full (sel, fp)));
	if (fp)
		i = mono_regstate_alloc_float (cfg->rs, 1 << sel);
	else
		i = mono_regstate_alloc_int (cfg->rs, 1 << sel);
	g_assert (i == sel);
	
	return sel;
}

static MonoInst*
create_copy_ins (MonoCompile *cfg, int dest, int src, MonoInst *ins, gboolean fp)
{
	MonoInst *copy;

	if (fp)
		MONO_INST_NEW (cfg, copy, OP_FMOVE);
	else
		MONO_INST_NEW (cfg, copy, OP_MOVE);

	copy->dreg = dest;
	copy->sreg1 = src;
	if (ins) {
		copy->next = ins->next;
		ins->next = copy;
	}
	DEBUG (g_print ("\tforced copy from %s to %s\n", mono_arch_regname (src), mono_arch_regname (dest)));
	return copy;
}

static MonoInst*
create_spilled_store (MonoCompile *cfg, int spill, int reg, int prev_reg, MonoInst *ins, gboolean fp)
{
	MonoInst *store;
	MONO_INST_NEW (cfg, store, fp ? OP_STORER8_MEMBASE_REG : OP_STORE_MEMBASE_REG);
	store->sreg1 = reg;
	store->inst_destbasereg = MONO_ARCH_BASEREG;
	store->inst_offset = mono_spillvar_offset (cfg, spill);
	if (ins) {
		store->next = ins->next;
		ins->next = store;
	}
	DEBUG (g_print ("\tSPILLED STORE (%d at 0x%08lx(%%ebp)) R%d (from %s)\n", spill, (long)store->inst_offset, prev_reg, mono_arch_regname_full (reg, fp)));
	return store;
}

static void
insert_before_ins (MonoInst *ins, InstList *item, MonoInst* to_insert)
{
	MonoInst *prev;
	if (item->next) {
		prev = item->next->data;

		while (prev->next != ins)
			prev = prev->next;
		to_insert->next = ins;
		prev->next = to_insert;
	} else {
		to_insert->next = ins;
	}
	/* 
	 * needed otherwise in the next instruction we can add an ins to the 
	 * end and that would get past this instruction.
	 */
	item->data = to_insert; 
}

/* flags used in reginfo->flags */
enum {
	MONO_X86_FP_NEEDS_LOAD_SPILL	= 1 << 0,
	MONO_X86_FP_NEEDS_SPILL			= 1 << 1,
	MONO_X86_FP_NEEDS_LOAD			= 1 << 2,
	MONO_X86_REG_NOT_ECX			= 1 << 3,
	MONO_X86_REG_EAX				= 1 << 4,
	MONO_X86_REG_EDX				= 1 << 5,
	MONO_X86_REG_ECX				= 1 << 6
};

static int
alloc_int_reg (MonoCompile *cfg, InstList *tmp, MonoInst *ins, guint32 dest_mask, int sym_reg, int flags)
{
	int val;

	/* FIXME: Add support for flags */
	val = mono_regstate_alloc_int (cfg->rs, dest_mask);
	if (val < 0)
		val = get_register_spilling (cfg, tmp, ins, dest_mask, sym_reg, FALSE);

	return val;
}

static int
alloc_float_reg (MonoCompile *cfg, InstList *tmp, MonoInst *ins, guint32 dest_mask, int sym_reg)
{
	int val;

	val = mono_regstate_alloc_float (cfg->rs, dest_mask);

	if (val < 0) {
		val = get_register_spilling (cfg, tmp, ins, dest_mask, sym_reg, TRUE);
	}

	return val;
}

static int
alloc_reg (MonoCompile *cfg, InstList *tmp, MonoInst *ins, guint32 dest_mask, int sym_reg, int flags, gboolean fp)
{
	if (fp)
		return alloc_float_reg (cfg, tmp, ins, dest_mask, sym_reg);
	else
		return alloc_int_reg (cfg, tmp, ins, dest_mask, sym_reg, flags);
}

static inline void
assign_reg (MonoRegState *rs, int reg, int hreg, gboolean fp)
{
	if (fp) {
		g_assert (reg >= MONO_MAX_FREGS);
		g_assert (hreg < MONO_MAX_FREGS);
		g_assert (! is_global_freg (hreg));

		rs->fassign [reg] = hreg;
		rs->fsymbolic [hreg] = reg;
		rs->ffree_mask &= ~ (1 << hreg);
	}
	else {
		g_assert (reg >= MONO_MAX_IREGS);
		g_assert (hreg < MONO_MAX_IREGS);
		g_assert (! is_global_ireg (hreg));

		rs->iassign [reg] = hreg;
		rs->isymbolic [hreg] = reg;
		rs->ifree_mask &= ~ (1 << hreg);
	}
}

static inline void
assign_ireg (MonoRegState *rs, int reg, int hreg)
{
	assign_reg (rs, reg, hreg, FALSE);
}

/*
 * Local register allocation.
 * We first scan the list of instructions and we save the liveness info of
 * each register (when the register is first used, when it's value is set etc.).
 * We also reverse the list of instructions (in the InstList list) because assigning
 * registers backwards allows for more tricks to be used.
 */
void
mono_local_regalloc (MonoCompile *cfg, MonoBasicBlock *bb)
{
	MonoInst *ins;
	MonoRegState *rs = cfg->rs;
	int i, val, fpcount;
	RegTrack *reginfo, *reginfof;
	RegTrack *reginfo1, *reginfo2, *reginfod;
	InstList *tmp, *reversed = NULL;
	const char *spec;
	GList *fspill_list = NULL;
	gboolean fp;
	int fspill = 0;

	if (!bb->code)
		return;

	rs->next_vireg = bb->max_ireg;
	rs->next_vfreg = bb->max_freg;
	mono_regstate_assign (rs);
	reginfo = g_malloc0 (sizeof (RegTrack) * rs->next_vireg);
	reginfof = g_malloc0 (sizeof (RegTrack) * rs->next_vfreg);
	rs->ifree_mask = MONO_ARCH_CALLEE_REGS;
	rs->ffree_mask = MONO_ARCH_CALLEE_FREGS;

	if (use_fpstack)
		rs->ffree_mask = 0xff & ~(1 << MONO_ARCH_FPSTACK_SIZE);

	ins = bb->code;

	/*if (cfg->opt & MONO_OPT_COPYPROP)
		local_copy_prop (cfg, ins);*/

	i = 1;
	fpcount = 0;
	DEBUG (g_print ("LOCAL regalloc: basic block: %d\n", bb->block_num));
	/* forward pass on the instructions to collect register liveness info */
	while (ins) {
		spec = ins_spec [ins->opcode];
		
		DEBUG (print_ins (i, ins));

		if (spec [MONO_INST_SRC1]) {
			if (spec [MONO_INST_SRC1] == 'f') {
				reginfo1 = reginfof;

				if (use_fpstack) {
					GList *spill;

					spill = g_list_first (fspill_list);
					if (spill && fpcount < MONO_ARCH_FPSTACK_SIZE) {
						reginfo1 [ins->sreg1].flags |= MONO_X86_FP_NEEDS_LOAD;
						fspill_list = g_list_remove (fspill_list, spill->data);
					} else
						fpcount--;
				}
			}
			else
				reginfo1 = reginfo;
			reginfo1 [ins->sreg1].prev_use = reginfo1 [ins->sreg1].last_use;
			reginfo1 [ins->sreg1].last_use = i;
			if (spec [MONO_INST_SRC1] == 'L') {
				/* The virtual register is allocated sequentially */
				reginfo1 [ins->sreg1 + 1].prev_use = reginfo1 [ins->sreg1 + 1].last_use;
				reginfo1 [ins->sreg1 + 1].last_use = i;
				if (reginfo1 [ins->sreg1 + 1].born_in == 0 || reginfo1 [ins->sreg1 + 1].born_in > i)
					reginfo1 [ins->sreg1 + 1].born_in = i;

				reginfo1 [ins->sreg1].flags |= MONO_X86_REG_EAX;
				reginfo1 [ins->sreg1 + 1].flags |= MONO_X86_REG_EDX;
			}
		} else {
			ins->sreg1 = -1;
		}
		if (spec [MONO_INST_SRC2]) {
			if (spec [MONO_INST_SRC2] == 'f') {
				reginfo2 = reginfof;

				if (use_fpstack) {
					GList *spill;

					spill = g_list_first (fspill_list);
					if (spill) {
						reginfo2 [ins->sreg2].flags |= MONO_X86_FP_NEEDS_LOAD;
						fspill_list = g_list_remove (fspill_list, spill->data);
						if (fpcount >= MONO_ARCH_FPSTACK_SIZE) {
							fspill++;
							fspill_list = g_list_prepend (fspill_list, GINT_TO_POINTER(fspill));
							reginfo2 [ins->sreg2].flags |= MONO_X86_FP_NEEDS_LOAD_SPILL;
						}
					} else
						fpcount--;
				}
			}
			else
				reginfo2 = reginfo;
			reginfo2 [ins->sreg2].prev_use = reginfo2 [ins->sreg2].last_use;
			reginfo2 [ins->sreg2].last_use = i;
			if (spec [MONO_INST_SRC2] == 'L') {
				/* The virtual register is allocated sequentially */
				reginfo2 [ins->sreg2 + 1].prev_use = reginfo2 [ins->sreg2 + 1].last_use;
				reginfo2 [ins->sreg2 + 1].last_use = i;
				if (reginfo2 [ins->sreg2 + 1].born_in == 0 || reginfo2 [ins->sreg2 + 1].born_in > i)
					reginfo2 [ins->sreg2 + 1].born_in = i;
			}
			if (spec [MONO_INST_CLOB] == 's') {
				reginfo2 [ins->sreg1].flags |= MONO_X86_REG_NOT_ECX;
				reginfo2 [ins->sreg2].flags |= MONO_X86_REG_ECX;
			}
		} else {
			ins->sreg2 = -1;
		}
		if (spec [MONO_INST_DEST]) {
			if (spec [MONO_INST_DEST] == 'f') {
				reginfod = reginfof;
				if (use_fpstack && (spec [MONO_INST_CLOB] != 'm')) {
					if (fpcount >= MONO_ARCH_FPSTACK_SIZE) {
						reginfod [ins->dreg].flags |= MONO_X86_FP_NEEDS_SPILL;
						fspill++;
						fspill_list = g_list_prepend (fspill_list, GINT_TO_POINTER(fspill));
						fpcount--;
					}
					fpcount++;
				}
			}
			else
				reginfod = reginfo;
			if (spec [MONO_INST_DEST] != 'b') /* it's not just a base register */
				reginfod [ins->dreg].killed_in = i;
			reginfod [ins->dreg].prev_use = reginfod [ins->dreg].last_use;
			reginfod [ins->dreg].last_use = i;
			if (reginfod [ins->dreg].born_in == 0 || reginfod [ins->dreg].born_in > i)
				reginfod [ins->dreg].born_in = i;
			if (spec [MONO_INST_DEST] == 'l' || spec [MONO_INST_DEST] == 'L') {
				/* The virtual register is allocated sequentially */
				reginfod [ins->dreg + 1].prev_use = reginfod [ins->dreg + 1].last_use;
				reginfod [ins->dreg + 1].last_use = i;
				if (reginfod [ins->dreg + 1].born_in == 0 || reginfod [ins->dreg + 1].born_in > i)
					reginfod [ins->dreg + 1].born_in = i;

				reginfod [ins->dreg].flags |= MONO_X86_REG_EAX;
				reginfod [ins->dreg + 1].flags |= MONO_X86_REG_EDX;
			}
		} else {
			ins->dreg = -1;
		}

		if (spec [MONO_INST_CLOB] == 'c') {
			/* A call instruction implicitly uses all registers in call->out_ireg_args */

			MonoCallInst *call = (MonoCallInst*)ins;
			GSList *list;

			list = call->out_ireg_args;
			if (list) {
				while (list) {
					guint64 regpair;
					int reg, hreg;

					regpair = (guint64) (list->data);
					hreg = regpair >> 32;
					reg = regpair & 0xffffffff;

					reginfo [reg].prev_use = reginfo [reg].last_use;
					reginfo [reg].last_use = i;

					list = g_slist_next (list);
				}
			}

			list = call->out_freg_args;
			if (!use_fpstack && list) {
				while (list) {
					guint64 regpair;
					int reg, hreg;

					regpair = (guint64) (list->data);
					hreg = regpair >> 32;
					reg = regpair & 0xffffffff;

					reginfof [reg].prev_use = reginfof [reg].last_use;
					reginfof [reg].last_use = i;

					list = g_slist_next (list);
				}
			}
		}

		reversed = inst_list_prepend (cfg->mempool, reversed, ins);
		++i;
		ins = ins->next;
	}

	// todo: check if we have anything left on fp stack, in verify mode?
	fspill = 0;

	DEBUG (print_regtrack (reginfo, rs->next_vireg));
	DEBUG (print_regtrack (reginfof, rs->next_vfreg));
	tmp = reversed;
	while (tmp) {
		int prev_dreg, prev_sreg1, prev_sreg2, clob_dreg;
		int dest_dreg, dest_sreg1, dest_sreg2, clob_reg;
		guint32 dreg_mask, sreg1_mask, sreg2_mask;
		--i;
		ins = tmp->data;
		spec = ins_spec [ins->opcode];
		prev_dreg = -1;
		clob_dreg = -1;
		clob_reg = -1;
		dest_dreg = -1;
		dest_sreg1 = -1;
		dest_sreg2 = -1;
		dreg_mask = dreg_is_fp (ins) ? MONO_ARCH_CALLEE_FREGS : MONO_ARCH_CALLEE_REGS;
		sreg1_mask = sreg1_is_fp (ins) ? MONO_ARCH_CALLEE_FREGS : MONO_ARCH_CALLEE_REGS;
		sreg2_mask = sreg2_is_fp (ins) ? MONO_ARCH_CALLEE_FREGS : MONO_ARCH_CALLEE_REGS;

		DEBUG (g_print ("processing:"));
		DEBUG (print_ins (i, ins));

		/*
		 * FIXED REGS
		 */
		dest_sreg1 = MONO_ARCH_INST_FIXED_REG (spec [MONO_INST_SRC1]);
		dest_sreg2 = MONO_ARCH_INST_FIXED_REG (spec [MONO_INST_SRC2]);
		dest_dreg = MONO_ARCH_INST_FIXED_REG (spec [MONO_INST_DEST]);
		clob_reg = MONO_ARCH_INST_FIXED_REG (spec [MONO_INST_CLOB]);
		sreg2_mask &= ~ (MONO_ARCH_INST_SREG2_MASK (spec));

		/*
		 * TRACK FP STACK
		 */
		if (use_fpstack && (spec [MONO_INST_CLOB] != 'm')) {
			if (spec [MONO_INST_DEST] == 'f') {
				if (reginfof [ins->dreg].flags & MONO_X86_FP_NEEDS_SPILL) {
					GList *spill_node;
					MonoInst *store;
					spill_node = g_list_first (fspill_list);
					g_assert (spill_node);

					store = create_spilled_store_float (cfg, GPOINTER_TO_INT (spill_node->data), ins->dreg, ins);
					insert_before_ins (ins, tmp, store);
					fspill_list = g_list_remove (fspill_list, spill_node->data);
					fspill--;
				}
			}

			if (spec [MONO_INST_SRC1] == 'f') {
				if (reginfof [ins->sreg1].flags & MONO_X86_FP_NEEDS_LOAD) {
					MonoInst *load;
					MonoInst *store = NULL;

					if (reginfof [ins->sreg1].flags & MONO_X86_FP_NEEDS_LOAD_SPILL) {
						GList *spill_node;
						spill_node = g_list_first (fspill_list);
						g_assert (spill_node);

						store = create_spilled_store_float (cfg, GPOINTER_TO_INT (spill_node->data), ins->sreg1, ins);		
						fspill_list = g_list_remove (fspill_list, spill_node->data);
					}

					fspill++;
					fspill_list = g_list_prepend (fspill_list, GINT_TO_POINTER(fspill));
					load = create_spilled_load_float (cfg, fspill, ins->sreg1, ins);
					insert_before_ins (ins, tmp, load);
					if (store) 
						insert_before_ins (load, tmp, store);
				}
			}

			if (spec [MONO_INST_SRC1] == 'f') {
				if (reginfof [ins->sreg2].flags & MONO_X86_FP_NEEDS_LOAD) {
					MonoInst *load;
					MonoInst *store = NULL;

					if (reginfof [ins->sreg2].flags & MONO_X86_FP_NEEDS_LOAD_SPILL) {
						GList *spill_node;

						spill_node = g_list_first (fspill_list);
						g_assert (spill_node);
						if (spec [MONO_INST_SRC1] == 'f' && (reginfof [ins->sreg1].flags & MONO_X86_FP_NEEDS_LOAD_SPILL))
							spill_node = g_list_next (spill_node);
	
						store = create_spilled_store_float (cfg, GPOINTER_TO_INT (spill_node->data), ins->sreg2, ins);
						fspill_list = g_list_remove (fspill_list, spill_node->data);
					}
				
					fspill++;
					fspill_list = g_list_prepend (fspill_list, GINT_TO_POINTER(fspill));
					load = create_spilled_load_float (cfg, fspill, ins->sreg2, ins);
					insert_before_ins (ins, tmp, load);
					if (store) 
						insert_before_ins (load, tmp, store);
				}
			}
		}

		/*
		 * TRACK FIXED SREG2
		 */
		if (dest_sreg2 != -1) {
			if (rs->ifree_mask & (1 << dest_sreg2)) {
				if (is_global_ireg (ins->sreg2)) {
					/* Argument already in hard reg, need to copy */
					MonoInst *copy = create_copy_ins (cfg, dest_sreg2, ins->sreg2, NULL, FALSE);
					insert_before_ins (ins, tmp, copy);
				}
				else {
					DEBUG (g_print ("\tshortcut assignment of R%d to %s\n", ins->sreg2, mono_arch_regname (dest_sreg2)));
					assign_ireg (rs, ins->sreg2, dest_sreg2);
				}
			} else {
				int need_spill = TRUE;

				dreg_mask &= ~ (1 << dest_sreg2);
				sreg1_mask &= ~ (1 << dest_sreg2);

				/* 
				 * First check if dreg is assigned to dest_sreg2, since we
				 * can't spill a dreg.
				 */
				val = rs->iassign [ins->dreg];
				if (val == dest_sreg2 && ins->dreg != ins->sreg2) {
					/* 
					 * the destination register is already assigned to 
					 * dest_sreg2: we need to allocate another register for it 
					 * and then copy from this to dest_sreg2.
					 */
					int new_dest;
					new_dest = alloc_int_reg (cfg, tmp, ins, dreg_mask, ins->dreg, reginfo [ins->dreg].flags);
					g_assert (new_dest >= 0);
					DEBUG (g_print ("\tchanging dreg R%d to %s from %s\n", ins->dreg, mono_arch_regname (new_dest), mono_arch_regname (dest_sreg2)));

					assign_ireg (rs, ins->dreg, new_dest);
					clob_dreg = ins->dreg;
					ins->dreg = new_dest;
					create_copy_ins (cfg, dest_sreg2, new_dest, ins, FALSE);
					need_spill = FALSE;
				}

				if (is_global_ireg (ins->sreg2)) {
					MonoInst *copy = create_copy_ins (cfg, dest_sreg2, ins->sreg2, NULL, FALSE);
					insert_before_ins (ins, tmp, copy);
				}
				else {
					val = rs->iassign [ins->sreg2];
					if (val == dest_sreg2) {
						/* sreg2 is already assigned to the correct register */
						need_spill = FALSE;
					}
					else if ((val >= 0) || (val < -1)) {
						/* FIXME: sreg2 already assigned to another register */
						g_assert_not_reached ();
					}
				}

				if (need_spill) {
					DEBUG (g_print ("\tforced spill of R%d\n", rs->isymbolic [dest_sreg2]));
					get_register_force_spilling (cfg, tmp, ins, rs->isymbolic [dest_sreg2], FALSE);
					mono_regstate_free_int (rs, dest_sreg2);
				}

				if (!is_global_ireg (ins->sreg2))
					/* force-set sreg2 */
					assign_ireg (rs, ins->sreg2, dest_sreg2);
			}
			ins->sreg2 = dest_sreg2;
		}

		/*
		 * TRACK DREG
		 */
		fp = dreg_is_fp (ins);
		if (spec [MONO_INST_DEST] && (!fp || (fp && !use_fpstack)) && is_soft_reg (ins->dreg, fp))
			prev_dreg = ins->dreg;

		if ((!fp || (fp && !use_fpstack)) && (is_soft_reg (ins->dreg, fp))) {
			if (dest_dreg != -1)
				dreg_mask = (1 << dest_dreg);

			val = rassign (cfg, ins->dreg, fp);

			if (val < 0) {
				int spill = 0;
				if (val < -1) {
					/* the register gets spilled after this inst */
					spill = -val -1;
				}
				val = alloc_reg (cfg, tmp, ins, dreg_mask, ins->dreg, reginfo [ins->dreg].flags, fp);
				assign_reg (rs, ins->dreg, val, fp);
				if (spill)
					create_spilled_store (cfg, spill, val, prev_dreg, ins, fp);
			}
			DEBUG (g_print ("\tassigned dreg %s to dest R%d\n", mono_arch_regname_full (val, fp), ins->dreg));
			ins->dreg = val;
		}

		if ((!fp || (fp && !use_fpstack)) && prev_dreg >= 0 && is_soft_reg (prev_dreg, fp) && (fp ? reginfof : reginfo) [prev_dreg].born_in >= i) {
			/* 
			 * In theory, we could free up the hreg even if the vreg is alive,
			 * but branches inside bblocks force us to assign the same hreg
			 * to a vreg every time it is encountered.
			 */
			int dreg = rassign (cfg, prev_dreg, fp);
			g_assert (dreg >= 0);
			DEBUG (g_print ("\tfreeable %s (R%d) (born in %d)\n", mono_arch_regname_full (dreg, fp), prev_dreg, (fp ? reginfof : reginfo) [prev_dreg].born_in));
			if (fp)
				mono_regstate_free_float (rs, dreg);
			else
				mono_regstate_free_int (rs, dreg);
		}

		if ((dest_dreg != -1) && (ins->dreg != dest_dreg)) {
			/* this instruction only outputs to dest_dreg, need to copy */
			if (!(rs->ifree_mask & (1 << dest_dreg)) && rs->isymbolic [dest_dreg] >= MONO_MAX_IREGS) {
				DEBUG (g_print ("\tforced spill of R%d\n", rs->isymbolic [dest_dreg]));
				get_register_force_spilling (cfg, tmp, ins, rs->isymbolic [dest_dreg], FALSE);
				mono_regstate_free_int (rs, dest_dreg);
			}

			create_copy_ins (cfg, ins->dreg, dest_dreg, ins, fp);
			ins->dreg = dest_dreg;
		}

		/*
		 * TRACK CLOBBERING
		 */
		if ((clob_reg != -1) && (!(rs->ifree_mask & (1 << clob_reg)))) {
			DEBUG (g_print ("\tforced spill of clobbered reg R%d\n", rs->isymbolic [clob_reg]));
			get_register_force_spilling (cfg, tmp, ins, rs->isymbolic [clob_reg], FALSE);
			mono_regstate_free_int (rs, clob_reg);
		}

		if (spec [MONO_INST_CLOB] == 'c') {
			int j, s, dreg;
			guint32 clob_mask;

			clob_mask = MONO_ARCH_CALLEE_REGS;

			/*
			 * Need to avoid spilling the dreg since the dreg is not really
			 * clobbered by the call.
			 */
			if ((prev_dreg != -1) && !dreg_is_fp (ins))
				dreg = rassign (cfg, prev_dreg, dreg_is_fp (ins));
			else
				dreg = -1;

			for (j = 0; j < MONO_MAX_IREGS; ++j) {
				s = 1 << j;
				if ((clob_mask & s) && !(rs->ifree_mask & s) && (j != ins->sreg1) && (j != dreg)) {
					get_register_force_spilling (cfg, tmp, ins, rs->isymbolic [j], FALSE);
					mono_regstate_free_int (rs, j);
				}
			}

			if (!use_fpstack) {
				clob_mask = MONO_ARCH_CALLEE_FREGS;
				if ((prev_dreg != -1) && dreg_is_fp (ins))
					dreg = rassign (cfg, prev_dreg, dreg_is_fp (ins));
				else
					dreg = -1;

				for (j = 0; j < MONO_MAX_FREGS; ++j) {
					s = 1 << j;
					if ((clob_mask & s) && !(rs->ffree_mask & s) && (j != ins->sreg1)) {
						get_register_force_spilling (cfg, tmp, ins, rs->fsymbolic [j], TRUE);
						mono_regstate_free_float (rs, j);
					}
				}
			}
		}

		/*
		 * TRACK ARGUMENT REGS
		 */
		if (spec [MONO_INST_CLOB] == 'c') {
			MonoCallInst *call = (MonoCallInst*)ins;
			GSList *list;

			/* 
			 * This needs to be done before assigning sreg1, so sreg1 will
			 * not be assigned one of the argument regs.
			 */

			/* 
			 * Assign all registers in call->out_reg_args to the proper 
			 * argument registers.
			 */

			list = call->out_ireg_args;
			if (list) {
				while (list) {
					guint64 regpair;
					int reg, hreg;

					regpair = (guint64) (list->data);
					hreg = regpair >> 32;
					reg = regpair & 0xffffffff;

					assign_reg (rs, reg, hreg, FALSE);

					DEBUG (g_print ("\tassigned arg reg %s to R%d\n", mono_arch_regname (hreg), reg));

					list = g_slist_next (list);
				}
				g_slist_free (call->out_ireg_args);
			}

			list = call->out_freg_args;
			if (list && !use_fpstack) {
				while (list) {
					guint64 regpair;
					int reg, hreg;

					regpair = (guint64) (list->data);
					hreg = regpair >> 32;
					reg = regpair & 0xffffffff;

					assign_reg (rs, reg, hreg, TRUE);

					DEBUG (g_print ("\tassigned arg reg %s to R%d\n", mono_arch_fregname (hreg), reg));

					list = g_slist_next (list);
				}
			}
			if (call->out_freg_args)
				g_slist_free (call->out_freg_args);
		}

		/*
		 * TRACK SREG1
		 */
		fp = sreg1_is_fp (ins);
		if ((!fp || (fp && !use_fpstack))) {
			if (dest_sreg1 != -1) {
				sreg1_mask = 1 << dest_sreg1;

				if (!(rs->ifree_mask & (1 << dest_sreg1))) {
					DEBUG (g_print ("\tforced spill of R%d\n", rs->isymbolic [dest_sreg1]));
					get_register_force_spilling (cfg, tmp, ins, rs->isymbolic [dest_sreg1], FALSE);
					mono_regstate_free_int (rs, dest_sreg1);
				}
				if (is_global_ireg (ins->sreg1)) {
					/* The argument is already in a hard reg, need to copy */
					MonoInst *copy = create_copy_ins (cfg, dest_sreg1, ins->sreg1, NULL, FALSE);
					insert_before_ins (ins, tmp, copy);
					ins->sreg1 = dest_sreg1;
				}
			}

			if (is_soft_reg (ins->sreg1, fp)) {
				val = rassign (cfg, ins->sreg1, fp);
				prev_sreg1 = ins->sreg1;
				if (val < 0) {
					int spill = 0;
					if (val < -1) {
						/* the register gets spilled after this inst */
						spill = -val -1;
					}

					if (((ins->opcode == OP_MOVE) || (ins->opcode == OP_SETREG)) && !spill && !fp && (!is_global_ireg (ins->dreg) && (rs->ifree_mask & (1 << ins->dreg)))) {
						/* 
						 * Allocate the same hreg to sreg1 as well so the 
						 * peephole can get rid of the move.
						 */
						sreg1_mask = 1 << ins->dreg;
					}

					val = alloc_reg (cfg, tmp, ins, sreg1_mask, ins->sreg1, reginfo [ins->sreg1].flags, fp);
					assign_reg (rs, ins->sreg1, val, fp);
					DEBUG (g_print ("\tassigned sreg1 %s to R%d\n", mono_arch_regname_full (val, fp), ins->sreg1));

					if (spill) {
						MonoInst *store = create_spilled_store (cfg, spill, val, prev_sreg1, NULL, fp);
						/*
						 * Need to insert before the instruction since it can
						 * overwrite sreg1.
						 */
						insert_before_ins (ins, tmp, store);
					}
				}
				else if ((dest_sreg1 != -1) && (dest_sreg1 != val)) {
					g_assert_not_reached ();
				}
				
				ins->sreg1 = val;
			}
			else {
				prev_sreg1 = -1;
			}
			sreg2_mask &= ~(1 << ins->sreg1);
		}

		/* handle clobbering of sreg1 */
		if (((spec [MONO_INST_DEST] == 'f' && spec [MONO_INST_SRC1] == 'f' && !use_fpstack) || spec [MONO_INST_CLOB] == '1') && ins->dreg != ins->sreg1) {
			MonoInst *sreg2_copy = NULL;
			MonoInst *copy;
			gboolean fp = (spec [MONO_INST_SRC1] == 'f');

			if (ins->dreg == ins->sreg2) {
				/* 
				 * copying sreg1 to dreg could clobber sreg2, so allocate a new
				 * register for it.
				 */
				int reg2 = alloc_reg (cfg, tmp, ins, dreg_mask, ins->sreg2, 0, fp);

				DEBUG (g_print ("\tneed to copy sreg2 %s to reg %s\n", mono_arch_regname_full (ins->sreg2, fp), mono_arch_regname_full (reg2, fp)));
				sreg2_copy = create_copy_ins (cfg, reg2, ins->sreg2, NULL, fp);
				prev_sreg2 = ins->sreg2 = reg2;

				if (fp)
					mono_regstate_free_float (rs, reg2);
				else
					mono_regstate_free_int (rs, reg2);
			}

			copy = create_copy_ins (cfg, ins->dreg, ins->sreg1, NULL, fp);
			DEBUG (g_print ("\tneed to copy sreg1 %s to dreg %s\n", mono_arch_regname_full (ins->sreg1, fp), mono_arch_regname_full (ins->dreg, fp)));
			insert_before_ins (ins, tmp, copy);

			if (sreg2_copy)
				insert_before_ins (copy, tmp, sreg2_copy);

			/*
			 * Need to prevent sreg2 to be allocated to sreg1, since that
			 * would screw up the previous copy.
			 */
			sreg2_mask &= ~ (1 << ins->sreg1);
			/* we set sreg1 to dest as well */
			prev_sreg1 = ins->sreg1 = ins->dreg;
			sreg2_mask &= ~ (1 << ins->dreg);
		}

		/*
		 * TRACK SREG2
		 */
		fp = sreg2_is_fp (ins);
		if ((!fp || (fp && !use_fpstack)) && (is_soft_reg (ins->sreg2, fp))) {
			val = rassign (cfg, ins->sreg2, fp);

			if (val < 0) {
				int spill = 0;
				if (val < -1) {
					/* the register gets spilled after this inst */
					spill = -val -1;
				}
				val = alloc_reg (cfg, tmp, ins, sreg2_mask, ins->sreg2, reginfo [ins->sreg2].flags, fp);
				assign_reg (rs, ins->sreg2, val, fp);
				DEBUG (g_print ("\tassigned sreg2 %s to R%d\n", mono_arch_regname_full (val, fp), ins->sreg2));
				if (spill)
					create_spilled_store (cfg, spill, val, prev_sreg2, ins, fp);
			}
			ins->sreg2 = val;
		}
		else {
			prev_sreg2 = -1;
		}

		/*if (reg_is_freeable (ins->sreg1) && prev_sreg1 >= 0 && reginfo [prev_sreg1].born_in >= i) {
			DEBUG (g_print ("freeable %s\n", mono_arch_regname (ins->sreg1)));
			mono_regstate_free_int (rs, ins->sreg1);
		}
		if (reg_is_freeable (ins->sreg2) && prev_sreg2 >= 0 && reginfo [prev_sreg2].born_in >= i) {
			DEBUG (g_print ("freeable %s\n", mono_arch_regname (ins->sreg2)));
			mono_regstate_free_int (rs, ins->sreg2);
		}*/
	
		DEBUG (print_ins (i, ins));
		/* this may result from a insert_before call */
		if (!tmp->next)
			bb->code = tmp->data;
		tmp = tmp->next;
	}

	g_free (reginfo);
	g_free (reginfof);
	g_list_free (fspill_list);
}
