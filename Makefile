party: main.c sm.c sm.h
	gcc -Wall -Wpedantic -O0 -g -o $@ $^ -luv

.PHONY: test
test: party
	rm -f fbulk.bin
	dd if=/dev/urandom of=bulk.bin bs=3K count=10
	./party leader 127.0.0.1 127.0.0.1 bulk.bin 2>&1  | tee leader_log.txt &
	sleep 1
	./party follower 127.0.0.1 127.0.0.1 fbulk.bin 2>&1  | tee follower_log.txt &
	sleep 4
	killall party
	grep -F LIBDQ follower_log.txt leader_log.txt > obs/trace.txt
	cd obs && rm -f tree_* chronoscope.db && ./env python3  -m chronoscope create -t trace.txt -c chronoscope.yaml
	cd obs && ./env ./browse chronoscope.db
	cmp bulk.bin fbulk.bin

