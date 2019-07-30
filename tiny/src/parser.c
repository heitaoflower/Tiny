#include <setjmp.h>
#include <assert.h>

#include "stringpool.h"

// Parser and parsing functions

// Note that if there is an error while parsing, all the code parsed up to that point
// will still be present in the string pool, symbol and type table; Presumably
// the user would discard the entire compilation unit on such a result since the
// entire unit might be unusable at that point.

typedef struct Parser
{
    Tiny_Context* ctx;
    Arena arena;

    Tiny_StringPool* sp;
    Symbols* sym;
    TypetagPool* tp;
    float** numbers;

    Lexer l;
    TokenType curTok;

    char* errorMessage;

    jmp_buf topLevelEnv;

    // All buffers (pointer to pointer the start of their mem) which the ASTs
    // store so we can clear them out in one go. It's a pointer to their
    // pointer because once they are resized, the actual buffer start pointer
    // might move
    void*** buffers;
} Parser;

static void InitParser(Parser* p, Tiny_Context* ctx, const char* fileName, const char* src, size_t len, Tiny_StringPool* sp, Symbols* sym, TypetagPool* tp, float** numbers)
{
    assert(*numbers);

    p->ctx = ctx;

    p->sp = sp;
    p->sym = sym;
    p->tp = tp;
    p->numbers = numbers;

    InitArena(&p->arena, ctx);
    InitLexer(&p->l, ctx, fileName, src, len);

    p->errorMessage = NULL;

    INIT_BUF(p->buffers, ctx);
}

#define PARSER_INIT_BUF(b, p) do { \
    INIT_BUF(b, (p)->ctx); \
    BUF_PUSH((p)->buffers, (void*)(&(b))); \
} while(0)

static AST* ParseExpr(Parser* p);
static AST* ParseStatement(Parser* p);

#define PARSER_ERROR_LONGJMP(p) do { \
    longjmp((p)->topLevelEnv, 1); \
} while(0)


#define PARSER_ERROR(p, fmt, ...) do { \
    (p)->errorMessage = MemPrintf((p)->ctx, (fmt), __VA_ARGS__); \
    PARSER_ERROR_LONGJMP(p); \
} while(0)

static AST* AllocAST(Parser* p, ASTType type)
{
    AST* ast = ArenaAlloc(&p->arena, sizeof(AST));

    if(!ast) {
        PARSER_ERROR(p, "Failed to allocate AST; out of memory.");
    }

    ast->type = type;
    ast->pos = p->l.pos;

    return ast;
}

static TokenType GetNextToken(Parser* p)
{
    p->curTok = GetToken(&p->l);

    if(p->curTok == TOK_ERROR) {
        PARSER_ERROR_LONGJMP(p);
    }

    return p->curTok;
}

#define EXPECT_TOKEN(p, type, fmt, ...) do { \
    if((p)->curTok != (type)) { \
        PARSER_ERROR(p, fmt, __VA_ARGS__); \
    } \
} while(0)

#define EAT_TOKEN(p, type, fmt, ...) do { \
    if((p)->curTok == (type)) { \
        GetNextToken(p); \
    } else { \
        PARSER_ERROR(p, fmt, __VA_ARGS__); \
    } \
} while(0)

static Sym* ParseType(Parser* p)
{
	EXPECT_TOKEN(p, TOK_IDENT, "Expected identifier for typename.");

	Sym* s = RegisterType(p->sym, p->l.lexeme, p->l.pos);
	GetNextToken(p);

	return s;
}

