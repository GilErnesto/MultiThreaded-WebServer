# Servidor Web Multi-Thread com IPC e Semáforos
## Autores
Rodrigo Machado Pereira - NMec: 125696
Gil Ernesto Leite Guedes - NMec: 125031

## Descrição
Servidor HTTP/1.1 concorrente implementado em C.
Usa arquitetura mestre–trabalhadores com múltiplos processos, pools de threads, memória partilhada e semáforos POSIX.
Serve ficheiros estáticos com cache, registos e estatísticas sincronizadas.

## Compilação
make            — compila
make clean      — limpa
make run        — compila + executa o servidor
make testSimple — compila + executa + corre testes simples
make testFull   — compila + executa + corre testes simples + testes complexos/longos

## Início Rápido e Utilização
Antes de executar, defina a página proibida sem permissões:
```
chmod 000 www/privado.html
```

Para iniciar o servidor: "make && ./server" ou "make run".

Durante a execução pode aceder a http://localhost:8080/index.html (porta 8080 definida em server.conf) e clicar em Dashboard para ver estatísticas. As mesmas estatísticas aparecem periodicamente no terminal.

Para parar: CTRL+C ou "pkill -f server".

## Testes
Existem duas suítes de testes: "testSimple" cobre funcionalidade e concorrência; "testFull" inclui também testes de carga, stress e sincronização (demoram mais).