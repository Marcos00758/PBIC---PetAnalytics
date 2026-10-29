# Avaliação dos BMP390

Relatório textual implementado em 2026-10-13: [avaliar_bmp.py](avaliar_bmp.py).
Abrange BMP0/1,
nos canais PCA 7/3, e a investigação da altura que anteriormente subia em repouso.

## Como executar

Na raiz do projeto, avaliar toda a sessão copiada do SD:

```powershell
& ".\.venv\Scripts\python.exe" -B python/avaliacao/avaliar_bmp.py data/S001
```

Trecho de repouso conhecido, elevação e retorno da S001, respectivamente:

```powershell
& ".\.venv\Scripts\python.exe" -B python/avaliacao/avaliar_bmp.py data/S001 --start-s 10 --end-s 50 --rest
& ".\.venv\Scripts\python.exe" -B python/avaliacao/avaliar_bmp.py data/S001 --start-s 240 --end-s 250 --reference-s 230 235
& ".\.venv\Scripts\python.exe" -B python/avaliacao/avaliar_bmp.py data/S001 --start-s 265 --end-s 309 --reference-s 230 235 --rest
```

Os tempos são relativos ao primeiro pacote válido, com fim exclusivo. Essas
janelas foram identificadas na S001; não são fases detectadas automaticamente
e não devem ser copiadas para outra captura sem conferir o protocolo.
`--rest` só identifica a interpretação da tendência; não detecta repouso nem
remove deriva. Todas as execuções verificam integridade do prefixo inteiro.

Sem `--reference-s INICIO FIM`, altura fica indisponível. Cada BMP usa a média
de sua pressão nesse intervalo como referência fixa. A referência precisa de
pelo menos dois pacotes compensáveis, nenhum pacote inutilizável nesse sensor
e cobertura completa do intervalo; uma
captura com CRC/sequência/timestamps inconsistentes não produz altura.
Janelas de subida e retorno devem usar a mesma referência.

A fórmula é `h = R*T/g * ln(P_ref/P)`, R=287,05 J/(kg·K), g=9,80665 m/s².
T é fixa: média da temperatura do próprio sensor no intervalo de referência,
como estimativa da temperatura do ar. Se houver medição independente, fornecer
`--air-temperature-c 24`, substituindo 24 pelo valor medido. O relatório registra
a fonte de temperatura, pressão de referência e coeficiente da conversão.
Não há altitude absoluta, integração sucessiva, fusão ou referência móvel.

Saída apenas no terminal; arquivos originais são preservados. Código de saída
`0`: integridade passou e pelo menos dois pacotes selecionados, sem valores
inválidos nos sensores da janela; NVM ausente permite diagnóstico cru com
`sensor_data_quality=raw_only`. Código `1`: captura/amostras insuficientes ou
valores crus/físicos inválidos. Código `2`: entrada, checkpoint ou parâmetros
inválidos. Nenhum desses códigos aprova automaticamente a precisão física.

## Entrada e regras

- Usar a sessão SD completa: `imu.bin`, `meta.txt`, `journal.bin`; complementar
  com `status.txt` e log serial, quando disponíveis.
- Reaproveitar [parse_data.py](../parse_data.py), exigindo limite válido do journal
  para a avaliação principal. Informar ausência/erro, sem validar pré-alocação
  ou cauda não confirmada como dados experimentais.
- Conferir identificação, taxa/configuração declarada e os 21 bytes NVM
  individuais de cada BMP. Não compartilhar coeficientes entre sensores.
  O script exige `bmpN_nvm_valid=1`; NVM ausente, malformada, toda zero ou toda
  FF deixa a conversão desse sensor indisponível. Informa endereço, canal e
  prontidão declarados; não mede nem confirma a configuração de hardware.
- Contagens são dados crus de 24 bits; não atribuir Pa/°C antes da compensação.
  O sentinela `0xFFFFFFFF` representa ausência de leitura válida.
- Sem filtros, suavização, integração da altura, correção automática de deriva
  ou alteração dos arquivos originais. Conversão de unidades e compensação
  do fabricante servem à avaliação e não substituem a captura crua.

## O que o script avalia

| Parte | Relatório implementado |
|---|---|
| Integridade da captura | CRC, lacunas, cauda parcial, duração e temporização dos pacotes, aproveitando o parser. |
| Dados crus | Faixas, sentinelas, mínimo/máximo, dispersão, mudanças observadas e trechos constantes de pressão/temperatura por BMP. |
| NVM e compensação | Validar tamanho/declaração dos coeficientes; conferir a compensação existente com referência Bosch independente; detectar resultados não finitos ou incompatíveis com o ensaio. |
| Pressão e temperatura | Comparar os dois sensores no mesmo ambiente, quantificar diferenças, dispersão e evolução durante repouso/aquecimento. |
| Altura relativa | Derivar de pressão compensada com fórmula e referência de pressão explicitadas e fixas durante o ensaio; verificar sinal, repetibilidade e retorno ao nível inicial. |
| Deriva | Medir tendências de pressão, temperatura e altura durante repouso; investigar unidades, sinais, coeficientes e referência antes de atribuir o efeito ao software. |

Mínimo, máximo, média e desvio populacional usam todos os pacotes válidos da
janela, incluindo cache, sem deduplicação. A tendência é regressão linear
descritiva, expressa em unidades por minuto, sem alterar a série. Comparação
entre BMPs é assinada: BMP0 menos BMP1. Contadores de erro se referem à janela
selecionada; CRC e sequência abrangem o prefixo completo.

Pares com sentinela ou contagem fora de 24 bits são contados separadamente e
excluídos das métricas válidas. Compensações não finitas ou fora da faixa de
operação de 30000–125000 Pa / −40–85 °C também são contadas e excluídas, sem
limitar artificialmente o valor. Ausência de erros não certifica o sensor.

