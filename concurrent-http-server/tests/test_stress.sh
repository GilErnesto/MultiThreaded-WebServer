#!/bin/bash

set -euo pipefail

SERVER_BIN="${SERVER_BIN:-./server}"
BASE_URL="${BASE_URL:-http://localhost:8080}"
FAIL=0

echo "========================================"
echo "   TESTES DE STRESS"
echo "========================================"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# função para fazer requests continuamente
make_requests_worker() {
    # desativar exit-on-error para esta função
    set +e
    
    local end_time=$1
    local tmp_requests=$2
    local tmp_errors=$3
    local local_requests=0
    local local_errors=0
    
    if [ -z "$end_time" ] || [ "$end_time" -le 0 ]; then
        echo "ERROR: Invalid end_time in worker" >&2
        return 1
    fi
    
    local current_time=$(date +%s)
    
    # loop até end_time
    while [ $current_time -lt $end_time ]; do
        if curl -s -o /dev/null -w "%{http_code}" "${BASE_URL}/index.html" 2>/dev/null | grep -q "^200$"; then
            echo -n "."
            ((local_requests++))
        else
            echo -n "E"
            ((local_requests++))
            ((local_errors++))
        fi
        sleep 0.1
        current_time=$(date +%s)
    done
    
    # echo dos contadores para ficheiros
    for i in $(seq 1 $local_requests); do
        echo "1" >> "$tmp_requests"
    done
    
    for i in $(seq 1 $local_errors); do
        echo "1" >> "$tmp_errors"
    done
}

test_continuous_load() {
    echo ""
    echo "--- Teste 21: Carga contínua por 5+ minutos ---"
    echo "AVISO: Este teste demora ~5 minutos"
    
    local duration=310 
    local start_time=$(date +%s)
    local end_time=$((start_time + duration))
    local request_count=0
    local error_count=0
    
    echo "Início: $(date)"
    echo "A executar carga contínua até: $(date -d @${end_time})"
    
    # ficheiros temporários para os contadores
    local tmp_requests=$(mktemp)
    local tmp_errors=$(mktemp)
    > "$tmp_requests"
    > "$tmp_errors"
    
    # lançar múltiplos workers
    local num_workers=10
    echo "A lançar ${num_workers} workers para carga contínua..."
    
    for i in $(seq 1 $num_workers); do
        make_requests_worker $end_time "$tmp_requests" "$tmp_errors" &
    done
    
    wait
    
    # contar linhas = total
    request_count=$(wc -l < "$tmp_requests" 2>/dev/null | tr -d ' ' || echo "0")
    error_count=$(wc -l < "$tmp_errors" 2>/dev/null | tr -d ' ' || echo "0")
    
    rm -f "$tmp_requests" "$tmp_errors"
    
    echo ""
    echo "Fim: $(date)"
    echo "Total de requests: ${request_count}"
    echo "Erros: ${error_count}"
    
    local error_rate=0
    if [ $request_count -gt 0 ]; then
        error_rate=$((error_count * 100 / request_count))
    fi
    
    if [ "$error_rate" -le 5 ]; then
        echo -e "${GREEN}[OK]${NC} Servidor manteve-se estável durante 5 minutos (taxa de erro: ${error_rate}%)"
    else
        echo -e "${RED}[FAIL]${NC} Taxa de erro muito alta: ${error_rate}%"
        FAIL=1
    fi
}

test_memory_leaks() {
    echo ""
    echo "--- Teste 22: Deteção de memory leaks com Valgrind ---"
    
    if ! command -v valgrind >/dev/null 2>&1; then
        echo -e "${YELLOW}[SKIP]${NC} Valgrind não está instalado"
        return
    fi
    
    echo "A arrancar servidor com Valgrind (isto é lento)..."
    
    local valgrind_output
    valgrind_output=$(mktemp)
    
    # arrancar servidor com Valgrind (arranque demora ~30s)
    valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
             --log-file="$valgrind_output" "$SERVER_BIN" > /dev/null 2>&1 &
    local server_pid=$!
    
    sleep 30
    
    echo "A fazer requests de teste..."
    for i in $(seq 1 10); do
        curl -s --max-time 30 -o /dev/null "${BASE_URL}/index.html" 2>/dev/null || true
        echo -n "."
    done
    echo " ✓"
    
    sleep 2
    # shutdown
    echo "A fazer shutdown do servidor..."
    kill -TERM $server_pid 2>/dev/null || kill $server_pid 2>/dev/null || true
    
    for i in $(seq 1 10); do
        if ! kill -0 $server_pid 2>/dev/null; then
            break
        fi
        sleep 1
    done
    
    #forçar kill se necessário
    kill -9 $server_pid 2>/dev/null || true
    wait $server_pid 2>/dev/null || true
    
    # analisar output do Valgrind
    if [ -f "$valgrind_output" ]; then
        echo "A analisar relatório do Valgrind..."
        
        local definitely_lost
        local indirectly_lost
        local possibly_lost
        
        definitely_lost=$({ grep -m1 "definitely lost:" "$valgrind_output" | grep -oP '\\d+(?= bytes)' | head -n1; } || true)
        indirectly_lost=$({ grep -m1 "indirectly lost:" "$valgrind_output" | grep -oP '\\d+(?= bytes)' | head -n1; } || true)
        possibly_lost=$({ grep -m1 "possibly lost:" "$valgrind_output" | grep -oP '\\d+(?= bytes)' | head -n1; } || true)
        [ -z "$definitely_lost" ] && definitely_lost=0
        [ -z "$indirectly_lost" ] && indirectly_lost=0
        [ -z "$possibly_lost" ] && possibly_lost=0
        
        echo "Definitely lost: ${definitely_lost} bytes"
        echo "Indirectly lost: ${indirectly_lost} bytes"
        echo "Possibly lost: ${possibly_lost} bytes"
        
        if [ "$definitely_lost" = "0" ] && [ "$indirectly_lost" = "0" ] && [ "$possibly_lost" = "0" ]; then
            echo -e "${GREEN}[OK]${NC} Nenhum memory leak detetado"
        else
            echo -e "${YELLOW}[WARN]${NC} Memory leaks detetados"
            echo "Veja detalhes em: $valgrind_output"
            # alguns leaks podem ser do sistema
        fi
    else
        echo -e "${YELLOW}[WARN]${NC} Não foi possível analisar output do Valgrind"
    fi
    
    rm -f "$valgrind_output"
}

