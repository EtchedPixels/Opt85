#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

struct label {
	struct label *next;
	struct instruction *instruction;
	const char *name;
	int16_t spbias;
};

struct effect {
	struct instruction *prev, *next;
	uint32_t need, set;
	uint16_t value[9];	/* X A B C D E H L PSW */
	/* We can do ranges and more later */
#define VALUE_KNOWN	0x0100
	uint32_t flags;
#define HL_SPBIAS	1	/* Tracking DAD SP */
	int spbias;		/* Tracked SP bias versus HL */
};

struct instruction {
	struct effect *prev, *next;
	struct label *lnext;
	struct label *label;
	const char *op;
	const char *opcode;
	struct optab *opinfo;
	const char *insn;
	uint8_t sr, dr;
	int addrconst;
	int spbias;
	int dead;
	uint32_t set;		/* Local copies not changed when we */
	uint32_t need;		/* propagate needs */
};

#define BIAS_UNKNOWN 0xFFFF0000
#define CONST_UNKNOWN 0xFFFF0000

struct instruction *codehead, *codetail;
unsigned int linenum;
int spbias;

static struct effect dummy_effect;

struct optab {
	const char *op;
	uint32_t flags;
#define OP_AOP		1	/* OP A,R/M */
#define OP_SPAIR	2	/* pair following is source */
#define OP_DPAIR	4	/* pair following is dest */
#define	OP_PAIROP	8	/* pair following is src/dst */
#define OP_C		16	/* op consumes carry */
#define OP_CC		32	/* uses condition codes */
#define OP_BRA		64	/* branching */
#define OP_CALL		128	/* calling */
#define OP_MOV		256	/* OP R/M,R/M */
#define OP_MVI		512	/* OP R,C */
#define OP_IMMED	1024	/* Immediate can be known */
#define OP_ADDR		2048	/* Fixed address reference */
#define OP_REGMOD	4096	/* Modifies following register or M */
#define OP_PAIRMOD	8192	/* Ditto for an RP, can't be M */
#define OP_RET		16384	/* Returns */
#define OP_KEEP		32768	/* Side effects */

	uint16_t imask, omask;
};

/* A-L must be 1-8 for value mask */
#define REG_A		1
#define REG_B		2
#define REG_C		3
#define REG_D		4
#define REG_E		5
#define REG_H		6
#define REG_L		7
#define REG_PSW		8
#define MEM_HL		9
#define MEMORY		10
#define REG_SP		12
#define MEM_HL_W	12
#define SIDEEFFECT	13

#define REGM_A		(1 << REG_A)
#define REGM_B		(1 << REG_B)
#define REGM_C		(1 << REG_C)
#define REGM_D		(1 << REG_D)
#define REGM_E		(1 << REG_E)
#define REGM_H		(1 << REG_H)
#define REGM_L		(1 << REG_L)
#define MEMM_HL		(1 << MEM_HL)
#define MEMORYM		(1 << MEMORY)
#define REGM_SP		(1 << REG_SP)
#define MEMM_HL_W	(1 << MEM_HL_W)
#define REGM_PSW	(1 << REG_PSW)
#define SIDEEFFECTM	(1 << SIDEEFFECT)

/* Things we might need to return to the caller */
#define REGM_RETS	(REGM_D|REGM_E|REGM_H|REGM_L|REGM_SP)

/* For barrier cases like jumping */
#define REGM_ALL	0xFFFF

/* We don't track values for PSW yet but we track usage */
#define KEEPMASK	(SIDEEFFECTM | MEMM_HL | MEMORYM | MEMM_HL_W | REGM_SP)
#define TRACKED		(REGM_A | REGM_B | REGM_C | REGM_D | REGM_E | REGM_H | REGM_L | REGM_PSW)

