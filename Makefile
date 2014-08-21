mongocachestat: mongocachestat.o
	${CXX} -pthread -o mongocachestat mongocachestat.o -lmongoclient

mongocachestat.o: mongocachestat.cpp
	${CXX} -Wall -Wformat=2 -c mongocachestat.cpp

clean:
	-rm mongocachestat.o
	-rm mongocachestat
