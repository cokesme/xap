#include <stdio.h>
#include <regs.h>
#include "cbit/htab.h"
#include "cbit/vec.h"
#include "cbit/str.h"
#include "cbit/misc.h"

/*
    pperf --prefix=insns -nul xas-tokens.txt
    pperf --prefix=regs -nul xas-regs.txt
*/
static inline long insns_lookup(const char *, int);
#include "insns.c"
static inline long regs_lookup(const char *, int);
#include "regs.c"

enum {
    MAX_REG_LEN = 5,
    MAX_INSN_LEN = 6,
    MAX_DOT_LEN = 4,
};

enum insn {
    insn_add, //
    insn_addc, //
    insn_and, //
    insn_asl, //
    insn_asr, //
    insn_bc,
    insn_bc2,
    insn_bcc,
    insn_bcs,
    insn_bcz,
    insn_beq,
    insn_bge,
    insn_bgt,
    insn_blt,
    insn_bmi,
    insn_bne,
    insn_bpl,
    insn_bra,
    insn_brk,
    insn_brxl,
    insn_bsr,
    insn_cmp, //
    insn_dc,
    insn_ds,
    insn_enter, //
    insn_enterl, //
    insn_ld,
    insn_leave, //
    insn_leavel, //
    insn_lsl, //
    insn_lsr, //
    insn_nadd, //
    insn_nop, //
    insn_or, //
    insn_print, //
    insn_rol,
    insn_ror,
    insn_rti,
    insn_rts,
    insn_sdiv,
    insn_sif,
    insn_sleep,
    insn_smult,
    insn_st,
    insn_reg,
    insn_sub, //
    insn_subc, //
    insn_tst,
    insn_udiv,
    insn_umult,
    insn_xor, //
    insn_module,
    insn_org,
    insn_endmod,
};

enum reg {
    reg_ah, reg_al, reg_x, reg_y,
    reg_uxh, reg_uxl, reg_uy,
    reg_ixh, reg_ixl, reg_iy,
    reg_flags, reg_pch, reg_pcl,
};

#define ensure cbit_serious_assert
#define unreachable() ensure(false)
#define STR_FMT_ARG(s) (int) (s)->name.length, (s)->name.els

enum seg { CODE_SEG, DATA_SEG, UNK_SEG };
enum { NUM_SEGS = 2 };

struct sym {
    str name;
    enum seg seg;
    size_t chunk_idx;
    int first_use_lineno;
};

static size_t str_hash(const str *s) {
    uint32_t h = 0x811c9dc5;
    for (size_t i = 0; i < s->length; i++)
        h = (h ^ s->els[i]) * 0x01000193;
    return h;
}
#define strp_null(s) (!*(s))
#define strp_hash(s) str_hash(*(s))
#define strp_eq(a, b) str_eq(*(a), *(b))

DECL_STATIC_HTAB_KEY(strp, str *, strp_hash, strp_eq, strp_null, 0)
struct empty {};
DECL_HTAB(sym, strp, struct empty);
DECL_VEC(struct sym *, symp);

enum operator {
    OP_CONST, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_SYM,
    OP_PAREN = OP_CONST, /* for parsing */
};
static uint8_t precedence[5] = {99, 2, 2, 1, 1};
DECL_VEC(enum operator, operator);

struct expr {
    enum operator op;
    union {
        int32_t k;
        struct {
            struct expr *l;
            struct expr *r;
        } bin;
        struct sym *sym;
    } u;
}
DECL_VEC(struct expr, expr);

struct chunk {
    int lineno;
    struct vec_uint16_t before;
    enum {
        CODE_RELOC_CHUNK,
        DATA_RELOC_CHUNK,
        SYM_CHUNK,
        SWITCH_CHUNK,
        ORG_CHUNK,
    } type;
    union {
        struct {
            uint8_t max_known_size;
            uint8_t base;
            struct expr target;
        } reloc;
        struct {
            uint32_t last_known_addr;
        } sym;
        struct {
            uint32_t addr;
        } org;
    } u;
};
DECL_VEC(uint16_t, uint16_t);
DECL_VEC(struct chunk, chunk);

