CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wshadow -pthread -Iinclude
LDLIBS  += -pthread

B := build

.PHONY: all test valgrind bench preload-test clean

all: $(B)/libma.a $(B)/libma.so $(B)/test_ma $(B)/test_threads $(B)/bench $(B)/demo

$(B):
	mkdir -p $(B)

$(B)/ma.o: src/ma.c include/ma.h | $(B)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(B)/ma_vg.o: src/ma.c include/ma.h | $(B)
	$(CC) $(CFLAGS) -DMA_VALGRIND -c $< -o $@

$(B)/ma_preload.o: src/ma_preload.c include/ma.h | $(B)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(B)/libma.a: $(B)/ma.o
	ar rcs $@ $^

$(B)/libma.so: $(B)/ma.o $(B)/ma_preload.o
	$(CC) -shared $(CFLAGS) $^ -o $@ $(LDLIBS)

$(B)/test_ma: tests/test_ma.c $(B)/libma.a
	$(CC) $(CFLAGS) $< $(B)/libma.a -o $@ $(LDLIBS)

$(B)/test_ma_vg: tests/test_ma.c $(B)/ma_vg.o
	$(CC) $(CFLAGS) -DMA_VALGRIND $< $(B)/ma_vg.o -o $@ $(LDLIBS)

$(B)/test_threads: tests/test_threads.c $(B)/libma.a
	$(CC) $(CFLAGS) $< $(B)/libma.a -o $@ $(LDLIBS)

$(B)/bench: bench/bench.c $(B)/libma.a
	$(CC) $(CFLAGS) $< $(B)/libma.a -o $@ $(LDLIBS) -lm

$(B)/demo: tests/demo.c $(B)/libma.a
	$(CC) $(CFLAGS) -O0 $< $(B)/libma.a -o $@ $(LDLIBS)

$(B)/leak_vg: tests/leak.c $(B)/ma_vg.o
	$(CC) $(CFLAGS) -O0 $< $(B)/ma_vg.o -o $@ $(LDLIBS)

test: $(B)/test_ma $(B)/test_threads $(B)/libma.so
	./$(B)/test_ma
	./$(B)/test_threads
	MA_ARENAS=1 ./$(B)/test_threads
	MA_NO_SBRK=1 ./$(B)/test_ma
	./scripts/preload_test.sh

# memcheck with the allocator's client requests: should be error-free...
valgrind: $(B)/test_ma_vg $(B)/leak_vg
	valgrind --error-exitcode=1 --leak-check=full -q ./$(B)/test_ma_vg --quick
	# ...and should catch the bugs planted in tests/leak.c (expected to fail)
	-valgrind --leak-check=full ./$(B)/leak_vg

bench: $(B)/bench
	./scripts/run_bench.sh

clean:
	rm -rf $(B)
