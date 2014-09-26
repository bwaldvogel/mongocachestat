LIBS=-lmongoclient -lboost_thread -lboost_system -lboost_regex -lboost_filesystem -lssl -lcrypto

mongocachestat: mongocachestat.cpp
	${CXX} -Wall -Wformat=2 mongocachestat.cpp -pthread ${LIBS} -o mongocachestat

clean:
	-rm mongocachestat
