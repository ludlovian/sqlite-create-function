#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/************************************************************************
 *
 *  Macros
 */
#ifdef TEST_TRACE
#define TRACE(fmt, ...) fprintf(stderr, "[TRACE] " fmt "\n", ##__VA_ARGS__)
#else
#define TRACE(fmt, ...)
#endif

/************************************************************************
 *
 *  Data structurues
 *
 */

typedef struct Function Function;
typedef struct Connection Connection;

/*
 * One of these is held for each UDF
 */

struct Function {
    char *name;
    int nArg;
    sqlite3_stmt *stmt;
    Connection *conn;
    Function *next;
};

/*
 * One of these is held for each connection
 */

struct Connection {
    sqlite3 *db;
    Function *first_function;
};

/************************************************************************
 *
 *  Destructors
 *
 *  The destroy_XXXX destructors are called automatically. They clear
 *  everything.
 *
 *  The clear_XXX destructor are also called by cleanup routine. They
 *  close any outstanding statments and unlink the lists, but leave
 *  the structs in place.
 *
 *  So, if we cleanup by the explicit cleanup routine, all that the
 *  formal destructors have to do is clear the main struct.
 *
 */

static Function* find_function (Connection *conn, const char *name, int nArg) {
    Function *func;
    for( func = conn->first_function; func; func = func->next ) {
        if( strcmp(name, func->name) == 0 && func->nArg == nArg ) return func;
    }
    return NULL;
}

static void clear_function(Function *p) {
    Function **pp;
    if( p && p->conn ) {
        TRACE("clear_function: '%s'", p->name);
        if( p->stmt ) sqlite3_finalize(p->stmt);

        sqlite3_free(p->name);

        /* Disconnect from parent's list */
        for( pp = &p->conn->first_function; *pp; pp = &((*pp)->next) ) {
            if (*pp == p) {
                *pp = p->next;
                break;
            }
        }
        p->name = NULL;
        p->stmt = NULL;
        p->conn = NULL;
    }
}

static void destroy_function(void *pAux) {
    Function *p = (Function *)pAux;
    if( p ) {
        TRACE("destroy_function: '%s'", p->name ? p->name : "");
        clear_function(p);
        free(p);
    }
}

static void clear_connection(Connection *conn) {
    Function *p;
    if( conn && conn->first_function ) {
        TRACE("clear_connection");
        for( p = conn->first_function; p; p = p->next ) {
            clear_function(p);
        }
    }
}

static void destroy_connection(void *pAux) {
    Connection *p = (Connection *)pAux;
    if( p ) {
        TRACE("destroy_connection");
        clear_connection(p);
        free(p);
    }
}

/************************************************************************
 *
 *  Generic UDF implementation
 *
 */

static void function_call(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    Function *p = (Function *)sqlite3_user_data(ctx);
    if (!p || !p->stmt) {
        sqlite3_result_error(ctx, "Function has been cleared", -1);
        return;
    }

    if( sqlite3_reset(p->stmt) != SQLITE_OK ) goto errexit;
    if( sqlite3_clear_bindings(p->stmt) != SQLITE_OK ) goto errexit;

    for (int i = 0; i < argc; i++) {
        if( sqlite3_bind_value(p->stmt, i + 1, argv[i]) != SQLITE_OK ) goto errexit;
    }

    int rc = sqlite3_step(p->stmt);
    if (rc == SQLITE_ROW) {
        sqlite3_result_value(ctx, sqlite3_column_value(p->stmt, 0));
    } else if (rc == SQLITE_DONE) {
        sqlite3_result_null(ctx);
    } else {
        goto errexit;
    }
    sqlite3_reset(p->stmt);
    return;
errexit:
    sqlite3_result_error(ctx, sqlite3_errmsg(p->conn->db), -1);
}

/************************************************************************
 *
 *  Function cleanup
 *
 */