struct as {
    const char *read_cursor, *read_end;
    const char *filename;
    uint32_t lineno;
    enum seg seg;
    bool have_err;

    struct htab_sym global_symtab, local_symtab;
    struct vec_symp syms_to_free;

    struct vec_uint16_t cur;

    struct seg_info {
        struct vec_chunk chunks;
    } seg_info[2];
};

static void err(struct as *as, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "ERROR [%s:%d]: ", as->filename, as->lineno);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    as->have_err = true;
}

static void err_noline(struct as *as, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    as->have_err = true;
}

static inline bool
is_local_sym_name(const str *name) {
    char first = name->els[0];
    return first == '?' || first == '^';
}

static inline struct sym *
find_sym(struct as *as, const str *name, bool local) {
    struct htab_sym *ht = local ? &as->local_symtab : &as->global_symtab;
    struct htab_bucket_sym *bucket = htab_setbucket_sym(ht, &name);
    if (!bucket->key) {
        struct sym *sym = malloc(sizeof(*sym));
        sym->name = *name;
        sym->seg = UNK_SEG;
        sym->first_use_lineno = as->lineno;
        bucket->key = &sym->name;
    }
    return (struct sym *) bucket->key;
}


static struct chunk *make_chunk(struct as *as) {
    struct chunk *cc = vec_appendp(&as->seg_info[as->seg].chunks);
    cc->lineno = as->lineno;
    cc->before = as->cur;
    VEC_INIT(&as->cur);
    return cc;
}

static void emit_code_with_ref(struct as *as, uint8_t base, struct expr ref) {
    struct chunk *cc = make_chunk(as);
    cc->type = CODE_RELOC_CHUNK;
    cc->u.reloc.base = base;
    cc->u.reloc.max_known_size = 1;
    cc->u.reloc.target = ref;
}

static void emit_code(struct as *as, uint16_t code) {
    vec_uint16_t_append(&as->cur, code);
}

static inline char peek(const struct as *as) {
    return *as->read_cursor;
}
static inline void advance(struct as *as) {
    ensure(as->read_cursor != as->read_end)
    assert(*as->read_cursor != '\n');
    as->read_cursor++;
}

static bool read_int(struct as *as, int32_t *out) {
    bool neg = false;
    char c;
    uint32_t val;
    while (1)
    switch ((c = peek(as))) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9': {
            // decimal
            val = c - '0';
            while (1) {
                char d = peek(as);
                if (!(d >= '0' && d <= '9'))
                    goto end;
                if (val > INT32_MAX/10) {
                    err(as, "numeric overflow");
                    return false;
                }
                advance(as);
                val = (val * 10) + (d - '0');
            }
            unreachable();
        }
        case 'H': {
            advance(as);
            if (peek(as) != '\'')
                return false;
            advance(as);
            // hex
            val = 0;
            bool got_any = false;
            while (1) {
                char d = peek(as);
                uint32_t charval;
                switch (d) {
                    case '0': case '1': case '2': case '3': case '4':
                    case '5': case '6': case '7': case '8': case '9':
                        charval = d - '0';
                        break;
                    case 'a': case 'b': case 'd': case 'd': case 'e': case 'f':
                        charval = 10 + (d - 'a');
                        break;
                    case 'A': case 'B': case 'd': case 'D': case 'E': case 'F':
                        charval = 10 + (d - 'A');
                        break;
                    default:
                        if (!got_any) {
                            err(as, "unexpected character '%d'", d);
                            return false;
                        }
                        goto end;
                }
                advance(as);
                if (val > INT32_MAX/16) {
                    err(as, "numeric overflow");
                    return false;
                }
                val = (avl * 16) + charval;
            }
            unreachable();
        }

        case '-':
            if (neg) {
                err(as, "double negative");
                return false;
            }
            neg = true;
            break;

        default:
            err(as, "unexpected character '%c'", c);
            return false;
    }
    unreachable();
