/*

  References:
 Looks just like how mine ended up :) - http://piumarta.com/software/lysp/
  
  Eval - http://norvig.com/lispy.html
  Enviornments - https://mitpress.mit.edu/sicp/full-text/book/book-Z-H-21.html#%_sec_3.2
  Cons Representation - http://www.more-magic.net/posts/internals-data-representation.html
  GC - http://www.more-magic.net/posts/internals-gc.html
  http://home.pipeline.com/~hbaker1/CheneyMTA.html

  Enviornments as first-class objects - https://groups.csail.mit.edu/mac/ftpdir/scheme-7.4/doc-html/scheme_14.html

  Reference counting symbol table? - http://sandbox.mc.edu/~bennet/cs404/ex/lisprcnt.html
*/

#include <stdlib.h>
#include <ctype.h>
#include <memory.h>
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "lisp.h"

enum
{
    GC_CLEAR = 0,
    GC_MOVED = (1 << 0), // has this block been moved to the to-space?
    GC_VISITED = (1 << 1), // has this block's pointers been moved?
};

typedef struct Page
{
    unsigned int size;
    unsigned int capacity;
    struct Page* next; 
    char buffer[];
} Page;

static Page* page_create(unsigned int capacity)
{
    Page* page = malloc(sizeof(Page) + capacity);
    page->capacity = capacity;
    page->size = 0;
    page->next = NULL;
    return page;
}

void page_destroy(Page* page) { free(page); }

typedef struct
{
    Page* page;
    Page* first_page;
    size_t size;
} Heap;

typedef struct
{
    unsigned int hash;
    char string[];
} Symbol;

// hash table
// linked list chaining
typedef struct 
{
    unsigned short size;
    unsigned short capacity;
    Lisp entries[];
} Table;

struct LispContext
{
    Heap heap;
    Heap to_heap;

    Lisp symbol_table;
    Lisp global_env;
    int lambda_counter;
};

#define PAGE_SIZE 8192

static void heap_init(Heap* heap)
{
    heap->first_page = page_create(PAGE_SIZE);
    heap->page = heap->first_page;
    heap->size = 0;
}

static void heap_reset(Heap* heap, size_t target_size)
{
    Page* page = heap->first_page;
    Page* previous = page;
    
    size_t size_counter = 0;

    // clear pages we will reuse
    while (page && size_counter < target_size)
    {
        size_counter += page->size;
        page->size = 0;
        
        previous = page;
        page = page->next;
    }
    
    previous->next = NULL;

    // destroy the remaining ones
    while (page)
    {
        Page* next = page->next;
        page_destroy(page);
        page = next;
   } 

   heap->page = heap->first_page;
   heap->size = 0;
}

static LispBlock* heap_alloc(unsigned int data_size, LispType type, int gc_flags, Heap* heap)
{
    unsigned int alloc_size = data_size + sizeof(LispBlock);

    Page* page = heap->page;
    if (page->size + alloc_size > page->capacity)
    {
        if (page->next && page->next->size + alloc_size > page->capacity)
        {
            // reuse the next page
            page = page->next;
            heap->page = page;
        }
        else
        {
            // we need a new page
            unsigned int size = (alloc_size <= PAGE_SIZE) ? PAGE_SIZE : alloc_size;
            Page* new_page = page_create(size);
            new_page->next = page->next;
            page->next = new_page;
            heap->page = new_page;
            page = new_page;
        }
    }
 
    LispBlock* block = (LispBlock*)(page->buffer + page->size);
    block->gc_flags = gc_flags;
    block->data_size = data_size;
    block->type = type;

    page->size += alloc_size;
    heap->size += alloc_size;
    return block;
}

static LispBlock* gc_alloc(unsigned int data_size, LispType type, LispContextRef ctx)
{
    return heap_alloc(data_size, type, GC_CLEAR, &ctx->heap);
}

Lisp lisp_null()
{
    Lisp l;
    l.type = LISP_NULL;
    l.int_val = 0;
    return l;
}

static Table* lisp_table(Lisp l)
{
    assert(lisp_type(l) == LISP_TABLE);
    LispBlock* block = l.val;
    return (Table*)block->data;
}

Lisp lisp_make_int(int n)
{
    Lisp l;
    l.type = LISP_INT;
    l.int_val = n;
    return l;
}

int lisp_int(Lisp l)
{
    if (l.type == LISP_FLOAT)
        return (int)l.float_val;
    return l.int_val;
}

Lisp lisp_make_float(float x)
{
    Lisp l;
    l.type = LISP_FLOAT;
    l.float_val = x;
    return l;
}

float lisp_float(Lisp l)
{
    if (l.type == LISP_INT)
        return(float)l.int_val;
    return l.float_val;
}

Lisp lisp_cons(Lisp car, Lisp cdr, LispContextRef ctx)
{
    LispBlock* block = gc_alloc(sizeof(Lisp) * 2, LISP_PAIR, ctx);
    Lisp l;
    l.type = block->type;
    l.val = block;
    lisp_set_car(l, car);
    lisp_set_cdr(l, cdr);
    return l;
}

static void back_append(Lisp* front, Lisp* back, Lisp item, LispContextRef ctx)
{
    Lisp new_l = lisp_cons(item, lisp_null(), ctx);
    if (lisp_is_null(*back))
    {
        *back = new_l;
        *front = *back;
    }
    else
    {
        lisp_set_cdr(*back, new_l);
        *back = new_l;
    }
}

Lisp lisp_append(Lisp l, Lisp l2, LispContextRef ctx)
{
    if (lisp_is_null(l)) return l;

    Lisp tail = lisp_cons(lisp_car(l), lisp_null(), ctx);
    Lisp start = tail;
    l = lisp_cdr(l);

    while (!lisp_is_null(l))
    {
        Lisp cell = lisp_cons(lisp_car(l), lisp_null(), ctx);
        lisp_set_cdr(tail, cell);
        tail = cell;
        l = lisp_cdr(l);
    }

    lisp_set_cdr(tail, l2);
    return start;
}

Lisp lisp_at_index(Lisp l, int i)
{
    while (i > 0)
    {
        if (l.type != LISP_PAIR) return lisp_null();
        l = lisp_cdr(l);
        --i;
    }
    return lisp_car(l);
}

Lisp lisp_nav(Lisp l, const char* c)
{
    if (toupper(*c) != 'C') return lisp_null();

    ++c;
    int i = 0;
    while (toupper(*c) != 'R' && *c)
    {
        ++i;
        ++c;
    }

    if (toupper(*c) != 'R') return lisp_null();
    --c;

    while (i > 0)
    {
        if (toupper(*c) == 'D')
        {
            l = lisp_cdr(l);
        }
        else if (toupper(*c) == 'A')
        {
            l = lisp_car(l);
        }
        else
        {
            return lisp_null();
        }
        --c;
        --i;
    }

    return l;
}

int lisp_length(Lisp l)
{
    int count = 0;
    while (!lisp_is_null(l))
    {
        ++count;
        l = lisp_cdr(l);
    }
    return count;
}

Lisp lisp_make_list(Lisp x, int n, LispContextRef ctx)
{
    Lisp front = lisp_null();
    Lisp back = front;

    for (int i = 0; i < n; ++i)
        back_append(&front, &back, x, ctx);

    return front;
}

Lisp lisp_make_listv(LispContextRef ctx, Lisp first, ...)
{
    Lisp front = lisp_cons(first, lisp_null(), ctx);
    Lisp back = front;

    va_list args;
    va_start(args, first);

    Lisp it = lisp_null();
    do
    {
        it = va_arg(args, Lisp);
        if (!lisp_is_null(it))
            back_append(&front, &back, it, ctx);
    } while (!lisp_is_null(it));

    va_end(args);

    return front;
}

Lisp lisp_reverse_inplace(Lisp l)
{
    Lisp p = lisp_null();

    while (lisp_type(l) == LISP_PAIR)
    {
        Lisp next = lisp_cdr(l);
        lisp_cdr(l) = p;
        p = l;
        l = next;        
    }

    return p;
}

Lisp lisp_assoc(Lisp l, Lisp key_symbol)
{
    while (!lisp_is_null(l))
    {
        Lisp pair = lisp_car(l);
        if (lisp_type(pair) == LISP_PAIR && lisp_eq(lisp_car(pair), key_symbol))
        {
            return pair;
        }

        l = lisp_cdr(l);
    }
    return lisp_null();
}