struct optab ops[] = {
	/* For these the immediate form *MUST* follow the non immediate */
	{ "MOV", OP_MOV, 0, 0 },
	{ "MVI", OP_MVI, 0, 0 },
	{ "LXI", OP_DPAIR | OP_IMMED, 0, 0 },
	{ "LDA", OP_ADDR, MEMORYM, REGM_A },
	{ "STA", OP_ADDR, REGM_A, MEMORYM },
	{ "LHLD", OP_ADDR, MEMORYM, REGM_H | REGM_L },
	{ "SHLD", OP_ADDR, REGM_H | REGM_L, MEMORYM },
	{ "LDAX", OP_ADDR | OP_SPAIR, MEMORYM, REGM_A },
	{ "STAX", OP_ADDR | OP_DPAIR, REGM_A, MEMORYM },
	/* Really xchg swaps over the properties - we should do likewise eventually */
	{ "XCHG", 0, REGM_D | REGM_E | REGM_H | REGM_L,
	 REGM_D | REGM_E | REGM_H | REGM_L },
	{ "INR", OP_REGMOD, 0, 0 },
	{ "DCR", OP_REGMOD, 0, 0 },
	{ "INX", OP_PAIRMOD, 0, 0 },
	{ "DEX", OP_PAIRMOD, 0, 0 },
	{ "DAD", OP_SPAIR, REGM_H | REGM_L, REGM_H | REGM_L | REGM_PSW },
	{ "DAA", 0, REGM_A | REGM_PSW, REGM_A | REGM_PSW },
	{ "RLC", 0, REGM_A | REGM_PSW, REGM_A | REGM_PSW },
	{ "RRC", 0, REGM_A | REGM_PSW, REGM_A | REGM_PSW },
	{ "RAL", 0, REGM_A | REGM_PSW, REGM_A | REGM_PSW },
	{ "RAR", 0, REGM_A | REGM_PSW, REGM_A | REGM_PSW },
	{ "CMA", 0, REGM_A | REGM_PSW, REGM_A | REGM_PSW },
	{ "CMC", 0, REGM_PSW, REGM_PSW },
	{ "STC", 0, REGM_PSW, REGM_PSW },
	/* For these the immediate form *MUST* follow the non immediate */
	{ "ADD", OP_AOP, REGM_A, REGM_A | REGM_PSW },
	{ "ADI", OP_AOP | OP_IMMED, REGM_A, REGM_A | REGM_PSW },
	{ "ADC", OP_AOP | OP_C, REGM_A | REGM_PSW, REGM_A | REGM_PSW },
	{ "ACI", OP_AOP | OP_IMMED, REGM_A | REGM_PSW, REGM_A | REGM_PSW },
	{ "SUB", OP_AOP, REGM_A, REGM_A | REGM_PSW },
	{ "SUI", OP_AOP | OP_IMMED, REGM_A, REGM_A | REGM_PSW },
	{ "SBC", OP_AOP | OP_C, REGM_A | REGM_PSW, REGM_A | REGM_PSW },
	{ "SBI", OP_AOP | OP_IMMED, REGM_A | REGM_PSW, REGM_A | REGM_PSW },
	{ "ANA", OP_AOP, REGM_A, REGM_A | REGM_PSW },
	{ "ANI", OP_AOP | OP_IMMED, REGM_A, REGM_A | REGM_PSW },
	{ "ORA", OP_AOP, REGM_A, REGM_A | REGM_PSW },
	{ "ORI", OP_AOP | OP_IMMED, REGM_A, REGM_A | REGM_PSW },
	{ "XRA", OP_AOP, REGM_A, REGM_A | REGM_PSW },
	{ "XRI", OP_AOP | OP_IMMED, REGM_A, REGM_A | REGM_PSW },
	{ "CMP", OP_AOP, REGM_A, REGM_A | REGM_PSW },
	{ "CPI", OP_AOP | OP_IMMED, REGM_A, REGM_A | REGM_PSW },
	/* Assume the worst case for branches for now. We can do better later
	   for single target forward jumps from the compiler */
	{ "JMP", OP_BRA, REGM_ALL, 0 },
	{ "JZ", OP_BRA, REGM_ALL, 0 },
	{ "JNZ", OP_BRA, REGM_ALL, 0 },
	{ "JC", OP_BRA, REGM_ALL, 0 },
	{ "JNC", OP_BRA, REGM_ALL, 0 },
	{ "JP", OP_BRA, REGM_ALL, 0 },
	{ "JM", OP_BRA, REGM_ALL, 0 },
	{ "JPO", OP_BRA, REGM_ALL, 0 },
	{ "JPE", OP_BRA, REGM_ALL, 0 },
	{ "PCHL", OP_BRA, REGM_ALL, 0 },
	/* Returns need DEHL and SP right */
	{ "RET", OP_RET, REGM_SP | REGM_RETS, REGM_SP },
	{ "RZ", OP_RET, REGM_PSW | REGM_SP | REGM_RETS, REGM_SP },
	{ "RNZ", OP_RET, REGM_PSW | REGM_SP | REGM_RETS, REGM_SP },
	{ "RC", OP_RET, REGM_PSW | REGM_SP | REGM_RETS, REGM_SP },
	{ "RNC", OP_RET, REGM_PSW | REGM_SP | REGM_RETS, REGM_SP },
	{ "RP", OP_RET, REGM_PSW | REGM_SP | REGM_RETS, REGM_SP },
	{ "RM", OP_RET, REGM_PSW | REGM_SP | REGM_RETS, REGM_SP },
	{ "RPO", OP_RET, REGM_PSW | REGM_SP | REGM_RETS, REGM_SP },
	{ "RPE", OP_RET, REGM_PSW | REGM_SP | REGM_RETS, REGM_SP },
	/* Calls need everything - needs review to see if we can spot the
	   special functions versus C calls that need nothing sane */
	{ "CALL", OP_CALL, REGM_ALL, REGM_ALL },
	{ "CZ", OP_CALL, REGM_ALL, REGM_ALL },
	{ "CNZ", OP_CALL, REGM_ALL, REGM_ALL },
	{ "CC", OP_CALL, REGM_ALL, REGM_ALL },
	{ "CNC", OP_CALL, REGM_ALL, REGM_ALL },
	{ "CP", OP_CALL, REGM_ALL, REGM_ALL },
	{ "CM", OP_CALL, REGM_ALL, REGM_ALL },
	{ "CPO", OP_CALL, REGM_ALL, REGM_ALL },
	{ "CPE", OP_CALL, REGM_ALL, REGM_ALL },
	/* Need to add smarts for compiler stubs */
	{ "RST", OP_CALL, REGM_ALL, REGM_ALL },
	{ "PUSH", OP_SPAIR, REGM_SP, REGM_SP | MEMORYM },
	{ "POP", OP_DPAIR, REGM_SP | MEMORYM, REGM_SP },
	{ "XTHL", 0, MEMORYM | REGM_SP | REGM_H | REGM_L,
	 MEMORYM | REGM_H | REGM_L },
	{ "SPHL", 0, REGM_H | REGM_L, REGM_SP },
	{ "IN", OP_KEEP, 0, REGM_A },
	{ "OUT", OP_KEEP, REGM_A, 0 },
	{ "EI", OP_KEEP, 0, SIDEEFFECTM },
	{ "DI", OP_KEEP, 0, SIDEEFFECTM },
	{ "HLT", OP_KEEP, 0, SIDEEFFECTM },
	{ "NOP", 0, 0, 0 },
	{ NULL, }
};

