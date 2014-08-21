mongocachestat: mongocachestat.o
	g++ -pthread -o mongocachestat mongocachestat.o /usr/lib64/libmongoclient.a /usr/lib64/libboost_filesystem.a /usr/lib64/libboost_thread.a /usr/lib64/libboost_system.a

mongocachestat.o: mongocachestat.cpp
	g++ -Wall -Wformat=2 -c mongocachestat.cpp

clean:
	-rm mongocachestat.o
	-rm mongocachestat