Lisp lisp_for_key(Lisp list, Lisp key_symbol)
{
    Lisp pair = lisp_assoc(list, key_symbol);
    return lisp_car(lisp_cdr(pair));
}

Lisp lisp_make_string(const char* string, LispContextRef ctx)
{
    unsigned int length = (unsigned int)strlen(string) + 1;
    LispBlock* block = gc_alloc(length, LISP_STRING, ctx);
    memcpy(block->data, string, length);
    
    Lisp l;
    l.type = block->type;
    l.val = block;
    return l;
}

const char* lisp_string(Lisp l)
{
    assert(l.type == LISP_STRING);
    LispBlock* block = l.val;
    return block->data;
}

const char* lisp_symbol(Lisp l)
{
    assert(l.type == LISP_SYMBOL);
    LispBlock* block = l.val;
    Symbol* symbol = (Symbol*)block->data;
    return symbol->string;
}

static unsigned int symbol_hash(Lisp l)
{
    assert(l.type == LISP_SYMBOL);
    LispBlock* block = l.val;
    Symbol* symbol = (Symbol*)block->data;
    return symbol->hash;
}

// TODO
#if defined(WIN32) || defined(WIN64)
#define strncasecmp _stricmp
#endif

static Lisp table_get_string(Lisp l, const char* string, unsigned int hash)
{
    Table* table = lisp_table(l);
    unsigned int index = hash % table->capacity;

    Lisp it = table->entries[index];

    while (!lisp_is_null(it))
    {
        Lisp pair = lisp_car(it);
        Lisp symbol = lisp_car(pair);

        if (strncasecmp(lisp_symbol(symbol), string, 2048) == 0)
        {
            return pair;
        }

        it = lisp_cdr(it);
    }

    return lisp_null();
}

static unsigned int hash_string(const char* c)
{     
    // adler 32
    int s1 = 1;
    int s2 = 0;

    while (*c)
    {
        s1 = (s1 + toupper(*c)) % 65521;
        s2 = (s2 + s1) % 65521;
        ++c; 
    } 
    return (s2 << 16) | s1;
}

Lisp lisp_make_symbol(const char* string, LispContextRef ctx)
{
    unsigned int hash = hash_string(string);
    Lisp pair = table_get_string(ctx->symbol_table, string, hash);
    
    if (lisp_is_null(pair))
    {
        size_t string_length = strlen(string) + 1;
        // allocate a new block
        LispBlock* block = gc_alloc(sizeof(Symbol) + string_length, LISP_SYMBOL, ctx);
        
        Symbol* symbol = (Symbol*)block->data;
        symbol->hash = hash;
        memcpy(symbol->string, string, string_length);

        // always convert symbols to uppercase
        char* c = symbol->string;
        
        while (*c)
        {
            *c = toupper(*c);
            ++c;
        }

        Lisp l;
        l.type = block->type;
        l.val = block;
        lisp_table_set(ctx->symbol_table, l, lisp_null(), ctx);
        return l;
    }
    else
    {
        return lisp_car(pair);
    }
}

Lisp lisp_make_func(LispFunc func)
{
    Lisp l;
    l.type = LISP_FUNC;
    l.val = (LispBlock*)func;
    return l;
}

typedef struct
{
    int identifier;
    Lisp args;
    Lisp body;
    Lisp env;
} Lambda;

Lisp lisp_make_lambda(Lisp args, Lisp body, Lisp env, LispContextRef ctx)
{
    LispBlock* block = gc_alloc(sizeof(Lambda), LISP_LAMBDA, ctx);

    Lambda data;
    data.identifier = ctx->lambda_counter++;
    data.args = args;
    data.body = body;
    data.env = env;
    memcpy(block->data, &data, sizeof(Lambda));

    Lisp l;
    l.type = block->type;
    l.val = block;
    return l;
}

static Lambda lisp_lambda(Lisp lambda)
{
    LispBlock* block = lambda.val;
    return *(const Lambda*)block->data;
}

#pragma mark -
#pragma mark Lexer

typedef enum
{
    TOKEN_NONE = 0,
    TOKEN_L_PAREN,
    TOKEN_R_PAREN,
    TOKEN_QUOTE,
    TOKEN_SYMBOL,
    TOKEN_STRING,
    TOKEN_INT,
    TOKEN_FLOAT,
} TokenType;

/* for debug
static const char* token_type_name[] = {
    "NONE", "L_PAREN", "R_PAREN", "QUOTE", "SYMBOL", "STRING", "INT", "FLOAT",
}; */

typedef struct
{
    FILE* file;

    const char* sc; // start of token
    const char* c;  // scanner
    unsigned int scan_length;
    TokenType token;

    int sc_buff_index;
    int c_buff_index; 

    char* buffs[2];
    int buff_number[2];
    unsigned int buff_size;
} Lexer;

static void lexer_shutdown(Lexer* lex)
{
    if (lex->file)
    {
        free(lex->buffs[0]);
        free(lex->buffs[1]);
    }
}

static void lexer_init(Lexer* lex, const char* program)
{
    lex->file = NULL;
    lex->sc_buff_index = 0;
    lex->c_buff_index = 0; 
    lex->buffs[0] = (char*)program;
    lex->buffs[1] = NULL;
    lex->buff_number[0] = 0;
    lex->buff_number[1] = -1;
    lex->sc = lex->c = lex->buffs[0];
    lex->scan_length = 0;
}

static void lexer_init_file(Lexer* lex, FILE* file)
{
    lex->file = file;

    lex->buff_size = 4096;

     // double input buffering
    lex->sc_buff_index = 0;
    lex->c_buff_index = 0;

    lex->buffs[0] = malloc(lex->buff_size + 1);
    lex->buffs[1] = malloc(lex->buff_size + 1);
    lex->buffs[0][lex->buff_size] = '\0';
    lex->buffs[1][lex->buff_size] = '\0';

    // sc the pointers out
    lex->sc = lex->c = lex->buffs[0];
    lex->scan_length = 0;

    // read a new block
    size_t read = fread(lex->buffs[0], 1, lex->buff_size, lex->file);
    lex->buffs[0][read] = '\0';

    lex->buff_number[0] = 0;
    lex->buff_number[1] = -1;
}

static void lexer_advance_start(Lexer* lex)
{
    lex->sc = lex->c;
    lex->sc_buff_index = lex->c_buff_index;
    lex->scan_length = 0;
}

static void lexer_restart_scan(Lexer* lex)
{
    lex->c = lex->sc;
    lex->c_buff_index = lex->sc_buff_index;
    lex->scan_length = 0;
}

static int lexer_step(Lexer* lex)
{
    ++lex->c;
    ++lex->scan_length;

    if (*lex->c == '\0')
    { 
        if (lex->file)
        {
            // flip the buffer
            int previous_index = lex->c_buff_index;
            int new_index = !previous_index;

            if (new_index == lex->sc_buff_index)
            {
                fprintf(stderr, "token too long\n");
                return 0;
            }
 
            // next block is older. so read a new one
            if (lex->buff_number[new_index] < lex->buff_number[previous_index])
            {
                if (!feof(lex->file))
                {
                    size_t read = fread(lex->buffs[new_index], 1, lex->buff_size, lex->file);
                    lex->buff_number[new_index] = lex->buff_number[previous_index] + 1;
                    lex->buffs[new_index][read] = '\0';
                }
                else
                {
                    return 0;
                }
            }

			lex->c_buff_index = new_index;
            lex->c = lex->buffs[new_index];
        }  
        else
        {
            return 0;
        }
    }

    return 1;
}

static void lexer_skip_empty(Lexer* lex)
{
    while (lex->c)
    {
        // skip whitespace
        while (isspace(*lex->c)) lexer_step(lex);

        // skip comments to end of line
        if (*lex->c == ';')
        {
            while (*lex->c && *lex->c != '\n') lexer_step(lex);
        }
        else
        {
            break;
        }
    }
}

static int lexer_match_int(Lexer* lex)
{
    lexer_restart_scan(lex);
    
    // need at least one digit or -
    if (!isdigit(*lex->c))
    {
        if (*lex->c == '-' || *lex->c == '+')
        {
            lexer_step(lex);
            if (!isdigit(*lex->c)) return 0;
        }
        else
        {
            return 0;
        }
    }

    lexer_step(lex);
    // + any other digits
    while (isdigit(*lex->c)) lexer_step(lex);
    return 1;
}

