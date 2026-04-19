# sqlite-create-function
An SQLite UDF to create other UDFs via SQL

It caches the prepared statement for speed. But that means that these have to
be cleared - either via SQL or a C function call - before closing.

If you use `sqlite3_close_v2` you _should_ be okay without doing this - but
it is good practice to clean up your own debris.

## create\_function (name, definition)

SELECT this to create a new UDF.

```sql
SELECT create_function('power_sum', '(?1 * ?1) + (?2 * ?2)'); -- returns OK

SELECT power_sum(3, 4); -- returns 25
```

You cannot redefine a UDF once created using this extension.

## create\_function\_clear()

SELECT this to close all the cached prepared statements.

Don't do anything much after this. Previously defined functions will now error.

## void create\_function\_clear (sqlite3*)

The C version of the cleanup function