Sem NVM utilizável, continuar apenas com diagnóstico cru e marcar pressão,
temperatura física e altura como indisponíveis. Não inventar coeficientes.
Referência de altura deverá ser registrada no resultado, sem assumir altitude
absoluta. Não integrar sucessivamente diferenças de pressão/altura.

Os BMPs são consultados a 25 Hz e repetidos no fluxo de pacotes a 100 Hz.
Não contar toda repetição como erro nem afirmar 25 conversões/s só pela taxa
dos pacotes. O formato atual não informa idade de cada leitura ou data-ready;
possível congelamento permanecerá inconclusivo sem evidência complementar.
Snapshots de status não são um histórico de falhas por amostra.

## Capturas de bancada

### Configuração verificável no firmware 0.6.2

Desde 2026-10-29, a inicialização escreve explicitamente a configuração crua
e relê PWR_CTRL, OSR, ODR, CONFIG e ERR_REG. Conferir em cada `BMP_INIT`:
`raw25hz=1 nvm_valid=1 readback_ok=1 config_verified=1 pwr_ctrl=0x33 osr=0x00
odr=0x03 iir_config=0x00 error_reg=0x00`. Os bytes também ficam em `meta.txt`;
divergência, erro do sensor ou falha I2C impede iniciar a aquisição.
O resumo serial é um snapshot do boot, não uma nova medição dos registradores.

Filtros continuam desligados e o pacote permanece v4/79 bytes. Essa etapa
verifica configuração, mas não resolve a mudança comum de pressão da S008,
não monitora DRDY/idade do cache e não certifica altura em centímetros.
Para postura, o ruído e a deriva observados impedem tratar altura barométrica
como referência única; seu valor como entrada auxiliar do modelo precisa de
validação comparando o desempenho com e sem essa entrada. Escadas/relevo são
um uso futuro a validar com percursos e alturas de referência próprios.

Verificação no computador, sem placa:

```powershell
& ".\.venv\Scripts\python.exe" -B -m unittest discover -s python/tests -p test_bmp390_config.py -v
```

O teste compila o driver real com I2C simulado e cobre configuração correta,
escrita reconhecida sem alteração do registrador, leitura/escrita falha,
fatal_err/cmd_err/conf_err e descarte de uma verificação anterior. Não substitui
a conferência dos registradores na placa real.

### Roteiro de movimento

O [roteiro do segundo ensaio](ROTEIRO_BMP.md) detalha a investigação após a S004:
um BMP permanece fixo enquanto o outro percorre +20 cm, zero e -20 cm, sem
contato direto nas placas, com cronograma e registro dos eventos reais.
O percurso foi reduzido em 2026-10-27 por limitação dos fios; os tempos foram
mantidos e a diferença entre extremos passou a 40 cm.

1. Repouso: 2–5 min em altura fixa; anotar tempo desde a alimentação,
   temperatura de referência se disponível e condições do ambiente.
2. Elevação controlada: registrar níveis/alturas conhecidos, manter patamares
   por 20–30 s e voltar ao nível inicial; repetir o percurso.
3. Comparação simultânea: sensores na mesma altura e ambiente, evitando sopro,
   correntes de ar e contato que aqueça diretamente um módulo.

Comparar a amplitude da elevação com a dispersão observada sem filtros.
Se não houver separação clara, concluir que a detectabilidade é inconclusiva
naquela configuração, em vez de aprovar artificialmente com suavização.
Variação térmica/ambiental pode produzir deriva real; tendência sozinha não
prova erro no algoritmo. Limites de aprovação dependem da referência e do uso.
Não adotar limites de ruído obtidos com oversampling/filtro IIR ativados
para aprovar automaticamente a configuração crua atual.

## Implementação por partes

1. Implementado: validação, diagnóstico cru e NVM por BMP.
2. Implementado: compensação em Pa/°C, comparação e métricas de repouso.
3. Implementado: altura relativa e tendências com referência explícita.
   Gráficos continuam opcionais e não fazem parte deste script.

Funções curtas para leitura, compensação, métricas e apresentação. Reaproveitar
`Bmp390Calibration` e `load_bmp390_calibrations` do parser; validar com valores
esperados de referência Bosch, não com cópia da mesma fórmula Python.
Analisar todos os pacotes válidos, sem exigir carregar a sessão completa em RAM.
Versão mínima sem dependências novas; resultados futuros em
`data/avaliacao/<ensaio>/bmp/`, separados da captura original.

Testes automatizados cobrem NVM ausente/inválida, sentinelas, valores fora de
24 bits/faixa física, cache, pressão constante, tendências, referência fixa,
elevação/retorno, CRC, rollover e cauda parcial. A compensação é comparada com
oito vetores gerados compilando o driver C Bosch v2.0.1 instalado, sem usar a
fórmula Python para gerar os resultados esperados. A fixture
`python/tests/fixtures/bmp390_bosch.json` registra fonte, hash e método.
Tolerâncias numéricas: 0,0001 Pa e 1e-9 °C; não são tolerâncias dos sensores.

```powershell
& ".\.venv\Scripts\python.exe" -B -m unittest discover -s python/tests -p test_avaliar_bmp.py -v
```

Validação experimental de exatidão, repetibilidade e deriva permanece pendente.
Conclusão deve separar integridade do transporte, validade da conversão e
adequação do barômetro à resolução de movimento necessária.

Referências: [datasheet Bosch BMP390](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmp390-ds002.pdf)
e [driver oficial BMP3](https://github.com/boschsensortec/BMP3_SensorAPI).

Dados/relatórios em `data/` são ignorados pelo Git e precisam de cópia de segurança.
Esta ferramenta não altera firmware, formato dos pacotes ou arquitetura de aquisição.