static AST* ParseFunc(Parser* p)
{
    assert(p->curTok == TOK_FUNC);

    if(p->sym->func) {
        PARSER_ERROR(p, "Attempted to define a function inside of function '%s'.", p->sym->func->name);
    }

    AST* ast = AllocAST(p, AST_PROC);

    GetNextToken(p);

    EXPECT_TOKEN(p, TOK_IDENT, "Function name must be identifier!");

    const char* funcName = Tiny_StringPoolInsert(p->sp, p->l.lexeme);

	ast->proc.decl = DeclareFunc(p->sym, funcName, p->l.pos);

    if(!ast->proc.decl) {
        PARSER_ERROR_LONGJMP(p);
    }
    
    p->sym->func = ast->proc.decl;

    GetNextToken(p);

    EAT_TOKEN(p, TOK_OPENPAREN, "Expected '(' after function name.");

    Typetag** argTypes = NULL;
    Typetag* retType = NULL;

    INIT_BUF(argTypes, p->ctx);
    
    while(p->curTok != TOK_CLOSEPAREN) {
        EXPECT_TOKEN(p, TOK_IDENT, "Expected identifier in function parameter list");

        const char* name = Tiny_StringPoolInsert(p->sp, p->l.lexeme);

        Sym* arg = DeclareVar(p->sym, name, p->l.pos, true);

        if(!arg) {
            PARSER_ERROR_LONGJMP(p);
        }

        GetNextToken(p);

        EAT_TOKEN(p, TOK_COLON, "Expected ':' after '%s'.", name);

        arg->var.type = ParseType(p);

        BUF_PUSH(argTypes, arg->var.type);

        if(p->curTok != TOK_CLOSEPAREN && p->curTok != TOK_COMMA) {
            PARSER_ERROR(p, "Expected ')' or ',' after parameter type in function parameter list.");
        }

        if(p->curTok == TOK_COMMA) {
            GetNextToken(p);
        }
    }

    GetNextToken(p);

    if(p->curTok != TOK_COLON) {
        retType = GetPrimitiveTypetag(p->tp, TYPETAG_VOID);
    } else {
        GetNextToken(p);
        retType = ParseType(p);
    }

    ast->proc.decl->func.type = InternFuncTypetag(p->tp, argTypes, retType, false);

    PushScope(p);
    
    ast->proc.body = ParseStatement(p);

    PopScope(p);

    p->sym->func = NULL;

    if(!ast->proc.body) {
        return NULL;
    }

    return ast;
}

#ifndef UCHAR_MAX
#define UCHAR_MAX 255
#endif

static Sym* ParseStruct(Parser* p)
{
    if(p->sym->func) {
        PARSER_ERROR(p, "Attempted to declare struct inside func '%s'. Can't do that bruh.", p->sym->func->name);
    }

    TokenPos pos = p->l.pos;

    GetNextToken(p);

    EXPECT_TOKEN_RETURN_NULL(p, TOK_IDENT, "Expected identifier after 'struct'.");

    const char* name = Tiny_StringPoolInsert(p->sp, p->l.lexeme);

    Sym* s = RegisterType(p->sym, name, pos);

    if(!s) {
        PARSER_ERROR_LONGJMP(p);
    }

    if(s->typetag) {
        PARSER_ERROR(p, "Attempted to define struct '%s' multiple times.", name);
    }

    GetNextToken(p);

    EAT_TOKEN_RETURN_NULL(p, TOK_OPENCURLY, "Expected '{' after struct name.");

    const char** names = NULL;
    Typetag** types = NULL;

    INIT_BUF(names, p->ctx);
    INIT_BUF(types, p->ctx);

    while(p->curTok != TOK_CLOSECURLY) {
        if(!EXPECT_TOKEN(p, TOK_IDENT, "Expected identifier in struct fields.")) {
            DESTROY_BUF(names);
            DESTROY_BUF(types);
            return NULL;
        }

        int count = BUF_LEN(names);

        if(count >= UCHAR_MAX) {
            DESTROY_BUF(names);
            DESTROY_BUF(types);
            PARSER_ERROR(p, "Too many fields in struct.");
        }

        const char* name = Tiny_StringPoolInsert(p->sp, p->l.lexeme);

        for(int i = 0; i < count; ++i) {
            if(names[i] == name) {
                DESTROY_BUF(names);
                DESTROY_BUF(types);
                PARSER_ERROR(p, "Declared multiple fields with the same name '%s'.", name);
            }
        }

        BUF_PUSH(names, name);

        GetNextToken(p);

		if(p->curTok == TOK_COLON) {
			GetNextToken(p);
		} else {
			DESTROY_BUF(names);
			DESTROY_BUF(types);
			PARSER_ERROR(p, "Expected ':' after field name.");
		}

        Typetag* type = ParseType(p);

        BUF_PUSH(types, type);
    }

    GetNextToken(p);

    s->typetag = InternStructTypetag(p->tp, names, types);

    return s;
}

static AST* ParseCall(Parser* p, const char* name)
{
    assert(p->curTok == TOK_OPENPAREN);

    AST* ast = AllocAST(p, AST_CALL);

    PARSER_INIT_BUF(ast->call.args, p->ctx);

    GetNextToken(p);

    while(p->curTok != TOK_CLOSEPAREN) {
        AST* a = ParseExpr(p);

        BUF_PUSH(ast->call.args, a);

        if(p->curTok == TOK_COMMA) {
            GetNextToken(p);
        } else if(p->curTok != TOK_CLOSEPAREN) {
            PARSER_ERROR(p, "Expected ')' after call.");
        }
    }

    ast->call.calleeName = name;

    GetNextToken(p);
    return ast;
}

