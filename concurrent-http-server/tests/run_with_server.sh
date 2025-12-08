#!/bin/bash

set +m
SERVER_BIN="$1"
TEST_MODE="$2"

if [ ! -x "$SERVER_BIN" ]; then
    echo "Erro: servidor '$SERVER_BIN' não encontrado ou não executável"
    exit 1
fi

cleanup() {
    local server_pid=$1
    
    {
        if ps -p $server_pid > /dev/null 2>&1; then
            kill -TERM $server_pid 2>/dev/null || true
            sleep 1
        fi
    
        if ps -p $server_pid > /dev/null 2>&1; then
            kill -9 $server_pid 2>/dev/null || true
            sleep 0.5
        fi
        
        # kill em qualquer outro processo do servidor que ainda corra 
        pkill -9 -f "$SERVER_BIN" 2>/dev/null || true
    } 2>/dev/null
}

setsid "$SERVER_BIN" > /dev/null 2>&1 &
SERVER_PID=$!
disown

sleep 5

if ! ps -p $SERVER_PID > /dev/null 2>&1; then
    echo "Erro: servidor falhou ao arrancar"
    exit 1
fi

# testes
STATUS=0
if [ "$TEST_MODE" = "full" ]; then
    tests/test_all.sh full || STATUS=$?
else
    tests/test_all.sh || STATUS=$?
fi

cleanup $SERVER_PID

exit $STATUS
