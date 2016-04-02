EXEC = \
	test-async \
	test-reactor \
	test-buffer \
	test-protocol-server \
	httpd

OUT ?= .build
.PHONY: all
all: $(OUT) $(EXEC)

CC ?= gcc
CFLAGS = -std=gnu99 -Wall -O2 -g
ifeq ($(strip $(PROFILE)),1)
PROF_FLAGS = -pg -g
CFLAGS += $(PROF_FLAGS)
LDFLAGS += $(PROF_FLAGS)
endif

CFLAGS += -I .
LDFLAGS = -lpthread

PT ?= async
XDOT = xdot
GPROF2DOT = gprof2dot
OBJS := \
	async.o \
	reactor.o \
	buffer.o \
	protocol-server.o
deps := $(OBJS:%.o=%.o.d)
OBJS := $(addprefix $(OUT)/,$(OBJS))
deps := $(addprefix $(OUT)/,$(deps))

httpd: $(OBJS) httpd.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test-%: $(OBJS) tests/test-%.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OUT)/%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ -MMD -MF $@.d $<

$(OUT):
	@mkdir -p $@

doc:
	@doxygen

perf:
	perf stat --repeat 10 \
	-e cache-misses,cache-references,instructions,cycles,branch-misses,branch-instructions \
	./test-$(PT)

plot: check-gmon
	gprof ./test-$(PT) | $(GPROF2DOT) > $@.dot; \
	dot -Tpng -o $@.png $@.dot; \
	$(XDOT) ./$@.dot;

check-gmon:
	@(test -s test-$(PT) || make PROFILE=1)
	@(test -s gmon.out || ./test-$(PT))
	@(test -s gmon.out || { echo "ERROR: PROFILE needed be set to 1"; exit 1; })

astyle:
	astyle --style=kr --indent=spaces=4 --indent-switches --suffix=none *.[ch]

bench:
	ab -c 32 -n 100 http://localhost:8080/

clean:
	$(RM) $(EXEC) $(OBJS) $(deps) *.dot *.png gmon.out
	@rm -rf $(OUT)

distclean: clean
	@rm -rf html
