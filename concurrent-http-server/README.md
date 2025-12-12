# Multi-Threaded Web Server with IPC and Semaphores

## Authors
Rodrigo Machado Pereira - NMec: 125696  
Gil Ernesto Leite Guedes - NMec: 125031

## Description
A concurrent HTTP/1.1 server implemented in C.  
Uses a master–worker architecture with multiple processes, thread pools, shared memory, and POSIX semaphores.  
Handles static files with caching, logging, and synchronized statistics.

## Compilation
make            --- build  
make clean      --- clean  
make run        --- build + run server  
make testSimple --- build + run server + simple tests  
make testFull   --- build + run server + simple tests + complex/long tests

## Quick Start & Usage
Before running, set the forbidden page to zero permissions (important to exercise 403 responses):
```
chmod 000 www/privado.html
```

To start the server, run either "make && ./server" or "make run".

While running you can access `http://localhost:8080/index.html` (port 8080 is the default in `server.conf`) and click **Dashboard** to see the stats. The same stats are also printed periodically in the terminal where the server is running.

To stop, press CTRL+C or run "pkill -f server".

## Testing
There are two test suites: "testSimple" covers functionality and concurrency; "testFull" runs those plus stress, load, and synchronization tests (can take a long time).