static struct optab *find_operation(const char *p)
{
	struct optab *o = ops;
	while (o->op) {
		if (strcasecmp(o->op, p) == 0)
			return o;
		o++;
	}
	return NULL;
}

static void *zalloc(size_t size)
{
	void *p = malloc(size);
	if (p == NULL) {
		fprintf(stderr, "Out of memory allocating %lu bytes.\n",
			(unsigned long) size);
		exit(1);
	}
	memset(p, 0, size);
	return p;
}

static void error(const char *p)
{
	fprintf(stderr, "%d: %s\n", linenum, p);
	exit(1);
}

static char regname(int reg)
{
	if (reg == MEM_HL)
		return 'M';
	if (reg <= REG_PSW)
		return "?ABCDEHLF"[reg];
	fprintf(stderr, "%d: bad regname %d\n", linenum, reg);
	exit(1);
}

void badreg8(void)
{
	error("Expected A,B,C,D,E,H,L or M");
}

void badreg16(void)
{
	error("Expected PSW, SP, B, D or H");
}

static void print_regmap(unsigned int m)
{
	int i;
	for (i = REG_A; i <= REG_L; i++) {
		if (m & (1 << i))
			putchar(regname(i));
		else
			putchar('-');
	}
}

static void print_values(struct effect *e)
{
	int i;
	for (i = REG_A; i <= REG_L; i++) {
		putchar(regname(i));
		if (e->value[i] & VALUE_KNOWN)
			printf("%02X", e->value[i] & 0xFF);
		else
			printf("??");
	}
}

static char *do_strtok(char *m, char *e)
{
	char *p = strtok(NULL, m);
	if (p == NULL)
		error(e);
	return p;
}


/*
 * Value tracking:
 *
 * For now we do simple constant tracking and nothing fancy. So we don't
 * track (HL) and fixed address label save/restores for optimization
 */
static uint16_t reg_value(struct effect *e, int reg)
{
	if (e->value[reg] & VALUE_KNOWN)
		return e->value[reg] & 0xFF;
	fprintf(stderr, "Regval %c is not known.\n", regname(reg));
	error("attempt to consume unknown value");
	return 0;
}

static void clear_reg_value(struct effect *e, int reg)
{
	if (reg > REG_L)
		return;
	e->value[reg] = 0;
}

static void set_reg_value(struct effect *e, int reg, int v)
{
	if (reg > REG_L)	/* Don't track M etc */
		return;
	e->value[reg] = (v & 0xFF) | VALUE_KNOWN;
}

static int know_reg_value(struct effect *e, int reg)
{
	if (reg > REG_L)	/* Untracked */
		return 0;
	return e->value[reg] & VALUE_KNOWN;
}

static uint16_t pair_value(struct effect *e, int reg)
{
	uint16_t v = reg_value(e, reg) << 8;
	v |= reg_value(e, reg + 1);
	return v;
}

static void set_pair_value(struct effect *e, int reg, int v)
{
	set_reg_value(e, reg + 1, v & 0xFF);
	set_reg_value(e, reg, v >> 8);
}

static int know_pair_value(struct effect *e, int reg)
{
	if (know_reg_value(e, reg) && know_reg_value(e, reg + 1))
		return 1;
	return 0;
}

static int find_reg_value(struct effect *e, int val)
{
	int i;
	if (val == CONST_UNKNOWN)
		return 0;

	for (i = REG_A; i <= REG_L; i++) {
		if (know_reg_value(e, i)
		    && reg_value(e, i) == (val & 0xFF))
			return i;
	}
	return 0;
}

/* For now on a label we require everything is as the compiler put it */
static void invalidate_regs(struct effect *e)
{
	int i;
	for (i = REG_A; i <= REG_L; i++)
		e->value[i] = 0;
	e->need = REGM_ALL;
}

static struct instruction *make_instruction(void)
{
	struct instruction *i = zalloc(sizeof(struct instruction));
	struct effect *e = zalloc(sizeof(struct effect));

	i->next = e;
	e->next = NULL;
	e->prev = i;
	return i;
}

struct instruction *new_instruction(void)
{
	struct instruction *i = make_instruction();

	if (codetail) {
		i->prev = codetail->next;
		codetail->next->next = i;
		codetail = i;
	} else {
		i->prev = &dummy_effect;
		codetail = codehead = i;
	}
	return i;
}

struct instruction *append_instruction(struct instruction *i)
{
	struct instruction *n = make_instruction();
	n->next->next = i->next->next;
	i->next->next = n;
	n->prev = i->next;
	if (n->next->next)
		n->next->next->prev = n->next;
	else
		codetail = n;
	return n;
}

struct label *new_label(void)
{
	return zalloc(sizeof(struct label));
}


/*
 *	This isn't a true syntax decoder. We are parsing the good output
 *	of the compiler in the format it creates, nothing more.
 */

