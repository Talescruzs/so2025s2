#!/bin/bash
# Script para executar o simulador automaticamente

# Remove log antigo
rm -f log_da_console

# Executa o simulador com comandos automáticos
# C = continua (execução contínua)
# Aguarda 10 segundos para dar tempo de processar
# F = fim
{
    sleep 0.5
    echo "c"  # Continua
    sleep 10  # Deixa executar por 10 segundos
    echo "f"  # Finaliza
} | timeout 15 ./main

# Mostra o log gerado
echo "========== LOG DA CONSOLE =========="
cat log_da_console | tail -200
echo "===================================="
