#!/bin/bash

set -euo pipefail

SERVER_BIN="${SERVER_BIN:-./server}"
BASE_URL="${BASE_URL:-http://localhost:8080}"
LOG_FILE="${LOG_FILE:-server.log}"
FAIL=0

echo "========================================"
echo "   TESTES DE SINCRONIZAÇÃO"
echo "========================================"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

test_helgrind() {
    echo ""
    echo "--- Teste 17: Helgrind para detetar race conditions ---"
    
    if ! command -v valgrind >/dev/null 2>&1; then
        echo -e "${YELLOW}[SKIP]${NC} Valgrind não está instalado"
        return
    fi
    
    echo "A arrancar servidor com Helgrind (sleep time de 2 minutos)..."
    
    local helgrind_output
    helgrind_output=$(mktemp --suffix=.helgrind.log)
    echo "Ficheiro de output: $helgrind_output"
    
    # arrancar servidor com Helgrind
    valgrind --tool=helgrind --log-file="$helgrind_output" "$SERVER_BIN" > /dev/null 2>&1 &
    local server_pid=$!
    
    # Valgrind crie o ficheiro
    sleep 5
    
    if [ ! -f "$helgrind_output" ]; then
        echo -e "${YELLOW}[WARN]${NC} Ficheiro de log do Helgrind não foi criado"
        kill $server_pid 2>/dev/null || true
        wait $server_pid 2>/dev/null || true
        return
    fi
    
    echo "Aguardar servidor inicializar completamente..."
    sleep 10 
    
    echo "A fazer requests de teste..."
    for i in $(seq 1 50); do
        curl -s -o /dev/null "${BASE_URL}/index.html" 2>/dev/null &
        if [ $((i % 10)) -eq 0 ]; then
            sleep 5
        fi
    done
    wait
    
    echo "Aguardar análise do Helgrind..."
    sleep 5
    
    echo "A terminar servidor..."
    kill -TERM $server_pid 2>/dev/null || true
    
    # aguardar que o Valgrind finalize e escreva o relatório
    local timeout=30
    while kill -0 $server_pid 2>/dev/null && [ $timeout -gt 0 ]; do
        sleep 1
        timeout=$((timeout - 1))
    done
    
    # se ainda estiver a correr, kill
    if kill -0 $server_pid 2>/dev/null; then
        kill -KILL $server_pid 2>/dev/null || true
    fi
    
    wait $server_pid 2>/dev/null || true
    
    # Valgrind termine de escrever
    sleep 5
    
    # analisar output do Helgrind
    if [ -f "$helgrind_output" ] && [ -s "$helgrind_output" ]; then
        echo "Analisando relatório do Helgrind..."
        
        # Verificar ERROR SUMMARY - extrair número de forma mais robusta
        local error_count
        error_count=$(grep "ERROR SUMMARY:" "$helgrind_output" | sed -n 's/.*ERROR SUMMARY: \([0-9]\+\) error.*/\1/p' | head -1)
        if [ -z "$error_count" ] || [ "$error_count" = "" ]; then 
            error_count=0
        fi
        
        # Procurar por data races
        local data_races
        data_races=$(grep -c "Possible data race" "$helgrind_output" 2>/dev/null || echo "0")
        
        # Procurar por outros problemas
        local lock_order
        lock_order=$(grep -c "lock order" "$helgrind_output" 2>/dev/null || echo "0")
        
        local total_issues=$((data_races + lock_order))
        
        echo "ERROR SUMMARY reportou: $error_count erros"
        echo "Data races encontrados: $data_races"
        echo "Problemas de lock order: $lock_order"
        
        if [ "$total_issues" -eq 0 ] && [ "$error_count" -eq 0 ]; then
            echo -e "${GREEN}[OK]${NC} Helgrind não detetou race conditions ou problemas de sincronização"
        else
            echo -e "${RED}[FAIL]${NC} Helgrind detetou problemas:"
            [ "$data_races" -gt 0 ] && echo "  - ${data_races} possíveis data races"
            [ "$lock_order" -gt 0 ] && echo "  - ${lock_order} problemas de lock order"
            [ "$error_count" -gt 0 ] && echo "  - ${error_count} erros no total"
            echo "Veja detalhes em: $helgrind_output"
            FAIL=1
        fi
        
        echo "Relatório completo salvo em: $helgrind_output"
    else
        echo -e "${YELLOW}[WARN]${NC} Ficheiro de log do Helgrind vazio ou não encontrado: $helgrind_output"
    fi
}

