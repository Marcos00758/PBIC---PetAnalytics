# Decisões para a migração ao ESP32-S3

Registro de 2026-10-05, atualizado após confirmação do hardware pelo usuário.
Destino: placa OEM ESP32-S3 Super Mini com ESP32-S3FH4R2.
O ambiente padrão agora é `esp32s3_supermini`, com uma entrada de preparação
separada. Desde 2026-10-10 há integração de aquisição/logger no ambiente
explícito `esp32s3_acquisition`; validação física/concorrência permanece pendente.

## Hardware e PlatformIO confirmados

- I2C do PCA9548A: SDA GPIO1, SCL GPIO2, 400 kHz.
- SD SPI: SCK GPIO7, MISO GPIO6, MOSI GPIO5, CS GPIO4.
- PCA9548A mantido: ICM0/1/2 nos canais 4/5/6; BMP0/1 nos canais 7/3.
  O mapa antigo da `main` foi corrigido em `constants.h` após confirmação.
- Alimentação: bateria externa com conversor regulado de 5 V na USB-C.
  O SD será alimentado pelo ESP32. Módulo informado: "Modulo Adaptador Cartão
  Micro Sd Tf Card Arduino Esp32 Pic", anúncio MLBU2296492711/MLB3617781629,
  registrado em HARDWARE_SPEC.md. A consulta ao anúncio foi bloqueada (403);
  circuito/revisão física permanecem sem esquema. O usuário confirmou depois
  VCC em 3,3 V e conexões concluídas; SD ligado ao 3V3 do ESP32.
- GPIO19/20 reservados para USB. GPIO48 reservado ao WS2812, sem driver LED ativo.
- Perfil `esp32-s3-devkitc-1` com overrides explícitos: flash 4 MB,
  partições `default.csv` de 4 MB, flash QIO a 80 MHz, PSRAM Quad SPI
  (`qio_qspi`, `BOARD_HAS_PSRAM`). CPU 240 MHz.
- USB Serial/JTAG CDC no boot, modo 1 herdado da placa-base. HID não ativado.
- `espressif32@7.1.3` e `greiman/SdFat@2.3.1` fixados no novo ambiente.
  O pacote do core Arduino 2.0.17 também é fixado explicitamente em
  `platformio/framework-arduinoespressif32@4.20017.260907+sha.dcc1105b`,
  conforme a versão instalada e compilada, para evitar resolução futura diferente.
  O ambiente Teensy conserva as bibliotecas de SD fornecidas por seu core.

Preferimos overrides do perfil suportado a um JSON próprio nesta primeira
etapa: são suficientes para memória/USB e evitam duplicar metadados do uploader.
Isso não transforma a placa OEM em uma DevKitC: pinagem externa, memórias e
descrição elétrica são as de [HARDWARE_SPEC.md](HARDWARE_SPEC.md).
O nome genérico N8/"No PSRAM" ainda aparece no cabeçalho do PlatformIO;
os overrides efetivos, e não esse nome, definem flash/PSRAM. O build utiliza
os headers SDK `esp32s3/qio_qspi` e a tabela de partições de 4 MB. O limite
estático de RAM reportado pelo PlatformIO é o do perfil/core (320 KiB), não a
soma física de SRAM e PSRAM. A detecção de PSRAM precisa da placa ligada.

`src/bringup/esp32s3_setup.cpp` é a única fonte compilada no novo ambiente.
Inicializa I2C/SPI na pinagem confirmada, mantém CS alto e informa flash/PSRAM
reais via USB a cada cinco segundos, permitindo abrir o monitor depois do boot.
Espera-se `flash_bytes=4194304`, `psram_bytes=2097152` e `memory_config_ok=1`.
Ainda não monta ou escreve no SD, não acessa os cinco sensores, não coleta áudio
e não inicia Wi-Fi/BLE. A pausa de 1 ms no loop de preparação cede CPU;
não é um agendador de aquisição. Não confundir esse programa com o futuro
teste de robustez do SD ou com o firmware final de coleta. O teste independente
agora existe no ambiente explícito `esp32s3_sd_test`; instruções e limites em
[SD_TEST.md](SD_TEST.md). Ele não substitui a preparação como ambiente padrão.

