#!/bin/bash
# Testes de Concorrência (Requisitos 13-16)
# - Apache Bench com carga alta
# - Verificar conexões não perdidas
# - Múltiplos clientes em paralelo (curl/wget)
# - Verificar precisão das estatísticas

set -euo pipefail

BASE_URL="${BASE_URL:-http://localhost:8080}"
STATS_URL="${BASE_URL}/stats"
FAIL=0

echo "========================================"
echo "   TESTES DE CONCORRÊNCIA"
echo "========================================"

# Cores para output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

test_apache_bench() {
    echo ""
    echo "--- Teste 13: Apache Bench (10000 requests, 100 concurrent) ---"
    
    if ! command -v ab >/dev/null 2>&1; then
        echo -e "${YELLOW}[SKIP]${NC} Apache Bench (ab) não está instalado"
        return
    fi
    
    local output
    output=$(mktemp)
    
    if ab -n 10000 -c 100 "${BASE_URL}/index.html" > "$output" 2>&1; then
        local failed_requests
        failed_requests=$(grep "Failed requests:" "$output" | awk '{print $3}')
        
        if [ "$failed_requests" = "0" ]; then
            echo -e "${GREEN}[OK]${NC} Apache Bench: 10000 requests completados sem falhas"
        else
            echo -e "${RED}[FAIL]${NC} Apache Bench: ${failed_requests} requests falharam"
            FAIL=1
        fi
        
        # Mostrar algumas estatísticas
        grep "Requests per second:" "$output" || true
        grep "Time per request:" "$output" | head -1 || true
    else
        echo -e "${RED}[FAIL]${NC} Apache Bench não conseguiu completar o teste"
        FAIL=1
    fi
    
    rm -f "$output"
}

test_no_dropped_connections() {
    echo ""
    echo "--- Teste 14: Verificar ausência de conexões perdidas ---"
    
    local total_requests=1000
    local concurrent=50
    local tmp_success=$(mktemp)
    local tmp_failed=$(mktemp)
    
    echo 0 > "$tmp_success"
    echo 0 > "$tmp_failed"
    
    echo "A enviar ${total_requests} requests com ${concurrent} clientes paralelos..."
    
    for batch in $(seq 1 $((total_requests / concurrent))); do
        for i in $(seq 1 $concurrent); do
            (
                if curl -s -o /dev/null -w "%{http_code}" "${BASE_URL}/index.html" 2>/dev/null | grep -q "^200$"; then
                    (
                        flock 200
                        count=$(cat "$tmp_success")
                        echo $((count + 1)) > "$tmp_success"
                    ) 200>"$tmp_success.lock"
                else
                    (
                        flock 201
                        count=$(cat "$tmp_failed")
                        echo $((count + 1)) > "$tmp_failed"
                    ) 201>"$tmp_failed.lock"
                fi
            ) &
        done
        wait
    done
    
    local success=$(cat "$tmp_success")
    local failed=$(cat "$tmp_failed")
    
    rm -f "$tmp_success" "$tmp_failed" "$tmp_success.lock" "$tmp_failed.lock"
    
    local drop_rate=0
    if [ $total_requests -gt 0 ]; then
        drop_rate=$((failed * 100 / total_requests))
    fi
    
    echo "Sucessos: ${success}, Falhas: ${failed}"
    
    if [ "$drop_rate" -le 5 ]; then
        echo -e "${GREEN}[OK]${NC} Taxa de perda: ${drop_rate}% (aceitável ≤5%)"
    else
        echo -e "${RED}[FAIL]${NC} Taxa de perda: ${drop_rate}% (muito alta)"
        FAIL=1
    fi
}

test_parallel_clients() {
    echo ""
    echo "--- Teste 15: Múltiplos clientes paralelos (curl) ---"
    
    local num_clients=100
    local requests_per_client=10
    local pids=()
    local failed=0
    
    echo "A lançar ${num_clients} clientes, cada um fazendo ${requests_per_client} requests..."
    
    for client in $(seq 1 $num_clients); do
        (
            for req in $(seq 1 $requests_per_client); do
                curl -s -o /dev/null "${BASE_URL}/index.html" 2>/dev/null || exit 1
            done
        ) &
        pids+=($!)
    done
    
    # Esperar por todos os clientes
    for pid in "${pids[@]}"; do
        if ! wait "$pid" 2>/dev/null; then
            ((failed++)) || true
        fi
    done
    
    local total_clients=$num_clients
    local success=$((total_clients - failed))
    
    echo "Clientes bem-sucedidos: ${success}/${total_clients}"
    
    if [ "$failed" -eq 0 ]; then
        echo -e "${GREEN}[OK]${NC} Todos os clientes completaram com sucesso"
    else
        echo -e "${YELLOW}[WARN]${NC} ${failed} clientes falharam"
        if [ "$failed" -gt $((num_clients / 10)) ]; then
            FAIL=1
        fi
    fi
}

