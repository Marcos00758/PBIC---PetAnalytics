# Plano de avaliação dos BMP390

Planejamento de 2026-10-13. Futuro arquivo: `avaliar_bmp.py`, nesta pasta.
Este documento não implementa nem executa a avaliação. Abrange BMP0/1,
nos canais PCA 7/3, e a investigação da altura que anteriormente subia em repouso.

## Entrada e regras

- Usar a sessão SD completa: `imu.bin`, `meta.txt`, `journal.bin`; complementar
  com `status.txt` e log serial, quando disponíveis.
- Reaproveitar [parse_data.py](../parse_data.py), exigindo limite válido do journal
  para a avaliação principal. Informar ausência/erro, sem validar pré-alocação
  ou cauda não confirmada como dados experimentais.
- Conferir identificação, taxa/configuração declarada e os 21 bytes NVM
  individuais de cada BMP. Não compartilhar coeficientes entre sensores.
- Contagens são dados crus de 24 bits; não atribuir Pa/°C antes da compensação.
  O sentinela `0xFFFFFFFF` representa ausência de leitura válida.
- Sem filtros, suavização, integração da altura, correção automática de deriva
  ou alteração dos arquivos originais. Conversão de unidades e compensação
  do fabricante servem à avaliação e não substituem a captura crua.

## O que o script avaliará

| Parte | Avaliação prevista |
|---|---|
| Integridade da captura | CRC, lacunas, cauda parcial, duração e temporização dos pacotes, aproveitando o parser. |
| Dados crus | Faixas, sentinelas, mínimo/máximo, dispersão, mudanças observadas e trechos constantes de pressão/temperatura por BMP. |
| NVM e compensação | Validar tamanho/declaração dos coeficientes; conferir a compensação existente com referência Bosch independente; detectar resultados não finitos ou incompatíveis com o ensaio. |
| Pressão e temperatura | Comparar os dois sensores no mesmo ambiente, quantificar diferenças, dispersão e evolução durante repouso/aquecimento. |
| Altura relativa | Derivar de pressão compensada com fórmula e referência de pressão explicitadas e fixas durante o ensaio; verificar sinal, repetibilidade e retorno ao nível inicial. |
| Deriva | Medir tendências de pressão, temperatura e altura durante repouso; investigar unidades, sinais, coeficientes e referência antes de atribuir o efeito ao software. |

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

1. Versão mínima: validação, diagnóstico cru e NVM por BMP.
2. Compensação em Pa/°C, comparação e métricas de repouso.
3. Altura relativa, investigação de deriva e gráficos, se necessários.

Funções curtas para leitura, compensação, métricas e apresentação. Reaproveitar
`Bmp390Calibration` e `load_bmp390_calibrations` do parser; validar com valores
esperados de referência Bosch, não com cópia da mesma fórmula Python.
Analisar todos os pacotes válidos, sem exigir carregar a sessão completa em RAM.
Versão mínima sem dependências novas; resultados futuros em
`data/avaliacao/<ensaio>/bmp/`, separados da captura original.

Testes futuros: NVM ausente/inválida, sentinelas, compensação conhecida,
repetições de cache, pressão constante, elevação e retorno, e cauda parcial.
Conclusão deve separar integridade do transporte, validade da conversão e
adequação do barômetro à resolução de movimento necessária.

Referências: [datasheet Bosch BMP390](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmp390-ds002.pdf)
e [driver oficial BMP3](https://github.com/boschsensortec/BMP3_SensorAPI).

Dados/relatórios em `data/` são ignorados pelo Git e precisam de cópia de segurança.
Este plano não altera firmware, formato dos pacotes ou arquitetura de aquisição.
