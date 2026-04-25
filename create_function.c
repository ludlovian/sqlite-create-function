#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

/************************************************************************
 *
 *  Macros
 */

#ifdef DEBUG_TRACE
  #define TRACE(fmt, ...) fprintf(stderr, "[TRACE] " fmt "\n", ##__VA_ARGS__)
#else
  #define TRACE(fmt, ...) ((void)0)
#endif

#ifdef DEBUG_ASSERT
  #define ASSERT(expr) \
    do { \
      if( !(expr) ) { \
        fprintf(stderr, "[ASSERT] %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        abort(); \
      } \
    } while (0)
#else
  #define ASSERT(expr) ((void)0)
#endif


/************************************************************************
 *
 *  Data structurues
 *
 *
 *  The core structure is a Connection. One is created for each connection
 *  and this is stored as clientdata. The destructor is automatically
 *  called when the connection is closed.
 *
 *  It contains a linked list of Function structs - one for each UDF we
 *  have created. Each function is registered with this as the userdata.
 *
 *  Similarly, a pointer to the relevant Function is stored as auxdata
 *  on the relevant execution context. It is stored using a negative
 *  number - an undocumented implementation feature, although used within
 *  the core library - which will keep it alive until the execution
 *  concludes. The destructor to this will "release" the prepared statement
 *  back - either to the cache or to be finalized.
 *
 */

typedef struct Connection Connection;
typedef struct Function Function;
typedef struct CallContext CallContext;

struct Connection {
    sqlite3 *db;
    int bCache;
    int nStmt;
    int nFunc;
    Function *first;
};

struct Function {
    char *zName;
    char *zSql;
    int nArg;
    int idFunc;
    sqlite3_stmt *stmt;
    Connection *conn;
    Function *next;
};

/* -(16777216*'c' + 65536*'f' + 256*'u' + 'n') */
#define CFUN_SLOT -1667659118



/************************************************************************
 *
 *  Forward declarations
 *
 */
static void run_function(sqlite3_context *, int, sqlite3_value **);


/************************************************************************
 *
 *  Function
 *
 */

static Function *function_create(Connection *conn){
    Function *p;

    ASSERT(conn);
    TRACE("function_create");

    p = sqlite3_malloc(sizeof(Function));
    if (!p) return NULL;

    memset(p, 0, sizeof(*p));
    p->idFunc = conn->nFunc++;
    p->conn = conn;
    p->next = conn->first;
    conn->first = p;

    return p;
}

static void function_clear(Function *func) {
    ASSERT(func);

    if( func->stmt ) {
        TRACE("function_clear(%s): finalize", func->zName);
        sqlite3_finalize(func->stmt);
        func->stmt = NULL;
        func->conn->nStmt--;
    }
}

static void function_destroy(Function *func) {
    ASSERT(func);

    TRACE("function_destroy(%s)", func->zName);

    function_clear(func);
    sqlite3_free(func->zName);
    sqlite3_free(func->zSql);
    sqlite3_free(func);
}

static Function *function_find(Connection *conn, const char *z) {
    Function *p;

    ASSERT(conn);
    for( p = conn->first; p; p = p->next ) {
        if (strcasecmp(z, p->zName) == 0) return p;
    }
    return NULL;
}

static int function_acquire(sqlite3_context *ctx, Function *func) {
    int rc;

    ASSERT(func);
    if( func->stmt ) return 1;

    TRACE("function_acquire(%s): prepare", func->zName);

    rc = sqlite3_prepare_v3(
        func->conn->db,
        func->zSql,
        -1,
        func->conn->bCache ? SQLITE_PREPARE_PERSISTENT : 0,
        &func->stmt,
        NULL
    );
    if( rc != SQLITE_OK ) {
        TRACE("function_acquire(%s): finalize on error", func->zName);
        sqlite3_finalize(func->stmt);
        sqlite3_result_error_code(ctx, rc);
        return 0;
    }
    func->conn->nStmt++;
    return 1;
}

static void function_release(Function *func) {
    ASSERT(func);

    if( !func->conn->bCache ) function_clear(func);
}

static void function_release_generic (void *data) {
    if( data ) function_release((Function *)data);
}

static int function_register(
    sqlite3_context *ctx,
    Connection *conn,
    const char *zName,
    const char *zSql
) {
    Function *func = NULL;
    sqlite3_stmt* stmt;
    int rc;

    ASSERT(conn);
    ASSERT(conn->db);

    TRACE("function_register(%s): prepare", zName);

    rc = sqlite3_prepare_v3(
        conn->db,
        zSql,
        -1,
        conn->bCache ? SQLITE_PREPARE_PERSISTENT : 0,
        &stmt,
        NULL
    );
    if( rc != SQLITE_OK ||
        !sqlite3_stmt_readonly(stmt) ||
        sqlite3_column_count(stmt) != 1
    ) {
        TRACE("function_register(%s): finalize on error", zName);
        sqlite3_finalize(stmt);
        goto badargs;
    }

    func = function_create(conn);
    if( !func ) {
        sqlite3_finalize(stmt);
        goto nomem;
    }

    func->zName = sqlite3_mprintf("%s", zName);
    func->zSql = sqlite3_mprintf("%s", zSql);
    if( !func->zName || !func->zSql ) {
        sqlite3_finalize(stmt);
        goto nomem;
    }

    func->nArg = sqlite3_bind_parameter_count(stmt);
    func->stmt = stmt;
    conn->nStmt++;

    rc = sqlite3_create_function(
        conn->db,
        func->zName,
        func->nArg,
        SQLITE_UTF8|SQLITE_DETERMINISTIC|SQLITE_INNOCUOUS,
        func,
        run_function, NULL, NULL
    );

    function_clear(func);

    if( rc != SQLITE_OK ) {
        TRACE("function_register: rc=%d", rc);
        sqlite3_result_error_code(ctx, rc);
        sqlite3_result_error(ctx, sqlite3_errmsg(conn->db), -1);
    }
    return (rc == SQLITE_OK);
badargs:
    char *errmsg = sqlite3_mprintf("Invalid function definition: %s", zName);
    sqlite3_result_error(ctx, errmsg, -1);
    sqlite3_free(errmsg);
    return 0;
nomem:
    sqlite3_result_error_nomem(ctx);
    return 0;
}

/************************************************************************
 *
 *  Connection
 *
 */

static void connection_destroy(void *data) {
    Connection *conn = (Connection *)data;

    ASSERT(conn);
    TRACE("connection_destroy");

    Function *func, *next;

    func = conn->first;
    while( func ) {
        next = func->next;
        function_destroy(func);
        func = next;
    }
    conn->first = NULL;
    sqlite3_free(conn);
}

static Connection *connection_get(sqlite3 *db) {
    Connection* conn;

    conn = (Connection *)sqlite3_get_clientdata(db, "create_function");
    if( conn ) return conn;

    TRACE("connection_get: create");

    conn = sqlite3_malloc(sizeof(Connection));
    if( !conn ) return NULL;
    memset(conn, 0, sizeof(Connection));
    conn->db = db;
#ifdef CACHE_STATEMENTS
    conn->bCache = 1;
#else
    conn->bCache = 0;
#endif

    sqlite3_set_clientdata(db, "create_function", conn, connection_destroy);

    return conn;
}

static void connection_clear(Connection *conn) {
    Function *p;

    ASSERT(conn);

    if( conn->nStmt ) {
        TRACE("connection_clear");
        for( p=conn->first; p; p=p->next ) {
            function_clear(p);
        }
    }
}

/************************************************************************
 *
 *  external version of connection_clear
 *
 */

void createfunction_enable_cache(sqlite3 *db, int bOnOff) {
    Connection* conn;

    ASSERT(db);
    conn = connection_get(db);
    if( !conn ) return;

    TRACE("createfunction_enable_cache(%d)", bOnOff);
    if( bOnOff ) {
        conn->bCache = 1;
    } else {
        connection_clear(conn);
        conn->bCache = 0;
    }
}

/************************************************************************
 *
 *  Call Context
 *
 */

static int context_ensure(sqlite3_context *ctx, Function *func) {
    int slot;

    ASSERT(func);

    slot = CFUN_SLOT + func->idFunc;
    if( ((Function *)sqlite3_get_auxdata(ctx, slot)) == func ) return 1;

    TRACE("context_ensure(%s)", func->zName);

    if( !function_acquire(ctx, func) ) return 0;

    sqlite3_set_auxdata(ctx, slot, func, function_release_generic);
    if( !sqlite3_get_auxdata(ctx, slot) ) goto nomem;

    return 1;
nomem:
    sqlite3_result_error_nomem(ctx);
    return 0;
}


/************************************************************************
 *
 *  Generic function call to evaluate the UDF
 *
 */

static void run_function(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    Function *func = (Function *)sqlite3_user_data(ctx);
    ASSERT(func);
    ASSERT(func->nArg == argc);

    if( !context_ensure(ctx, func) ) goto errexit;

    ASSERT(func->stmt);
    ASSERT(sqlite3_bind_parameter_count(func->stmt) == func->nArg);

    if( sqlite3_reset(func->stmt) != SQLITE_OK ) goto errexit;
    if( sqlite3_clear_bindings(func->stmt) != SQLITE_OK ) goto errexit;

    for (int i = 0; i < argc; i++) {
        if( sqlite3_bind_value(func->stmt, i + 1, argv[i]) != SQLITE_OK ) goto errexit;
    }

    int rc = sqlite3_step(func->stmt);
    if (rc == SQLITE_ROW) {
        sqlite3_result_value(ctx, sqlite3_column_value(func->stmt, 0));
    } else if (rc == SQLITE_DONE) {
        sqlite3_result_null(ctx);
    } else {
        goto errexit;
    }
    sqlite3_reset(func->stmt);
    return;
errexit:
    sqlite3_result_error(ctx, sqlite3_errmsg(func->conn->db), -1);
}


/************************************************************************
 *
 *  create_function
 *
 */

static void show_function(sqlite3_context *ctx, Connection *conn, const char *zName) {
    Function *func;

    ASSERT(conn);
    ASSERT(zName);

    TRACE("create_function: show(%s)", zName);

    func = function_find(conn, zName);
    if( !func ) {
        sqlite3_result_null(ctx);
    } else {
        sqlite3_result_text(ctx, func->zSql, -1, SQLITE_TRANSIENT);
    }
}

static void do_command(sqlite3_context *ctx, Connection *conn, const char *zCmd) {

    ASSERT(conn);
    ASSERT(zCmd);

    TRACE("create_function: command(%s)", zCmd);

    if( strcasecmp(zCmd, "cache") == 0 ) {
        int nStmt = conn->nStmt;
        conn->bCache = 1;
        sqlite3_result_text(
            ctx,
            sqlite3_mprintf("%d statement%s cached", nStmt, nStmt == 1 ? "" : "s"),
            -1,
            sqlite3_free
        );
    } else if( strcasecmp(zCmd, "clear") == 0 ) {
        int nStmt = conn->nStmt;
        connection_clear(conn);
        conn->bCache = 0;
        sqlite3_result_text(
            ctx,
            sqlite3_mprintf("%d statement%s cleared", nStmt, nStmt == 1 ? "" : "s"),
            -1,
            sqlite3_free
        );
    } else {
        sqlite3_result_error(ctx, "Unknown command", -1);
    }
}


static void create_function_call(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    sqlite3 *db = NULL;
    Connection *conn;
    Function *func;
    const char *zName, *zSql = NULL;

    db = sqlite3_context_db_handle(ctx);
    ASSERT(db);

    conn = connection_get(db);
    if( !conn ) goto nomem;

    if( argc == 1 ) {
        if( sqlite3_value_type(argv[0]) != SQLITE_TEXT ) goto badargs;
        show_function(ctx, conn, (const char *)sqlite3_value_text(argv[0]));
        return;
    }

    if( sqlite3_value_type(argv[0]) == SQLITE_NULL ) {
        if( sqlite3_value_type(argv[1]) != SQLITE_TEXT ) goto badargs;
        do_command(ctx, conn, (const char *)sqlite3_value_text(argv[1]));
        return;
    }

    if( sqlite3_value_type(argv[0]) != SQLITE_TEXT || 
        sqlite3_value_type(argv[1]) != SQLITE_TEXT ) goto badargs;


    zName = (const char *)sqlite3_value_text(argv[0]);
    zSql = (const char *)sqlite3_value_text(argv[1]);

    func = function_find(conn, zName);
    if( func ) {
        sqlite3_result_error(ctx, "Cannot redefine a function", -1);
        return;
    }

    TRACE("create_function: create(%s)", zName);

    if( !function_register(ctx, conn, zName, zSql) ) return;

    sqlite3_result_text(ctx, "OK", -1, SQLITE_STATIC);
    return;
badargs:
    sqlite3_result_error(ctx, "Bad arguments to create_function", -1);
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
    sqlite3_create_function(db, "create_function", 1, SQLITE_UTF8|SQLITE_INNOCUOUS, NULL, create_function_call, NULL, NULL);
    sqlite3_create_function(db, "create_function", 2, SQLITE_UTF8|SQLITE_INNOCUOUS, NULL, create_function_call, NULL, NULL);
    return SQLITE_OK;
}

