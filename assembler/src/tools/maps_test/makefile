DIR=../../build/tools/maps_test

program: filter.o sequence.o
	g++ -O3 $(DIR)/filter.o $(DIR)/sequence.o -L/usr/local/lib -L/usr/local/apr/lib -lz -llog4cxx -lapr-1 -laprutil-1 -o $(DIR)/filter

filter.o: 
	mkdir -p $(DIR)
	rm -rf $(DIR)/*
	g++ -O3 -c -Wall -pedantic -Wno-long-long -Wno-deprecated -std=c++0x  -I./../../src/common -I./../../src/common/read -I./../../src/common/sequence -I./../../src -I./..  filter.cpp -o $(DIR)/filter.o

sequence.o:
	g++ -O3 -c -Wall -pedantic -Wno-long-long -Wno-deprecated -std=c++0x   -I./../../src/common -I./../../src/common/read -I./../../src/common/sequence -I./../../src ../../src/common/sequence/sequence.cpp -o $(DIR)/sequence.o

clean:
	rm -rf $(DIR)/*