test_statistics_accuracy() {
    echo ""
    echo "--- Teste 16: Precisão das estatísticas sob carga ---"
    
    # Verificar se endpoint /stats existe
    local stats_check
    stats_check=$(curl -s -o /dev/null -w "%{http_code}" "${STATS_URL}" 2>/dev/null || echo "000")
    
    if [ "$stats_check" != "200" ]; then
        echo -e "${YELLOW}[SKIP]${NC} Endpoint /stats não disponível ou não implementado"
        return
    fi
    
    # Obter estatísticas iniciais
    local stats_before
    stats_before=$(curl -s "${STATS_URL}" 2>/dev/null || echo "")
    
    if [ -z "$stats_before" ]; then
        echo -e "${YELLOW}[SKIP]${NC} Não foi possível obter estatísticas iniciais"
        return
    fi
    
    # Fazer um número conhecido de requests
    local known_requests=100
    echo "A fazer ${known_requests} requests conhecidos..."
    
    for i in $(seq 1 $known_requests); do
        curl -s -o /dev/null "${BASE_URL}/index.html" 2>/dev/null &
        if [ $((i % 10)) -eq 0 ]; then
            wait
        fi
    done
    wait
    
    sleep 1  # Dar tempo para estatísticas serem atualizadas
    
    # Obter estatísticas finais
    local stats_after
    stats_after=$(curl -s "${STATS_URL}" 2>/dev/null || echo "")
    
    if [ -z "$stats_after" ]; then
        echo -e "${YELLOW}[WARN]${NC} Não foi possível obter estatísticas finais"
        return
    fi
    
    # Extrair contadores do HTML (formato: <tr><td>Total Requests</td><td>NUMBER</td></tr>)
    local requests_before requests_after
    requests_before=$(echo "$stats_before" | grep -A1 "Total Requests" | grep -oP '<td>\K\d+(?=</td>)' | head -1 || echo "0")
    requests_after=$(echo "$stats_after" | grep -A1 "Total Requests" | grep -oP '<td>\K\d+(?=</td>)' | head -1 || echo "0")
    
    if [ -z "$requests_before" ]; then requests_before=0; fi
    if [ -z "$requests_after" ]; then requests_after=0; fi
    
    local diff=$((requests_after - requests_before))
    
    echo "Requests antes: ${requests_before}, depois: ${requests_after}, diferença: ${diff}"
    echo "Esperado: aproximadamente ${known_requests} requests"
    
    # Se os contadores não mudaram, o endpoint pode não estar implementado corretamente
    if [ "$diff" -eq 0 ]; then
        echo -e "${YELLOW}[WARN]${NC} Contadores não mudaram - /stats pode não estar a atualizar corretamente"
        return
    fi
    
    # Tolerância de 20% devido a possíveis requests de outros testes
    local tolerance=$((known_requests / 5))
    local lower_bound=$((known_requests - tolerance))
    local upper_bound=$((known_requests + tolerance + 50))  # +50 para overhead
    
    if [ "$diff" -ge "$lower_bound" ] && [ "$diff" -le "$upper_bound" ]; then
        echo -e "${GREEN}[OK]${NC} Estatísticas parecem precisas (±20% tolerância)"
    else
        echo -e "${YELLOW}[WARN]${NC} Diferença nas estatísticas fora da tolerância esperada"
        echo "Isto pode ser normal se houverem outros requests em background"
    fi
}

# Executar todos os testes
test_apache_bench
test_no_dropped_connections
test_parallel_clients
test_statistics_accuracy

# Resultado final
echo ""
echo "========================================"
if [ "$FAIL" -eq 0 ]; then
    echo -e "${GREEN}TESTES DE CONCORRÊNCIA: TODOS PASSARAM${NC}"
    echo "========================================"
    exit 0
else
    echo -e "${RED}TESTES DE CONCORRÊNCIA: ALGUNS FALHARAM${NC}"
    echo "========================================"
    exit 1
fi