end:
    if (neg)
        val = -val;
    *out = val;
    return true;
}

static inline void skip_line(struct as *as) {
    do { advance(as); c = peek(as); } while (c != '\n' && c != '\0');
}

static void skip_white(struct as *as) {
    while (1) {
        char c = peek(as);
        switch (c) {
            case ';':
                skip_line(as);
                break;
            case '\\':
                advance(as);
                if (peek(as) != '\n') {
                    as->read_cursor--;
                    return;
                }
                advance(as);
                as->lineno++;
                break;
            case ' ': case '\t':
                advance(as);
                break;
            default:
                return;
        }
    }
}

static str read_token(struct as *as) {
    str it = str_borrow(as->read_cursor, 0);
    while (1) {
        char c = peek(as);
        switch (c) {
            case ' ':
            case '\t':
            case '\n':
            case '\0':
            case ',':
            case '(': case ')':
            case '+': case '-': case '*': case '/':
            case '-':
                goto end;
            case '\\':
                advance(as);
                if (peek(as) == '\n') {
                    as->read_cursor--;
                    goto end;
                }
                it.length++;
                break;

            default:
                advance(as);
                it.length++;
                break;
        }
    }
end:
    skip_white(as);
    return it;
}

static void clear_symtab(struct htab_sym *ht) {
    htab_free_storage_sym(ht);
}

static bool parse_reg(struct as *as, enum reg *regp) {
    str tok = read_token(as);
    if (tok.length == 0) {
        err(as, "unexpected character");
        return false;
    }
    if (tok.length > MAX_REG_LEN)
        goto unk;
    char reg[MAX_REG_LEN+1];
    for (size_t i = 0; i < tok.length; i++)
        reg[i] = tolower(tok.els[i]);
    reg[tok.length] = 0;
    long which = regs_lookup(insn, tok.length);
    if (which == -1) {
        unk:
        err(as, "unknown reg %.*s", STR_FMT_ARG(&tok));
        return false;
    }
    *regp = (enum reg) which;
    return true;
}

static void expr_free(struct expr *expr) {
    switch (expr->op) {
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_DIV:
            expr_free(expr->u.bin.l);
            free(expr->u.bin.l);
            expr_free(expr->u.bin.r);
            free(expr->u.bin.r);
            break;
    }
}

