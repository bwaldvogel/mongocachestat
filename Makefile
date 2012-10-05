mongocachestat: mongocachestat.o
	g++ -pthread -o mongocachestat mongocachestat.o /usr/lib/libmongoclient.a /usr/lib/libboost_filesystem.a /usr/lib/libboost_thread.a /usr/lib/libboost_system.a

mongocachestat.o: mongocachestat.cpp
	g++ -Wall -Werror -Wformat=2 -c mongocachestat.cpp

clean:
	-rm mongocachestat.o
	-rm mongocachestat