static int lexer_match_float(Lexer* lex)
{
    lexer_restart_scan(lex);
    
    // need at least one digit or -
    if (!isdigit(*lex->c))
    {
        if (*lex->c == '-' || *lex->c == '+')
        {
            lexer_step(lex);
            if (!isdigit(*lex->c)) return 0;
        }
        else
        {
            return 0;
        }
    }
    lexer_step(lex);

    int found_decimal = 0;
    while (isdigit(*lex->c) || *lex->c == '.')
    {
        if (*lex->c == '.') found_decimal = 1;
        lexer_step(lex);
    }

    // must have a decimal to be a float
    return found_decimal;
}

static int is_symbol(char c)
{
    if (c < '!' || c > 'z') return 0;
    const char* illegal= "()#;";
    do
    {
        if (c == *illegal) return 0;
        ++illegal;
    } while (*illegal);
    return 1;
}

static int lexer_match_symbol(Lexer* lex)
{   
    lexer_restart_scan(lex);
    // need at least one valid symbol character
    if (!is_symbol(*lex->c)) return 0;
    lexer_step(lex);
    while (is_symbol(*lex->c)) lexer_step(lex);
    return 1;
}

static int lexer_match_string(Lexer* lex)
{
    lexer_restart_scan(lex);
    // start with quote
    if (*lex->c != '"') return 0;
    lexer_step(lex);

    while (*lex->c != '"')
    {
        if (*lex->c == '\0' || *lex->c == '\n')
            return 0;
        lexer_step(lex);
    }

    lexer_step(lex);
    return 1;    
}

static void lexer_copy_token(Lexer* lex, unsigned int start_index, unsigned int length, char* dest)
{
    int token_length = lex->scan_length;
    assert((start_index + length) <= token_length);

    if (lex->c_buff_index == lex->sc_buff_index)
    {
        // both pointers are in the same buffer
        memcpy(dest, lex->sc + start_index, length);
    }
    else
    {
        // the pointers are split across buffers. So do two copies.
        const char* sc_end = lex->buffs[lex->sc_buff_index] + lex->buff_size;
        const char* sc = (lex->sc + start_index);
        
        ptrdiff_t first_part_length = sc_end - sc;  
        if (first_part_length < 0) first_part_length = 0;
        if (first_part_length > length) first_part_length = length;
        
        if (first_part_length > 0)
        {
            // copy from sc to the end of its buffer (or to length)
            memcpy(dest, sc, first_part_length);
        }
        
        ptrdiff_t last_part = length - first_part_length;        
        if (last_part > 0)
        {
            // copy from the start of c's buffer
            memcpy(dest + first_part_length, lex->buffs[lex->c_buff_index], length - first_part_length);
        }
    }
}

static void lexer_next_token(Lexer* lex)
{
    lexer_skip_empty(lex);
    lexer_advance_start(lex);

    if (*lex->c == '\0')
    {
        lex->token = TOKEN_NONE;
    }
    else if (*lex->c == '(')
    {
        lex->token = TOKEN_L_PAREN;
        lexer_step(lex);
    }
    else if (*lex->c == ')')
    {
        lex->token = TOKEN_R_PAREN;
        lexer_step(lex);
    }
    else if (*lex->c == '\'')
    {
        lex->token = TOKEN_QUOTE;
        lexer_step(lex);
    }
    else if (lexer_match_string(lex))
    {
        lex->token = TOKEN_STRING;
    }
    else if (lexer_match_float(lex))
    {
        lex->token = TOKEN_FLOAT;
    }
    else if (lexer_match_int(lex))
    {
        lex->token = TOKEN_INT;
    }
    else if (lexer_match_symbol(lex))
    {
        lex->token = TOKEN_SYMBOL;
    }
    else
    {
        lex->token = TOKEN_NONE;
    }
}

#pragma mark -
#pragma mark Read

#define SCRATCH_MAX 1024

static Lisp parse_atom(Lexer* lex, jmp_buf error_jmp,  LispContextRef ctx)
{ 
    char scratch[SCRATCH_MAX];
    int length = lex->scan_length;
    Lisp l = lisp_null();

    switch (lex->token)
    {
        case TOKEN_INT:
        {
            lexer_copy_token(lex, 0, length, scratch);
            scratch[length] = '\0';
            l = lisp_make_int(atoi(scratch));
            break;
        }
        case TOKEN_FLOAT:
        {
            lexer_copy_token(lex, 0, length, scratch);
            scratch[length] = '\0'; 
            l = lisp_make_float(atof(scratch));
            break;
        }
        case TOKEN_STRING:
        {
            // -2 length to skip quotes
            LispBlock* block = gc_alloc(length - 1, LISP_STRING, ctx);
            lexer_copy_token(lex, 1, length - 2, block->data);
            block->data[length - 2] = '\0';
            
            l.type = block->type;
            l.val = block;
            break;
        }
        case TOKEN_SYMBOL:
        {
            // always convert symbols to uppercase
            lexer_copy_token(lex, 0, length, scratch);
            scratch[length] = '\0';
            l = lisp_make_symbol(scratch, ctx);
            break;
        }
        default: 
            longjmp(error_jmp, LISP_ERROR_BAD_TOKEN);
    }
    
    lexer_next_token(lex);
    return l;
}

// read tokens and construct S-expresions
static Lisp parse_list_r(Lexer* lex, jmp_buf error_jmp, LispContextRef ctx)
{  
    switch (lex->token)
    {
        case TOKEN_NONE:
            longjmp(error_jmp, LISP_ERROR_PAREN_EXPECTED);
        case TOKEN_L_PAREN:
        {
            Lisp front = lisp_null();
            Lisp back = lisp_null();

            // (
            lexer_next_token(lex);
            while (lex->token != TOKEN_R_PAREN)
            {
                Lisp l = parse_list_r(lex, error_jmp, ctx);
                back_append(&front, &back, l, ctx);
            }
            // )
            lexer_next_token(lex);
            return front;
        }
        case TOKEN_R_PAREN:
            longjmp(error_jmp, LISP_ERROR_PAREN_UNEXPECTED);
        case TOKEN_QUOTE:
        {
             // '
             lexer_next_token(lex);
             Lisp l = lisp_cons(parse_list_r(lex, error_jmp, ctx), lisp_null(), ctx);
             return lisp_cons(lisp_make_symbol("QUOTE", ctx), l, ctx);
        }
        default:
        {
            return parse_atom(lex, error_jmp, ctx);
        } 
    }
}

static Lisp parse(Lexer* lex, LispError* out_error, LispContextRef ctx)
{
    jmp_buf error_jmp;
    LispError error = setjmp(error_jmp);

    if (error != LISP_ERROR_NONE)
    {
        if (out_error) *out_error = error;
        return lisp_null();
    }

    lexer_next_token(lex);
    Lisp result = parse_list_r(lex, error_jmp, ctx);
    
    if (lex->token != TOKEN_NONE)
    {
        Lisp back = lisp_cons(result, lisp_null(), ctx);
        Lisp front = lisp_cons(lisp_make_symbol("BEGIN", ctx), back, ctx);
        
        while (lex->token != TOKEN_NONE)
        {
            Lisp next_result = parse_list_r(lex, error_jmp, ctx);
            back_append(&front, &back, next_result, ctx);
        } 

       result = front;
    }

    if (out_error) *out_error = error;
    return result;
}

