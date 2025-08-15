CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
BACKEND := tunnel_backend_server
TEST_BACKEND := test_tunnel_backend_server
FRONTEND := tunnel_frontend_server
TEST_FRONTEND := test_tunnel_frontend_server

all: $(BACKEND) $(TEST_BACKEND) $(FRONTEND) $(TEST_FRONTEND)

$(BACKEND): $(BACKEND).o
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_BACKEND): $(TEST_BACKEND).o $(BACKEND)
	$(CC) $(CFLAGS) -o $@ $(TEST_BACKEND).o -pthread

$(BACKEND).o: $(BACKEND).c
	$(CC) $(CFLAGS) -c $<

$(TEST_BACKEND).o: $(TEST_BACKEND).c
	$(CC) $(CFLAGS) -c $< -pthread

$(FRONTEND): $(FRONTEND).o
	$(CC) $(CFLAGS) -o $@ $^ -lcurl

$(FRONTEND).o: $(FRONTEND).c
	$(CC) $(CFLAGS) -c $<

$(TEST_FRONTEND): $(TEST_FRONTEND).o $(FRONTEND)
	$(CC) $(CFLAGS) -o $@ $(TEST_FRONTEND).o -pthread

$(TEST_FRONTEND).o: $(TEST_FRONTEND).c
	$(CC) $(CFLAGS) -c $< -pthread

clean:
	rm -f $(BACKEND) $(BACKEND).o $(TEST_BACKEND) $(TEST_BACKEND).o $(FRONTEND) $(FRONTEND).o $(TEST_FRONTEND) $(TEST_FRONTEND).o

test: all
	./$(TEST_BACKEND)
	./$(TEST_FRONTEND)

.PHONY: all clean test
