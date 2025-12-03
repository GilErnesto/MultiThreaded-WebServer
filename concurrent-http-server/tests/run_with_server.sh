#!/bin/bash
# Wrapper para rodar testes com servidor, lidando com cleanup
# Uso: run_with_server.sh <server_binary> <test_mode>
# Exemplo: run_with_server.sh ./server normal

set +m  # Desabilitar job control messages

SERVER_BIN="$1"
TEST_MODE="$2"

if [ ! -x "$SERVER_BIN" ]; then
    echo "Erro: servidor '$SERVER_BIN' não encontrado ou não executável"
    exit 1
fi

# Função de cleanup
cleanup() {
    local server_pid=$1
    
    # Desabilitar output de job control temporariamente
    {
        # Tentar SIGTERM primeiro (graceful)
        if ps -p $server_pid > /dev/null 2>&1; then
            kill -TERM $server_pid 2>/dev/null || true
            sleep 1
        fi
        
        # Se ainda estiver rodando, forçar com SIGKILL
        if ps -p $server_pid > /dev/null 2>&1; then
            kill -9 $server_pid 2>/dev/null || true
            sleep 0.5
        fi
        
        # Limpar qualquer outro processo servidor remanescente
        pkill -9 -f "$SERVER_BIN" 2>/dev/null || true
    } 2>/dev/null
}

# Arrancar servidor em background (usando setsid para nova sessão)
setsid "$SERVER_BIN" > /dev/null 2>&1 &
SERVER_PID=$!
disown  # Desassociar do job control

# Dar tempo para arrancar
sleep 2

# Verificar se arrancou
if ! ps -p $SERVER_PID > /dev/null 2>&1; then
    echo "Erro: servidor falhou ao arrancar"
    exit 1
fi

# Executar testes
STATUS=0
if [ "$TEST_MODE" = "full" ]; then
    tests/test_all.sh full || STATUS=$?
else
    tests/test_all.sh || STATUS=$?
fi

# Cleanup
cleanup $SERVER_PID

# Retornar status dos testes
exit $STATUS
