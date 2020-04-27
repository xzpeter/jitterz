TARGETS = jitterz
CFLAGS		= -O2 -Wall -D _GNU_SOURCE

%: %.c
	$(CC) $(CFLAGS) $< -o $@

all: $(TARGETS)

clean:
	rm -f *.o $(TARGETS)