static int DecodeReg8(const char *r)
{
	if (r[1])
		badreg8();
	switch (toupper(r[0])) {
	case 'A':
		return REG_A;
	case 'B':
		return REG_B;
	case 'C':
		return REG_C;
	case 'D':
		return REG_D;
	case 'E':
		return REG_E;
	case 'H':
		return REG_H;
	case 'L':
		return REG_L;
	}
	badreg8();
	exit(1);
}

static int DecodeReg8M(const char *r)
{
	if (r[1])
		badreg8();
	if (r[0] == 'm' || r[0] == 'M')
		return MEM_HL;
	return DecodeReg8(r);
}

static int DecodePair(const char *r)
{
	if (strcasecmp(r, "PSW") == 0)
		return REG_PSW;
	if (strcasecmp(r, "SP") == 0)
		return REG_SP;
	if (r[1])
		badreg16();
	switch (toupper(r[0])) {
	case 'B':
		return REG_B;
	case 'D':
		return REG_D;
	case 'H':
		return REG_H;
	}
	badreg16();
	exit(1);
}

static int DecodeConst(const char *p)
{
	char *t;
	long v = strtol(p, &t, 0);
	if (t == p)
		return CONST_UNKNOWN;
	return v;
}

static void ParseR8Pair(int *sr, int *dr)
{
	char *r = do_strtok(",", "comma expected");
	char *d = do_strtok("", "register or m expected");

	*sr = DecodeReg8M(r);
	*dr = DecodeReg8M(d);

	/* Shouldn't ever see these */
	if (*sr == *dr && *sr == MEM_HL)
		error("invalid move");
}

static void ParseR8Const(int *sr, int *cv)
{
	char *r = do_strtok(",", "comma expected");
	char *d = do_strtok("", "constant expected");
	*sr = DecodeReg8(r);
	*cv = DecodeConst(d);
}

static void ParseR8M(int *sr)
{
	char *r = do_strtok("", "register or m expected");
	*sr = DecodeReg8M(r);
}

static void ParsePair(int *r)
{
	char *p = do_strtok("", "register pair expected");
	*r = DecodePair(p);
}

static void ParsePairConst(int *r, int *c)
{
	char *p = do_strtok(",", "comma expected");
	char *d = do_strtok("", "constant expected");
	*r = DecodePair(p);
	*c = DecodeConst(d);
}

static void ParseConst(int *a)
{
	char *p = do_strtok("", "constant expected");
	*a = DecodeConst(p);
}

static void ParseAddr(int *a)
{
	/* TODO */
	ParseConst(a);
}

/* Given a register pair return the mask of bits it affects */

static int PairMask(int reg)
{
	switch (reg) {
	case REG_B:
		return REGM_B | REGM_C;
	case REG_D:
		return REGM_D | REGM_E;
	case REG_H:
		return REGM_H | REGM_L;
	case REG_PSW:
		return REGM_PSW | REGM_A;
	case REG_SP:
		return REGM_SP;
	}
	error("invalid pair to mask");
	return 0;
}

static void parse_instruction(struct instruction *i)
{
	char *p = strdup(i->op);
	char *op = strtok(p, " \t");
	int l, r;
	struct optab *o;

	if (op == NULL)
		error("label alone not supported");

	/* Should be an 8085 op code but might be meta stuff */
	o = find_operation(op);
	if (o == NULL) {
		fprintf(stderr, "%d: Unknown operation '%s'.\n", linenum,
			op);
		exit(1);
	}

	i->prev->need = o->imask;
	i->next->set = o->omask;
	i->opinfo = o;
	i->opcode = op;		/* FIXME: would be better as a code */

	/* Register to register move, 8 bit */
	if (o->flags & OP_MOV) {
		ParseR8Pair(&l, &r);
		/* We need the source, we set the dest */
		i->prev->need |= (1 << r);
		i->next->set |= (1 << l);
		i->sr = r;
		i->dr = l;
	}
	/* Immediate to register move, 8bit */
	if (o->flags & OP_MVI) {
		ParseR8Const(&l, &r);
		i->next->set = (1 << l);
		i->dr = l;
		i->addrconst = r;
	}
	/* General immediates */
	if (o->flags & OP_IMMED) {
		/* 16bit source/destinations eg lxi */
		if (o->flags & (OP_SPAIR | OP_DPAIR)) {
			ParsePairConst(&l, &r);
			if (o->flags & OP_SPAIR) {
				i->sr = l;
				i->prev->need |= PairMask(l);
			} else {
				i->dr = l;
				i->next->set |= PairMask(l);
				/* For a destination update the constant value */
				if (r != CONST_UNKNOWN)
					set_pair_value(i->next, i->dr, r);
			}
			i->addrconst = r;
		}
		/* Arithmetic/logic op 8bit with immediate */
		if (o->flags & OP_AOP) {
			ParseConst(&l);
			/* This is a simplification. Strictly speaking AND 0, XOR 0 and
			   OR 0xFF don't need the sr */
			i->sr = i->dr = REG_A;
			i->addrconst = l;
		}
	} else {
		/* reg pair as destination eg pop b */
		/* We don't do any stack constant tracking but we probably should */
		if (o->flags & OP_DPAIR) {
			ParsePair(&l);
			i->dr = l;
			i->next->set |= PairMask(l);
		}
		/* reg pair as source - eg push b */
		else if (o->flags & OP_SPAIR) {
			ParsePair(&l);
			i->sr = l;
			/* DAD has an implicit destination */
			if (strcasecmp(op, "DAD") == 0)
				i->dr = REG_H;
			i->prev->need |= PairMask(l);
		}
		/* Arithmetic op without constant */
		else if (o->flags & OP_AOP) {
			ParseR8M(&l);
			i->dr = REG_A;
			i->sr = l;
			i->prev->need |= (1 << l);
		}
	}
	/* Register modify - eg inr a */
	if (o->flags & OP_REGMOD) {
		ParseR8M(&l);
		i->next->set |= (1 << l);
		i->prev->need |= (1 << l);
		i->dr = i->sr = l;
	}
	/* Pair modify - eg inx b */
	if (o->flags & OP_PAIRMOD) {
		ParsePair(&l);
		i->next->set |= PairMask(l);
		i->prev->need |= PairMask(l);
		i->dr = i->sr = l;
	}
	/* Address target */
	if (o->flags & OP_ADDR) {
		ParseAddr(&l);
		/* But do nothing with it yet */
		i->addrconst = l;
	}

	/* Save our direct needs so we can do eliminations easily */
	i->set = i->next->set;
	i->need = i->prev->need;

	/* For now call/branch etc are treated as side effects so we don't
	   remove any */
	if (o->flags & (OP_RET | OP_CALL | OP_BRA))
		i->next->set |= SIDEEFFECTM;
}