static int RegisterNumber(Parser* p, float f)
{
    int c = BUF_LEN(*p->numbers);

    for(int i = 0; i < c; ++i) {
        if((*p->numbers)[i] == f) {
            return i;
        }
    }

    BUF_PUSH(*p->numbers, f);
    return c;
}

static AST* ParseFactor(Parser* p)
{
    switch(p->curTok) {
        case TOK_NULL: {
            AST* ast = AllocAST(p, AST_NULL);
            GetNextToken(p);

            return ast;
        } break;

        case TOK_BOOL: {
            AST* ast = AllocAST(p, AST_BOOL);

            ast->boolean = p->l.bValue;

            return ast;
        } break;

        case TOK_IDENT: {
            const char* name = Tiny_StringPoolInsert(p->sp, p->l.lexeme);

            GetNextToken(p);

            if(p->curTok == TOK_OPENPAREN) {
                return ParseCall(p, name);
            }

            AST* ast = AllocAST(p, AST_ID);

            ast->id.name = name;
            ast->id.sym = ReferenceVar(p->sym, name);

            while(p->curTok == TOK_DOT) {
                AST* a = AllocAST(p, AST_DOT);

                GetNextToken(p);

                EXPECT_TOKEN_RETURN_NULL(p, TOK_IDENT, "Expected identifier after '.'");

                a->dot.lhs = ast;
                a->dot.field = Tiny_StringPoolInsert(p->sp, p->l.lexeme);

                GetNextToken(p);

                ast = a;
            }

            return ast;
        } break;

        case TOK_MINUS: case TOK_BANG: {
            int op = p->curTok;

            GetNextToken(p);

            AST* ast = AllocAST(p, AST_UNARY);

            ast->unary.op = op;
            ast->unary.exp = ParseFactor(p);

            return ast;
        } break;

        case TOK_CHAR: {
            AST* ast = AllocAST(p, AST_CHAR);
            ast->iValue = p->l.iValue;

            GetNextToken(p);

            return ast;
        } break;

        case TOK_INT: {
            AST* ast = AllocAST(p, AST_INT);
            ast->iValue = p->l.iValue;

            GetNextToken(p);

            return ast;
        } break;

        case TOK_FLOAT: {
            AST* ast = AllocAST(p, AST_INT);
            ast->fIndex = RegisterNumber(p->cp, p->l.fValue);

            GetNextToken(p);

            return ast;
        } break;

        case TOK_STRING: {
            AST* ast = AllocAST(p, AST_STRING);
            ast->str = Tiny_StringPoolInsert(p->sp, p->l.lexeme);

            GetNextToken(p);

            return ast;
        } break;

        case TOK_OPENPAREN: {
            AST* ast = AllocAST(p, AST_PAREN);

            GetNextToken(p);

            ast->paren = ParseExpr(p);

            EAT_TOKEN_RETURN_NULL(p, TOK_CLOSEPAREN, "Expected ')' to match previous '('.");

            return ast;
        } break;

        case TOK_NEW: {
            AST* ast = AllocAST(p, AST_CONSTRUCTOR);

            GetNextToken(p);

            const char* typeName = Tiny_StringPoolInsert(p->sp, p->l.lexeme);

            ast->constructor.structSym = RegisterType(p->sym, typeName);

            if(!ast->constructor.structSym) {
                PARSER_ERROR_LONGJMP(p);
            }

            GetNextToken(p);

            EAT_TOKEN_RETURN_NULL(p, TOK_OPENCURLY, "Expected '{' after struct name.");

            PARSER_INIT_BUF(ast->constructor.args, p);

            while(p->curTok != TOK_CLOSECURLY) {
                AST* a = ParseExpr(p);

                BUF_PUSH(ast->constructor.args, a);

                if(p->curTok == TOK_COMMA) {
                    GetNextToken(p);
                } else if(p->curTok != TOK_CLOSECURLY) {
                    PARSER_ERROR(p, "Expected '}' or ',' in constructor arg list.");
                }
            }

            GetNextToken(p);

            return ast;
        } break;

        case TOK_CAST: {
            AST* ast = AllocAST(p, AST_CAST);

            GetNextToken(p);

            EAT_TOKEN_RETURN_NULL(p, TOK_OPENPAREN, "Expected '(' after 'cast'.");

            ast->cast.value = ParseExpr(p);
            ast->cast.tag = ParseType(p);

            EAT_TOKEN_RETURN_NULL(p, TOK_CLOSEPAREN, "Expected ')' to match previous '(' after cast.");

            return ast;
        } break;

        default: break;
    }

    PARSER_ERROR(p, "Unexpected token '%s'.\n", p->l.lexeme);
    return NULL;
}

