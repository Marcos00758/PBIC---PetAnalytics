# Plano de avaliação dos ICM-20948

Planejamento de 2026-10-13. Futuro arquivo: `avaliar_icm.py`, nesta pasta.
Este documento não implementa nem executa a avaliação. Abrange ICM0/1/2
(canais PCA 4/5/6), incluindo acelerômetros, giroscópios e AK09916.

## Entrada e regras

- Análise no computador de uma sessão SD completa: `imu.bin`, `meta.txt`,
  `journal.bin`; `status.txt` e log serial, quando disponíveis.
- Reaproveitar [parse_data.py](../parse_data.py) para decodificação, CRC,
  sequência, timestamps e limite confirmado pelo journal. Verificar a presença
  e validade desse limite antes de usar o parser, que hoje permite fallback.
- Conferir versão v4/79 bytes, faixas e configuração por sensor nos metadados.
  Metadado insuficiente deve produzir conclusão inconclusiva, não escala presumida.
- Preservar contagens originais. Conversão em g/m/s², graus/s e µT serve
  somente à interpretação. Sem filtros, suavização, remoção de ruído, fusão,
  integração para posição ou correção automática de bias/hard/soft iron.
- Estatísticas descritivas não substituem a série crua. Não eliminar repetições,
  preencher lacunas ou sobrescrever arquivos de entrada.

## O que o script avaliará

| Parte | Avaliação prevista |
|---|---|
| Integridade da captura | Pacotes válidos, CRC, lacunas, cauda parcial, duração, taxa média e distribuição dos intervalos. Cauda incompleta será informada separadamente de corrupção. |
| Acelerômetros | Mínimo/máximo, média e desvio por eixo; proximidade do limite; norma do vetor em repouso; resposta e sinais nas seis posições estáticas. |
| Giroscópios | Offset e dispersão em repouso; resposta por eixo em rotações identificadas; proximidade do limite e trechos constantes durante movimento. |
| Magnetômetros | Eixos e norma em µT sem correções; mudanças observadas, duração das repetições e resposta às rotações. Investigar zeros persistentes e possíveis perturbações magnéticas. |
| Comparação entre ICMs | Coerência de normas, resposta ao mesmo movimento e diferenças sistemáticas. Comparar eixos diretamente somente com orientações de montagem conhecidas. |

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

## Implementação por partes

1. Versão mínima: entrada, validação da captura e relatório textual de acel/gyro.
2. Acrescentar magnetômetros e comparação entre os três sensores.
3. Exportar resumo JSON e gráficos crus apenas se os relatórios exigirem isso.

Funções curtas para leitura, métricas, avaliação e apresentação; métricas sobre
todos os pacotes válidos, sem exigir carregar toda a sessão em RAM.
Reaproveitar o que for adequado em [analyze_imu.py](../analyze_imu.py), sem
reescrever o analisador existente. Não adicionar dependências na versão mínima.
Resultados futuros em `data/avaliacao/<ensaio>/icm/`, separados da sessão original.

Testes futuros: casos independentes de repouso/posição, saturação, sequência
com rollover, cauda parcial e repetições de cache; incluir captura real conhecida.
Separar integridade do transporte, diagnóstico de valores e adequação física.
Exatidão requer referência; um CSV/CRC correto não basta para aprovar o sensor.
Nos trechos de repouso identificados, avaliar norma da aceleração próxima de
1 g e giroscópio próximo de zero, quantificando desvios antes de definir aprovação.
Não adotar como limites de ruído especificações obtidas com DLPF ativado.

Referência técnica: [datasheet TDK ICM-20948](https://product.tdk.com/system/files/dam/doc/product/sensor/mortion-inertial/imu/data_sheet/ds-000189-icm-20948-v1.5.pdf).

Dados/relatórios em `data/` são ignorados pelo Git e precisam de cópia de segurança.
Este plano não altera firmware, formato dos pacotes ou arquitetura de aquisição.