static bool parse_expr(struct as *as, bool *is_indexed_p, enum reg *regp, struct expr *indexp) {
#define POP_EXPR(oper) do { \
    struct expr e, *lp = malloc(sizeof(*lp)), *rp = malloc(sizeof(*rp)); \
    ensure(output.length >= 2); \
    *lp = output.els[output.length - 2]; \
    *rp = output.els[output.length - 1]; \
    e.op = (oper); \
    e.u.bin.l = lp; \
    e.u.bin.r = rp; \
    output.els[output.length - 2] = e; \
    output.length--; \
    opers.length--; \
} while(0)
    *is_indexed_p = false;
    bool ret = false;
    struct vec_storage_expr output = VEC_STORAGE_INITER(&output, expr);
    struct vec_storage_operator opers = VEC_STORAGE_INITER(&opers, operator);
    int nparens = 0;
    {
        expect_oper:
        char c = peek(as);
        enum operator oper;
        switch (c) {
            case '+': oper = OP_ADD; break;
            case '-': oper = OP_SUB; break;
            case '*': oper = OP_MUL; break;
            case '/': oper = OP_DIV; break;
            case ',':
                advance(as);
                skip_white(as);
                goto expect_reg;
            default:
                err(as, "expected operator");
                goto end;
        }
        advance(as);
        skip_white(as);
        int my_prec = precedence[oper];
        enum operator last_oper;
        while (opers.length > 0 &&
               (last_oper = opers.els[opers.length - 1],
                my_prec >= precedence[last_oper])) {
            POP_EXPR(last_oper);
        }
        vec_operator_append(&opers, oper);
        goto expect_opnd;
    }
    {
        expect_opnd:
        char c = peek(as);
        switch (c) {
            case '\0':
            case '\n':
                goto ok;

            case 'H':
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9': {
                int32_t val;
                if (!read_int(as, &val))
                    goto end;
                struct expr e = {
                    .op = OP_CONST,
                    .u.k = val,
                };
                vec_expr_append(&output, e);
                goto expect_oper;
            }

            case '(':
                advance(as);
                skip_white(as);
                vec_operator_append(&opers, OP_PAREN);
                nparens++;
                goto expect_opnd;

            case ')': {
                close_paren:
                advance(as);
                skip_white(as);
                bool got_any = false;
                while (1) {
                    if (opers.length == 0) {
                        err(as, "unbalanced close paren");
                        goto end;
                    }
                    enum operator last_oper;
                    if ((last_oper = opers.els[opers.length - 1] )
                        == OP_PAREN) {
                        if (!got_any) {
                            err(as, "empty parens");
                            goto end;
                        }
                        opers.length--;
                        nparens--;
                        goto expect_oper;
                    }
                    POP_EXPR(last_oper);
                }
            }

            default: {
                str tok = read_token(as);
                struct sym *sym = find_sym(as, &tok, is_local_sym_name(&tok));
                struct expr e = {
                    .op = OP_SYM,
                    .u.sym = sym,
                };
                vec_expr_append(&output, e);
                goto expect_oper;
            }
        }
    }
    {
        expect_reg:
        if (nparens != 1) {
            err(as, "wrong number of parens for comma");
            goto end;
        }
        if (!parse_reg(as, regp))
            goto end;
        *is_indexed_p = true;
        if (peek(as) != ')') {
            err(as, "extraneous stuff after register");
            goto end;
        }
        goto close_paren;
    }
    {
        ok:
        while (output.length >= 2)
            POP_EXPR(opers.els[opers.length - 1]);
        *indexp = opers.els[0];
        opers.length = 0;
        ret = true;
        goto end;
    }
end:
    skip_line(as);
    vec_free_storage_operator(&opers);
    VEC_FOREACH(&output, size_t i, struct expr *expr)
        expr_free(expr);
    vec_free_storage_expr(&output);
    return ret;
#undef PARSE_EXPR
}

static void handle_data_op(struct as *as, uint8_t base, bool no_imm) {
    char c = peek(as);
    enum reg reg;
    bool is_indexed;
    struct expr index;
    if (c == '@') {
        advance(as);
        skip_white(as);
        if (!parse_expr(as, &is_indexed, &reg, &index))
            return;
        if (is_indexed) {
            if (reg != reg_x && reg != reg_y) {
                err(as, "indexed arg base reg must be x or y");
                expr_free(&index);
                return;
            }
            base |= reg == reg_x ? 2 : 3;
        } else {
            base |= 1;
        }
        emit_code_with_ref(as, base, index);
    } else if (c == '#') {
        advance(as);
        skip_white(as);
        struct expr imm;
        if (!parse_expr(as, &is_indexed, &reg, &index))
            return;
        if (is_indexed) {
            err(as, "can't have indexed immediate");
            expr_free(&index);
            return;
        }
        emit_code_with_ref(as, base | 0, imm);
    } else if (parse_reg(as, &reg)) {
        uint8_t addr_hi;
        switch (reg) {
            case reg_ah:    addr_hi = 0xe0; break;
            case reg_al:    addr_hi = 0xe1; break;
            case reg_uxh:   addr_hi = 0xe2; break;
            case reg_uxl:   addr_hi = 0xe3; break;
            case reg_uy:    addr_hi = 0xe4; break;
            case reg_ixh:   addr_hi = 0xe5; break;
            case reg_ixl:   addr_hi = 0xe6; break;
            case reg_iy:    addr_hi = 0xe7; break;
            case reg_flags: addr_hi = 0xe8; break;
            case reg_pch:   addr_hi = 0xe9; break;
            case reg_pcl:   addr_hi = 0xea; break;
            default:
                err(as, "bad second argument");
                return;
        }
        emit_code(as, base | 1 | (addr_hi << 8));
    }
}