/*
 *	Compute the actual register effects of an instruction
 */
static void compute_effects(struct instruction *i)
{
	const char *op = i->opcode;
	int n;

	if (i->opinfo->flags & OP_MOV) {
		/* Propagate known constants */
		if (know_reg_value(i->prev, i->sr))
			set_reg_value(i->next, i->dr, reg_value(i->prev, i->sr));
	}
	if (i->opinfo->flags & OP_MVI)
		set_reg_value(i->next, i->dr, i->addrconst);

	if (i->opinfo->flags & OP_IMMED) {
		if (i->opinfo->flags & OP_AOP)
			set_reg_value(i->next, i->dr, i->addrconst);
		else
			set_pair_value(i->next, i->dr, i->addrconst);
	}
		
	/* Calculate the stack/frame offset */
	if (strcasecmp(op, "PUSH") == 0)
		if (i->spbias != BIAS_UNKNOWN)
			i->spbias += 2;
	if (strcasecmp(op, "POP") == 0) {
		if (i->spbias != BIAS_UNKNOWN)
			i->spbias -= 2;
		if (i->spbias < 0)
			error("negative frame bias");
	}
	if (strcasecmp(op, "INX") == 0 && i->dr == REG_SP) {
		if (i->spbias != BIAS_UNKNOWN)
			i->spbias--;
	}
	if (strcasecmp(op, "DEX") == 0 && i->dr == REG_SP) {
		if (i->spbias != BIAS_UNKNOWN)
			i->spbias++;
	}
	/*
	 *  This next block looks for the cases that the stack pointer is adjusted
	 *  using LXI H,nn; DAD SP; SPHL
	 */
	if (strcasecmp(op, "DAD") && i->sr == REG_SP) {
		if ((i->prev->value[REG_H] & VALUE_KNOWN) &&
		    (i->prev->value[REG_L] & VALUE_KNOWN)) {
			/* We are tracking a dad sp / lxi sp set */
			i->next->flags |= HL_SPBIAS;
			/* 16bit signed */
			i->next->spbias = pair_value(i->prev, REG_H);
			/* FIXME: we need to propogate this down so maybe do this
			   logic later. It's ok for now as the DAD SPHL are paired */
		}
	}
	if (strcasecmp(op, "SPHL") && i->dr == REG_SP
	    && i->spbias != BIAS_UNKNOWN) {
		if (i->prev->flags & HL_SPBIAS)
			i->spbias += (int16_t) (i->prev->spbias & 0xFFFF);
		else
			i->spbias = BIAS_UNKNOWN;
	}

	/* General operation tracking. Simple for now as we don't try to tackle
	   flag, stack, label or memory tracking at all */

	for (n = REG_A; n <= REG_L; n++) {
		if (!(i->next->set & (1 << n))) {
			if (know_reg_value(i->prev, n)) {
				set_reg_value(i->next, n,
					      reg_value(i->prev, n));
			}
		}
	}

	/* TODO: when we know the result we should consider swapping
	   a lot of these for loads and adjusting the prev->need so we
	   can run a second elimination pass ? */
	/* INC and DEC */
	if (strcasecmp(op, "DCR") == 0 && know_reg_value(i->prev, i->dr))
		set_reg_value(i->next, i->dr,
			      (reg_value(i->prev, i->dr) - 1) & 0xFF);
	if (strcasecmp(op, "INR") == 0 && know_reg_value(i->prev, i->dr))
		set_reg_value(i->next, i->dr,
			      (reg_value(i->prev, i->dr) + 1) & 0xFF);
	if (strcasecmp(op, "DCX") == 0 && know_pair_value(i->prev, i->dr))
		set_pair_value(i->next, i->dr,
			       (pair_value(i->prev, i->dr) - 1) & 0xFFFF);
	if (strcasecmp(op, "INX") == 0 && know_pair_value(i->prev, i->dr))
		set_pair_value(i->next, i->dr,
			       (pair_value(i->prev, i->dr) + 1) & 0xFFFF);

	/* Logic: mostly to deal with XRA A */
	if (strcasecmp(op, "ANA") == 0 && know_reg_value(i->prev, i->sr)
	    && know_reg_value(i->prev, REG_A))
		set_reg_value(i->next, i->dr,
			      reg_value(i->prev,
					REG_A) & reg_value(i->prev,
							   i->sr));
	if (strcasecmp(op, "ORA") == 0 && know_reg_value(i->prev, i->sr)
	    && know_reg_value(i->prev, REG_A))
		set_reg_value(i->next, i->dr,
			      reg_value(i->prev,
					REG_A) | reg_value(i->prev,
							   i->sr));
	/* XRA A is sort of special. Handle it as a mvi of 0 */
	if (strcasecmp(op, "XRA") == 0 && i->sr == REG_A) {
		i->prev->need &= ~REG_A;
		set_reg_value(i->next, REG_A, 0);
	}
	if (strcasecmp(op, "XRA") == 0 && know_reg_value(i->prev, i->sr)
	    && know_reg_value(i->prev, REG_A))
		set_reg_value(i->next, i->dr,
			      reg_value(i->prev,
					REG_A) ^ reg_value(i->prev,
							   i->sr));
	/* Maths: not yet with carry tracking */
	if (strcasecmp(op, "ADA") == 0 && know_reg_value(i->prev, i->sr)
	    && know_reg_value(i->prev, REG_A))
		set_reg_value(i->next, i->dr,
			      reg_value(i->prev,
					REG_A) + reg_value(i->prev,
							   i->sr));
	if (strcasecmp(op, "SBA") == 0 && know_reg_value(i->prev, i->sr)
	    && know_reg_value(i->prev, REG_A))
		set_reg_value(i->next, i->dr,
			      reg_value(i->prev,
					REG_A) - reg_value(i->prev,
							   i->sr));

	if (i->addrconst != CONST_UNKNOWN) {
		if (strcasecmp(op, "ANI") == 0
		    && know_reg_value(i->prev, REG_A))
			set_reg_value(i->next, i->dr,
				      reg_value(i->prev,
						REG_A) & i->addrconst);
		if (strcasecmp(op, "ORI") == 0
		    && know_reg_value(i->prev, REG_A))
			set_reg_value(i->next, i->dr,
				      reg_value(i->prev,
						REG_A) | i->addrconst);
		if (strcasecmp(op, "XRI") == 0
		    && know_reg_value(i->prev, REG_A))
			set_reg_value(i->next, i->dr,
				      reg_value(i->prev,
						REG_A) ^ i->addrconst);
		if (strcasecmp(op, "ADI") == 0
		    && know_reg_value(i->prev, REG_A))
			set_reg_value(i->next, i->dr,
				      reg_value(i->prev,
						REG_A) + i->addrconst);
		if (strcasecmp(op, "SUI") == 0
		    && know_reg_value(i->prev, REG_A))
			set_reg_value(i->next, i->dr,
				      reg_value(i->prev,
						REG_A) - i->addrconst);
	}
	/* 16bit add */
	if (strcasecmp(op, "DAD") == 0 && know_pair_value(i->prev, REG_H)
	    && know_pair_value(i->prev, i->sr))
		set_pair_value(i->next, REG_H,
			       pair_value(i->prev,
					  REG_H) + pair_value(i->prev,
							      i->sr));
	/* Might be worth doing rotates and complement FIXME */
}

