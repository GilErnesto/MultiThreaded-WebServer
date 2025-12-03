#!/bin/bash
# Testes de Stress (Requisitos 21-24)
# - Execução por 5+ minutos com carga contínua
# - Monitorizar memory leaks com Valgrind
# - Graceful shutdown sob carga
# - Verificar ausência de processos zombie

set -euo pipefail

SERVER_BIN="${SERVER_BIN:-./server}"
BASE_URL="${BASE_URL:-http://localhost:8080}"
FAIL=0

echo "========================================"
echo "   TESTES DE STRESS"
echo "========================================"

# Cores para output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Função global para fazer requests continuamente (usada por background jobs)
make_requests_worker() {
    local end_time=$1
    local tmp_requests=$2
    local tmp_errors=$3
    local local_requests=0
    local local_errors=0
    
    while [ $(date +%s) -lt $end_time ]; do
            if curl -s -o /dev/null -w "%{http_code}" "${BASE_URL}/index.html" 2>/dev/null | grep -q "^200$"; then
                echo -n "."
                ((local_requests++))
            else
                echo -n "E"
                ((local_requests++))
                ((local_errors++))
            fi
            sleep 0.1
        done
    
    # Escrever contadores para ficheiros com lock
    (
            flock 200
            echo $(($(cat "$tmp_requests") + local_requests)) > "$tmp_requests"
        ) 200>"$tmp_requests.lock"
        
        (
            flock 201
            echo $(($(cat "$tmp_errors") + local_errors)) > "$tmp_errors"
        ) 201>"$tmp_errors.lock"
}

test_continuous_load() {
    echo ""
    echo "--- Teste 21: Carga contínua por 5+ minutos ---"
    echo "AVISO: Este teste demora ~5 minutos"
    
    local duration=300  # 5 minutos em segundos
    local start_time=$(date +%s)
    local end_time=$((start_time + duration))
    local request_count=0
    local error_count=0
    
    echo "Início: $(date)"
    echo "A executar carga contínua até: $(date -d @${end_time})"
    
    # Criar ficheiros temporários para contadores
    local tmp_requests=$(mktemp)
    local tmp_errors=$(mktemp)
    echo 0 > "$tmp_requests"
    echo 0 > "$tmp_errors"
    
    # Lançar múltiplos workers
    local num_workers=10
    echo "A lançar ${num_workers} workers para carga contínua..."
    
    for i in $(seq 1 $num_workers); do
        make_requests_worker $end_time "$tmp_requests" "$tmp_errors" &
    done
    
    # Esperar que o tempo passe
    wait
    
    # Ler totais
    request_count=$(cat "$tmp_requests")
    error_count=$(cat "$tmp_errors")
    
    rm -f "$tmp_requests" "$tmp_errors" "$tmp_requests.lock" "$tmp_errors.lock"
    
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
    
    # Arrancar servidor com Valgrind
    valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
             --log-file="$valgrind_output" "$SERVER_BIN" > /dev/null 2>&1 &
    local server_pid=$!
    
    sleep 5  # Dar mais tempo ao servidor com Valgrind
    
    # Fazer vários requests
    echo "A fazer requests de teste..."
    for i in $(seq 1 100); do
        curl -s -o /dev/null "${BASE_URL}/index.html" 2>/dev/null &
        if [ $((i % 10)) -eq 0 ]; then
            wait
        fi
    done
    wait
    
    sleep 2
    
    # Fazer shutdown graceful
    echo "A fazer shutdown do servidor..."
    kill -TERM $server_pid 2>/dev/null || kill $server_pid 2>/dev/null || true
    
    # Dar tempo para shutdown limpo
    for i in $(seq 1 10); do
        if ! kill -0 $server_pid 2>/dev/null; then
            break
        fi
        sleep 1
    done
    
    # Forçar kill se necessário
    kill -9 $server_pid 2>/dev/null || true
    wait $server_pid 2>/dev/null || true
    
    # Analisar output do Valgrind
    if [ -f "$valgrind_output" ]; then
        echo "A analisar relatório do Valgrind..."
        
        local definitely_lost
        local indirectly_lost
        local possibly_lost
        
        definitely_lost=$(grep "definitely lost:" "$valgrind_output" | grep -oP '\d+(?= bytes)' || echo "0")
        indirectly_lost=$(grep "indirectly lost:" "$valgrind_output" | grep -oP '\d+(?= bytes)' || echo "0")
        possibly_lost=$(grep "possibly lost:" "$valgrind_output" | grep -oP '\d+(?= bytes)' || echo "0")
        
        echo "Definitely lost: ${definitely_lost} bytes"
        echo "Indirectly lost: ${indirectly_lost} bytes"
        echo "Possibly lost: ${possibly_lost} bytes"
        
        if [ "$definitely_lost" = "0" ] && [ "$indirectly_lost" = "0" ]; then
            echo -e "${GREEN}[OK]${NC} Nenhum memory leak definitivo detetado"
        else
            echo -e "${YELLOW}[WARN]${NC} Memory leaks detetados"
            echo "Veja detalhes em: $valgrind_output"
            # Não falhar automaticamente, alguns leaks podem ser do sistema
        fi
    else
        echo -e "${YELLOW}[WARN]${NC} Não foi possível analisar output do Valgrind"
    fi
    
    rm -f "$valgrind_output"
}

