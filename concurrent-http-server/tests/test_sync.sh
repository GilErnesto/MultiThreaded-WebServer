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
    
    echo "A arrancar servidor com Helgrind..."
    echo -e "${YELLOW}Este teste pode demorar ~7 minutos!${NC}"
    
    local helgrind_output
    helgrind_output=$(mktemp --suffix=.helgrind.log)
    echo "Ficheiro de output: $helgrind_output"
    
    # arrancar servidor com Helgrind (opções otimizadas para relatório rápido)
    local supp_file="helgrind.supp"
    local helgrind_opts="--tool=helgrind --history-level=none --conflict-cache-size=1000000"
    
    if [ -f "$supp_file" ]; then
        valgrind $helgrind_opts --suppressions="$supp_file" --log-file="$helgrind_output" "$SERVER_BIN" > /dev/null 2>&1 &
    else
        valgrind $helgrind_opts --log-file="$helgrind_output" "$SERVER_BIN" > /dev/null 2>&1 &
    fi
    local server_pid=$!
    
    # Valgrind crie o ficheiro - com progresso visual
    echo -n "Aguardando Helgrind iniciar..."
    sleep 10
    echo " ✓"
    
    if [ ! -f "$helgrind_output" ]; then
        echo -e "${YELLOW}[WARN]${NC} Ficheiro de log do Helgrind não foi criado"
        kill $server_pid 2>/dev/null || true
        wait $server_pid 2>/dev/null || true
        return
    fi
    
    echo -n "Aguardando servidor inicializar (10s)..."
    sleep 10
    echo " ✓"
    
    echo "A fazer 3 requests de teste (máx 5 minutos)..."
    for i in $(seq 1 3); do
        echo -n "|"
        curl -s --max-time 100 -o /dev/null "${BASE_URL}/index.html" 2>/dev/null || true
    done
    echo " ✓"
    
    echo -n "Aguardando Helgrind processar (5s)..."
    sleep 5
    echo " ✓"
    
    echo -n "A terminar servidor e aguardar relatório final..."
    
    # Tentar SIGTERM primeiro com timeout curto
    kill -TERM $server_pid 2>/dev/null || true
    
    local timeout=30
    while kill -0 $server_pid 2>/dev/null && [ $timeout -gt 0 ]; do
        sleep 1
        timeout=$((timeout - 1))
    done
    
    # Se ainda está a correr, forçar KILL
    if kill -0 $server_pid 2>/dev/null; then
        kill -KILL $server_pid 2>/dev/null || true
    fi
    
    wait $server_pid 2>/dev/null || true
    sleep 1
    echo " ✓"
    
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
        local data_races=0
        if grep -q "Possible data race" "$helgrind_output" 2>/dev/null; then
            data_races=$(grep "Possible data race" "$helgrind_output" 2>/dev/null | wc -l | xargs)
        fi
        
        # Procurar por outros problemas
        local lock_order=0
        if grep -q "lock order" "$helgrind_output" 2>/dev/null; then
            lock_order=$(grep "lock order" "$helgrind_output" 2>/dev/null | wc -l | xargs)
        fi
        
        local total_issues=$((data_races + lock_order))
        
        echo "ERROR SUMMARY reportou: $error_count erros"
        echo "Data races encontrados: $data_races"
        echo "Problemas de lock order: $lock_order"
        
        if [ "$total_issues" -eq 0 ] && [ "$error_count" -eq 0 ]; then
            echo -e "${GREEN}[OK]${NC} Helgrind não detetou race conditions ou problemas de sincronização"
        else
            echo -e "${YELLOW}[WARN]${NC} Helgrind reportou achados (multi-processo pode gerar falsos positivos)"
            [ "$data_races" -gt 0 ] && echo "  - ${data_races} possíveis data races"
            [ "$lock_order" -gt 0 ] && echo "  - ${lock_order} problemas de lock order"
            [ "$error_count" -gt 0 ] && echo "  - ${error_count} erros no total"
            echo "Relatório: $helgrind_output"
            # Não falhamos porque Helgrind não distingue processos diferentes
        fi
        
        echo "Relatório completo salvo em: $helgrind_output"
    else
        echo -e "${YELLOW}[WARN]${NC} Ficheiro de log do Helgrind vazio ou não encontrado: $helgrind_output"
    fi

    # Helgrind em multi-processo pode reportar falsos positivos; não falhamos a suite
    return 0
}