static void compute_values(void)
{
	struct instruction *i = codehead;
	/* e tracks the previous live registers known, as we need to
	   ignore any dead stuff when we copy them through */
	while (i) {
		int n;
		compute_effects(i);
		/* We assume everything at a label is unknown because we can't know
		   the callers */
		if (i->label == NULL) {
			/* Propagate known values */
			for (n = REG_A; n <= REG_L; n++) {
				if (!(i->next->set & (1 << n))) {
					if (know_reg_value(i->prev, n))
						set_reg_value(i->next, n, reg_value(i->prev, n));
				}
				/* Worth debug checks here if i->next->set is clear but value
				   already known as it shouldn't happen ?? */
			}
		}
		i = i->next->next;
	}
}

static void make_op(struct instruction *i, const char *m)
{
	char *p = zalloc(8);
	sprintf(p, "%s %c,%c", m, regname(i->dr), regname(i->sr));
	i->op = p;
	i->opcode = m;
	i->opinfo = find_operation(m);
}

static void make_op1(struct instruction *i, const char *m)
{
	char *p = zalloc(8);
	sprintf(p, "%s %c", m, regname(i->dr));
	i->op = p;
	i->opcode = m;
	i->opinfo = find_operation(m);
}

static void make_op2_r(struct instruction *i, const char *m, int rd, int rs)
{
	char *p = zalloc(8);
	sprintf(p, "%s %c,%c", m, regname(rd), regname(rs));
	i->op = p;
	i->opcode = m;
	i->opinfo = find_operation(m);
}

/* The caller is responsible for fixing up the register values resulting
   in any split: see adjust_immed16() */
static struct instruction *add_op1(struct instruction *i, const char *m)
{
	struct instruction *n = append_instruction(i);
	make_op1(n, m);
	compute_effects(i);
	compute_effects(n);
	i->next->need = i->prev->need & ~i->next->set;
	n->next->need = n->prev->need & ~n->next->set;
	return n;
}

static struct instruction * add_op2_r(struct instruction *i, const char *m, int rd, int rs)
{
	struct instruction *n = append_instruction(i);
	make_op2_r(n, m, rd, rs);
	compute_effects(i);
	compute_effects(n);
	i->next->need = i->prev->need & ~i->next->set;
	n->next->need = n->prev->need & ~n->next->set;
	return n;
}

