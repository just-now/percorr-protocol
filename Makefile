.PHONY: all
all:
	gcc -Wall -Wpedantic -O0 -g -o party main.c sm.c -luv

.PHONY: test
test:
	rm -f file.x
	./party leader 127.0.0.1 127.0.0.1   2>&1  | tee leader_log.txt &
	sleep 1
	./party follower 127.0.0.1 127.0.0.1 2>&1  | tee follower_log.txt &
	sleep 4
	killall party
	cat follower_log.txt leader_log.txt | grep LIBDQ > obs/trace.txt
	cd obs && rm -f tree_* chronoscope.db && ./env python3  -m chronoscope create -t trace.txt -c chronoscope.yaml
	cd obs && ./env ./browse chronoscope.db ' '
	diff -u main.c file.x