static Lisp expand_r(Lisp l, jmp_buf error_jmp, LispContextRef ctx)
{
    // 1. expand extended syntax into primitive syntax
    // 2. perform optimizations
    // 3. check syntax    
    if (lisp_type(l) == LISP_SYMBOL &&
        strcmp(lisp_symbol(l), "QUOTE") == 0)
    {
        // don't expand quotes
        return l;
    }
    else if (lisp_type(l) == LISP_PAIR)
    {
        const char* op = NULL;
        if (lisp_type(lisp_car(l)) == LISP_SYMBOL)
            op = lisp_symbol(lisp_car(l));

		if (op && strcmp(op, "QUOTE") == 0)
		{
            if (lisp_length(l) != 2) longjmp(error_jmp, LISP_ERROR_BAD_QUOTE);
			// don't expand quotes
			return l;
		}
        else if (op && strcmp(op, "DEFINE") == 0)
        {
            if (lisp_length(l) < 3) longjmp(error_jmp, LISP_ERROR_BAD_DEFINE);

            Lisp rest = lisp_cdr(l);        
            Lisp sig = lisp_car(rest);
 
            switch (lisp_type(sig))
            {
                case LISP_PAIR:
                {
                    // (define (<name> <arg0> ... <argn>) <body0> ... <bodyN>)
                    // -> (define <name> (lambda (<arg0> ... <argn>) <body> ... <bodyN>))
                    Lisp name = lisp_at_index(sig, 0);
                    if (lisp_type(name) != LISP_SYMBOL) longjmp(error_jmp, LISP_ERROR_BAD_DEFINE);

                    Lisp body = lisp_cdr(lisp_cdr(l));

                    Lisp lambda = lisp_make_listv(ctx,
                            lisp_make_symbol("LAMBDA", ctx),
                            lisp_cdr(sig), // args
                            lisp_null()); 

                    lisp_set_cdr(lisp_cdr(lambda), body);

                    lisp_set_cdr(l, lisp_make_listv(ctx,
                                name,
                                expand_r(lambda, error_jmp, ctx),
                                body,
                                lisp_null()));
                    return l;
                }
                case LISP_SYMBOL:
                {
                    lisp_set_cdr(rest, expand_r(lisp_cdr(rest), error_jmp, ctx));
                    return l;
                }
                default:
                    longjmp(error_jmp, LISP_ERROR_BAD_DEFINE);
                    break;
            }
        }
        else if (op && strcmp(op, "SET!") == 0)
        {
            if (lisp_length(l) != 3) longjmp(error_jmp, LISP_ERROR_BAD_SET);

            Lisp var = lisp_at_index(l, 1);
            if (lisp_type(var) != LISP_SYMBOL) longjmp(error_jmp, LISP_ERROR_BAD_SET);
            Lisp expr = expand_r(lisp_at_index(l, 2), error_jmp, ctx);

            return lisp_make_listv(ctx,
                    lisp_at_index(l, 0), // SET!
                    var,
                    expr,
                    lisp_null());
        }
        else if (op && strcmp(op, "COND") == 0)
        {
            // (COND (<pred0> <expr0>)
            //       (<pred1> <expr1>)
            //        ...
            //        (else <expr-1>)) ->
            //
            //  (IF <pred0> <expr0>
            //      (if <pred1> <expr1>
            //          ....
            //      (if <predN> <exprN> <expr-1>)) ... )

            Lisp conds = lisp_reverse_inplace(lisp_cdr(l));
            Lisp outer = lisp_null();

            Lisp cond_pair = lisp_car(conds);

            // error checks
            if (lisp_type(cond_pair) != LISP_PAIR) longjmp(error_jmp, LISP_ERROR_BAD_COND);
            if (lisp_length(cond_pair) != 2) longjmp(error_jmp, LISP_ERROR_BAD_COND);

            Lisp cond_pred = lisp_car(cond_pair);
            Lisp cond_expr = lisp_null();

            if ((lisp_type(cond_pred) == LISP_SYMBOL) &&
                    strcmp(lisp_symbol(cond_pred), "ELSE") == 0)
            {
                cond_expr = expand_r(lisp_car(lisp_cdr(cond_pair)), error_jmp, ctx);
                outer = cond_expr;
                conds = lisp_cdr(conds);
            }

            Lisp if_symbol = lisp_make_symbol("IF", ctx);
       
            while (!lisp_is_null(conds))
            {
                cond_pair = lisp_car(conds);

                // error checks
                if (lisp_type(cond_pair) != LISP_PAIR) longjmp(error_jmp, LISP_ERROR_BAD_COND);
                if (lisp_length(cond_pair) != 2) longjmp(error_jmp, LISP_ERROR_BAD_COND);

                cond_pred = expand_r(lisp_car(cond_pair), error_jmp, ctx);
                cond_expr = expand_r(lisp_car(lisp_cdr(cond_pair)), error_jmp, ctx);

                outer = lisp_make_listv(ctx,
                                       if_symbol,
                                       cond_pred,
                                       cond_expr,
                                       outer,
                                       lisp_null());

                conds = lisp_cdr(conds);
            }

            return outer;
        }
        else if (op && strcmp(op, "AND") == 0)
        {
            // (AND <pred0> <pred1> ... <predN>) 
            // -> (IF <pred0> 
            //      (IF <pred1> ...
            //          (IF <predN> t f)
            if (lisp_length(l) < 2) longjmp(error_jmp, LISP_ERROR_BAD_AND);

            Lisp if_symbol = lisp_make_symbol("IF", ctx);

            Lisp preds = lisp_reverse_inplace(lisp_cdr(l));
            Lisp p = expand_r(lisp_car(preds), error_jmp, ctx);
            
            Lisp outer = lisp_make_listv(ctx,
                                        if_symbol,
                                        p,
                                        lisp_make_int(1),
                                        lisp_make_int(0),
                                        lisp_null());

            preds = lisp_cdr(preds);

            while (!lisp_is_null(preds))
            {
                p = expand_r(lisp_car(preds), error_jmp, ctx);

                outer = lisp_make_listv(ctx,
                        if_symbol,
                        p,
                        outer,
                        lisp_make_int(0),
                        lisp_null());

                preds = lisp_cdr(preds);
            }
                      
           return outer;
        }
        else if (op && strcmp(op, "OR") == 0)
        {
            // (OR <pred0> <pred1> ... <predN>)
            // -> (IF (<pred0>) t
            //      (IF <pred1> t ...
            //          (if <predN> t f))
            if (lisp_length(l) < 2) longjmp(error_jmp, LISP_ERROR_BAD_OR);

            Lisp if_symbol = lisp_make_symbol("IF", ctx);

            Lisp preds = lisp_reverse_inplace(lisp_cdr(l));
            Lisp p = expand_r(lisp_car(preds), error_jmp, ctx);
            
            Lisp outer = lisp_make_listv(ctx,
                                        if_symbol,
                                        p,
                                        lisp_make_int(1),
                                        lisp_make_int(0),
                                        lisp_null());

            preds = lisp_cdr(preds);

            while (!lisp_is_null(preds))
            {
                p = expand_r(lisp_car(preds), error_jmp, ctx);

                outer = lisp_make_listv(ctx,
                        if_symbol,
                        p,
                        lisp_make_int(1),
                        outer,
                        lisp_null());

                preds = lisp_cdr(preds);
            }
                      
           return outer;

        }
        else if (op && strcmp(op, "LET") == 0)
        {
            // (LET ((<var0> <expr0>) ... (<varN> <expr1>)) <body0> ... <bodyN>)
            //  -> ((LAMBDA (<var0> ... <varN>) <body0> ... <bodyN>) <expr0> ... <expr1>)            
            Lisp pairs = lisp_at_index(l, 1);
            if (lisp_type(pairs) != LISP_PAIR) longjmp(error_jmp, LISP_ERROR_BAD_LET);

            Lisp body = lisp_cdr(lisp_cdr(l));

            Lisp vars_front = lisp_null();
            Lisp vars_back = vars_front;
            
            Lisp exprs_front = lisp_null();
            Lisp exprs_back = exprs_front;
            
            while (!lisp_is_null(pairs))
            {
                if (lisp_type(lisp_car(pairs)) != LISP_PAIR) longjmp(error_jmp, LISP_ERROR_BAD_LET);

                Lisp var = lisp_at_index(lisp_car(pairs), 0);

                if (lisp_type(var) != LISP_SYMBOL) longjmp(error_jmp, LISP_ERROR_BAD_LET);
                back_append(&vars_front, &vars_back, var, ctx);

                Lisp val = expand_r(lisp_at_index(lisp_car(pairs), 1), error_jmp, ctx);
                back_append(&exprs_front, &exprs_back, val, ctx);
                pairs = lisp_cdr(pairs);
            }
            
            Lisp lambda = lisp_make_listv(ctx, 
                                        lisp_make_symbol("LAMBDA", ctx), 
                                        vars_front, 
                                        lisp_null());

            lisp_set_cdr(lisp_cdr(lambda), body);

            return lisp_cons(expand_r(lambda, error_jmp, ctx), exprs_front, ctx);
        }
        else if (op && strcmp(op, "LAMBDA") == 0)
        {
            // (LAMBDA (<var0> ... <varN>) <expr0> ... <exprN>)
            // (LAMBDA (<var0> ... <varN>) (BEGIN <expr0> ... <expr1>)) 
            int length = lisp_length(l);

            if (length > 3)
            {
                Lisp body_exprs = expand_r(lisp_cdr(lisp_cdr(l)), error_jmp, ctx); 
                Lisp begin = lisp_cons(lisp_make_symbol("BEGIN", ctx), body_exprs, ctx);

                Lisp vars = lisp_at_index(l, 1);
                if (lisp_type(vars) != LISP_PAIR) longjmp(error_jmp, LISP_ERROR_BAD_LAMBDA);

                return lisp_make_listv(ctx, 
                                      lisp_at_index(l, 0),  // LAMBDA
                                      vars,  // vars
                                      begin,
                                      lisp_null()); 
            }
            else
            {
				Lisp body = lisp_cdr(lisp_cdr(l));
                lisp_set_cdr(lisp_cdr(l), expand_r(body, error_jmp, ctx));
                return l;
            }
        }
        else if (op && strcmp(op, "ASSERT") == 0)
        {
            Lisp statement = lisp_car(lisp_cdr(l));
            // here we save a quoted version of the code so we can see
            // what happened to trigger the assertion
            Lisp quoted = lisp_make_listv(ctx,
                                         lisp_make_symbol("QUOTE", ctx),
                                         statement,
                                         lisp_null());
            return lisp_make_listv(ctx,
                                  lisp_at_index(l, 0),
                                  expand_r(statement, error_jmp, ctx),
                                  quoted,
                                  lisp_null());
                                  
        }
        else
        {
            Lisp it = l;
            while (!lisp_is_null(it))
            {
                lisp_set_car(it, expand_r(lisp_car(it), error_jmp, ctx));
                it = lisp_cdr(it);
            }

            return l;
        }
    } 
    else 
    {
        return l;
    }
}

