PROG=zmk-connector

all: $(PROG)

LDFLAGS+=-L /opt/homebrew/lib/ -lhidapi -lcjson
CFLAGS+=-isystem /opt/homebrew/include -g

SOURCES = connector.c

$(PROG): $(SOURCES)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

clean:
	rm $(PROG)
