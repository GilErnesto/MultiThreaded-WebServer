#!/bin/bash
# Testes Funcionais (Requisitos 9-12)
# - GET requests para vários tipos de ficheiros
# - Códigos de status HTTP corretos (200, 404, 403, 500)
# - Directory index serving
# - Content-Type headers corretos

set -euo pipefail

BASE_URL="${BASE_URL:-http://localhost:8080}"
FAIL=0

echo "========================================"
echo "   TESTES FUNCIONAIS"
echo "========================================"

# Cores para output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

test_status() {
    local path="$1"
    local expected="$2"
    local description="$3"
    local code

    code=$(curl -s -o /dev/null -w "%{http_code}" "${BASE_URL}${path}" 2>/dev/null || echo "000")

    if [ "$code" = "$expected" ]; then
        echo -e "${GREEN}[OK]${NC} ${description}: ${path} -> ${code}"
    else
        echo -e "${RED}[FAIL]${NC} ${description}: ${path} -> ${code} (esperado ${expected})"
        FAIL=1
    fi
}

test_content_type() {
    local path="$1"
    local expected_pattern="$2"
    local description="$3"
    local ct

    ct=$(curl -s -I "${BASE_URL}${path}" 2>/dev/null | grep -i "^content-type:" | cut -d' ' -f2- | tr -d '\r\n' || echo "")

    if [ -z "$ct" ]; then
        echo -e "${RED}[FAIL]${NC} ${description}: sem Content-Type header"
        FAIL=1
        return
    fi

    if echo "$ct" | grep -iq "$expected_pattern"; then
        echo -e "${GREEN}[OK]${NC} ${description}: Content-Type contém '${expected_pattern}'"
    else
        echo -e "${RED}[FAIL]${NC} ${description}: Content-Type é '${ct}' (esperado conter '${expected_pattern}')"
        FAIL=1
    fi
}

test_get_file_types() {
    echo ""
    echo "--- Teste 9: GET requests para vários tipos de ficheiros ---"
    
    # HTML
    test_status "/index.html" "200" "HTML file"
    
    # CSS
    test_status "/style.css" "200" "CSS file"
    
    # JavaScript
    test_status "/app.js" "200" "JavaScript file"
    
    # Image (PNG)
    test_status "/img/logo.png" "200" "PNG image"
}

test_http_status_codes() {
    echo ""
    echo "--- Teste 10: Códigos de status HTTP corretos ---"
    
    # 200 OK
    test_status "/index.html" "200" "200 OK"
    
    # 404 Not Found
    test_status "/nao_existe_arquivo_123456.html" "404" "404 Not Found"
    
    # 403 Forbidden (ficheiro sem permissões de leitura)
    test_status "/privado.html" "403" "403 Forbidden"
    
    # 500 Internal Server Error (endpoint especial para testes)
    test_status "/cause500" "500" "500 Internal Server Error"
}

test_directory_index() {
    echo ""
    echo "--- Teste 11: Directory index serving ---"
    
    local tmp
    tmp=$(mktemp)
    
    curl -s "${BASE_URL}/" -o "$tmp" 2>/dev/null
    
    if [ -s "$tmp" ]; then
        # Verifica se é HTML válido
        if grep -q "html\|HTML" "$tmp"; then
            echo -e "${GREEN}[OK]${NC} Directory index (/) retorna conteúdo HTML"
        else
            echo -e "${YELLOW}[WARN]${NC} Directory index retorna conteúdo mas pode não ser HTML"
        fi
    else
        echo -e "${RED}[FAIL]${NC} Directory index não retorna conteúdo"
        FAIL=1
    fi
    
    rm -f "$tmp"
}

test_content_type_headers() {
    echo ""
    echo "--- Teste 12: Content-Type headers corretos ---"
    
    # HTML
    test_content_type "/index.html" "text/html" "HTML Content-Type"
    
    # CSS
    test_content_type "/style.css" "text/css" "CSS Content-Type"
    
    # JavaScript
    test_content_type "/app.js" "javascript" "JavaScript Content-Type"
    
    # PNG Image
    test_content_type "/img/logo.png" "image" "PNG Content-Type"
}

# Executar todos os testes
test_get_file_types
test_http_status_codes
test_directory_index
test_content_type_headers

# Resultado final
echo ""
echo "========================================"
if [ "$FAIL" -eq 0 ]; then
    echo -e "${GREEN}TESTES FUNCIONAIS: TODOS PASSARAM${NC}"
    echo "========================================"
    exit 0
else
    echo -e "${RED}TESTES FUNCIONAIS: ALGUNS FALHARAM${NC}"
    echo "========================================"
    exit 1
fi