void create_function_clear(sqlite3 *db) {
    Connection *p = (Connection *)sqlite3_get_clientdata(db, "create_function");
    if( p ) {
        TRACE("create_function_clear");
        clear_connection(p);
    }
}

static void create_function_clear_call(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    sqlite3 *db = sqlite3_context_db_handle(ctx);
    create_function_clear(db);
    sqlite3_result_text(ctx, "OK", -1, SQLITE_STATIC);
}

/************************************************************************
 *
 *  Factory Function
 *
 */

static void create_function_call(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    sqlite3 *db;
    const char *zName, *zSqlBody;
    char *zFuncSql = NULL;
    Connection *conn;
    Function *func = NULL;
    sqlite3_stmt *stmt = NULL;
    int nArg;
    int rc;

    db = sqlite3_context_db_handle(ctx);
    zName = (const char *)sqlite3_value_text(argv[0]);
    zSqlBody = (const char *)sqlite3_value_text(argv[1]);

    if (!zName || !zSqlBody) {
        sqlite3_result_error(ctx, "Invalid arguments", -1);
        return;
    }

    conn = (Connection *)sqlite3_get_clientdata(db, "create_function");
    if( !conn ) {
        conn = calloc(1, sizeof(Connection));
        if( !conn ) goto nomem;
        conn->db = db;
        sqlite3_set_clientdata(db, "create_function", conn, destroy_connection);
        TRACE("create_function: Connection created");
    }

    zFuncSql = sqlite3_mprintf("SELECT %s", zSqlBody);
    if( sqlite3_prepare_v2(db, zFuncSql, -1, &stmt, NULL) != SQLITE_OK ) {
        sqlite3_free(zFuncSql);
        goto errexit;
    }
    sqlite3_free(zFuncSql);

    if( !sqlite3_stmt_readonly(stmt) || sqlite3_column_count(stmt) != 1 ) {
        sqlite3_result_error(ctx, "Invalid function definition", -1);
        sqlite3_finalize(stmt);
        goto errexit;
    }
    nArg = sqlite3_bind_parameter_count(stmt);

    /* if this is already registered, then we error out. You cannot redefine
     * a function whilst processing a statement (ie now) */
    if( find_function(conn, zName, nArg) ) {
        sqlite3_result_error(ctx, "Cannot redefine a function", -1);
        sqlite3_finalize(stmt);
        goto errexit;
    }

    func = calloc(1, sizeof(Function));
    if( !func ) {
        sqlite3_finalize(stmt);
        goto nomem;
    }

    func->name = sqlite3_mprintf("%s", zName);
    func->nArg = nArg;
    func->stmt = stmt;
    func->conn = conn;
    func->next = conn->first_function;
    conn->first_function = func;

    TRACE("create_function: '%s' with %d params", zName, nArg);

    rc = sqlite3_create_function_v2(db, func->name, func->nArg,
            SQLITE_UTF8 | SQLITE_DETERMINISTIC,
            func, function_call, NULL, NULL, destroy_function);

    /* If create function failed the destructor will have been called */
    if( rc != SQLITE_OK ) {
        TRACE("create_func: register rc=%d", rc);
        goto errexit;
    }

    sqlite3_result_text(ctx, "OK", -1, SQLITE_STATIC);
    return;
errexit:
    sqlite3_result_error(ctx, sqlite3_errmsg(db), -1);
    return;
nomem:
    sqlite3_result_error_nomem(ctx);
}

/************************************************************************
 *
 *  Entry Point
 *
 */

int sqlite3_createfunction_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi) {
    SQLITE_EXTENSION_INIT2(pApi);
    sqlite3_create_function(db, "create_function", 2, SQLITE_UTF8, NULL, create_function_call, NULL, NULL);
    sqlite3_create_function(db, "create_function_clear", 0, SQLITE_UTF8, NULL, create_function_clear_call, NULL, NULL);
    return SQLITE_OK;
}