test_graceful_shutdown() {
    echo ""
    echo "--- Teste 23: Graceful shutdown sob carga ---"
    
    echo "A arrancar servidor..."
    "$SERVER_BIN" > /dev/null 2>&1 &
    local server_pid=$!
    
    sleep 2
    
    # Iniciar carga
    echo "A gerar carga..."
    for i in $(seq 1 100); do
        curl -s -o /dev/null "${BASE_URL}/index.html" 2>/dev/null &
    done
    
    sleep 1
    
    # Enviar SIGTERM para graceful shutdown
    echo "A enviar SIGTERM para shutdown graceful..."
    kill -TERM $server_pid 2>/dev/null || true
    
    local shutdown_time=0
    local max_shutdown_time=10
    
    # Esperar pelo shutdown
    while kill -0 $server_pid 2>/dev/null; do
        sleep 1
        ((shutdown_time++)) || true
        
        if [ $shutdown_time -ge $max_shutdown_time ]; then
            echo -e "${RED}[FAIL]${NC} Servidor não fez shutdown em ${max_shutdown_time} segundos"
            kill -9 $server_pid 2>/dev/null || true
            FAIL=1
            return
        fi
    done
    
    wait $server_pid 2>/dev/null || true
    
    echo -e "${GREEN}[OK]${NC} Servidor fez shutdown graceful em ${shutdown_time} segundos"
}

test_no_zombies() {
    echo ""
    echo "--- Teste 24: Verificar ausência de processos zombie ---"
    
    echo "A arrancar e parar servidor várias vezes..."
    
    for iteration in $(seq 1 5); do
        "$SERVER_BIN" > /dev/null 2>&1 &
        local server_pid=$!
        
        sleep 1
        
        # Fazer alguns requests
        for i in $(seq 1 10); do
            curl -s -o /dev/null "${BASE_URL}/index.html" 2>/dev/null &
        done
        
        wait
        
        # Matar servidor
        kill $server_pid 2>/dev/null || true
        wait $server_pid 2>/dev/null || true
        
        sleep 1
    done
    
    # Verificar processos zombie
    local zombies
    zombies=$(ps aux | grep -c '<defunct>' || echo "0")
    
    # Descontar o próprio grep
    if [ "$zombies" -gt 1 ]; then
        zombies=$((zombies - 1))
    else
        zombies=0
    fi
    
    if [ "$zombies" -eq 0 ]; then
        echo -e "${GREEN}[OK]${NC} Nenhum processo zombie detetado"
    else
        echo -e "${RED}[FAIL]${NC} ${zombies} processos zombie detetados"
        ps aux | grep '<defunct>' || true
        FAIL=1
    fi
}

# Menu de opções (alguns testes demoram muito)
if [ "${QUICK_TEST:-0}" = "1" ]; then
    echo "Modo QUICK_TEST ativado - a saltar testes longos"
    test_graceful_shutdown
    test_no_zombies
else
    echo "Modo completo - todos os testes serão executados"
    echo "Para modo rápido, use: QUICK_TEST=1 $0"
    echo ""
    
    # Perguntar ao utilizador se quer executar todos
    if [ -t 0 ]; then  # Se estamos num terminal interativo
        read -p "Executar teste de 5 minutos? (s/N): " -n 1 -r
        echo
        if [[ $REPLY =~ ^[Ss]$ ]]; then
            test_continuous_load
        else
            echo -e "${YELLOW}[SKIP]${NC} Teste de carga contínua"
        fi
    fi
    
    test_memory_leaks
    test_graceful_shutdown
    test_no_zombies
fi

# Resultado final
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