static int GetTokenPrec(int tok)
{
    int prec = -1;

    switch(tok) {
        case TOK_STAR: case TOK_SLASH: case TOK_PERCENT: case TOK_AND: case TOK_OR: prec = 5; break;

        case TOK_PLUS: case TOK_MINUS:                prec = 4; break;

        case TOK_LTE: case TOK_GTE:
        case TOK_EQUALS: case TOK_NOTEQUALS:
        case TOK_LT: case TOK_GT:                prec = 3; break;

        case TOK_LOG_AND: case TOK_LOG_OR:        prec = 2; break;
    }

    return prec;
}

static AST* ParseBinRHS(Parser* p, AST* lhs, int exprPrec)
{
    while(true) {
        int prec = GetTokenPrec(p->curTok);

        if(prec < exprPrec) {
            return lhs;
        }

        int op = p->curTok;

        GetNextToken(p);

        AST* rhs = ParseFactor(p);

        int nextPrec = GetTokenPrec(p->curTok);

        if(prec < nextPrec) {
            rhs = ParseBinRHS(p, rhs, prec + 1);
        }

        AST* newLhs = AllocAST(p, AST_BINARY);

        newLhs->binary.lhs = lhs;
        newLhs->binary.rhs = rhs;
        newLhs->binary.op = op;

        lhs = newLhs;
    }
}

static AST* ParseExpr(Parser* p)
{
    AST* factor = ParseFactor(p);
    return ParseBinRHS(p, factor, 0);
}

static AST* ParseStatement(Parser* p);

static AST* ParseBlock(Parser* p)
{
    assert(p->curTok == TOK_OPENCURLY);

    AST* ast = AllocAST(p, AST_BLOCK);
    GetNextToken(p);

    PARSER_INIT_BUF(ast->block, p);

    while(p->curTok != TOK_CLOSECURLY) {
        AST* a = ParseStatement(p);
        BUF_PUSH(ast->block, a);
    }

    GetNextToken(p);

    return ast;
}

static AST* ParseIf(Parser* p)
{
    assert(p->curTok == TOK_IF);

    AST* ast = AllocAST(p, AST_IF);

    GetNextToken(p);

    ast->ifx.cond = ParseExpr(p);

    PushScope(p);
    ast->ifx.body = ParseStatement(p);
    PopScope(p);
    
    if(p->curTok == TOK_ELSE) {
        GetNextToken(p);
        PushScope(p);
        ast->ifx.alt = ParseStatement(p);
        PopScope(p);
    } else {
        ast->ifx.alt = NULL;
    }

    return NULL;
}

