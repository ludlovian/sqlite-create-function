# sqlite-create-function
An SQLite UDF to create other UDFs via SQL.

Everything is done via one UDF call `create_function`.

## Basic usage

### create\_function (name, definition)

This will create a new user-defined function with the parameterised SQL as its
definition.

```sql
SELECT create_function('power_sum', '(?1 * ?1) + (?2 * ?2)'); -- returns OK

SELECT power_sum(3, 4); -- returns 25
```

It is best to only use numbered bind parameters, rather than named ones.

### create\_function (name)

This will return the definition of the function, or `NULL` if it is not defined.


## Advanced usage

### create\_function (NULL, command)

The configuration can be adjusted by sending a command. This is done by
setting the first parameter to `create_function` and sending the command as
the second parameter.

#### cache

This will turn on statement caching. This means that statements will not be
finalized at the end of the outer SQL call, but kept open to avoid re-preparing.

Having statements open has implications. In particular calling `sqlite3_close`
will likely fail. And UDFs (not jsut these) cannot be removed or changed
whilst statements remain open.

Many bindings use `sqlite3_close_v2` which _may well_ work (untested). But it
is good practice to clear the cache before quitting.

Caching is turned off initially, unless the extension is compiled with
`CACHE_STATEMENTS` defined, in which case it defaults to on.

The return value is a string decribing how many statements are currently
cached - which will be zero if you have only just turned caching on, but
also allows youl to see the size of the cache.


#### clear

This clears any cached statements and turns caching off.

If you have turned caching on, then call this before quitting.

The return value is a string describing how many cached statements were
cleared.

### void createfunction\_enable\_cache (sqlite3\*, int bOnOff)

This C function carries out the same as the "clear" and "cache" commands,
depending on the `bOnOff` value. It allows custom bindings to set caching
at the start and and later undo it just before closing the connection.


## Limitations

You cannot undefine or modify UDFs once created with this. They will be
destroyed when you close the connection.

**Care**: UDFs created here are tagged as INNOCUOUS and DETERMINISTIC.
So make sure they are, or be prepared to take your own risks.

No thread safety. Everything is stored per connection, so one thread per
connection will be fine. If you want multiple threads to share a connection,
then you will have headaches - and not just from this library.