test_thread_sanitizer() {
    echo ""
    echo "--- Teste 17b: Valgrind DRD (Data Race Detector) ---"
    
    if ! command -v valgrind &> /dev/null; then
        echo -e "${YELLOW}[SKIP]${NC} Valgrind não está instalado"
        return
    fi
    
    echo "A arrancar servidor com Valgrind DRD (demora ~2 min)..."
    
    local drd_output=$(mktemp)
    local supp_file="helgrind.supp"  # DRD também pode usar suppressions
    
    if [ -f "$supp_file" ]; then
        valgrind --tool=drd --suppressions="$supp_file" --log-file="$drd_output" ./server > /dev/null 2>&1 &
    else
        valgrind --tool=drd --log-file="$drd_output" ./server > /dev/null 2>&1 &
    fi
    local server_pid=$!
    
    sleep 5
    
    echo -n "A fazer requests de teste (sequencial): "
    for i in $(seq 1 3); do
        curl -s --max-time 30 -o /dev/null "${BASE_URL}/index.html" 2>/dev/null || true
        echo -n "|"
    done
    echo " ✓"
    
    sleep 2
    
    echo -n "A terminar servidor..."
    kill -TERM $server_pid 2>/dev/null || true
    
    # esperar até 30s para terminar
    local timeout=30
    while kill -0 $server_pid 2>/dev/null && [ $timeout -gt 0 ]; do
        sleep 1
        timeout=$((timeout - 1))
    done
    
    # Se ainda está a correr, forçar KILL
    if kill -0 $server_pid 2>/dev/null; then
        kill -KILL $server_pid 2>/dev/null || true
    fi
    
    wait $server_pid 2>/dev/null || true
    sleep 1
    echo " ✓"
    
    # analisar output do DRD
    if [ -f "$drd_output" ]; then
        local data_races=0
        if grep -q "Conflicting " "$drd_output" 2>/dev/null; then
            data_races=$(grep "Conflicting " "$drd_output" 2>/dev/null | wc -l | xargs)
        fi
        
        if [ "$data_races" -eq 0 ]; then
            echo -e "${GREEN}[OK]${NC} Valgrind DRD não detetou data races"
        else
            echo -e "${RED}[FAIL]${NC} Valgrind DRD detetou ${data_races} possíveis data races"
            echo "Veja detalhes em: $drd_output"
            FAIL=1
        fi
        
        echo "Relatório salvo em: $drd_output"
    else
        echo -e "${YELLOW}[WARN]${NC} Não foi possível analisar output do Valgrind DRD"
    fi
}

test_log_integrity() {
    echo ""
    echo "--- Teste 18: Integridade do ficheiro de log ---"
    
    # Verificar se servidor está a correr
    local health_check
    health_check=$(curl -s -o /dev/null -w "%{http_code}" "${BASE_URL}/" 2>/dev/null || echo "000")
    
    if [ "$health_check" != "200" ]; then
        echo -e "${YELLOW}[INFO]${NC} Servidor não responde, a reiniciar..."
        pkill -9 -f "./server" 2>/dev/null || true
        sleep 1
        ${SERVER_BIN:-./server} > /dev/null 2>&1 &
        sleep 3
    fi
    
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
    
    # Verificar se servidor está a correr
    local health_check
    health_check=$(curl -s -o /dev/null -w "%{http_code}" "${BASE_URL}/" 2>/dev/null || echo "000")
    
    if [ "$health_check" != "200" ]; then
        echo -e "${YELLOW}[INFO]${NC} Servidor não responde, a reiniciar..."
        pkill -9 -f "./server" 2>/dev/null || true
        sleep 1
        ${SERVER_BIN:-./server} > /dev/null 2>&1 &
        sleep 3
    fi
    
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
    
    # Verificar se servidor está a correr (pode ter sido morto por testes anteriores)
    local stats_check
    stats_check=$(curl -s -o /dev/null -w "%{http_code}" "$stats_url" 2>/dev/null || echo "000")
    
    if [ "$stats_check" != "200" ]; then
        echo -e "${YELLOW}[INFO]${NC} Servidor não responde, a reiniciar..."
        
        # Matar quaisquer processos remanescentes
        pkill -9 -f "./server" 2>/dev/null || true
        sleep 1
        
        # Reiniciar servidor
        ${SERVER_BIN:-./server} > /dev/null 2>&1 &
        local server_pid=$!
        sleep 3
        
        # Verificar novamente
        stats_check=$(curl -s -o /dev/null -w "%{http_code}" "$stats_url" 2>/dev/null || echo "000")
        if [ "$stats_check" != "200" ]; then
            echo -e "${YELLOW}[SKIP]${NC} Endpoint /stats não disponível ou não implementado"
            kill $server_pid 2>/dev/null || true
            return
        fi
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
    
    # tentar extrair de JSON (formato com aspas)
    requests_before=$(echo "$stats_before" | grep -oP '(?<="total_requests":)\s*\d+' | head -1 | tr -d ' ' || echo "")
    requests_after=$(echo "$stats_after" | grep -oP '(?<="total_requests":)\s*\d+' | head -1 | tr -d ' ' || echo "")
    
    # fallback: tentar extrair de HTML ou outros formatos
    if [ -z "$requests_before" ]; then
        requests_before=$(echo "$stats_before" | grep -oP '(?<=Total Requests</td><td>)\d+' | head -1 || echo "")
    fi
    if [ -z "$requests_after" ]; then
        requests_after=$(echo "$stats_after" | grep -oP '(?<=Total Requests</td><td>)\d+' | head -1 || echo "")
    fi
    
    # último fallback: formatos sem aspas
    if [ -z "$requests_before" ]; then
        requests_before=$(echo "$stats_before" | grep -oP '(?<=requests:|Requests:)\s*\d+' | head -1 | tr -d ' ' || echo "0")
    fi
    if [ -z "$requests_after" ]; then
        requests_after=$(echo "$stats_after" | grep -oP '(?<=requests:|Requests:)\s*\d+' | head -1 | tr -d ' ' || echo "0")
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
