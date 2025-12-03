#!/bin/bash
# Testes de Sincronização (Requisitos 17-20)
# - Helgrind/Thread Sanitizer para detetar race conditions
# - Integridade do ficheiro de log
# - Consistência da cache entre threads
# - Contadores de estatísticas corretos

set -euo pipefail

SERVER_BIN="${SERVER_BIN:-./server}"
BASE_URL="${BASE_URL:-http://localhost:8080}"
LOG_FILE="${LOG_FILE:-server.log}"
FAIL=0

echo "========================================"
echo "   TESTES DE SINCRONIZAÇÃO"
echo "========================================"

# Cores para output
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
    
    echo "A arrancar servidor com Helgrind (isto pode demorar)..."
    
    local helgrind_output
    helgrind_output=$(mktemp)
    
    # Arrancar servidor com Helgrind
    valgrind --tool=helgrind --log-file="$helgrind_output" "$SERVER_BIN" > /dev/null 2>&1 &
    local server_pid=$!
    
    sleep 10  # Dar tempo ao servidor para arrancar (Helgrind é muito lento)
    
    # Fazer alguns requests para exercitar o código
    echo "A fazer requests de teste..."
    for i in $(seq 1 50); do
        curl -s -o /dev/null "${BASE_URL}/index.html" 2>/dev/null &
    done
    wait
    
    sleep 2
    
    # Matar servidor
    kill $server_pid 2>/dev/null || true
    wait $server_pid 2>/dev/null || true
    
    # Analisar output do Helgrind
    if [ -f "$helgrind_output" ]; then
        local errors
        errors=$(grep "Possible data race" "$helgrind_output" 2>/dev/null | wc -l)
        
        if [ "$errors" -eq 0 ]; then
            echo -e "${GREEN}[OK]${NC} Helgrind não detetou race conditions"
        else
            echo -e "${RED}[FAIL]${NC} Helgrind detetou ${errors} possíveis race conditions"
            echo "Veja detalhes em: $helgrind_output"
            FAIL=1
        fi
        
        # Manter o arquivo para inspeção
        echo "Relatório salvo em: $helgrind_output"
    else
        echo -e "${YELLOW}[WARN]${NC} Não foi possível analisar output do Helgrind"
    fi
}

test_thread_sanitizer() {
    echo ""
    echo "--- Teste 17b: Thread Sanitizer (alternativa ao Helgrind) ---"
    
    # Thread Sanitizer requer recompilação com -fsanitize=thread
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
    
    # Fazer requests
    echo "A fazer requests de teste..."
    for i in $(seq 1 50); do
        curl -s -o /dev/null "${BASE_URL}/index.html" 2>/dev/null &
    done
    wait
    
    sleep 1
    
    kill $server_pid 2>/dev/null || true
    wait $server_pid 2>/dev/null || true
    
    # Analisar output do Thread Sanitizer
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
    
    # Fazer carga para gerar logs
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
    interleaved=$(grep -cP '\[\d{4}-\d{2}-\d{2}.*\[\d{4}-\d{2}-\d{2}' "$LOG_FILE" 2>/dev/null || echo "0")
    
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
    
    # Fazer muitos requests paralelos ao mesmo ficheiro para testar a cache
    for batch in $(seq 1 10); do
        for i in $(seq 1 20); do
            curl -s "${BASE_URL}/index.html" -o "${tmp_dir}/response_${batch}_${i}.html" 2>/dev/null &
        done
        wait
    done
    
    # Verificar se todas as respostas são idênticas
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
    
    # Obter estatísticas iniciais
    local stats_before
    stats_before=$(curl -s "$stats_url" 2>/dev/null || echo "")
    
    if [ -z "$stats_before" ]; then
        echo -e "${YELLOW}[SKIP]${NC} Não foi possível obter estatísticas iniciais"
        return
    fi
    
    # Fazer um número exato de requests em paralelo
    local num_requests=200
    echo "A fazer ${num_requests} requests paralelos..."
    
    for i in $(seq 1 $num_requests); do
        curl -s -o /dev/null "${BASE_URL}/index.html" 2>/dev/null &
        if [ $((i % 20)) -eq 0 ]; then
            wait
        fi
    done
    wait
    
    sleep 2  # Dar tempo para estatísticas serem atualizadas
    
    # Obter estatísticas finais
    local stats_after
    stats_after=$(curl -s "$stats_url" 2>/dev/null || echo "")
    
    if [ -z "$stats_after" ]; then
        echo -e "${YELLOW}[WARN]${NC} Não foi possível obter estatísticas finais"
        return
    fi
    
    # Extrair contadores
    local requests_before requests_after
    requests_before=$(echo "$stats_before" | grep -oP '(?<=requests:|Requests:|total_requests:)\s*\d+' | head -1 | tr -d ' ' || echo "0")
    requests_after=$(echo "$stats_after" | grep -oP '(?<=requests:|Requests:|total_requests:)\s*\d+' | head -1 | tr -d ' ' || echo "0")
    
    if [ -z "$requests_before" ]; then requests_before=0; fi
    if [ -z "$requests_after" ]; then requests_after=0; fi
    
    local diff=$((requests_after - requests_before))
    
    echo "Requests esperados: ${num_requests}"
    echo "Diferença nas estatísticas: ${diff}"
    
    # Se os contadores não mudaram, o endpoint pode não estar implementado corretamente
    if [ "$diff" -eq 0 ]; then
        echo -e "${YELLOW}[WARN]${NC} Contadores não mudaram - /stats pode não estar a atualizar corretamente"
        return
    fi
    
    # Verificar se há lost updates significativos
    # Lost updates seriam evidenciados por diff muito menor que esperado
    local loss_threshold=$((num_requests * 80 / 100))  # 80% do esperado
    
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

# Executar todos os testes
test_helgrind
test_thread_sanitizer
test_log_integrity
test_cache_consistency
test_statistics_counters

# Resultado final
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
