# Referências M5Stack para a futura coleta ESP32-S3

Recebidas do usuário em **2026-10-07**. Cópias integrais dos arquivos de
`C:/Users/cida3/Downloads/`, sem correções ou adaptações. São material de estudo,
fora de `src/` e dos builds PlatformIO. A placa M5Stack exata, as versões das
dependências e os resultados de execução não foram informados.

| Arquivo | SHA-256 da cópia original |
|---|---|
| [peitoral.ino](peitoral.ino) | `d62a4da74c44d95d5ee6051513c8778fc3e674ee052c53624d908e55b148997c` |
| [Capsule.ino](Capsule.ino) | `4744ca2fa2cdc71370f204086920c6ca64df5568db20e69258ab8428f6f3f837` |

## O que aproveitar

`Capsule.ino` separa aquisição e envio em tarefas `xTaskCreatePinnedToCore`:
coleta no núcleo 0, prioridade 1, e Firebase no núcleo 1, prioridade 2.
Usa dois buffers JSON e um mutex. É a referência para o padrão
produtor/consumidor, não para copiar literalmente sua sincronização.

`peitoral.ino` integra IMU interna, duas BMI270 externas, áudio e SD. Mantém
arquivos abertos e realiza manutenção periódica. Não cria tarefas próprias
fixadas em núcleos: as operações da aplicação ocorrem sequencialmente em
`loop()`. Bibliotecas podem ter tarefas internas; isso não separa explicitamente
a aquisição de IMU da escrita SD neste sketch.

## Problemas observados na leitura

- **Concorrência no Capsule:** `imuReadingsCount` é incrementado sob mutex,
  mas o mutex é liberado antes de `activeJsonData->set(...)`. O consumidor
  pode trocar os ponteiros e zerar o contador antes dessa escrita. A última
  amostra pode entrar no buffer errado ou coincidir com envio/limpeza do JSON.
  A posse do buffer precisa durar até a publicação de um lote completo.
- **Perdas no Capsule:** o envio com erro também executa `clear()`; amostras
  são descartadas quando o limite está cheio, sem um contador de perdas.
  Dois buffers não oferecem capacidade ilimitada se o consumidor atrasar.
- **Cadência e tempo no Capsule:** NTP/Serial estão no caminho de coleta;
  `vTaskDelay(1)` é uma espera em ticks, não uma cadência explícita de 100 Hz.
  O timer de um segundo atualiza o horário, não agenda as leituras da IMU.
  `volatile bool timerFlag` não conta eventos acumulados e pode perder um
  evento entre teste e limpeza do flag. Não serve de modelo para deadlines.
- **Memória no Capsule:** `heap/2800` não mede o custo real dos objetos JSON.
  O limite é calculado antes das alocações de Wi-Fi/Firebase. Pilhas de 15.000
  bytes por tarefa devem ser dimensionadas por medição, não copiadas.
  Falhas de criação de mutex/tarefas não são verificadas no exemplo.
- **Peitoral:** captura, CSV, áudio e `flush()` compartilham o mesmo caminho;
  atrasos de SD afetam o intervalo entre leituras. CSV com duas casas decimais
  perde precisão e não preserva contagens cruas. Uma IMU externa sem dado
  disponível resulta em zeros, sem distinguir indisponibilidade de medida zero.

## Compatibilidade e decisões

O Capsule usa `timerBegin(frequency)`, `timerAttachInterrupt(timer, callback)`
e `timerAlarm(...)`, APIs do Arduino-ESP32 3.x. O projeto fixa **2.0.17**:
o header instalado exige `timerBegin(timer, divider, countUp)`, parâmetro `edge`
em `timerAttachInterrupt` e usa `timerAlarmWrite`/`timerAlarmEnable`.
Não atualizar o core apenas para copiar o exemplo. A diferença é descrita no
[guia oficial de migração](https://docs.espressif.com/projects/arduino-esp32/en/latest/migration_guides/2.x_to_3.0.html).

M5Unified, M5_IMU_PRO, BMI270, RTC/botão do M5Stack, áudio, Wi-Fi, NTP e Firebase
não serão incorporados nesta etapa. Permanecem três ICM-20948, dois BMP390,
PCA9548A, SdFat e dados crus, sem filtros. O exemplo não confirma presença de
RTC externo na placa OEM nem fornece pinagem aplicável ao projeto.

No FreeRTOS da Espressif, pilhas são especificadas em bytes e tarefas podem
ser fixadas aos núcleos 0/1. Desabilitar interrupções em um núcleo não protege
dados acessados pelo outro. Fonte compatível com a geração do core atual:
[FreeRTOS ESP-IDF 4.4.7 para ESP32-S3](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-guides/freertos-smp.html).

A proposta específica, ainda não implementada, está em
[ARCHITECTURE.md](../../ARCHITECTURE.md#distribuição-planejada-entre-os-núcleos).
O SD continua sem aprovação física; assumir seu funcionamento para planejar
não substitui o ensaio no novo protótipo.
