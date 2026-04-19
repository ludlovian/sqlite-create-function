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
    char *zName;
    char *zSql;
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
 *  ths strings & structs, and hence the lists.
 *
 *  The clear_XXX destructor are also called by cleanup routine. They
 *  close any outstanding statments but leave the lists, structs and
 *  strings in place
 *
 *  This leaves the function structs in place after clearing, but with
 *  no active stmt.
 *
 */

static Function* find_function(Connection *conn, const char *z) {
    Function *func;
    for( func = conn->first_function; func; func = func->next ) {
        if( strcmp(z, func->zName) == 0 ) return func;
    }
    return NULL;
}

static void clear_function(Function *p) {
    if( p && p->stmt ) {
        TRACE("clear_function: '%s'", p->zName);
        sqlite3_finalize(p->stmt);
        p->stmt = NULL;
    }
}

static void destroy_function(void *pAux) {
    Function *p = (Function *)pAux;
    if( p ) {
        TRACE("destroy_function: '%s'", p->zName ? p->zName : "");
        clear_function(p);
        sqlite3_free(p->zName);
        sqlite3_free(p->zSql);
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

static Connection* get_connection(sqlite3 *db) {
    Connection *conn;
    conn = (Connection *)sqlite3_get_clientdata(db, "create_function");
    if( conn ) return conn;

    conn = calloc(1, sizeof(Connection));
    if( !conn ) return NULL;

    conn->db = db;
    sqlite3_set_clientdata(db, "create_function", conn, destroy_connection);
    TRACE("get_connection: Connection created");
    return conn;
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
    const char *zName, *zSqlBody = NULL;
    char *zSql = NULL;
    Connection *conn;
    Function *func = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc;

    db = sqlite3_context_db_handle(ctx);
    zName = (const char *)sqlite3_value_text(argv[0]);
    if( argc == 2 ) zSqlBody = (const char *)sqlite3_value_text(argv[1]);

    if( !zName || (argc == 2 && !zSqlBody) ) {
        sqlite3_result_error(ctx, "Invalid arguments", -1);
        return;
    }

    conn = get_connection(db);
    if( !conn ) goto nomem;

    func = find_function(conn, zName);

    if( argc == 1 ) {
        if( func ) {
            if (func->stmt) {
                sqlite3_result_text(ctx, func->zSql+7, -1, SQLITE_TRANSIENT);
            } else {
                sqlite3_result_text(ctx, "cleared", -1, SQLITE_STATIC);
            }
        } else {
            sqlite3_result_null(ctx);
        }
        return;
    }

    if( func ) {
        sqlite3_result_error(ctx, "Cannot redefine a function", -1);
        return;
    }


    zSql = sqlite3_mprintf("SELECT %s", zSqlBody);
    if( !zSql ) goto nomem;

    if( sqlite3_prepare_v2(db, zSql, -1, &stmt, NULL) != SQLITE_OK ) {
        sqlite3_free(zSql);
        goto errexit;
    }

    if( !sqlite3_stmt_readonly(stmt) || sqlite3_column_count(stmt) != 1 ) {
        sqlite3_result_error(ctx, "Invalid function definition", -1);
        sqlite3_free(zSql);
        sqlite3_finalize(stmt);
        return;
    }

    func = calloc(1, sizeof(Function));
    if( !func ) {
        sqlite3_free(zSql);
        sqlite3_finalize(stmt);
        goto nomem;
    }

    func->zName = sqlite3_mprintf("%s", zName);
    func->zSql = zSql;
    func->stmt = stmt;
    func->conn = conn;
    func->next = conn->first_function;
    conn->first_function = func;

    TRACE("create_function: '%s'", zName);

    rc = sqlite3_create_function_v2(db, func->zName,
            sqlite3_bind_parameter_count(stmt), SQLITE_UTF8 | SQLITE_DETERMINISTIC,
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
    sqlite3_create_function(db, "create_function", 1, SQLITE_UTF8, NULL, create_function_call, NULL, NULL);
    sqlite3_create_function(db, "create_function", 2, SQLITE_UTF8, NULL, create_function_call, NULL, NULL);
    sqlite3_create_function(db, "create_function_clear", 0, SQLITE_UTF8, NULL, create_function_clear_call, NULL, NULL);
    return SQLITE_OK;
}