O anúncio OEM informado não abriu pela ferramenta de pesquisa. Recursos de
carga/BOOST e layout vêm da descrição fornecida e aguardam validação elétrica.

## Bibliotecas pesquisadas

| Uso | Decisão recomendada para o ESP32-S3 | Motivo e validação pendente |
|---|---|---|
| Framework | Arduino-ESP32, fornecido pela plataforma `espressif32` do PlatformIO | Preserva a organização Arduino/C++ dos drivers. Fixar plataforma e pacotes, sem URLs `master`. |
| I2C | `Wire` do Arduino-ESP32 | Já vem com o framework. Inicializar SDA/SCL explicitamente e conferir timeout/recuperação do barramento. |
| SPI | `SPI` do Arduino-ESP32 | Já vem com o framework. Inicializar SCK/MISO/MOSI/CS conforme a futura pinagem. |
| SD | **SdFat 2.3.1**, dependência externa explícita usada em `esp32s3_sd_test` | Oferece FAT16/FAT32/exFAT, `FsFile`, pré-alocação, `sync()` e `truncate()`. Build S3 aprovado; integridade na placa precisa ser validada antes da integração ao logger. |
| ICM-20948 | Manter `Adafruit ICM20X@2.0.7` | Declara `architectures=*`; driver atual usa `TwoWire`. Portabilidade declarada não substitui o teste dos três ICMs/AK09916. |
| BMP390 | Manter `Adafruit BMP3XX Library@2.1.6` | Declara `architectures=*`; preservar leitura crua e NVM individual. Não usar API de altitude para transformar o arquivo bruto. |
| Dependências Adafruit | Manter `Adafruit BusIO@1.17.4` e `Adafruit Unified Sensor@1.1.15` | Já utilizadas pelos drivers dos sensores, com suporte genérico declarado. |
| PCA9548A | Manter o driver local | Usa `Wire`; não requer biblioteca nova. |
| USB serial | `Serial`/CDC do Arduino-ESP32 | Não adicionar biblioteca USB externa nesta etapa. Confirmar conector, modo USB e configuração da placa real. |
| Duas tarefas e fila | FreeRTOS incluído no framework ESP32 | Não adicionar outra biblioteca de RTOS. Implementação futura precisa de propriedade exclusiva dos barramentos e fila limitada. |
| Áudio | Nenhuma biblioteca de áudio no build ativo | A biblioteca PJRC/Teensy `Audio` não será portada. Retomada exigirá driver I2S próprio do framework escolhido. |

A API `SD.h` da Espressif não é uma substituição direta da versão Teensy:
o logger atual depende de `SD.sdfs` e `FsFile`. Recomendo usar SdFat diretamente,
com `SdFs` e uma instância SPI explícita, para reduzir mudanças nas operações
de integridade. Isto ainda exige adaptar `File`, abertura, status, diretórios e
tratamento de erros; não basta trocar o `#include`.

`SD` nativo da Espressif é uma alternativa SPI mais simples, mas exige rever as
operações específicas de SdFat. `SD_MMC` altera a interface elétrica e só deve
ser escolhido se a pinagem e o módulo SD permitirem. O teste independente de SD
deverá decidir frequência SPI, blocos e estratégia de pré-alocação por medições.

Na pesquisa, `platformio/espressif32` **7.1.3** referencia o pacote Arduino
`~4.20017.0` (core **2.0.17**). Versão da plataforma não é versão do core.
Esse conjunto foi selecionado para a preparação Arduino. As APIs I2S
recentes do core 3.x não devem ser copiadas
para um projeto no core 2.x. Se a retomada do áudio justificar atualizar o core,
registrar e testar essa mudança separadamente.

Fontes primárias consultadas:

