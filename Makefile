PROG = sprites_test.c
BIN = sprites_test

all: $(BIN)

$(OSDBIN): $(PROG)
	gcc -o $@ $<

clean:
	rm -f $(BIN)
