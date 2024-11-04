.PHONY: all
all:
	gcc -Wall -O0 -g -o party main.c sm.c -luv