static void eliminate_instruction(struct instruction *i)
{
	struct instruction *p;

	printf("Eliminate %p %p\n", (void *)i, (void *)i->prev->prev);
	/* Find the previous live instruction */
	p = i->prev->prev;

	/* Unlink ourself but keep our own pointers valid */
	if (p) {
		p->next->next = i->next->next;
		i->next->next->prev = p->next;
	} else {
		i->next->next->prev = &dummy_effect;
	}
	if (codehead == i)
		codehead = i->next->next;

	
	printf("Eliminating %s\n", i->op);
	i->set = 0;
	i->dead = 1;
	i->prev->need = i->next->need;

#if 0
	/* We are dead, so any values we know are the values our predecessor
	   knew because we changed nothing - unless they knew because we set
	   them */
	for (n = REG_A; n <= REG_L; n++) {
		if (know_reg_value(p->next, n) && !(i->next->set & (1 << n))) {
			set_reg_value(i->next, n, reg_value(i->prev, n));
		} else {
			clear_reg_value(i->next, n);
		}
	}
#endif	
	/* We are now a do nothing */
	i->next->set = 0;
}

/* We should do this for all the 8bit immediates. We don't bother looking
   for mov a,0 because the compiler is smart enough already */
static void adjust_immed8(void)
{
	struct instruction *i = codehead;
	int r;
	while (i) {
		int kdr = know_reg_value(i->prev, i->dr);
		/* Remove anybody who loads a preloaded value */
		/* Use icr/dcr otherwise - should be safe but needs review
		   of compiler rules. We might need to flag this with
		   a PSW check but I don't think ack generates delayed
		   conditionals this way */
		if ((i->opinfo->flags & OP_MVI) && kdr) {
			uint8_t v = reg_value(i->prev, i->dr);
			if (v == (uint8_t)i->addrconst)
				eliminate_instruction(i);
			else if (v == (uint8_t)(i->addrconst + 1))
				make_op1(i, "DCR");
			else if (v == (uint8_t)(i->addrconst - 1))
				make_op1(i, "INR");
		}
		if (i->opinfo->flags & OP_MOV) {
			if (know_reg_value(i->prev, i->dr) &&
			    know_reg_value(i->prev, i->sr) && 
				reg_value(i->prev, i->dr) == reg_value(i->prev, i->sr)) {
				eliminate_instruction(i);
			}
		}
		/* For each 8bit operation with an immediate source look to see if
		   we can find the value lurking in a register. For 0, 1 and 255 at
		   least it's got a fair chance of being there somewhere */
		else if (((i->opinfo->flags & (OP_IMMED | OP_AOP)) ==
		     (OP_IMMED | OP_AOP)) || (i->opinfo->flags & OP_MVI)) {
//			printf("Candidate %s want %d\n", i->op,
//			       i->addrconst);
			r = find_reg_value(i->prev, i->addrconst);
			if (r) {
				i->sr = r;
				/* Convert to normal op from immediate */
				i->opinfo--;
				make_op(i, i->opinfo->op);
			}
		}
		i = i->next->next;
	}
}

/* For 16bit we really need labels sorted so we can spot label relative
   inx/dex for fixup and 16bit duplicates. For now we can only do numeric
   constants */
static void adjust_immed16(void)
{
	struct instruction *i = codehead;
	struct instruction *n;
	while (i) {
		int kdr = know_pair_value(i->prev, i->dr);
		/* Optimise LXI if we can */
		if (strcasecmp(i->opcode, "LXI") == 0 && i->addrconst != CONST_UNKNOWN ) {
			uint16_t v;
			if (kdr)
				v = reg_value(i->prev, i->dr);

			if (kdr && v == (uint8_t)i->addrconst)
				eliminate_instruction(i);
			else if (kdr && v == (uint16_t)(i->addrconst + 1))
				make_op1(i, "DEX");
			else if (kdr && v == (uint16_t)(i->addrconst - 1))
				make_op1(i, "INX");
			else if (kdr && v == (uint16_t)(i->addrconst + 2)) {
				make_op1(i, "DEX");
				set_pair_value(i->next, REG_H, pair_value(i->prev, REG_H) - 1);
				n = add_op1(i, "DEX");
				set_pair_value(n->next, REG_H, pair_value(i->prev, REG_H) - 1);
			} else if (kdr && v == (uint16_t)(i->addrconst - 2)) {
				make_op1(i, "INX");
				set_pair_value(i->next, REG_H, pair_value(i->prev, REG_H) + 1);
				n = add_op1(i, "INX");
				set_pair_value(n->next, REG_H, pair_value(i->prev, REG_H) + 1);
			} else {
				/* Look for our register values in a pair of others.
				   It's only a win if they are both present and we are
				   not exchanging halves with ourself */
				int rl = find_reg_value(i->prev, i->addrconst & 0xFF);
				int rh = find_reg_value(i->prev, i->addrconst >> 8);
				/* We get in a mess if we want to load de from ed */
				if (rl && rh && !(rl == i->dr && rh == i->dr + 1)) {
					/* If the low part is in the register
					   we are setting up do it first */
					if (rl == i->dr || rl == i->dr + 1) {
						make_op2_r(i, "MOV", i->dr+1, rl);
						set_reg_value(i->next, i->dr+1, i->addrconst & 0xFF);
						clear_reg_value(i->next, i->dr);
						n = add_op2_r(i, "MOV", i->dr, rh);
						set_pair_value(n->next, i->dr, i->addrconst);
					} else {
						make_op2_r(i, "MOV", i->dr, rh);
						n = add_op2_r(i, "MOV", i->dr+1, rl);
						set_reg_value(i->next, i->dr+1, i->addrconst & 0xFF);
						clear_reg_value(i->next, i->dr);
						set_pair_value(n->next, i->dr, i->addrconst);
					}
				}
			}
		}
		else if (strcasecmp(i->opcode, "DAD") == 0 && know_pair_value(i->prev, i->sr)) {
			/* Not much to say here. At this level we don't
			   eliminate constant maths but we can fix up
			   DAD to INX and DEX */
			uint16_t v = pair_value(i->prev, i->sr);
			/* Need to review these for flags */
			if (!(i->next->need & REG_PSW)) {
				if (v == 0)
					eliminate_instruction(i);
				if (v == 1)
					make_op1(i, "INX");
				if (v == -1)
					make_op1(i, "DEX");
				if (v == 2) {
					make_op1(i, "INX");
					set_pair_value(i->next, REG_H, pair_value(i->prev, REG_H) + 1);
					n = add_op1(i, "INX");
					set_pair_value(n->next, REG_H, pair_value(i->prev, REG_H) + 1);
				}
				if (v == -2) {
					make_op1(i, "DEX");
					set_pair_value(i->next, REG_H, pair_value(i->prev, REG_H) - 1);
					n = add_op1(i, "DEX");
					set_pair_value(n->next, REG_H, pair_value(i->prev, REG_H) - 1);
				}
			}
		}
		i = i->next->next;
	}
}