Lisp lisp_read(const char* program, LispError* out_error, LispContextRef ctx)
{
    Lexer lex;
    lexer_init(&lex, program);
    Lisp l = parse(&lex, out_error, ctx);
    lexer_shutdown(&lex);
    return l;
}

Lisp lisp_read_file(FILE* file, LispError* out_error, LispContextRef ctx)
{
    Lexer lex;
    lexer_init_file(&lex, file);
    Lisp l = parse(&lex, out_error, ctx);
    lexer_shutdown(&lex);
    return l;
}

Lisp lisp_read_path(const char* path, LispError* out_error, LispContextRef ctx)
{
    FILE* file = fopen(path, "r");

    if (!file)
    {
        *out_error = LISP_ERROR_FILE_OPEN;
        return lisp_null();
    }

    Lisp l = lisp_read_file(file, out_error, ctx);
    fclose(file);
    return l;
}

Lisp lisp_expand(Lisp lisp, LispError* out_error, LispContextRef ctx)
{
    jmp_buf error_jmp;
    LispError error = setjmp(error_jmp);

    if (error == LISP_ERROR_NONE)
    {
        Lisp result = expand_r(lisp, error_jmp, ctx);
        *out_error = error;
        return result;
    }
    else
    {
        *out_error = error;
        return lisp_null();
    }
}
 
// TODO?
static const char* lisp_type_name[] = {
    "NULL",
    "FLOAT",
    "INT",
    "PAIR",
    "SYMBOL",
    "STRING",
    "LAMBDA",
    "PROCEDURE",
    "ENV",
};

Lisp lisp_make_table(unsigned int capacity, LispContextRef ctx)
{
    unsigned int size = sizeof(Table) + sizeof(Lisp) * capacity;
    LispBlock* block = gc_alloc(size, LISP_TABLE, ctx);

    Table* table = (Table*)block->data;
    table->size = 0;
    table->capacity = capacity;

    // clear the table
    for (int i = 0; i < capacity; ++i)
        table->entries[i] = lisp_null();

    Lisp l;
    l.type = block->type;
    l.val = block;
    return l;
}

void lisp_table_set(Lisp l, Lisp symbol, Lisp value, LispContextRef ctx)
{
    Table* table = lisp_table(l);
    
    unsigned int index = symbol_hash(symbol) % table->capacity;
    Lisp pair = lisp_assoc(table->entries[index], symbol);

    if (lisp_is_null(pair))
    {
        // new value. prepend to front of chain
        pair = lisp_cons(symbol, value, ctx);
        table->entries[index] = lisp_cons(pair, table->entries[index], ctx);
        ++table->size;
    }
    else
    {
        // reassign cdr value (key, val)
        lisp_set_cdr(pair, value);
    }
}

Lisp lisp_table_get(Lisp l, Lisp symbol, LispContextRef ctx)
{
    Table* table = lisp_table(l);
    unsigned int index = symbol_hash(symbol) % table->capacity;
    return lisp_assoc(table->entries[index], symbol);
}

void lisp_table_add_funcs(Lisp table, const char** names, LispFunc* funcs, LispContextRef ctx)
{
    const char** name = names;
    LispFunc* func = funcs;

    while (*name)
    {
        lisp_table_set(table, lisp_make_symbol(*name, ctx), lisp_make_func(*func), ctx);
        ++name;
        ++func;
    }
}

Lisp lisp_make_env(Lisp table, LispContextRef ctx)
{
    return lisp_cons(table, lisp_null(), ctx);
}

Lisp lisp_env_extend(Lisp env, Lisp table, LispContextRef ctx)
{
    return lisp_cons(table, env, ctx);
}

Lisp lisp_env_lookup(Lisp env, Lisp symbol, LispContextRef ctx)
{
    Lisp it = env;
    while (!lisp_is_null(it))
    {
        Lisp x = lisp_table_get(lisp_car(it), symbol, ctx);
        if (!lisp_is_null(x)) return x;
        it = lisp_cdr(it);
    }
    
    return lisp_null();
}

void lisp_env_define(Lisp env, Lisp symbol, Lisp value, LispContextRef ctx)
{
    lisp_table_set(lisp_car(env), symbol, value, ctx);
}

void lisp_env_set(Lisp env, Lisp symbol, Lisp value, LispContextRef ctx)
{
    Lisp pair = lisp_env_lookup(env, symbol, ctx);
    if (lisp_is_null(pair))
    {
        fprintf(stderr, "error: unknown variable: %s\n", lisp_symbol(symbol));
    }
    lisp_set_cdr(pair, value);
}

static void lisp_print_r(FILE* file, Lisp l, int is_cdr)
{
    switch (lisp_type(l))
    {
        case LISP_INT:
            fprintf(file, "%i", lisp_int(l));
            break;
        case LISP_FLOAT:
            fprintf(file, "%f", lisp_float(l));
            break;
        case LISP_NULL:
            fprintf(file, "NIL");
            break;
        case LISP_SYMBOL:
            fprintf(file, "%s", lisp_symbol(l));
            break;
        case LISP_STRING:
            fprintf(file, "\"%s\"", lisp_string(l));
            break;
        case LISP_LAMBDA:
            fprintf(file, "lambda-%i", lisp_lambda(l).identifier);
            break;
        case LISP_FUNC:
            fprintf(file, "function-%p", l.val); 
            break;
        case LISP_TABLE:
        {
            Table* table = lisp_table(l);
            fprintf(file, "{");
            for (int i = 0; i < table->capacity; ++i)
            {
                if (lisp_is_null(table->entries[i])) continue;
                lisp_print_r(file, table->entries[i], 0);
                fprintf(file, " ");
            }
            fprintf(file, "}");
            break;
        }
        case LISP_PAIR:
            if (!is_cdr) fprintf(file, "(");
            lisp_print_r(file, lisp_car(l), 0);

            if (lisp_cdr(l).type != LISP_PAIR)
            {
                if (!lisp_is_null(lisp_cdr(l)))
                { 
                    fprintf(file, " . ");
                    lisp_print_r(file, lisp_cdr(l), 0);
                }

                fprintf(file, ")");
            }
            else
            {
                fprintf(file, " ");
                lisp_print_r(file, lisp_cdr(l), 1);
            } 
            break; 
    }
}

void lisp_printf(FILE* file, Lisp l) { lisp_print_r(file, l, 0);  }
void lisp_print(Lisp l) {  lisp_printf(stdout, l); }

