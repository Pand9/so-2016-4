# you can set some environment variables for make with syntax NAME1=VALUE1 [NAME2=VALUE2 ...] make <targets>
# NO_LOGGING=1 => server/client generates practically no diagnostic output
# For clang users:
# COMPILER=<some clang compiler> => clang will be used instead of default gcc
# 	compiler flags will be adjusted to clang (no -posix + some sanitizers)
# SANITIZE=thread/address => clang will compile with tsan/asan



COMPILER?=gcc
CC=$(COMPILER)

ifeq ($(NO_LOGGING),1)
	NO_LOGGING=-DNO_LOGGING
else
	NO_LOGGING=
endif

ifeq ($(CC),gcc)
	FLAGS=-Wall -std=c99 -Wextra -D_XOPEN_SOURCE=600 \
		-Wextra -Wshadow -Wswitch-default \
		-Wno-unused-parameter -Wformat -pthread -g -O0 \
		-Wno-missing-field-initializers -pedantic \
		-posix $(NO_LOGGING)
else
	ifeq ($SANITIZE,address)
		SANITIZE=-fsanitize=address
	else
		ifeq ($SANITIZE,thread)
			SANITIZE=-fsanitize=thread
		else
			SANITIZE=
		endif
	endif
	FLAGS=-Wall -std=c99 -Wextra -D_XOPEN_SOURCE=600 \
		-Wextra -Wshadow -Wswitch-default \
		-Wno-unused-parameter -Wformat -pthread -g -O0 \
		-Wno-missing-field-initializers -pedantic \
		$(SANITIZE) $(NO_LOGGING)
endif


ALL=serwer-m klient-m serwer-s klient-s

all: $(ALL)

serwer-m: serwer.c kom-m.o err.o 
	$(CC) $(FLAGS) -o $@ $^

klient-m: klient.c kom-m.o err.o 
	$(CC) $(FLAGS) -o $@ $^ 

serwer-s: serwer.c kom-s.o err.o
	$(CC) $(FLAGS) -o $@ $^ 

klient-s: klient.c kom-s.o err.o
	$(CC) $(FLAGS) -o $@ $^ 

kom-m.o: kom-m.c kom.h
	$(CC) $(FLAGS) -c $<

kom-s.o: kom-s.c kom.h 
	$(CC) $(FLAGS) -c $<

err.o: err.c err.h
	$(CC) $(FLAGS) -c $<

clean:
	rm -f *.o $(ALL)

.PHONY: all clean