/* We walk backwards to propagate need values. A need is copied back until
   a set for it is found. We have some artificial needs on call/ret etc so
   that we don't optimize out stuff like return values
   
   A value is needed if it was needed by the instruction after and not
   set by this one. It may still be needed even if set because the set may be
   an operation depending upon it (eg inr a) */

static void propagate_need(void)
{
	struct instruction *i = codetail;

	while (i) {
		/* Can we eliminate the instruction we are considering ? */
		/* If it has no side effects and we don't need any of its outputs
		   kill it off */
		if (!(i->next->need & i->next->set)
		    && !(i->next->set & KEEPMASK))
			eliminate_instruction(i);
		else {
			/* If not propagate the requirements it had */
//			printf("%s: need was %x now ", i->op,
//			       i->prev->need);
			i->prev->need |= i->next->need & ~i->next->set;
//			printf("%x\n", i->prev->need);
		}
		i = i->prev->prev;
	}
}



/* FIXME: parse ; as statement separator */
static void parse_line(char *p)
{
	struct instruction *i;
	struct label *l;

	char *x = strchr(p, '!');
	char *lab = NULL;
	/* Strip comment */
	if (x)
		*x = 0;
	x = p;
	/* Look for a label */
	while (*x && *x != '\'' && *x != '\"') {
		if (*x == ':') {
			*x++ = 0;
			lab = p;
			p = x;
			break;
		}
		x++;
	}
	/* Now deal with the post label stuff */
	x = p;
	while (*x && isspace(*x))
		x++;

	/* FIXME: support label only lines */
	if (*x == 0 && !lab)
		return;

	i = new_instruction();
	i->op = x;
	if (lab) {
		l = new_label();
		l->name = lab;
		l->next = NULL;
		l->instruction = i;
		i->label = l;
		/* TODO: for now take the simple approach - any label invalidates
		   all known values. We can improve on this later */
		invalidate_regs(i->prev);
	}
	parse_instruction(i);
}

/* Debug for now asm output eventually */
static void dump_output(void)
{
	struct instruction *i = codehead;
	while (i) {
		if (i->dead)
			printf("---- BEGIN DEAD ----\n");
		print_regmap(i->prev->need);
		printf("\n");
		if (i->label)
			printf("%s:", i->label->name);
		printf("%s\n", i->op);
		print_regmap(i->next->set);
		printf("\n");
		print_values(i->next);
		printf("\n");

		if (i->dead)
			printf("----  END DEAD  ----\n");
		
		i = i->next->next;
	}
}

static void load_file(FILE * fp)
{
	char buf[512];

	while (fgets(buf, 511, stdin)) {
		char *p = buf;
		char *x;

		linenum++;

		while (*p && isspace(*p))
			p++;
		if (*p == 0)
			continue;
		x = strchr(p, '\n');
		if (x)
			*x = 0;
		p = strdup(p);
		if (p == NULL) {
			fprintf(stderr, "Out of memory.\n");
			exit(1);
		}
		parse_line(p);
	}
}

int main(int argc, char *argv[])
{
	load_file(stdin);
	/* Join all the labels together */
	/* TODO link_labels(); */
	/* Set the need flags so we can do unused elimination */
	printf("Propagate:\n");
	propagate_need();
	/* Simple constant propagation */
	printf("Values:\n");
	compute_values();
	/* Constant loads to register for 8bit operations */
	printf("Immed8:\n");
	adjust_immed8();
	printf("Immed16:\n");
	adjust_immed16();
	/* Look for assignments we can move about and make into pair loads */
	/* TODO move_assignments(); */
	/* Check our fp/sp biasing model is consistent */
	/* TODO validate_spbias(); */
	/* Replace the 8080 helpers with ldsi/lhlx */
	/* TODO eliminate_helpers(); */
	/* Look for cases we can use ldhi ? */
	printf("Dump:\n");
	dump_output();
	return 0;
}