static Lisp eval_r(Lisp x, Lisp env, jmp_buf error_jmp, LispContextRef ctx)
{
    while (1)
    {
        assert(!lisp_is_null(env));
        
        switch (lisp_type(x))
        {
            case LISP_INT:
            case LISP_FLOAT:
            case LISP_STRING:
            case LISP_LAMBDA:
            case LISP_NULL: 
                return x; // atom
            case LISP_SYMBOL: // variable reference
            {
                Lisp pair = lisp_env_lookup(env, x, ctx);
                
                if (lisp_is_null(pair))
                {
                    fprintf(stderr, "cannot find variable: %s\n", lisp_symbol(x));
                    longjmp(error_jmp, LISP_ERROR_UNKNOWN_VAR); 
                    return lisp_null();
                }
                return lisp_cdr(pair);
            }
            case LISP_PAIR:
            {
                const char* op_name = NULL;
                if (lisp_type(lisp_car(x)) == LISP_SYMBOL)
                    op_name = lisp_symbol(lisp_car(x));
                
                if (op_name && strcmp(op_name, "IF") == 0) // if conditional statemetns
                {
                    Lisp predicate = lisp_at_index(x, 1);
                    Lisp conseq = lisp_at_index(x, 2);
                    Lisp alt = lisp_at_index(x, 3);
 
                    if (lisp_int(eval_r(predicate, env, error_jmp, ctx)) != 0)
                    {
                        x = conseq; // while will eval
                    }
                    else
                    {
                        x = alt; // while will eval
                    } 
                }
                else if (op_name && strcmp(op_name, "BEGIN") == 0)
                {
                    Lisp it = lisp_cdr(x);
                    if (lisp_is_null(it)) return it;
                    
                    // eval all but last
                    while (!lisp_is_null(lisp_cdr(it)))
                    {
                        eval_r(lisp_car(it), env, error_jmp, ctx);
                        it = lisp_cdr(it);
                    }
                    
                    x = lisp_car(it); // while will eval last
                }
                else if (op_name && strcmp(op_name, "QUOTE") == 0)
                {
                    return lisp_at_index(x, 1);
                }
                else if (op_name && strcmp(op_name, "DEFINE") == 0) // variable definitions
                {
                    Lisp symbol = lisp_at_index(x, 1);
                    Lisp value = eval_r(lisp_at_index(x, 2), env, error_jmp, ctx);
                    lisp_env_define(env, symbol, value, ctx);
                    return lisp_null();
                }
                else if (op_name && strcmp(op_name, "SET!") == 0)
                {
                    // mutablity
                    // like def, but requires existence
                    // and will search up the environment chain
                    Lisp symbol = lisp_at_index(x, 1);
                    lisp_env_set(env, symbol, eval_r(lisp_at_index(x, 2), env, error_jmp, ctx), ctx);
                    return lisp_null();
                }
                else if (op_name && strcmp(op_name, "LAMBDA") == 0) // lambda defintions (compound procedures)
                    
                {
                    Lisp args = lisp_at_index(x, 1);
                    Lisp body = lisp_at_index(x, 2);
                    return lisp_make_lambda(args, body, env, ctx);
                }
                else // operator application
                {
                    Lisp operator = eval_r(lisp_car(x), env, error_jmp, ctx);
                    Lisp arg_expr = lisp_cdr(x);
                    
                    Lisp args_front = lisp_null();
                    Lisp args_back = lisp_null();
                    
                    while (!lisp_is_null(arg_expr))
                    {
                        Lisp new_arg = eval_r(lisp_car(arg_expr), env, error_jmp, ctx);
                        back_append(&args_front, &args_back, new_arg, ctx);
                        arg_expr = lisp_cdr(arg_expr);
                    }
                    
                    switch (lisp_type(operator))
                    {
                        case LISP_LAMBDA: // lambda call (compound procedure)
                        {
                            Lambda lambda = lisp_lambda(operator);
                            // make a new environment
                            Lisp new_table = lisp_make_table(13, ctx);
                            // bind parameters to arguments
                            // to pass into function call
                            Lisp keyIt = lambda.args;
                            Lisp valIt = args_front;
                            
                            while (!lisp_is_null(keyIt))
                            {
                                lisp_table_set(new_table, lisp_car(keyIt), lisp_car(valIt), ctx);
                                keyIt = lisp_cdr(keyIt); valIt = lisp_cdr(valIt);
                            }
                            
                            // normally we would eval the body here
                            // but while will eval
                            x = lambda.body;
                            
                            // extend the environment
                            env = lisp_env_extend(lambda.env, new_table, ctx);
                            break;
                        }
                        case LISP_FUNC: // call into C functions
                        {
                            // no environment required
                            LispFunc func = operator.val;
                            LispError e = LISP_ERROR_NONE;
                            Lisp result = func(args_front, &e, ctx);
                            if (e != LISP_ERROR_NONE) longjmp(error_jmp, e);
                            return result;
                        }
                        default:
                            
                            fprintf(stderr, "apply error: not an operator %s\n", lisp_type_name[operator.type]);
                            longjmp(error_jmp, LISP_ERROR_BAD_OP);
                    }
                }
                break;
            }
            default:
                longjmp(error_jmp, LISP_ERROR_UNKNOWN_EVAL);
        }
    }
}

Lisp lisp_eval(Lisp l, Lisp env, LispError* out_error, LispContextRef ctx)
{
    jmp_buf error_jmp;
    LispError error = setjmp(error_jmp);

    if (error == LISP_ERROR_NONE)
    {
        Lisp result = eval_r(l, env, error_jmp, ctx);

        if (out_error)
            *out_error = error;  

        return result; 
    }
    else
    {
        if (out_error)
            *out_error = error;

        return lisp_null();
    }
}

#pragma Garbage Collection

static inline Lisp gc_move(Lisp l, Heap* to)
{
    switch (l.type)
    {
        case LISP_PAIR:
        case LISP_SYMBOL:
        case LISP_STRING:
        case LISP_LAMBDA:
        {
            LispBlock* block = l.val;
            if (!(block->gc_flags & GC_MOVED))
            {
                // copy the data to new block
                LispBlock* dest = heap_alloc(block->data_size, block->type, GC_CLEAR, to);               
                memcpy(dest->data, block->data, block->data_size);
                
                // save forwarding address (offset in to)
                block->data_size = (uint64_t)dest;
                block->gc_flags = GC_MOVED;
            }
            
            // return the moved block address
            l.val = (LispBlock*)block->data_size;
            return l;
        }
        case LISP_TABLE:
        {
            LispBlock* block = l.val;
            if (!(block->gc_flags & GC_MOVED))
            {
                Table* table = (Table*)block->data;
                float load_factor = table->size / (float)table->capacity;
                
                unsigned int new_capacity = table->capacity;
                
                if (load_factor > 0.75f || load_factor < 0.1f)
                {
                    new_capacity = (table->size * 3) - 1;
                    if (new_capacity < 1) new_capacity = 1;
                }

                unsigned int new_data_size = sizeof(Table) + new_capacity * sizeof(Lisp);
                LispBlock* dest = heap_alloc(new_data_size, block->type, GC_CLEAR, to);
 
                Table* dest_table = (Table*)dest->data;
                dest_table->size = table->size;
                dest_table->capacity = new_capacity;
                
                // clear the table
                for (int i = 0; i < new_capacity; ++i)
                    dest_table->entries[i] = lisp_null();
                
                // save forwarding address (offset in to)
                block->data_size = (uint64_t)dest;
                block->gc_flags = GC_MOVED;
                
                if (LISP_DEBUG && new_capacity != table->capacity)
                {
                    printf("resizing table %i -> %i\n", table->capacity, new_capacity);
                }
                
                for (int i = 0; i < table->capacity; ++i)
                {
                    Lisp it = table->entries[i];
                    
                    while (!lisp_is_null(it))
                    {
                        unsigned int new_index = i;
                        
                        if (new_capacity != table->capacity)
                            new_index = symbol_hash(lisp_car(lisp_car(it))) % new_capacity;
                        
                        Lisp pair = gc_move(lisp_car(it), to);

                        // allocate a new cell in the to space
                        LispBlock* cons_block = heap_alloc(sizeof(Lisp) * 2, LISP_PAIR, GC_VISITED, to);

                        Lisp new_cell;
                        new_cell.type = cons_block->type;
                        new_cell.val = cons_block;
    
                        // (pair, entry)
                        lisp_set_car(new_cell, pair);
                        lisp_set_cdr(new_cell, dest_table->entries[new_index]); 
                        dest_table->entries[new_index] = new_cell;
                         
                        it = lisp_cdr(it);
                    }
                }
            }
            
            // return the moved table address
            l.val = (LispBlock*)block->data_size;
            return l;
        }
        default:
            return l;
    }
}


