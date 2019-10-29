TARGETS = jitterz
CFLAGS		= -O2 -Wall

%: %.c
	$(CC) $(CFLAGS) $< -o $@

all: $(TARGETS)

clean:
	rm -f *.o $(TARGETS)