test_thread_sanitizer() {
    echo ""
    echo "--- Teste 17b: Thread Sanitizer (alternativa ao Helgrind) ---"
    
    local tsan_binary="./server_tsan"
    if [ ! -f "$tsan_binary" ]; then
        echo -e "${YELLOW}[SKIP]${NC} Servidor não compilado com Thread Sanitizer"
        echo "Para habilitar: make tsan"
        return
    fi
    
    echo "A arrancar servidor com Thread Sanitizer..."
    
    local tsan_output=$(mktemp)
    "$tsan_binary" > /dev/null 2>"$tsan_output" &
    local server_pid=$!
    
    sleep 2
    
    echo "A fazer requests de teste..."
    for i in $(seq 1 50); do
        curl -s -o /dev/null "${BASE_URL}/index.html" 2>/dev/null &
    done
    wait
    
    sleep 1
    
    kill $server_pid 2>/dev/null || true
    wait $server_pid 2>/dev/null || true
    
    # analisar output do Thread Sanitizer
    if [ -f "$tsan_output" ]; then
        local data_races
        data_races=$(grep "WARNING: ThreadSanitizer: data race" "$tsan_output" 2>/dev/null | wc -l)
        
        if [ "$data_races" -eq 0 ]; then
            echo -e "${GREEN}[OK]${NC} Thread Sanitizer não detetou data races"
        else
            echo -e "${RED}[FAIL]${NC} Thread Sanitizer detetou ${data_races} data races"
            echo "Veja detalhes em: $tsan_output"
            FAIL=1
        fi
        
        echo "Relatório salvo em: $tsan_output"
    else
        echo -e "${YELLOW}[WARN]${NC} Não foi possível analisar output do Thread Sanitizer"
    fi
}

test_log_integrity() {
    echo ""
    echo "--- Teste 18: Integridade do ficheiro de log ---"
    
    if [ ! -f "$LOG_FILE" ]; then
        echo -e "${YELLOW}[SKIP]${NC} Ficheiro de log não encontrado: $LOG_FILE"
        return
    fi
    
    echo "A gerar logs com carga paralela..."
    for i in $(seq 1 100); do
        curl -s -o /dev/null "${BASE_URL}/index.html" 2>/dev/null &
    done
    wait
    
    sleep 1
    
    # Verificar se há linhas de log intercaladas (sem \n correto)
    local total_lines
    local complete_lines
    
    total_lines=$(wc -l < "$LOG_FILE" 2>/dev/null || echo "0")
    
    # Procurar por padrões de log intercalado (duas timestamps na mesma linha)
    local interleaved
    interleaved=$(grep -cP '\[\d{4}-\d{2}-\d{2}.*\[\d{4}-\d{2}-\d{2}' "$LOG_FILE" 2>/dev/null | tr -d '\n' || echo "0")
    if [ -z "$interleaved" ]; then interleaved=0; fi
    
    if [ "$interleaved" -eq 0 ]; then
        echo -e "${GREEN}[OK]${NC} Nenhuma linha de log intercalada detetada"
    else
        echo -e "${RED}[FAIL]${NC} ${interleaved} linhas de log intercaladas detetadas"
        FAIL=1
    fi
    
    echo "Total de linhas no log: ${total_lines}"
}

