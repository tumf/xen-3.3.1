obj-$(x86_32) += common.o guest_2.o guest_3.o
obj-$(x86_64) += common.o guest_2.o guest_3.o guest_4.o

guest_%.o: multi.c $(HDRS) Makefile
	$(CC) $(CFLAGS) -DGUEST_PAGING_LEVELS=$* -c $< -o $@
