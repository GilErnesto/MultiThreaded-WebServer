#!/bin/bash
set -euo pipefail
set +m

BASE_URL="${BASE_URL:-http://localhost:8080}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

TEST_FUNCTIONAL="${SCRIPT_DIR}/test_functional.sh"
TEST_CONCURRENCY="${SCRIPT_DIR}/test_concurrency.sh"
TEST_LOAD="${SCRIPT_DIR}/test_load.sh"
TEST_SYNC="${SCRIPT_DIR}/test_sync.sh"
TEST_STRESS="${SCRIPT_DIR}/test_stress.sh"
TEST_CONCURRENT="${SCRIPT_DIR}/test_concurrent"

FAIL=0
SERVER_PID=""

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        echo ""
        echo "A terminar servidor (PID=$SERVER_PID)..."
        
        {
            if ps -p $SERVER_PID > /dev/null 2>&1; then
                kill -TERM $SERVER_PID 2>/dev/null || true
                sleep 1
            fi
        
            if ps -p $SERVER_PID > /dev/null 2>&1; then
                kill -9 $SERVER_PID 2>/dev/null || true
                sleep 0.5
            fi
            
            # kill em qualquer outro processo do servidor que ainda corra 
            pkill -9 -f "./server" 2>/dev/null || true
        } 2>/dev/null
    fi
}

trap cleanup EXIT INT TERM

run_test_suite() {
    local test_file="$1"
    local test_name="$2"
    local skip_condition="${3:-}"
    
    echo ""
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}  ${test_name}${NC}"
    echo -e "${BLUE}========================================${NC}"
    
    if [ -n "$skip_condition" ] && [ "$skip_condition" = "skip" ]; then
        echo -e "${YELLOW}[SKIP]${NC} ${test_name}"
        return
    fi
    
    if [ ! -f "$test_file" ]; then
        echo -e "${YELLOW}[SKIP]${NC} ${test_name} - ficheiro não encontrado: ${test_file}"
        return
    fi
    
    if [ ! -x "$test_file" ]; then
        chmod +x "$test_file"
    fi
    
    if "$test_file"; then
        echo -e "${GREEN}✓ ${test_name} PASSOU${NC}"
    else
        echo -e "${RED}✗ ${test_name} FALHOU${NC}"
        FAIL=1
    fi
}

start_server() {
    local server_bin="${SERVER_BIN:-./server}"
    
    if [ ! -x "$server_bin" ]; then
        echo -e "${RED}Erro: servidor '$server_bin' não encontrado ou não executável${NC}"
        exit 1
    fi
    
    echo "A arrancar servidor: $server_bin"
    setsid "$server_bin" > /dev/null 2>&1 &
    SERVER_PID=$!
    disown
    
    echo "Aguardando servidor inicializar (5s)..."
    sleep 5
    
    if ! ps -p $SERVER_PID > /dev/null 2>&1; then
        echo -e "${RED}Erro: servidor falhou ao arrancar${NC}"
        exit 1
    fi
    
    echo -e "${GREEN}Servidor arrancado com PID=$SERVER_PID${NC}"
}

main() {
    local run_mode="${1:-normal}"
    
    echo -e "${BLUE}========================================"
    echo "   SUITE COMPLETA DE TESTES"
    echo "========================================"
    echo -e "Servidor: ${BASE_URL}${NC}"
    echo ""
    
    # variáveis para os sub-scripts
    export BASE_URL
    export SERVER_BIN="${SERVER_BIN:-./server}"
    export LOG_FILE="${LOG_FILE:-server.log}"
    
    # Arrancar servidor
    start_server
    
    # Executar testes conforme modo
    case "$run_mode" in
        quick|fast)
            echo "Modo rápido - a executar apenas testes essenciais"
            export QUICK_TEST=1
            run_test_suite "$TEST_FUNCTIONAL" "Testes Funcionais"
            run_test_suite "$TEST_LOAD" "Teste de Carga (ab)"
            ;;
            
        full|complete)
            echo "Modo completo - a executar TODOS os testes (incluindo longos)"
            run_test_suite "$TEST_FUNCTIONAL" "Testes Funcionais"
            run_test_suite "$TEST_CONCURRENCY" "Testes de Concorrência"
            run_test_suite "$TEST_LOAD" "Teste de Carga (ab)"
            
            if [ -x "$TEST_CONCURRENT" ]; then
                echo ""
                echo -e "${BLUE}========================================"
                echo "  Teste Concorrente em C"
                echo -e "========================================${NC}"
                if "$TEST_CONCURRENT" localhost 8080 /index.html 10 20; then
                    echo -e "${GREEN}✓ Teste Concorrente C PASSOU${NC}"
                else
                    echo -e "${RED}✗ Teste Concorrente C FALHOU${NC}"
                    FAIL=1
                fi
            fi
            
            run_test_suite "$TEST_SYNC" "Testes de Sincronização"
            run_test_suite "$TEST_STRESS" "Testes de Stress"
            ;;
            
        sync)
            echo "Modo sincronização - apenas testes de sincronização"
            run_test_suite "$TEST_SYNC" "Testes de Sincronização"
            ;;
            
        stress)
            echo "Modo stress - apenas testes de stress"
            run_test_suite "$TEST_STRESS" "Testes de Stress"
            ;;
            
        *)
            echo "Modo normal - testes essenciais sem os muito longos"
            export QUICK_TEST=1
            run_test_suite "$TEST_FUNCTIONAL" "Testes Funcionais"
            run_test_suite "$TEST_CONCURRENCY" "Testes de Concorrência"
            run_test_suite "$TEST_LOAD" "Teste de Carga (ab)"
            
            if [ -x "$TEST_CONCURRENT" ]; then
                echo ""
                echo -e "${BLUE}========================================"
                echo "  Teste Concorrente em C"
                echo -e "========================================${NC}"
                if "$TEST_CONCURRENT" localhost 8080 /index.html 10 20; then
                    echo -e "${GREEN}✓ Teste Concorrente C PASSOU${NC}"
                else
                    echo -e "${RED}✗ Teste Concorrente C FALHOU${NC}"
                    FAIL=1
                fi
            fi
            
            echo ""
            echo -e "${YELLOW}Nota: Testes de sincronização e stress não executados em modo normal${NC}"
            echo -e "${YELLOW}Use 'make testFull' para executar todos os testes${NC}"
            ;;
    esac
    
    echo ""
    echo -e "${BLUE}========================================"
    echo "   RESUMO FINAL"
    echo -e "========================================${NC}"
    
    if [ "$FAIL" -eq 0 ]; then
        echo -e "${GREEN}✓✓✓ TODOS OS TESTES PASSARAM ✓✓✓${NC}"
        exit 0
    else
        echo -e "${RED}✗✗✗ ALGUNS TESTES FALHARAM ✗✗✗${NC}"
        exit 1
    fi
}

main "$@"