test_cache_consistency() {
    echo ""
    echo "--- Teste 19: Consistência da cache entre threads ---"
    
    echo "A fazer múltiplos requests paralelos ao mesmo ficheiro..."
    
    local tmp_dir
    tmp_dir=$(mktemp -d)
    local failed=0
    
    # requests paralelos ao mesmo ficheiro para testar a cache
    for batch in $(seq 1 10); do
        for i in $(seq 1 20); do
            curl -s "${BASE_URL}/index.html" -o "${tmp_dir}/response_${batch}_${i}.html" 2>/dev/null &
        done
        wait
    done
    
    # verificar se todas as respostas são idênticas
    local first_file="${tmp_dir}/response_1_1.html"
    
    if [ ! -f "$first_file" ]; then
        echo -e "${RED}[FAIL]${NC} Nenhuma resposta recebida"
        rm -rf "$tmp_dir"
        FAIL=1
        return
    fi
    
    for file in "${tmp_dir}"/*.html; do
        if ! diff -q "$first_file" "$file" > /dev/null 2>&1; then
            echo -e "${RED}[FAIL]${NC} Inconsistência detetada: $file difere do esperado"
            failed=1
            break
        fi
    done
    
    if [ "$failed" -eq 0 ]; then
        echo -e "${GREEN}[OK]${NC} Todas as respostas da cache são consistentes"
    else
        FAIL=1
    fi
    
    rm -rf "$tmp_dir"
}

test_statistics_counters() {
    echo ""
    echo "--- Teste 20: Contadores de estatísticas sem lost updates ---"
    
    local stats_url="${BASE_URL}/stats"
    
    # Verificar se endpoint /stats existe
    local stats_check
    stats_check=$(curl -s -o /dev/null -w "%{http_code}" "$stats_url" 2>/dev/null || echo "000")
    
    if [ "$stats_check" != "200" ]; then
        echo -e "${YELLOW}[SKIP]${NC} Endpoint /stats não disponível ou não implementado"
        return
    fi
    
    # estatísticas iniciais
    local stats_before
    stats_before=$(curl -s "$stats_url" 2>/dev/null || echo "")
    
    if [ -z "$stats_before" ]; then
        echo -e "${YELLOW}[SKIP]${NC} Não foi possível obter estatísticas iniciais"
        return
    fi
    
    # número exato de requests em paralelo
    local num_requests=200
    echo "A fazer ${num_requests} requests paralelos..."
    
    for i in $(seq 1 $num_requests); do
        curl -s -o /dev/null "${BASE_URL}/index.html" 2>/dev/null &
        if [ $((i % 20)) -eq 0 ]; then
            wait
        fi
    done
    wait
    
    sleep 5
    
    # estatísticas finais
    local stats_after
    stats_after=$(curl -s "$stats_url" 2>/dev/null || echo "")
    
    if [ -z "$stats_after" ]; then
        echo -e "${YELLOW}[WARN]${NC} Não foi possível obter estatísticas finais"
        return
    fi
    
    # extrair contadores
    local requests_before requests_after
    
    # tentar extrair de HTML 
    requests_before=$(echo "$stats_before" | grep -oP '(?<=Total Requests</td><td>)\d+' | head -1 || echo "")
    requests_after=$(echo "$stats_after" | grep -oP '(?<=Total Requests</td><td>)\d+' | head -1 || echo "")
    
    # se não encontrou no HTML, tentar outros formatos
    if [ -z "$requests_before" ]; then
        requests_before=$(echo "$stats_before" | grep -oP '(?<=requests:|Requests:|total_requests:)\s*\d+' | head -1 | tr -d ' ' || echo "0")
    fi
    if [ -z "$requests_after" ]; then
        requests_after=$(echo "$stats_after" | grep -oP '(?<=requests:|Requests:|total_requests:)\s*\d+' | head -1 | tr -d ' ' || echo "0")
    fi
    
    if [ -z "$requests_before" ]; then requests_before=0; fi
    if [ -z "$requests_after" ]; then requests_after=0; fi
    
    local diff=$((requests_after - requests_before))
    
    echo "Requests esperados: ${num_requests}"
    echo "Diferença nas estatísticas: ${diff}"
    
    # se os contadores não mudaram, o endpoint pode não estar implementado corretamente
    if [ "$diff" -eq 0 ]; then
        echo -e "${YELLOW}[WARN]${NC} Contadores não mudaram - /stats pode não estar a atualizar corretamente"
        return
    fi
    
    # Verificar se há lost updates significativos
    # Lost updates seriam evidenciados por diff muito menor que esperado
    local loss_threshold=$((num_requests * 80 / 100))
    
    if [ "$diff" -lt "$loss_threshold" ]; then
        echo -e "${RED}[FAIL]${NC} Possível lost update: diferença (${diff}) muito menor que esperado (${num_requests})"
        FAIL=1
        return
    fi
    
    # Tolerância de ±10% devido a possíveis requests de background
    local tolerance=$((num_requests * 10 / 100))
    local lower=$((num_requests - tolerance))
    local upper=$((num_requests + tolerance + 30))  # +30 para overhead
    
    if [ "$diff" -ge "$lower" ] && [ "$diff" -le "$upper" ]; then
        echo -e "${GREEN}[OK]${NC} Contadores de estatísticas parecem corretos (±10%)"
    else
        echo -e "${YELLOW}[WARN]${NC} Contadores fora do esperado mas sem lost updates críticos"
        echo "Isto pode ser normal se houverem outros requests em background"
    fi
}

test_helgrind
test_thread_sanitizer
test_log_integrity
test_cache_consistency
test_statistics_counters

echo ""
echo "========================================"
if [ "$FAIL" -eq 0 ]; then
    echo -e "${GREEN}TESTES DE SINCRONIZAÇÃO: TODOS PASSARAM${NC}"
    echo "========================================"
    exit 0
else
    echo -e "${RED}TESTES DE SINCRONIZAÇÃO: ALGUNS FALHARAM${NC}"
    echo "========================================"
    exit 1
fi
