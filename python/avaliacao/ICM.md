# Avaliação dos ICM-20948

Etapas 1 e 2 implementadas em 2026-10-13: [avaliar_icm.py](avaliar_icm.py).
Relatório textual no computador. Abrange ICM0/1/2
(canais PCA 4/5/6), incluindo acelerômetros, giroscópios e AK09916.

## Como executar

Na raiz do projeto, com os arquivos da sessão copiados do SD:

```powershell
& ".\.venv\Scripts\python.exe" -B python/avaliacao/avaliar_icm.py data/S002
```

Para um trecho que você **sabe ter sido capturado em repouso**:

```powershell
& ".\.venv\Scripts\python.exe" -B python/avaliacao/avaliar_icm.py data/S002 --start-s 0 --end-s 60 --rest
```

O início é relativo ao primeiro pacote válido; o fim é exclusivo. A janela
seleciona as estatísticas, mas a integridade é verificada em todo o prefixo
confirmado. `--rest` identifica a média do giroscópio como offset observado
em repouso; não subtrai esse valor nem detecta repouso automaticamente.
O relatório aparece no terminal e nenhum arquivo da sessão é alterado.

Códigos de saída: `0` = integridade passou com pelo menos dois pacotes na
janela; `1` = captura com problemas ou amostras insuficientes; `2` = entrada,
metadados, checkpoint ou parâmetros inválidos. Passar na integridade não
certifica exatidão ou adequação física dos sensores.

## Entrada e regras

- Análise no computador de uma sessão SD completa: `imu.bin`, `meta.txt`,
  `journal.bin`; `status.txt` e log serial, quando disponíveis.
- Reaproveita [parse_data.py](../parse_data.py) para decodificação, CRC,
  sequência, timestamps e limite confirmado pelo journal. Exige checkpoint
  binário válido e limite dentro do arquivo; não usa fallback de `journal.txt`.
  As demais ferramentas mantêm seu comportamento anterior.
- Conferir versão v4/79 bytes, faixas e configuração por sensor nos metadados.
  Metadado insuficiente deve produzir conclusão inconclusiva, não escala presumida.
- Preservar contagens originais. Conversão em g, graus/s e µT serve
  somente à interpretação. Sem filtros, suavização, remoção de ruído, fusão,
  integração para posição ou correção automática de bias/hard/soft iron.
- Estatísticas descritivas não substituem a série crua. Não eliminar repetições,
  preencher lacunas ou sobrescrever arquivos de entrada.

## O que o script avalia

| Parte | Relatório implementado |
|---|---|
| Integridade da captura | Pacotes válidos, CRC, lacunas, sequências duplicadas/ambíguas, inconsistências de timestamps, cauda parcial, duração, taxa média e estatísticas dos intervalos. Cauda incompleta é informada separadamente de corrupção. |
| Acelerômetros | Mínimo/máximo, média e desvio por eixo; norma, proximidade do limite e duração das repetições. Selecionar cada posição estática pela janela para avaliar sinais e resposta. |
| Giroscópios | Métricas por eixo e norma; média como offset apenas com `--rest`; proximidade do limite e duração das repetições. Rotações precisam de intervalos identificados pelo usuário. |
| Magnetômetros | Eixos e norma em µT sem correções; mudanças observadas, duração das repetições e pacotes com vetor zero. Contadores disponíveis em `status.txt`, incluindo overruns e overflows. |
| Comparação entre ICMs | Estatísticas da diferença absoluta entre normas, para os três pares. Não compara eixos assumindo alinhamento nem aplica correção de montagem. |

As estatísticas usam todos os pacotes válidos da janela, inclusive valores em
cache; o desvio padrão é populacional. As escalas nominais são faixa/32768
para aceleração e giroscópio, e 0,15 µT/contagem para o magnetômetro. Em ±8 g,
4096 contagens correspondem a 1 g, conforme o datasheet. Faixa ausente ou
inválida deixa a conversão correspondente indisponível; contradição com os
bits de faixa do readback declarado interrompe a análise.
O limiar de 32000 contagens conta amostras próximas do limite do ADC, sem
certificar saturação física. Relatórios com integridade falha não devem ser
usados para aprovar métricas físicas.

Leitura pelo ESP32 a 100 Hz é diferente da conversão interna dos ICMs.
Registrar bypass e taxas nominais dos metadados; não prometer conversões
sincronizadas nem ausência de aliasing. Timestamp é da rodada, não de cada sensor.

