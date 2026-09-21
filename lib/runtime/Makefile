# BSTM runtime (Agent G)
CC      ?= gcc
OPT     ?= -O3
# x86-64-v3 = AVX2 + BMI2（transpose 的四輪 block swap 會用到）。
# 換平台請改這裡：ARM 用 -mcpu=native，或留空退回純量版。
ARCH    ?= -march=x86-64-v3
CFLAGS  ?= $(OPT) $(ARCH) -std=c11 -Wall -Wextra -MMD -MP -I. -I../../include -pthread
ARFLAGS  = rcs

SRC = transpose.c bstf_reader.c refill.c batch.c pool.c stats.c
OBJ = $(SRC:.c=.o)
DEP = $(OBJ:.o=.d)
LIB = libbstm.a

all: $(LIB)

$(LIB): $(OBJ)
	$(AR) $(ARFLAGS) $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(DEP) $(LIB)

test: $(LIB)
	$(MAKE) -C ../../test test

bench: $(LIB)
	$(MAKE) -C ../../test bench

.PHONY: all clean test bench
-include $(DEP)
