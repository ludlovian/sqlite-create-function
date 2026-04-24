name = create_function

CFLAGS := -std=c99 -Wall -O3 $(CFLAGS)

soext = so
src = $(name).c
module = $(name).$(soext)

.PHONY: all clean test

$(module): $(src)
	$(CC) -fPIC -shared $(CFLAGS) -o $@ $^

all: $(module)

test: $(module)
	sqlite3 test.sql

clean:
	$(RM) $(module)