Lisp lisp_collect(Lisp root_to_save, LispContextRef ctx)
{
    Heap* to = &ctx->to_heap;

    // move root object
    ctx->symbol_table = gc_move(ctx->symbol_table, to);
    ctx->global_env = gc_move(ctx->global_env, to);
    Lisp result = gc_move(root_to_save, to);

    // move references  
    Page* page = to->first_page;
    while (page)
    {
        size_t offset = 0;
        while (offset < page->capacity)
        {
            LispBlock* block = (LispBlock*)(page->buffer + offset);

            if (!(block->gc_flags & GC_VISITED))
            {
                switch (block->type)
                {
                    // these add to the buffer!
                    // so lists are handled in a single pass 
                    case LISP_PAIR:
                    {
                        // move the CAR and CDR
                        Lisp* ptrs = (Lisp*)block->data;
                        for (int i = 0; i < 2; ++i)
                        {
                            if (!lisp_is_null(ptrs[i]))
                                ptrs[i] = gc_move(ptrs[i], to);
                        }
                        break;
                    }
                    case LISP_LAMBDA:
                    {
                        // move the body and args
                        Lambda* lambda = (Lambda*)block->data;
                        lambda->args = gc_move(lambda->args, to);
                        lambda->body = gc_move(lambda->body, to);
                        lambda->env = gc_move(lambda->env, to);
                        break; 
                    }
                    default: break;
                }

                block->gc_flags |= GC_VISITED;
            }

            offset += sizeof(LispBlock) + block->data_size;
        }

        page = page->next;
    }

    // DEBUG, check offsets
    page = to->first_page;
    while (page)
    {
        size_t offset = 0;
        while (offset < page->size)
        {
            LispBlock* block = (LispBlock*)(page->buffer + offset);
            offset += sizeof(LispBlock) + block->data_size;
        }
        assert(offset == page->size);
        
        page = page->next;
    }
  
    size_t diff = ctx->heap.size - ctx->to_heap.size;

    // swap the heaps
    Heap temp = ctx->heap;
    heap_reset(&temp, to->size);
    ctx->heap = ctx->to_heap;
    ctx->to_heap = temp;

    if (LISP_DEBUG)
        printf("gc collected: %lu heap: %i\n", diff, ctx->heap.size);

    return result;
}

Lisp lisp_global_env(LispContextRef ctx)
{
    return ctx->global_env;
}

void lisp_shutdown(LispContextRef ctx)
{
    heap_reset(&ctx->heap, 0);
    heap_reset(&ctx->to_heap, 0);
    free(ctx);
}

const char* lisp_error_string(LispError error)
{
    switch (error)
    {
        case LISP_ERROR_NONE:
            return "none";
        case LISP_ERROR_FILE_OPEN:
            return "file error: could not open file";
        case LISP_ERROR_PAREN_UNEXPECTED:
            return "syntax error: unexpected ) paren";
        case LISP_ERROR_PAREN_EXPECTED:
            return "syntax error: expected ) paren";
        case LISP_ERROR_BAD_TOKEN:
            return "syntax error: bad token";
        case LISP_ERROR_BAD_DEFINE:
            return "expand error: bad define (define var x)";
        case LISP_ERROR_BAD_SET:
            return "expand error: bad set (set! var x)";
        case LISP_ERROR_BAD_COND:
            return "expand error: bad cond";
        case LISP_ERROR_BAD_AND:
            return "expand error: bad and (and a b)";
        case LISP_ERROR_BAD_OR:
            return "expand error: bad or (or a b)";
        case LISP_ERROR_BAD_LET:
            return "expand error: bad let";
        case LISP_ERROR_BAD_LAMBDA:
            return "expand error: bad lambda";
        case LISP_ERROR_UNKNOWN_VAR:
            return "eval error: unknown variable";
        case LISP_ERROR_BAD_OP:
            return "eval error: application was not an operator";
        case LISP_ERROR_UNKNOWN_EVAL:
            return "eval error: got into a bad state";
        case LISP_ERROR_BAD_ARG:
            return "func error: bad argument type";
        default:
            return "unknown error code";
    }
}

#pragma mark Default Interpreter

static Lisp func_cons(Lisp args, LispError* e, LispContextRef ctx)
{
    return lisp_cons(lisp_car(args), lisp_car(lisp_cdr(args)), ctx);
}

static Lisp func_car(Lisp args, LispError* e, LispContextRef ctx)
{
    return lisp_car(lisp_car(args));
}

static Lisp func_cdr(Lisp args, LispError* e, LispContextRef ctx)
{
    return lisp_cdr(lisp_car(args));
}

static Lisp func_nav(Lisp args, LispError* e, LispContextRef ctx)
{
    Lisp path = lisp_car(args);
    Lisp l = lisp_car(lisp_cdr(args));

    return lisp_nav(l, lisp_string(path));
}

static Lisp func_eq(Lisp args, LispError* e, LispContextRef ctx)
{
    void* a = lisp_car(args).val;
    void* b = lisp_car(lisp_cdr(args)).val;

    return lisp_make_int(a == b);
}

static Lisp func_is_null(Lisp args, LispError* e, LispContextRef ctx)
{
    while (!lisp_is_null(args))
    {
        if (!lisp_is_null(lisp_car(args))) return lisp_make_int(0);
        args = lisp_cdr(args);
    }
    return lisp_make_int(1);
}

static Lisp func_display(Lisp args, LispError* e, LispContextRef ctx)
{
    Lisp l = lisp_car(args);
    if (lisp_type(l) == LISP_STRING)
    {
        printf("%s", lisp_string(l));
    }
    else
    {
        lisp_print(l); 
    }
    return lisp_null();
}

static Lisp func_newline(Lisp args, LispError* e, LispContextRef ctx)
{
    printf("\n"); return lisp_null(); 
}

static Lisp func_assert(Lisp args, LispError* e, LispContextRef ctx)
{
   if (lisp_int(lisp_car(args)) != 1)
   {
       fprintf(stderr, "assertion: ");
       lisp_printf(stderr, lisp_car(lisp_cdr(args)));
       fprintf(stderr, "\n");
       assert(0);
   }

   return lisp_null();
}

static Lisp func_equals(Lisp args, LispError* e, LispContextRef ctx)
{
    Lisp to_check  = lisp_car(args);
    if (lisp_is_null(to_check)) return lisp_make_int(1);
    
    args = lisp_cdr(args);
    while (!lisp_is_null(args))
    {
        if (lisp_int(lisp_car(args)) != lisp_int(to_check)) return lisp_make_int(0);
        args = lisp_cdr(args);
    }
    
    return lisp_make_int(1);
}

static Lisp func_list(Lisp args, LispError* e, LispContextRef ctx) { return args; }

static Lisp func_append(Lisp args, LispError* e, LispContextRef ctx)
{
    Lisp l = lisp_car(args);

    if (lisp_type(l) != LISP_PAIR)
    {
        *e = LISP_ERROR_BAD_ARG;
        return lisp_null();
    }

    args = lisp_cdr(args);
    while (!lisp_is_null(args))
    {
        l = lisp_append(l, lisp_car(args), ctx);
        args = lisp_cdr(args);
    }

    return l;
}

static Lisp func_map(Lisp args, LispError* e, LispContextRef ctx)
{
    Lisp op = lisp_car(args);

    if (lisp_type(op) != LISP_FUNC && lisp_type(op) != LISP_LAMBDA)
    {
        *e = LISP_ERROR_BAD_ARG;
        return lisp_null();
    }

    // multiple lists can be passed in
    Lisp lists = lisp_cdr(args);      
    int n = lisp_length(lists);
    if (n == 0) return lisp_null();


    Lisp result_lists = lisp_make_list(lisp_null(), n, ctx);
    Lisp result_it = result_lists;

    while (!lisp_is_null(lists))
    {        
        // advance all the lists
        Lisp it = lisp_car(lists);

        Lisp front = lisp_null();
        Lisp back = front;

        while (!lisp_is_null(it))
        { 
            Lisp expr = lisp_cons(op, lisp_cons(lisp_car(it), lisp_null(), ctx), ctx);
            Lisp result = lisp_eval(expr, lisp_global_env(ctx), NULL, ctx);

            back_append(&front, &back, result, ctx);

            it = lisp_cdr(it);
        } 

        lisp_set_car(result_it, front);
        lists = lisp_cdr(lists);
        result_it = lisp_cdr(result_it);
    }

    if (n == 1)
    {
        return lisp_car(result_lists);
    }
    else
    {
        return result_lists;
    }
}