Os magnetômetros usam cache. Valores iguais não comprovam falta de atualização;
mudanças também não medem diretamente a ODR de 20 Hz. O pacote atual não traz
idade/validade individual ou ST1/ST2. Usar contadores disponíveis como evidência
complementar, respeitando que `status.txt` é um snapshot periódico.
Suspeita de congelamento depende de movimento conhecido e deve ser sinalizada
como suspeita, sem certificar conexão contínua a partir do arquivo sozinho.

## Capturas de bancada

1. Repouso: 60 s em superfície estável, registrando orientação e ambiente.
2. Seis posições: aproximadamente 20 s por posição, anotando os intervalos.
3. Rotações: movimentos lentos e identificados nos três eixos; usar referência
   externa se a intenção for quantificar a exatidão do giroscópio.
4. Movimento representativo: captura curta para observar amplitude/saturação.

Não classificar movimento usando uma janela de repouso desconhecida: cada
ensaio deve informar seus intervalos. Registrar posição relativa dos três ICMs
e presença de metais/ímãs. Avaliar adequação ao movimento que a pesquisa pretende
medir; estabelecer tolerâncias antes de aprovar, sem números arbitrários.

## Investigação dos picos da S003 — 2026-10-29

Revisão sem alteração do código ICM: nove valores de aceleração/giroscópio
atingiram exatamente -32768/+32767; outro gyro chegou a 32377. Vários picos
de gyro possuem vizinhos de centenas de graus/s e mudança simultânea da norma
da aceleração. Em 65,22-65,23 s, ICM2 gyro Z atingiu os dois extremos; em
722,78 s, ICM0 acel Y atingiu -8 g e a norma aproximadamente 9,71 g, com gyro
da mesma rodada próximo de 359 graus/s. São indícios compatíveis com movimento,
impacto/vibração e saturação, não comprovação de causa mecânica.

Hipóteses e discriminação: primeiro observar fixação/cabos e sincronizar vídeo
com dados crus; comparar movimento suave, repouso e movimento representativo.
DLPF em bypass com leitura host de 100 Hz pode admitir aliasing de vibrações
acima de 50 Hz. Avaliar aquisição diagnóstica mais rápida e, se autorizada,
limitação de banda antes da redução de taxa. Filtro após a aquisição não recupera
picos saturados nem remove aliasing já ocorrido. Se houver saturação real do
acelerômetro, avaliar ±16 g; o gyro já está na maior faixa, ±2000 graus/s.
Persistindo extremos em repouso sem perturbação, investigar alimentação,
comunicação, atualização dos registradores e integridade antes do CRC do pacote.

A decodificação big-endian e o bloco de 12 bytes correspondem aos registradores
de acel/gyro. Não foi identificada inversão sistemática de bytes que explique
isoladamente os picos. O congelamento magnético do ICM1 observado na S003 é
uma pendência separada; nenhum filtro deve mascarar dados antigos ou inválidos.

## Implementação por partes

1. Implementado: entrada, validação da captura e relatório textual de acel/gyro.
2. Implementado: magnetômetros e comparação entre os três sensores.
3. Pendente: exportar resumo JSON e gráficos crus apenas se os relatórios exigirem isso.

Funções curtas para leitura, métricas, avaliação e apresentação; métricas sobre
todos os pacotes válidos, sem exigir carregar toda a sessão em RAM.
Reaproveitar o que for adequado em [analyze_imu.py](../analyze_imu.py), sem
reescrever o analisador existente. Não adicionar dependências na versão mínima.
Resultados futuros em `data/avaliacao/<ensaio>/icm/`, separados da sessão original.

Testes automatizados: vetores conhecidos, desvio populacional, proximidade do
limite, CRC, sequência/timestamp com rollover, cauda parcial, checkpoint,
metadados, janelas, cache e preservação dos arquivos de entrada. Executar:

```powershell
& ".\.venv\Scripts\python.exe" -B -m unittest discover -s python/tests -p test_avaliar_icm.py -v
```

Validação física pendente: capturas com repouso, posições e rotações conhecidos.
Separar integridade do transporte, diagnóstico de valores e adequação física.
Exatidão requer referência; um CSV/CRC correto não basta para aprovar o sensor.
Nos trechos de repouso identificados, avaliar norma da aceleração próxima de
1 g e giroscópio próximo de zero, quantificando desvios antes de definir aprovação.
Não adotar como limites de ruído especificações obtidas com DLPF ativado.

Referência técnica: [datasheet TDK ICM-20948](https://product.tdk.com/system/files/dam/doc/product/sensor/mortion-inertial/imu/data_sheet/ds-000189-icm-20948-v1.5.pdf).

Dados/relatórios em `data/` são ignorados pelo Git e precisam de cópia de segurança.
Esta ferramenta não altera firmware, formato dos pacotes ou arquitetura de aquisição.
