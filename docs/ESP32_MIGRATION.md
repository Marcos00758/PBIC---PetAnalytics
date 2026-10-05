# Decisões para a migração ao ESP32-S3

Registro de 2026-10-05. Destino informado: placa OEM ESP32-S3 Super Mini com
ESP32-S3FH4R2. Esta alteração preserva a etapa Teensy e suspende o áudio;
**a migração do firmware e da pinagem ainda não foi implementada**.
O build ativo continua `teensy40`. A arquitetura elétrica da nova placa está pendente.

## Bibliotecas pesquisadas

| Uso | Decisão recomendada para o ESP32-S3 | Motivo e validação pendente |
|---|---|---|
| Framework | Arduino-ESP32, fornecido pela plataforma `espressif32` do PlatformIO | Preserva a organização Arduino/C++ dos drivers. Fixar plataforma e pacotes, sem URLs `master`. |
| I2C | `Wire` do Arduino-ESP32 | Já vem com o framework. Inicializar SDA/SCL explicitamente e conferir timeout/recuperação do barramento. |
| SPI | `SPI` do Arduino-ESP32 | Já vem com o framework. Inicializar SCK/MISO/MOSI/CS conforme a futura pinagem. |
| SD | **SdFat 2.3.1**, dependência externa explícita, candidata ao teste de SD | Oferece FAT16/FAT32/exFAT, `FsFile`, pré-alocação, `sync()` e `truncate()`. Confirmar compilação e integridade no S3 antes de integrar ao logger. |
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
Esse conjunto é um candidato para a primeira migração Arduino, ainda não
instalado/validado aqui. As APIs I2S recentes do core 3.x não devem ser copiadas
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

O modo de apresentação existente permanece selecionado nesta alteração;
a volta à gravação SD e a coleta sem filtros pertencem à próxima integração.
Nenhum filtro foi acrescentado nem os filtros existentes foram alterados aqui.

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

## Próximas validações

Receber pinagem/esquema OEM, alimentação, módulo/cartão SD e interface USB.
Configurar flash/PSRAM para o modelo real, depois compilar e testar SdFat de
forma isolada. Portar os cinco sensores, conferir a compensação dos BMP390 com
dados crus/NVM, restaurar gravação SD e testar tarefas/fila sob carga prolongada.
Sem filtros de sensores ou pós-processamento na nova coleta, conforme pedido.

## Validação desta alteração

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