static Lisp func_nth(Lisp args, LispError* e, LispContextRef ctx)
{
    Lisp index = lisp_car(args);
    Lisp list = lisp_car(lisp_cdr(args));
    return lisp_at_index(list, lisp_int(index));
}

static Lisp func_length(Lisp args, LispError* e, LispContextRef ctx)
{
    return lisp_make_int(lisp_length(lisp_car(args)));
}

static Lisp func_reverse_inplace(Lisp args, LispError* e, LispContextRef ctx)
{
    return lisp_reverse_inplace(lisp_car(args));
}

static Lisp func_assoc(Lisp args, LispError* e, LispContextRef ctx)
{
    return lisp_assoc(lisp_car(args), lisp_car(lisp_cdr(args)));
}

static Lisp func_add(Lisp args, LispError* e, LispContextRef ctx)
{
    Lisp accum = lisp_car(args);
    args = lisp_cdr(args);

    while (!lisp_is_null(args))
    {
        if (lisp_type(accum) == LISP_INT)
        {
            accum.int_val += lisp_int(lisp_car(args));
        }
        else if (lisp_type(accum) == LISP_FLOAT)
        {
            accum.float_val += lisp_float(lisp_car(args));
        }
        args = lisp_cdr(args);
    }
    return accum;
}

static Lisp func_sub(Lisp args, LispError* e, LispContextRef ctx)
{
    Lisp accum = lisp_car(args);
    args = lisp_cdr(args);

    while (!lisp_is_null(args))
    {
        if (lisp_type(accum) == LISP_INT)
        {
            accum.int_val -= lisp_int(lisp_car(args));
        }
        else if (lisp_type(accum) == LISP_FLOAT)
        {
            accum.float_val -= lisp_float(lisp_car(args));
        }
        else
        {
            *e = LISP_ERROR_BAD_ARG;
            return lisp_null();
        }
        args = lisp_cdr(args);
    }
    return accum;
}

static Lisp func_mult(Lisp args, LispError* e, LispContextRef ctx)
{
    Lisp accum = lisp_car(args);
    args = lisp_cdr(args);

    while (!lisp_is_null(args))
    {
        if (lisp_type(accum) == LISP_INT)
        {
            accum.int_val *= lisp_int(lisp_car(args));
        }
        else if (lisp_type(accum) == LISP_FLOAT)
        {
            accum.float_val *= lisp_float(lisp_car(args));
        }
        else
        {
            *e = LISP_ERROR_BAD_ARG;
            return lisp_null();
        } 
        args = lisp_cdr(args);
    }
    return accum;
}

static Lisp func_divide(Lisp args, LispError* e, LispContextRef ctx)
{
    Lisp accum = lisp_car(args);
    args = lisp_cdr(args);

    while (!lisp_is_null(args))
    {
        if (lisp_type(accum) == LISP_INT)
        {
            accum.int_val /= lisp_int(lisp_car(args));
        }
        else if (lisp_type(accum) == LISP_FLOAT)
        {
            accum.float_val /= lisp_float(lisp_car(args));
        }
        else
        {
            *e = LISP_ERROR_BAD_ARG;
            return lisp_null();
        } 
        args = lisp_cdr(args);
    }
    return accum;
}

static Lisp func_less(Lisp args, LispError* e, LispContextRef ctx)
{
    Lisp accum = lisp_car(args);
    args = lisp_cdr(args);
    int result = 0;

    if (lisp_type(accum) == LISP_INT)
    {
        result = lisp_int(accum) < lisp_int(lisp_car(args));
    }
    else if (lisp_type(accum) == LISP_FLOAT)
    {
        result = lisp_float(accum) < lisp_float(lisp_car(args));
    }
    else
    {
        *e = LISP_ERROR_BAD_ARG;
        return lisp_null();
    } 
    return lisp_make_int(result);
}

static Lisp func_greater(Lisp args, LispError* e, LispContextRef ctx)
{
    Lisp accum = lisp_car(args);
    args = lisp_cdr(args);
    int result = 0;

    if (lisp_type(accum) == LISP_INT)
    {
        result = lisp_int(accum) > lisp_int(lisp_car(args));
    }
    else if (lisp_type(accum) == LISP_FLOAT)
    {
        result = lisp_float(accum) > lisp_float(lisp_car(args));
    }
    else
    {
        *e = LISP_ERROR_BAD_ARG;
        return lisp_null();
    } 
    return lisp_make_int(result);
}

static Lisp func_less_equal(Lisp args, LispError* e, LispContextRef ctx)
{
    // a <= b = !(a > b)
    Lisp l = func_greater(args, e, ctx);
    return  lisp_make_int(!lisp_int(l));
}

static Lisp func_greater_equal(Lisp args, LispError* e, LispContextRef ctx)
{
    // a >= b = !(a < b)
    Lisp l = func_less(args, e, ctx);
    return  lisp_make_int(!lisp_int(l));
}

static Lisp func_even(Lisp args, LispError* e, LispContextRef ctx)
{
    while (!lisp_is_null(args))
    {
        if ((lisp_int(lisp_car(args)) & 1) == 1) return lisp_make_int(0);
        args = lisp_cdr(args);
    } 
    return lisp_make_int(1);
}

static Lisp func_odd(Lisp args, LispError* e, LispContextRef ctx)
{
    while (!lisp_is_null(args))
    {
        if ((lisp_int(lisp_car(args)) & 1) == 0) return lisp_make_int(0);
        args = lisp_cdr(args);
    } 

    return lisp_make_int(1); 
}

static Lisp func_read_path(Lisp args, LispError *e, LispContextRef ctx)
{
    const char* path = lisp_string(lisp_car(args));
    Lisp result = lisp_read_path(path, e, ctx);
    return result;
}

static Lisp func_expand(Lisp args, LispError* e, LispContextRef ctx)
{
    Lisp expr = lisp_car(args);
    Lisp result = lisp_expand(expr, e, ctx);
    return result;
}

LispContextRef lisp_init_raw(int symbol_table_size)
{
    LispContextRef ctx = malloc(sizeof(struct LispContext));
    ctx->lambda_counter = 0;
    heap_init(&ctx->heap);
    heap_init(&ctx->to_heap);

    ctx->symbol_table = lisp_make_table(symbol_table_size, ctx);
    ctx->global_env = lisp_null();
    return ctx;
}

LispContextRef lisp_init_interpreter(void)
{
    LispContextRef ctx = lisp_init_raw(512);
    Lisp table = lisp_make_table(256, ctx);
    lisp_table_set(table, lisp_make_symbol("NULL", ctx), lisp_null(), ctx);

    const char* names[] = {
        "CONS",
        "CAR",
        "CDR",
        "NAV",
        "EQ?",
        "NULL?",
        "LIST",
        "APPEND",
        "MAP",
        "NTH",
        "LENGTH",
        "REVERSE!",
        "ASSOC",
        "DISPLAY",
        "NEWLINE",
        "ASSERT",
        "READ-PATH",
        "EXPAND",
        "=",
        "+",
        "-",
        "*",
        "/",
        "<",
        ">",
        "<=",
        ">=",
        "EVEN?",
        "ODD?",
        NULL,
    };

    LispFunc funcs[] = {
        func_cons,
        func_car,
        func_cdr,
        func_nav,
        func_eq,
        func_is_null,
        func_list,
        func_append,
        func_map,
        func_nth,
        func_length,
        func_reverse_inplace,
        func_assoc,
        func_display,
        func_newline,
        func_assert,
        func_read_path,
        func_expand,
        func_equals,
        func_add,
        func_sub,
        func_mult,
        func_divide,
        func_less,
        func_greater,
        func_less_equal,
        func_greater_equal,
        func_even,
        func_odd,
        NULL,
    };

    lisp_table_add_funcs(table, names, funcs, ctx);
    ctx->global_env = lisp_make_env(table, ctx); 

    return ctx;
}

LispContextRef lisp_init_reader(void)
{
    LispContextRef ctx = lisp_init_raw(512);
    return ctx;
}



