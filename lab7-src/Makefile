CXX = g++ -fPIC
NETLIBS= -lnsl -lpthread

all: daytime-server daytime-client myhttpd

daytime-client : daytime-client.o
	$(CXX) -o $@ $@.o $(NETLIBS)

daytime-server : daytime-server.o
	$(CXX) -o $@ $@.o $(NETLIBS)

myhttpd : myhttpd.o
	$(CXX) -o $@ $@.o $(NETLIBS)

%.o: %.cc
	@echo 'Building $@ from $<'
	$(CXX) -o $@ -c -I. $<

clean:
	rm -f *.o daytime-server daytime-client myhttpd