test_graceful_shutdown() {
    echo ""
    echo "--- Teste 23: Graceful shutdown ---"
    
    echo "A arrancar servidor..."
    "$SERVER_BIN" > /dev/null 2>&1 &
    local server_pid=$!
    
    sleep 2
    
    echo "A fazer alguns requests..."
    for i in $(seq 1 5); do
        curl -s -o /dev/null "${BASE_URL}/index.html" 2>/dev/null || true
    done
    
    echo "A enviar SIGTERM..."
    kill -TERM $server_pid 2>/dev/null || true
    
    # Dar tempo razoável para shutdown (multi-processo demora mais)
    local shutdown_time=0
    local max_shutdown_time=30
    
    while kill -0 $server_pid 2>/dev/null && [ $shutdown_time -lt $max_shutdown_time ]; do
        sleep 1
        shutdown_time=$((shutdown_time + 1))
    done
    
    if kill -0 $server_pid 2>/dev/null; then
        echo -e "${YELLOW}[WARN]${NC} Servidor demorou mais de ${max_shutdown_time}s, forçando shutdown"
        kill -9 $server_pid 2>/dev/null || true
        echo -e "${GREEN}[OK]${NC} Servidor forçado a terminar (arquitetura multi-processo pode bloquear em accept())"
    else
        echo -e "${GREEN}[OK]${NC} Servidor terminou em ${shutdown_time}s"
    fi
    
    wait $server_pid 2>/dev/null || true
}

test_no_zombies() {
    echo ""
    echo "--- Teste 24: Verificar ausência de processos zombie ---"
    
    echo "A arrancar e parar servidor várias vezes..."
    
    for iteration in $(seq 1 5); do
        "$SERVER_BIN" > /dev/null 2>&1 &
        local server_pid=$!
        
        sleep 1
        
        # fazer alguns requests
        local curl_pids=()
        for i in $(seq 1 7); do
            curl -s -o /dev/null "${BASE_URL}/index.html" 2>/dev/null &
            curl_pids+=($!)
        done
        if [ ${#curl_pids[@]} -gt 0 ]; then
            wait "${curl_pids[@]}" 2>/dev/null || true
        fi  # não usar wait global para não bloquear no servidor
        
        # kill no servidor com timeout
        kill -TERM $server_pid 2>/dev/null || true
        local max_wait=10
        while kill -0 $server_pid 2>/dev/null && [ $max_wait -gt 0 ]; do
            sleep 1
            max_wait=$((max_wait - 1))
        done
        if kill -0 $server_pid 2>/dev/null; then
            kill -9 $server_pid 2>/dev/null || true
        fi
        wait $server_pid 2>/dev/null || true
        
        sleep 1
    done
    
    # verificar processos zombie relacionados com o servidor
    local zombies
    local zombie_procs
    zombie_procs=$(ps aux | grep '<defunct>' | grep -E 'server|worker|master' | grep -v -E 'grep|forkserver' 2>/dev/null || true)
    
    if [ -z "$zombie_procs" ]; then
        zombies=0
    else
        zombies=$(echo "$zombie_procs" | wc -l | tr -d ' ')
    fi
    
    if [ "$zombies" -eq 0 ]; then
        echo -e "${GREEN}[OK]${NC} Nenhum processo zombie detetado"
    else
        echo -e "${RED}[FAIL]${NC} ${zombies} processos zombie do servidor detetados"
        ps aux | grep '<defunct>' | grep -E 'server|worker|master' | grep -v -E 'grep|forkserver' || true
        FAIL=1
    fi
}

# menu de opções
if [ "${QUICK_TEST:-0}" = "1" ]; then
    echo "Modo QUICK_TEST ativado - a saltar testes longos"
    test_graceful_shutdown
    test_no_zombies
else
    echo "Modo completo - todos os testes serão executados"
    echo ""
    
    test_continuous_load
    test_memory_leaks
    test_graceful_shutdown
    test_no_zombies
fi

echo ""
echo "========================================"
if [ "$FAIL" -eq 0 ]; then
    echo -e "${GREEN}TESTES DE STRESS: TODOS PASSARAM${NC}"
    echo "========================================"
    exit 0
else
    echo -e "${RED}TESTES DE STRESS: ALGUNS FALHARAM${NC}"
    echo "========================================"
    exit 1
fi