static void parse_insn(struct as *as, str *tok) {
    if (tok.length > MAX_INSN_LEN)
        goto unknown_insn;
    char insn[MAX_INSN_LEN+1];
    for (size_t i = 0; i < tok->length; i++)
        insn[i] = tolower(tok->els[i]);
    insn[tok->length] = 0;
    long which = insns_lookup(insn, tok->length);
    enum reg reg;
    uint8_t base;
    switch (which) {
        case -1:
        unknown_insn:
            err(as, "unknown instruction %.*s", STR_FMT_ARG(tok));
            goto bad;
        case insn_add:  base = 0x30; goto two_op;
        case insn_addc: base = 0x40; goto two_op;
        case insn_sub:  base = 0x50; goto two_op;
        case insn_subc: base = 0x60; goto two_op;
        case insn_nadd: base = 0x70; goto two_op;
        case insn_cmp:  base = 0x80; goto two_op;
        case insn_or:   base = 0xb0; goto two_op;
        case insn_and:  base = 0xc0; goto two_op;
        case insn_xor:  base = 0xd0; goto two_op;

        two_op:
            if (!parse_reg(as, &reg) ||
                reg > reg_y) {
                err(as, "first argument must be ah/al/x/y");
                goto bad;
            }
            base |= reg << 2;
            if (!expect_plus_white(as, ','))
                goto bad;
            handle_data_op(as, base, /* no_imm */ false);
            return;

        case insn_lsl:  UNSIGNED(); /* fallthrough */
        case insn_asl:  base = 0xa0; goto one_op;
        case insn_lsr:  UNSIGNED(); /* fallthrough */
        case insn_asr:  base = 0xa4; goto one_op;

        one_op:
            handle_data_op(as, base, /* no_imm */ false);
            return;

        case insn_ld:
        case insn_st:
            if (!parse_reg(as, &reg))
                goto ldst_bad_first;
            if (!expect_plus_white(as, ','))
                goto bad;
            switch (reg) {
                case reg_ah:
                case reg_al:
                case reg_x:
                case reg_y:
                    base |= reg << 2;
                    handle_data_op(as, base, /* no_imm */ which == insn_st);
                    return;
                case reg_flags: base = 1;  goto special_ldst;
                case reg_ux:    base = 2;  goto special_ldst;
                case reg_uy:    base = 3;  goto special_ldst;
                case reg_xh:    base = 0xa; goto special_ldst;
                special_ldst: {
                    if (which == insn_ld)
                        base |= 4;
                    enum reg reg2;
                    struct expr index;
                    bool is_indexed;
                    if (!expect_plus_white(as, '@')) {
                        err(as, "second arg must be @(_, y)";
                        goto bad;
                    }
                    if (!parse_expr(as, &is_indexed, &reg2, &index))
                        goto bad;
                    if (!is_indexed || reg2 != reg_y) {
                        err(as, "second arg must be @(_, y)";
                        goto bad;
                    }
                    emit_code_with_ref(as, base, index);
                    return;
                }

                default:
                ldst_bad_first:
                    err(as, "first argument must be ah/al/x/y or flags/ux/uy/xh");
                    goto bad;
            }
            switch (reg) {
                err(as, "first argument must be ah/al/x/y");
                goto bad;
            }

        case insn_nop:  emit_code(as, 0x0000); return;

        case insn_print:
            err(as, "print not supported");
            goto bad;

        case insn_enter:  base = 0x0b; neg = false; goto enter_leave;
        case insn_enterl: base = 0x0b; neg = true;  goto enter_leave;
        case insn_leave:  base = 0x0f; neg = false; goto enter_leave;
        case insn_leavel: base = 0x0f; neg = true;  goto enter_leave;
        enter_leave:
            if (!expect(peek_imm(as))
                goto bad_arg;
            if (!parse_imm(as, &val))
                goto bad;
            emit_code(as, base | prefix_const(as, val));
            return;

        bad_arg:
            err(as, "bad argument");
            goto bad;

    }

bad:
    skip_line(as);
}

static void switch_seg(struct as *as, enum seg seg) {
    if (as->seg != UNK_SEG) {
        struct chunk *cc = make_chunk(as);
        cc->type = SWITCH_CHUNK;
    }
    ensure(seg != UNK_SEG);
    as->seg = seg;
}
static uint32_t max_addr_in_seg(enum seg seg) {
    return seg == CODE_SEG ? 0x1000000 : 0x10000;
}

static void parse_dot(struct as *as) {
    str tok = read_token(as);
    char buf[11];
    if (tok.length > MAX_DOT_LEN) {
        buf[0] = 0;
    } else {
        memcpy(buf, tok.els, tok.length);
        buf[tok.length] = 0;
    }
    if (!strcasecmp(buf, "code")) {
        switch_seg(as, SEG_CODE);
    } else if (!strcasecmp(buf, "data")) {
        switch_seg(as, SEG_DATA);
    } else if (!strcasecmp(buf, "org")) {
        if (as->seg == UNK_SEG)
            goto unk_not_ok;
        bool is_indexed;
        enum reg reg;
        struct expr addy;
        if (!parse_expr(as, &is_indexed, &reg, &addy))
            return;
        if (is_indexed || addy.op != OP_CONST) {
            err(as, "bad .ORG address expression");
            free_expr(&addy);
            return;
        }
        int32_t k = addy.u.k;
        if (k < 0 || k >= max_addr_in_seg(as->seg)) {
            err(as, ".ORG address out of range");
            return;
        }
        struct chunk *cc = make_chunk(as);
        cc->type = ORG_CHUNK;
        cc->u.org.addr = (uint32_t) k;
    } else {
        err(as, "unknown directive: %.*s", STR_FMT_ARG(&tok));
    }
    return;

unk_not_ok:
    err(as, "got '%s' directive without .CODE or .DATA", but);
    skip_line(as);
    return;
}

static void free_symtab(struct as *as, struct htab_sym *ht) {
    HTAB_FOREACH(ht, str **sp, struct empty *ep, sym) {
        struct sym *sym = (struct sym *) *sp;
        if (sym->seg == UNK_SEG) {
            err_noline(as, "ERROR [%s:%d]: undefined symbol '%s'\n",
                       as->filename, sym->first_use_lineno);
        }
        vec_append_symp(&as->syms_to_free, sym);
    }
    htab_free_storage(ht);
}

static void parse_neutral(struct as *as) {
    enum seg seg = as->seg;
    skip_white(as);
    char c = peek(as);
    if (c == '.') {
        parse_dot(as);
        return;
    }
    if (c == '\0')
        return;
    if (seg == UNK_SEG) {
        err(as, "got stuff without .CODE or .DATA");
        skip_line(as);
        return;
    }
    str tok = read_token(as);
    if (peek(as) == ':') {
        bool local = is_local_sym_name(&tok);
        if (!local && as->local_symtab.length > 0) {
            free_symtab(as, &as->local_symtab);
            HTAB_INIT(&as->local_symtab);
        }
        struct sym *sym = find_sym(as, &tok, local);
        if (sym->seg != UNK_SEG) {
            err("ignoring duplicate definition of symbol %.*s",
                STR_FMT_ARG(&sym->name));
        } else {
            sym->seg = seg;
            sym->chunk_idx = as->chunks.length;
            struct chunk *ch = make_chunk(as);
            ch->type = SYM_CHUNK;
            ch->u.sym.last_known_addr = -1u;
        }
        if (is_local_sym_name(&tok))
            clear_symtab(as->local_sym);
        return;
    }

    // ok, it's an instruction (or pseudo)
    parse_insn(as, &tok);
}

static void parse_all(struct as *as) {
    char c = peek(as);
    if (c != '\0')
    while (1) {
        parse_neutral(as);
        c = peek(as);
        if (c == '\0')
            break;
        if (c != '\n') {
            char *nl = memchr(as->read_cursor, '\n', as->read_end - as->read_cursor);
            if (!nl)
                nl = as->read_end;
            err(as, "extraneous data at end of line: '%.*s'",
                (int) (nl - as->read_cursor), as->read_cursor);
            skip_line(as);
            if (c == '\0')
                break;
        }
        advance(as);
        as->lineno++;
    }
}

static bool expr_eval(struct as *as, const struct expr *expr, int32_t *out) {
    switch (expr->op) {
        case OP_CONST:
            *out = expr->u.k;
            return true;
        case OP_SYM: {
            struct sym *sym = expr->u.sym;
            struct seg seg = sym->seg;
            if (seg == UNK_SEG) {
                // hopeless, already diagnosed
                *out = 0;
                return true;
            }
            size_t idx = sym->chunk_idx;
            struct chunk *chunk = &as->seg_info[seg].chunks.els[idx];
            ensure(chunk->type == SYM_CHUNK);
            uint32_t val = chunk->u.sym.last_known_addr;
            if (val == -1u)
                return false;
            ensure(val <= INT32_MAX);
            *out = (int32_t) val;
            return true;
        }
    }
    // binary
    int32_t l, r;
    if (!expr_eval(as, expr->u.bin.l, &l) ||
        !expr_eval(as, expr->u.bin.r, &r))
        return false;
    *out = (int32_t) ((uint32_t) l + (uint32_t) r);
    return true;
}

static void layout(struct as *as) {
    free_symtab(as, &as->global_symtab);
    bool need_again;
    do {
        need_again = false;
        for (int seg = 0; seg < NUM_SEGS; seg++) {
            struct seg_info *si = &as->seg_info[seg];
            uint32_t addr = 0;
            uint32_t max = max_addr_in_seg(seg);
            VEC_FOREACH(&si->chunks, size_t i, struct chunk *chunk, chunk) {
                size_t length = chunk->before.length, length2;
                switch (chunk->type) {
                    case CODE_RELOC_CHUNK: {
                        int32_t dest;
                        uint8_t *mks = &chunk->u.reloc.max_known_size;
                        if (expr_eval(as, &chunk->u.reloc.target, &dest)) {
                            uint32_t diff = addr - (uint32_t) dest;
                            uint8_t size = diff == (uint32_t) (int8_t) diff ? 1
                                         : diff == (uint32_t) (int16_t) diff ? 2
                                         : 3;
                            if (size > *mks) {
                                need_again = true;
                                *mks = size;
                            }
                        } else
                            need_again = true;
                        length2 = *mks;
                        break;
                    }
                    case DATA_RELOC_CHUNK:
                        length2 = 2;
                        break;
                    case SYM_CHUNK:
                        chunk->sym.last_known_addr = addr;
                        /* fallthrough */
                    default:
                        length2 = 0;
                        break;
                }
                size_t total_length = length + length2;
                if (total_length < length ||
                    total_length > max - addr) {
                    err_noline(as, "ERROR: ran out of segment space around %s:%d",
                               as->filename, chunk->lineno);
                    return;
                }
                if (chunk->type == ORG_CHUNK)
                    addr = chunk->u.org.addr;
                else
                    addr += total_length;
            }
        }
    } while(need_again);
}

static void write_word(FILE *out, uint32_t addr, uint16_t word) {
    fprintf(out, "@%06x\t%02x\n", addr, word);
}

static void emit_txt(const struct as *as, enum seg seg, FILE *out, char **argv) {
    fprintf(out, "// Version      : xas\n");
    fprintf(out, "// Title        : \"%s Table\"\n",
            seg == CODE_SEG ? "Code" : "Constant");
    char buf[128];
    struct timeval tv = 0};
    gettimeofday(&tv, NULL); // or fail, whatever
    fprintf(out, "// Date         : %s\n",
            ctime_r(&tv.tv_sec, buf));
    fprintf(out, "// Command Line :");
    for (char **arg = argv; *arg; arg++)
        fprintf(out, " %s", *arg);
    fprintf(out, "\n\n");
    const struct seg_info *si = &as->seg_info[seg];
    uint32_t addr = 0;
    VEC_FOREACH(&si->chunks, size_t i, struct chunk *chunk, chunk) {
        VEC_FOREACH(&chunk->before, size_t j, uint16_t *word)
            write_word(out, addr++, *word);
        switch (chunk->type) {
            case CODE_RELOC_CHUNK: {
                int32_t dest;
                ensure (expr_eval(as, &chunk->u.reloc.target, &dest));
                int32_t diff = (int32_t) (addr - (uint32_t) dest);
                int8_t byte3 = (int8_t) (diff & 0xff);
                int16_t byte2 = (int16_t) ((diff - byte3) & 0xffff);
                int32_t byte1 = diff - (byte2 + byte3);
                ensure((byte1 & 0xffff) == 0 && (byte2 & 0xff) == 0);
                ensure(byte1 >= -0x800000 && byte1 <= 0x7f0000);
                uint8_t len = chunk->u.reloc.max_known_size;
                if (len >= 3)
                    write_word(out, addr++, byte1 >> 8);
                if (len >= 2)
                    write_word(out, addr++, byte2);
                len++;
                write_word(out, addr++, byte3 << 8 | chunk->u.reloc.base);
                break;
            }
            case DATA_RELOC_CHUNK: {
                int32_t dest;
                ensure (expr_eval(as, &chunk->u.reloc.target, &dest));
                if (dest < -0x8000 || dest > 0xffff) {
                    err_noline(as, "ERROR: reloc from %s:%d evaluates to %ld which can't fit as either signed or unsigned",
                               as->filename, chunk->lineno, (long) dest);
                }
                write_word(out, addr++, (uint16_t) dest);
                break;
            }
            case ORG_CHUNK:
                addr = chunk->u.org.addr;
                break;
        }
    }
}

static void as_init(struct as *as, const char *read_cursor,
                    const char *read_end, const char *filename) {
    as->read_cursor = read_cursor;
    as->read_end = read_end;
    as->filename = filename;
    as->lineno = 1;
    as->seg = UNK_SEG;
    as->have_err = false;
    HTAB_INIT(&as->global_symtab);
    HTAB_INIT(&as->local_symtab);
    VEC_INIT(&as->syms_to_free);
    VEC_INIT(&as->cur);
    for (int i = 0; i < NUM_SEGS; i++)
        VEC_INIT(&as->seg_info[i].chunks);
}

static void as_free(struct as *as) {
    htab_free_storage_sym(&as->global_symtab);
    htab_free_storage_sym(&as->local_symtab);
    VEC_FOREACH(&as->syms_to_free, size_t i, struct sym **symp)
        free(*symp);
    vec_free_storage_symp(&as->syms_to_free);
    vec_free_storage_uint16_t(&as->cur);
    for (int i = 0; i < NUM_SEGS; i++) {
        struct seg_info *si = &as->seg_info[seg];
        VEC_FOREACH(&si->chunks, size_t i, struct chunk *chunk, chunk) {
            if (chunk->type == CODE_RELOC_CHUNK ||
                chunk->type == DATA_RELOC_CHUNK)
                expr_free(&chunk->u.reloc.target);
        }
        vec_free_storage_uint16_t(&si->chunks);
    }
}

int main(int argc, char **argv) {
    str text = STR_INITER;
    str_fread(&text, stdin, SIZE_MAX);
    if (ferror(stdin)) {
        fprintf(stderr, "read error\n");
        return 1;
    }
    if (!feof(stdin)) {
        fprintf(stderr, "input is too big for me\n");
        return 1;
    }
    struct as as;
    as_init(&as, text.els, text.els + text.length, "<stdin>");
    parse_all(&as);
    if (as.have_err)
        return 1;
    layout(&as);
    if (as.have_err)
        return 1;
    if (as.seg_info[CODE_SEG].chunks.length)
        emit_txt(&as, CODE_SEG, stdout, argv);
    if (as.seg_info[DATA_SEG].chunks.length)
        emit_txt(&as, DATA_SEG, stdout, argv);

    as_free(as); // unnecessary but just to test it
}