static AST* ParseStatement(Parser* p)
{
    switch(p->curTok) {
        case TOK_OPENCURLY: return ParseBlock(p);
        
        case TOK_IDENT: {
            const char* name = Tiny_StringPoolInsert(p->sp, p->l.lexeme);
            TokenPos pos = p->l.pos;

            GetNextToken(p);

            if(p->curTok == TOK_OPENPAREN) {
                return ParseCall(p, name);
            }

            AST* lhs = AllocAST(p, AST_ID);

            lhs->id.sym = ReferenceVar(p->sym, name);
            lhs->id.name = name;

            while(p->curTok == TOK_DOT) {
                AST* a = AllocAST(p, AST_DOT);

                GetNextToken(p);

                EXPECT_TOKEN_RETURN_NULL(p, TOK_IDENT, "Expected identifier after '.'");

                a->dot.lhs = lhs;
                a->dot.field = Tiny_StringPoolInsert(p->sp, p->l.lexeme);

                GetNextToken(p);

                lhs = a;
            }

            int op = p->curTok;

            if(op == TOK_DECLARE || op == TOK_COLON) {
                if(lhs->type != AST_ID) {
                    PARSER_ERROR(p, "Left hand side of declaration must be an identifier.");
                }

                lhs->id.sym = DeclareVar(p->sym, name, pos, false);

                if(op == TOK_COLON) {
                    GetNextToken(p);
                    lhs->id.sym->var.type = ParseType(p);

                    EXPECT_TOKEN_RETURN_NULL(p, TOK_EQUAL, "Expected '=' after typename.");

                    op = TOK_EQUAL;
                }
            }

            // If the precedence is >= 0 then it's an expression operator
            if(GetTokenPrec(op) >= 0) {
                PARSER_ERROR(p, "Expected assignment statement.");
            }

            GetNextToken(p);

            AST* rhs = ParseExpr(p);

            if(op == TOK_DECLARECONST) {
                if(lhs->type != AST_ID) {
                    PARSER_ERROR(p, "Left hand side of declaration must be an identifier.");
                }

                switch(rhs->type) {
                    case AST_BOOL: {
                        DeclareConst(p->sym, name, pos, GetPrimitiveTypetag(p->tp, TYPETAG_BOOL))->constant.bValue = rhs->boolean;
                    } break;

                    case AST_CHAR: {
                        DeclareConst(p->sym, name, pos, GetPrimitiveTypetag(p->tp, TYPETAG_CHAR))->constant.iValue = rhs->iValue;
                    } break;

                    case AST_INT: {
                        DeclareConst(p->sym, name, pos, GetPrimitiveTypetag(p->tp, TYPETAG_INT))->constant.iValue = rhs->iValue;
                    } break;

                    case AST_FLOAT: {
                        DeclareConst(p->sym, name, pos, GetPrimitiveTypetag(p->tp, TYPETAG_FLOAT))->constant.fIndex = rhs->fIndex;
                    } break;

                    case AST_STRING: {
                        DeclareConst(p->sym, name, pos, GetPrimitiveTypetag(p->tp, TYPETAG_STR))->constant.str = rhs->str;
                    } break;

                    default: {
                        PARSER_ERROR(p, "Expected bool, char, int, float, or string literal as right hand side for constant '%s'.", name);
                    } break;
                }
            }

            AST* ast = AllocAST(p, AST_BINARY);

            ast->binary.lhs = lhs;
            ast->binary.rhs = rhs;
            ast->binary.op = op;
        } break;
        
        case TOK_FUNC: return ParseFunc(p);  
        case TOK_IF: return ParseIf(p);

        case TOK_WHILE: {
            AST* ast = AllocAST(p, AST_WHILE);

            GetNextToken(p);

            ast->whilex.cond = ParseExpr(p);

            PushScope(p);
            ast->whilex.body = ParseStatement(p);
            PopScope(p);

            return ast;
        } break;

        case TOK_FOR: {
            AST* ast = AllocAST(p, AST_FOR);

            GetNextToken(p);

            PushScope(p);

            ast->forx.init = ParseStatement(p);
            
            EAT_TOKEN_RETURN_NULL(p, TOK_SEMI, "Expected ';' after for initializer.");

            ast->forx.cond = ParseExpr(p);

            EAT_TOKEN_RETURN_NULL(p, TOK_SEMI, "Expected ';' after for condition.");

            ast->forx.step = ParseStatement(p);

            ast->forx.body = ParseStatement(p);

            PopScope(p);

            return ast;
        } break;

        case TOK_RETURN: {
            if(!p->sym->func) {
                PARSER_ERROR(p, "Attempted to return from outside a function. Why? Why would you do that? Why would you do any of that?");
            }

            AST* ast = AllocAST(p, AST_RETURN);

            GetNextToken(p);

            if(p->curTok == TOK_SEMI) {
                GetNextToken(p);
                ast->retExpr = NULL;
                return ast;
            }

            if(p->sym->func.type->func.ret->type == TYPETAG_VOID) {
                PARSER_ERROR(p, "Attempted to return value from function which is supposed to return nothing (void).");
            }

            ast->retExpr = ParseExpr(p);

            return ast;
        } break;
    }
}

static AST** ParseProgram(Parser* p)
{
    int c = setjmp(p->topLevelEnv);
    if(c) {
        return NULL;
    }

    GetNextToken(p);

    AST** asts;

    PARSER_INIT_BUF(asts, p);

    while(p->curTok != TOK_EOF) {
        if(p->curTok == TOK_STRUCT) {
            ParseStruct(p);
        } else {
            AST* ast = ParseStatement(p);
            BUF_PUSH(asts, ast);
        }
    }

    return asts;
}

static void ClearParserError(Parser* p)
{
    if(p->errorMessage) {
        TFree(p->ctx, p->errorMessage);
    }
}

static void DestroyParser(Parser* p)
{
    ClearParserError(p);

    for(int i = 0; i < BUF_LEN(p->buffers); ++i) {
        DESTROY_BUF(p->buffers[i]);
    }

    DestroyLexer(&p->l);
    DestroyArena(&p->arena);
}
