CFLAGS = -I ./NtyCo/core/ -L ./NtyCo/ -luring -lntyco -lpthread -ldl

OBJS = kvstore.o reactor.o proactor.o kvs_array.o ntyco.o kvs_rbtree.o kvs_hash.o

.PHONY: all NtyCo/

all: NtyCo/ kvstore

NtyCo/:
	@echo ./NtyCo/
	make -C NtyCo/

kvstore: $(OBJS)
	gcc -o kvstore kvstore.o reactor.o proactor.o kvs_array.o ntyco.o kvs_rbtree.o kvs_hash.o $(CFLAGS)

kvstore.o: kvstore.c
	gcc $(CFLAGS) -c kvstore.c -o kvstore.o

reactor.o: reactor.c
	gcc $(CFLAGS) -c reactor.c -o reactor.o

proactor.o: proactor.c
	gcc $(CFLAGS) -c proactor.c -o proactor.o

kvs_array.o: kvs_array.c
	gcc $(CFLAGS) -c kvs_array.c -o kvs_array.o

ntyco.o: ntyco.c
	gcc $(CFLAGS) -c ntyco.c -o ntyco.o

kvs_rbtree.o: kvs_rbtree.c
	gcc $(CFLAGS) -c kvs_rbtree.c -o kvs_rbtree.o

kvs_hash.o: kvs_hash.c
	gcc $(CFLAGS) -c kvs_hash.c -o kvs_hash.o