- [Manifesto PlatformIO 7.1.3](https://github.com/platformio/platform-espressif32/blob/v7.1.3/platform.json).
- [I2C Arduino-ESP32](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/i2c.html).
- [SD da Espressif](https://github.com/espressif/arduino-esp32/blob/master/libraries/SD/src/SD.h).
- [SD_MMC da Espressif](https://github.com/espressif/arduino-esp32/blob/master/libraries/SD_MMC/README.md).
- [SdFat e cuidados com pré-alocação exFAT](https://github.com/greiman/SdFat).
- [Adafruit ICM20X](https://github.com/adafruit/Adafruit_ICM20X/blob/master/library.properties).
- [Adafruit BMP3XX](https://github.com/adafruit/Adafruit_BMP3XX/blob/master/library.properties).
- [Adafruit BusIO 1.17.4](https://github.com/adafruit/Adafruit_BusIO/blob/1.17.4/library.properties).
- [Adafruit Unified Sensor 1.1.15](https://github.com/adafruit/Adafruit_Sensor/blob/1.1.15/library.properties).
- [FreeRTOS e dois núcleos no ESP32-S3](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-guides/freertos-smp.html).
- [I2S Arduino-ESP32 atual, para futura avaliação](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/i2s.html).

## Suspensão do áudio implementada

**Contexto do registro anterior:** as alterações de suspensão abaixo foram
feitas na branch `presentation`. Ao voltar à `main`, somente o snapshot,
os registros de migração e `audio_types.h` haviam sido incluídos; a aplicação
Teensy da `main` ainda contém os caminhos antigos de áudio. O build ESP32 atual
exclui toda essa aplicação e todos os módulos de áudio por seu filtro de fontes.
Não interpretar os passos abaixo como uma descrição do ambiente Teensy atual
da `main`. A retomada de áudio permanece fora do escopo do ESP32.

Motivo: validar primeiro apenas três ICM-20948 e dois BMP390, sem a carga
de captura, interrupções e escrita de áudio, nem dependências exclusivas do Teensy.

1. O estado anterior está preservado em `legacy/teensy40/`, incluindo as sete
   alterações locais e os dados experimentais; detalhes em `SNAPSHOT.md`.
2. `platformio.ini` exclui `ics43434.cpp`, `audio_capture.cpp` e
   `audio_sd_diagnostic.cpp`, e ignora a biblioteca `Audio`.
3. `main.cpp` não inclui nem instancia captura/diagnósticos de áudio e não
   executa preflight, inicialização I2S ou roteamento PCM.
4. `kAudioSubsystemEnabled` e os três flags históricos permanecem `false`.
   Um `static_assert` rejeita a tentativa de reativá-los isoladamente.
5. `src/data/audio_types.h` contém somente tipos históricos, sem `Audio.h`.
   O logger usa contadores zerados e rejeita sessões que solicitem áudio.
6. A fila de 512 blocos de captura não é instanciada e o buffer SD de áudio
   de 32 KiB não é reservado; há apenas um byte de armazenamento residual
   para manter a compilação das rotinas históricas inacessíveis do logger.
7. Campos de áudio em `meta.txt`, `status.txt` e `journal.bin` são mantidos
   por compatibilidade, com áudio desabilitado e sem PCM. O pacote de sensores
   continua v4/79 bytes. A versão do firmware passa a `0.5.2`.
8. `python/export_audio.py`, `python/analyze_sd_blocks.py` e seus testes
   permanecem disponíveis para ler dados históricos. Não fazem coleta ou
   ativação de áudio na placa. Nenhum dado antigo foi apagado.

Na etapa `presentation` o modo de apresentação permaneceu selecionado.
Na preparação ESP32 atual não há aquisição. A volta à gravação SD e a coleta
sem filtros pertencem à próxima integração. Nenhum filtro foi acrescentado.

## Como retomar áudio posteriormente

- Definir pinagem, alimentação e modelo do microfone na nova montagem.
- Decidir taxa, canal e formato PCM. O histórico usa 44.100 Hz, mono, PCM16,
  128 amostras/bloco; isso não é uma obrigação da arquitetura ESP32.
  O ICS43434 fornece 24 bits; justificar truncamento, alinhamento e escala.
- Escolher uma versão fixada do core e seu driver I2S. Core 2.x e 3.x têm
  caminhos de API distintos; não reutilizar `AudioInputI2S`/`AudioStream`
  ou primitivas de interrupção do Teensy.
- Criar teste I2S separado: silêncio, DC/RMS, clipping, bits úteis, taxa real,
  DMA/overflow, timestamp inicial e perdas. Depois testar interferência do SD
  na alimentação e no sinal antes de integrar aos sensores.
- Dimensionar filas e restaurar armazenamento real de áudio no logger;
  revisar arbitragem e o consumidor PCM antes de remover os bloqueios de build.
  A aplicação antiga não drenava PCM no loop normal integrado; copiar somente
  seus flags não restaura um pipeline completo.
- Decidir explicitamente se blocos perdidos continuam recebendo silêncio.
  Se mantido, registrar perdas e inserções para rastreabilidade científica.
- Validar sincronização de áudio/sensores e tempo de serviço do SD, preservar
  arquivos separados e atualizar documentação/parser se formato ou metadados mudarem.
- Usar os resultados históricos M002/M004/S007/S012/S017 como referências de
  falhas de sinal, espaço e concorrência, sem assumir sua repetição no ESP32.

## Dois núcleos, SD e desligamento

O hardware dual-core pode permitir aquisição e escrita em paralelo, mas não
divide automaticamente o `loop()` atual. A recomendação é uma tarefa para
aquisição, proprietária de `Wire`/PCA9548A, e outra para SD/SPI, comunicadas por
fila FreeRTOS limitada. Somente a tarefa de SD acessará arquivos e operações
de manutenção. O buffer atual do logger não é seguro para uso concorrente direto.

Medir latência máxima de escrita e `sync()`, jitter, perdas, ocupação e vazão
antes de escolher afinidade/prioridades. Dimensionar a fila pela maior pausa
observada e margem: sensores geram nominalmente **7.900 bytes/s**. Por exemplo,
8 KiB acomodam aproximadamente um segundo de dados; isso não é garantia para
qualquer cartão. Dois núcleos não eliminam pausas internas do cartão ou falhas
de energia. Não manter mutex/seção crítica entre núcleos durante escrita de SD.
Não reutilizar os comandos de desabilitar interrupções do Teensy como proteção
entre núcleos.

**Decisão do usuário:** não implementar botão/comando de encerramento normal
nesta etapa. Manter sessão contínua até desligamento/falha e recuperação pelo
último journal confirmado. A cauda ainda em RAM ou posterior ao último journal
pode ser perdida (cadência nominal atual de aproximadamente dez segundos).
O journal não garante recuperação de corrupção do sistema de arquivos causada
por perda de energia. Validar desligamentos abruptos no teste independente de SD;
na ausência de truncamento final, o leitor deve respeitar o prefixo confirmado
e os detalhes de pré-alocação exFAT da versão SdFat escolhida.

## Referências de coleta recebidas em 2026-10-07

O usuário enviou `peitoral.ino` e `Capsule.ino` como referências M5Stack para a
futura coleta no ESP32-S3. Originais preservados, com hashes e análise, em
[references/m5stack/README.md](references/m5stack/README.md). O Capsule fornece
o exemplo de duas tarefas por afinidade; o peitoral exemplifica arquivos abertos
e manutenção periódica, mas não separa aquisição/SD em tarefas da aplicação.
Não copiar a sincronização insegura de buffers JSON nem suas APIs de timer 3.x
para o core 2.0.17 atual.

Plano atualizado: coleta proprietária do I2C no núcleo 1 e consumidor
proprietário do SD no núcleo 0, comunicados por fila limitada de pacotes crus.
Afinidades, prioridades e memória dependem de validação sob carga; detalhes em
[ARCHITECTURE.md](ARCHITECTURE.md#distribuição-planejada-entre-os-núcleos).
Não houve implementação dessa distribuição nem alteração de bibliotecas.

**Situação do SD:** assumir sucesso apenas para avançar no planejamento, conforme
pedido do usuário. Os erros anteriores de inicialização não estão resolvidos
por evidência experimental. O usuário montará outro protótipo com outro ESP32
para isolar o hardware; os resultados devem ser registrados antes de aprovar
armazenamento/coleta integrada. Não reclassificar os ensaios anteriores como sucesso.

## Próximas validações

**Correção localizada ICM, firmware S3 0.6.1:** mantida Adafruit ICM20X 2.0.7,
contornando o registrador incorreto em `enableGyrolDLPF()` no nosso driver.
ACCEL_CONFIG=0x04/GYRO_CONFIG_1=0x06 desativam os DLPFs e preservam as faixas;
readback dos dois registradores e de CNTL2=0x04 (AK09916) é obrigatório no boot.
Constantes distinguem conversão nominal 4500/9000 Hz de leitura ESP32 a 100 Hz.
Serial/metadados mostram os registradores confirmados. Sem alteração de pinos,
tarefas, SD ou pacotes; somente metadados textuais recebem campos específicos.
O bypass está documentado nas tabelas 16/18 do
[datasheet TDK](https://product.tdk.com/system/files/dam/doc/product/sensor/mortion-inertial/imu/data_sheet/ds-000189-icm-20948-v1.5.pdf);
o defeito pode ser conferido no
[código Adafruit 2.0.7](https://github.com/adafruit/Adafruit_ICM20X/blob/2.0.7/Adafruit_ICM20X.cpp#L543).
Validar na placa três `ICM_INIT` com `ok=1 config_verified=1`, registradores
0x04/0x06/0x04 e os mesmos valores em meta.txt. Taxas físicas não foram medidas.
Alias acima de 50 Hz permanece possível; nenhum filtro foi acrescentado.
Verificação desta correção: builds Teensy/ESP32 aprovados e 43 testes host
aprovados (estes não exercitam o driver físico). ESP32: 51.380 bytes de RAM
estática e 346.197 bytes de flash. Sem upload; readback na montagem pendente.

**Atualização em 2026-10-10:** o usuário substituiu ESP32 e módulo SD, mantendo
FH4R2 e pinos. Houve montagem/recuperação de prefixo, mas timeout de escrita
abortou a suíte. [SD_TEST.md](SD_TEST.md) registra resultados e motivo de reset
agora exposto no serial. Não assumir estabilidade resolvida pela troca.

Integração inicial disponível em `esp32s3_acquisition`: coleta núcleo 1,
SD núcleo 0, fila estática por valor, sensores crus v4 e journal existentes.
Logger usa SdFat externo no S3 e preserva backend Teensy. Áudio fica excluído,
sem instalar M5Unified/Firebase/NTP. Afinidades, prioridades e pilhas precisam
de medição em placa; roteiro de bancada no README. Não houve upload automático.

Pinagem externa, canais, alimentação por conversor e USB foram definidos.
VCC do módulo SD foi confirmado em 3,3 V e a montagem está concluída. Registrar
cartão utilizado, corrente do conversor/regulador e detalhes do esquema OEM.
Validar flash/PSRAM/USB e executar o novo `esp32s3_sd_test` na placa para
avaliar SdFat isoladamente. Validar os cinco sensores no novo ambiente,
conferir compensação dos BMP390 com dados crus/NVM e testar tarefas/fila e
gravação prolongada. Não aprovar sessões longas enquanto timeouts e
interrupções USB/reset continuarem sem diagnóstico.
Sem filtros de sensores ou pós-processamento na nova coleta, conforme pedido.

## Validação da integração — 2026-10-10

- Quatro builds aprovados: teensy40, esp32s3_supermini, esp32s3_sd_test e
  esp32s3_acquisition. Coleta recompilada após completar metadados:
  **51.348 bytes de RAM estática / 345.777 bytes de flash**. Pilhas do RTOS
  e alocações feitas no boot não estão incluídas nessa RAM estática.
- 43 testes Python aprovados. Quatro novos compilam aquisição/pacote/CRC C++
  reais com relógio e sensores injetados, conferindo leitura em Python,
  deadlines sem rajadas, falha de sensor e rollover. Não validam drivers
  físicos nem escalonamento real dos dois núcleos.
- Snapshot preservado: 58 arquivos, patch local e 44 experimentos,
  236.521.430 bytes. `git diff --check` aprovado.
- Não houve upload, commit ou push pelo agente. Estado físico disponível
  é somente o log SD fornecido pelo usuário; não aprova esta integração.
- Pendentes: sensores/NVM na montagem nova, fila sob atraso/falha do SD,
  jitter, pilhas, recuperação por journal e gravações prolongadas.

Arquivos criados/alterados nesta integração:

- Entrada e configuração: `src/esp32/main.cpp`, `src/config/esp32_runtime.h`,
  `src/config/constants.h`, `platformio.ini`.
- Serviços compartilhados: `src/services/sd_logger.cpp`, `sd_logger.h`,
  `imu_acquisition.cpp`, `imu_acquisition.h`, `audio_capture.h` (somente
  unificação dos tipos históricos, sem ativar áudio no S3).
- Motivo de reset: `src/utils/esp32_reset_reason.h`,
  `src/diagnostics/esp32s3_sd_test.cpp` (preservando alterações anteriores).
- Testes: `test/acquisition_host.cpp`, `test/acquisition_host/Arduino.h`,
  `test/acquisition_host/drivers/icm20948.h`,
  `test/acquisition_host/drivers/bmp390.h`, `python/tests/test_acquisition_host.py`.
- Documentação: `README.md`, `docs/ARCHITECTURE.md`, `docs/DATA_FORMAT.md`,
  `docs/ESP32_MIGRATION.md`, `docs/HARDWARE_SPEC.md`, `docs/SD_TEST.md`.

As referências M5Stack já preservadas não foram alteradas nesta etapa.
Commit sugerido: `feat(esp32): add dual-core raw sensor acquisition with SdFat`.

## Validação da preparação atual

**Validação da preparação atual em 2026-10-05:** dependências instaladas e
build `esp32s3_supermini` aprovado com o core exato fixado, SdFat 2.3.1,
SDK `qio_qspi`, USB modo 1 e `BOARD_HAS_PSRAM`. Firmware de preparação:
19.316 bytes de RAM estática e 281.305 bytes de flash. A tabela binária de
partições foi conferida: todas as partições cabem nos 4 MiB físicos.
O slot de aplicação do `default.csv` tem 1.310.720 bytes; os slots OTA e
partições de dados reservados pela tabela não ativam serviços OTA/SPIFFS.
O build explícito `teensy40` também passou após a atualização dos canais.
`verify_snapshot.py` confirmou os 58 arquivos, patch e 44 dados experimentais
do legado preservado. `git diff --check` passou. Nenhum upload foi realizado;
detecção real de PSRAM/flash, USB, alimentação, sensores e cartão permanece
pendente de teste na placa física.

## Validação anterior da suspensão na branch presentation

- Build PlatformIO `teensy40` aprovado antes e depois da suspensão.
  Pacotes locais utilizados: Teensyduino `1.162.0` (1.62), ferramentas Teensy
  `1.162.0` e toolchain `1.150201.0`. A dependência `Audio` saiu do build.
- RAM1 de variáveis: **232.704 → 21.408 bytes**, redução de **211.296 bytes**,
  no build com apresentação selecionada. Esse resultado não estima uso de
  memória nem desempenho da futura compilação ESP32-S3.
- Os **103 testes Python existentes passaram**, incluindo leitura/exportação
  de áudio histórico, parser, journal e ferramentas dos sensores.
- `verify_snapshot.py` confirmou os 58 arquivos preservados, o patch das
  alterações locais e os 44 arquivos experimentais contidos no ZIP.
- Nenhum upload, medição em hardware ou build ESP32-S3 foi realizado.

Repetir na raiz do projeto:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e teensy40
& ./.venv/Scripts/python.exe -B -m unittest discover -s python/tests -v
& ./.venv/Scripts/python.exe -B legacy/teensy40/verify_snapshot.py
git diff --check
```
