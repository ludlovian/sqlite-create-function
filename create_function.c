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
 */

typedef struct Function Function;
typedef struct Connection Connection;
typedef struct CallContext CallContext;

/*
 *  One of these is held for each databse connection. It is attached to the
 *  connection as clientdata and destroyed automatically when the
 *  connection is closed
 *
 *      connection_get          retrieves and/or creates one
 *      connection_destroy      destructor called by SQLite
 *
 *      connection_clear        removes all cached statements
 */

struct Connection {
    sqlite3 *db;
    int bCache;
    int nStmt;
    int nFunc;
    Function *first;
};

static Connection *connection_get(sqlite3 *);
static void connection_destroy(void *);
static void connection_clear(Connection *);

/*
 *  One of these is held for each UDF we know about. It is attached to the
 *  function as user_data and destroyed automatically as the function is
 *  removed
 *
 *      function_create         constructor, adds to linked list
 *      function_find           finds the function in the list
 *      function_destroy        destructor call by SQLite - removes from list
 *
 *      fucntion_acquire        gets the prepared statement for use by the VM
 *      function_release        releases the statment once the VM has finished
 *      function_clear          ensures the statement is uncached
 *
 *      function_register       registers the UDF with SQLite
 */

struct Function {
    char *zName;
    char *zSql;
    int nArg;
    int idFunc;
    sqlite3_stmt *stmt;
    Connection *conn;
    Function *next;
};

static Function *function_create(Connection *);
static void function_destroy(void *);
static Function *function_find(Connection *,const char *);

static sqlite3_stmt *function_acquire(sqlite3_context *,Function *);
static void function_release(Function *);

static void function_clear(Function *);
static int function_register(sqlite3_context *, Function *);

/*
 *  One of these is held during the top level VM involving this function.
 *  It is created on demand, and stored as auxdata on the context with a
 *  negative slot. This is an undocumnet implementation feature, but used
 *  within SQLite - for example in the JSON functions - so I am in good
 *  company. The destructor is called automatically when the VM finishes.
 *
 *      context_get             retrieves or creates the context
 *      context_destroy         destructor called by SQLite
 */

/* -(16777216*'c' + 65536*'f' + 256*'u' + 'n') */
#define CFUN_SLOT -1667659118

struct CallContext {
    Function *func;
    sqlite3_stmt* stmt;
};

static CallContext *context_get(sqlite3_context *,Function *);
static void context_destroy(void *);

/************************************************************************
 *
 *  Forward declarations
 *
 */
static void run_function(sqlite3_context *, int, sqlite3_value **);


/************************************************************************
 *
 *  Connection
 *
 */

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

static void connection_destroy(void *data) {
    Connection *conn = (Connection *)data;

    ASSERT(conn);
    TRACE("connection_destroy");

    connection_clear(conn);
    sqlite3_free(conn);
}

static void connection_clear(Connection *conn) {
    Function *p;

    ASSERT(conn);

    if( conn->nStmt ) {
        TRACE("connection_clear");
        for( p = conn->first; p; p = p->next ) {
            function_clear(p);
        }
    }
}

/************************************************************************
 *
 *  external version of connection_clear
 *
 */

void create_function_clear(sqlite3 *db) {
    Connection* conn;

    ASSERT(db);
    conn = (Connection *)sqlite3_get_clientdata(db, "create_function");

    if( conn ) {
        TRACE("create_function_clear");
        connection_clear(conn);
    }
}

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

static void function_destroy(void *data) {
    Function *func = (Function *)data;

    TRACE("function_destroy(%s)", func->zName);

    Function **pp;
    function_clear(func);
    for( pp = &func->conn->first; *pp; pp = &((*pp)->next) ) {
        if( *pp == func ) {
            *pp = func->next;
            break;
        }
    }
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

static sqlite3_stmt *function_acquire(sqlite3_context *ctx, Function *func) {
    int rc;

    ASSERT(func);
    if( func->stmt ) return func->stmt;

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
        return NULL;
    }
    func->conn->nStmt++;
    return func->stmt;
}

static void function_release(Function *func) {
    ASSERT(func);

    if( !func->conn->bCache ) function_clear(func);
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

static int function_register(sqlite3_context *ctx, Function *func) {
    sqlite3_stmt* stmt;
    sqlite3* db;
    int rc;

    ASSERT(func);
    ASSERT(func->conn);
    ASSERT(func->conn->db);

    TRACE("function_register(%s)", func->zName);

    db = func->conn->db;
    stmt = function_acquire(ctx, func);

    if( !stmt ||
        !sqlite3_stmt_readonly(stmt) ||
         sqlite3_column_count(stmt) != 1
    ) {
        function_release(func);
        sqlite3_result_error(ctx, "Invalid function definition", -1);
        return 0;
    }

    func->nArg = sqlite3_bind_parameter_count(stmt);

    rc = sqlite3_create_function_v2(
        func->conn->db,
        func->zName,
        func->nArg,
        SQLITE_UTF8|SQLITE_DETERMINISTIC|SQLITE_INNOCUOUS,
        func,
        run_function, NULL, NULL,
        function_destroy
    );

    function_release(func);

    if( rc != SQLITE_OK ) {
        TRACE("function_register: rc=%d", rc);
        sqlite3_result_error_code(ctx, rc);
        sqlite3_result_error(ctx, sqlite3_errmsg(db), -1);
    }
    return rc == SQLITE_OK;
}


/************************************************************************
 *
 *  Call Context
 *
 */

static CallContext *context_get(sqlite3_context *ctx, Function *func) {
    CallContext *p;
    int slot;

    ASSERT(func);

    slot = CFUN_SLOT + func->idFunc;
    p = (CallContext *)sqlite3_get_auxdata(ctx, slot);
    if (p) return p;

    TRACE("context_get(%s)", func->zName);

    p = sqlite3_malloc(sizeof(CallContext));
    if (!p) goto nomem;
    p->func = func;
    p->stmt = function_acquire(ctx, func);
    if (!p->stmt) {
        sqlite3_free(p);
        return NULL;
    }

    sqlite3_set_auxdata(ctx, slot, p, context_destroy);
    if( !sqlite3_get_auxdata(ctx, slot) ) goto nomem;

    return p;
nomem:
    sqlite3_result_error_nomem(ctx);
    return NULL;
}

static void context_destroy(void *data) {
    CallContext *p = (CallContext *)data;


    ASSERT(p);
    ASSERT(p->func);
    TRACE("context_destroy(%s)", p->func->zName);

    function_release(p->func);
    p->stmt = NULL;
    sqlite3_free(p);
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

    CallContext *p = context_get(ctx, func);
    if (!p) goto errexit;

    ASSERT(p->stmt);
    ASSERT(sqlite3_bind_parameter_count(p->stmt) == func->nArg);

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
    sqlite3 *db;
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

    func = function_create(conn);
    if( !func ) goto nomem;

    func->zName = sqlite3_mprintf("%s", zName);
    func->zSql = sqlite3_mprintf("%s", zSql);
    if( !func->zName || !func->zSql ) {
        function_destroy(func);
        goto nomem;
    }

    if( !function_register(ctx, func) != SQLITE_OK ) return;

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

