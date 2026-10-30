# Arquitetura

## Integração ESP32-S3 de sensores e SD — 2026-10-10

O ambiente explícito `esp32s3_acquisition` compila `src/esp32/main.cpp`, drivers
I2C, aquisição, logger e utilitários. O padrão `esp32s3_supermini` permanece
preparação sem escrita; `esp32s3_sd_test` continua isolado. Teensy mantém sua
entrada original, excluindo `esp32/`. Aprovação física continua pendente:
o ensaio SD novo abortou por timeout, conforme [SD_TEST.md](SD_TEST.md).

`esp32s3_pca_test` é outro ambiente isolado (2026-10-13), compilando apenas
`diagnostics/esp32s3_pca_test.cpp` e Wire. Consulta `0x70`–`0x77` e escreve
`0x00` nos endereços que responderem, comparando 100/400 kHz a cada 5 s.
Não usa os drivers de aquisição nem SdFat e não habilita canais ou sensores.
Detalhes em [PCA_TEST.md](PCA_TEST.md); o ambiente padrão permanece preparação.

`pbic-sensors`, prioridade 3, núcleo 1, é dono exclusivo de Wire/PCA9548A e
cinco sensores, inclusive inicialização. I2C GPIO1/2 a 400 kHz, timeout 10 ms.
Todos os sensores e as duas NVM precisam estar válidos para iniciar aquisição.
Mantém agendador anti-rajada de 100 Hz, BMPs a 25 Hz e cache de magnetômetros.
Cede um tick (1 ms, exigido no build) entre consultas; não promete precisão
de microsegundos sem medição física. Não executa SD, áudio ou filtros.

Firmware S3 `0.6.1`: o driver escreve e relê ACCEL_CONFIG (banco 2, 0x14)
e GYRO_CONFIG_1 (0x01), exigindo respectivamente 0x04 e 0x06: ±8 g,
±2000 graus/s e FCHOICE=0 (DLPF em bypass). Confirma também CNTL2=0x04
do AK09916 (20 Hz). Falha de escrita/leitura ou divergência impede inicialização.
Isso contorna `enableGyrolDLPF()` da Adafruit 2.0.7, que escreve em ACCEL_CONFIG.
Serial e metadados registram o readback de cada ICM; confirmação física pendente.
As taxas internas nominais em bypass são 4500/9000 Hz (accel/gyro), conforme
o [datasheet TDK, tabelas 16/18](https://product.tdk.com/system/files/dam/doc/product/sensor/mortion-inertial/imu/data_sheet/ds-000189-icm-20948-v1.5.pdf).
Os divisores 10 permanecem escritos, mas não determinam essas taxas em bypass.
Leitura pelo ESP32 continua a 100 Hz: pode haver aliasing acima de 50 Hz.
Não há interpolação, média ou filtro de software para evitar esse efeito.

Firmware S3 `0.6.2` (2026-10-29): o BMP entra em sleep antes da configuração
explícita OSR=0x00, ODR=0x03, CONFIG=0x00 e PWR_CTRL=0x33 (normal, pressão e
temperatura habilitadas). Após 10 ms, o driver relê os quatro registradores e
ERR_REG (0x02), confere os campos definidos e exige ausência de fatal_err,
cmd_err e conf_err. Escrita/leitura falha ou divergência impede iniciar a coleta.
O resultado é copiado para o diagnóstico de boot e `meta.txt`; o consumidor
não acessa o I2C para repetir o resumo. Filtros continuam desligados. A verificação
é inicial: não monitora mudanças de configuração em execução nem comprova que
cada leitura posterior seja uma conversão nova. DRDY/idade do cache permanecem
pendentes de uma etapa própria; a configuração verificada não corrige deriva.

Pinagem SD revista em 2026-10-29: CS GPIO7, MISO GPIO4, SCK GPIO6 e MOSI GPIO5,
centralizada em `config/pins.h` e usada na aquisição, preparação e diagnósticos.
Os metadados novos identificam esses GPIOs; registros históricos mantêm os anteriores.

`pbic-sd`, prioridade 2, núcleo 0, recebe o resultado de setup por cópia, monta
SdFat com SPIClass FSPI próprio e `SHARED_SPI | USER_SPI_BEGIN`, cria sessão e
libera aquisição. Só essa tarefa chama o logger, inclusive enqueue/manutenção.
Backend Teensy usa SD.sdfs; ESP32 usa SdFs externo. Textos usam FsFile e
conferem escrita/sync/fechamento. Mantém buffer 8 KiB, blocos 512 bytes, 12 MHz,
pré-alocação de dez minutos, journal após sync e sessão contínua sem comando
de encerramento. Configuração SPI é inicial, sem aprovação pela suíte física.

Fila FreeRTOS estática em SRAM: **256 pacotes completos por valor**, 20.224
bytes, 2,56 s nominais, além do buffer privado do logger. Com buffer cheio,
consumidor deixa pacotes na fila. Com fila cheia, produtor rejeita pacote novo,
conta perda e mantém sequência. High-water é observado após enqueue; rejeição
por fila cheia registra explicitamente o máximo. Uma fila estática de tamanho
1 publica snapshots dos contadores por `xQueueOverwrite`. Consumidor recebe
cópias, nunca referências ao objeto vivo do produtor. Event group comunica
início/gravação; nenhum mutex/seção crítica é mantido durante SD/I2C.

Pilhas iniciais: 6.144/8.192 bytes para aquisição/SD, alocadas pelo RTOS.
Criação das filas e tarefas é conferida; ambas aguardam um gate antes de tocar
os barramentos. Não há alocação por amostra. `config/esp32_runtime.h` reúne
afinidades, prioridades e tamanhos. `loop()` só cede CPU. Status serial a cada
5 s mostra produção, I2C, perdas, fila, maior atraso/tempo de aquisição,
latências SD e margem das pilhas. USB binária permanece desabilitada.

Diagnóstico de inicialização (2026-10-13): `SensorSetup` guarda resultados de
I2C/PCA, desativação final dos canais, IDs/endereço dos ICMs e chip ID/init dos
BMPs, além dos metadados existentes. Produtor envia essa estrutura por cópia
na fila de setup. Consumidor repete o resultado a cada 5 s em
`SENSOR_INIT_SUMMARY source=boot_snapshot`, seguido de I2C/PCA/ICM/BMP.
I2C/PCA falho identifica sensores não testados (`sensors_skipped=1`).
Esse relatório não consulta drivers/barramentos, não reinicializa dispositivos
e não altera pacotes, arquivos SD ou critérios para liberar aquisição.

Se SD falhar, coleta continua para diagnóstico, contando pacotes novos sem
armazenamento. Consumidor conta também pendências descartadas após falha.
Gravação exige reboot para retomar; SPI é encerrado após o atraso previsto,
sem aplicar o LED Teensy ao WS2812. Journal delimita dados confirmados.
Depois da falha, contadores novos podem existir só no serial.

### Contagem de espaço livre e revisão de chamadas longas — 2026-10-13

Backtrace do ELF `7b6ed35c22c58dfe` localizou o watchdog de IDLE0 em
`SdLogger::beginCard()` → `freeClusterCount()` → leitura SPI da FAT.
A mesma consulta ocorre em `hasSpaceForNextSession()`. Ambas passam agora
por `countFreeClusters()`: no S3, salva a prioridade da tarefa chamadora,
reduz para `tskIDLE_PRIORITY` (0) durante a chamada e restaura ao retornar,
inclusive quando a biblioteca indica erro. Contagem, validação de capacidade,
reserva de espaço e suporte FAT/exFAT permanecem na SdFat.

A configuração instalada usa preempção e time slicing; o build exige ambas.
IDLE0 pode executar entre ticks enquanto a contagem prossegue no núcleo 0.
Prioridade normal de armazenamento continua 2; aquisição permanece no núcleo 1.
Watchdog, biblioteca instalada e driver SPI não foram alterados. A função registra
`SD_FREE_SCAN phase=begin/end`, duração em ms e prioridade antes/depois.
Pode aumentar o tempo de inicialização; não garante prazo máximo ou recuperação
de uma transferência SPI que nunca termine. Efeito real depende do ensaio físico.
Referências: [escalonamento SMP](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-guides/freertos-smp.html)
e [watchdog](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/system/wdts.html).

Revisão limitada de outras operações, **sem alterações nesta etapa**:

| Operação | Resultado da investigação |
|---|---|
| `openAndPreallocateDataFiles()` / `preAllocate()` | Busca de clusters contíguos pode percorrer grande parte da FAT sem pausas explícitas. Risco com fragmentação ou pouco espaço; não apareceu no backtrace fornecido. |
| `chooseSessionNumber()` | Laço de busca de pastas existentes sem pausa explícita; tempo cresce com as colisões e leituras de diretório. |
| `sync`, `truncate`, metadados/journal | Chamadas síncronas podem agrupar operações SD. O `vTaskDelay(1)` do consumidor só ocorre após elas; latências precisam de medição. |
| Teste SD independente | Ensaio máximo e verificação usam `yield()`, que não garante execução de tarefas de prioridade inferior; o ensaio cadenciado usa `delay(1)` enquanto espera. Revisar em etapa própria se ocorrer watchdog nesse ambiente. |
| Inicialização ICM/BMP | Polling auxiliar próprio tem limite de tentativas; resets Adafruit usam `delay`, cedendo CPU, mas alguns laços não têm prazo global. Não reproduzem diretamente a varredura contínua identificada aqui. |

O transporte SPI instalado também usa transferências byte a byte no caminho
identificado pelo backtrace, aumentando o custo das leituras. Otimização de SPI
e adaptação das operações listadas ficam para tarefas separadas, com medição.

Verificação: builds `esp32s3_acquisition` e `teensy40` aprovados;
`git diff --check` sem erros. No S3: 51.396 bytes de RAM estática e 346.833
bytes de flash. Sem upload; ausência de watchdog durante a varredura ainda
precisa ser confirmada na placa com `SD_FREE_SCAN phase=end` nas duas consultas.

Áudio está excluído das fontes e dependências. Tipos históricos foram unificados
em `data/audio_types.h`; S3 não instancia captura, I2S ou buffer PCM de 32 KiB,
e rejeita sessão pedindo áudio. Retomada exige etapa própria. A seção planejada
de 2026-10-07 abaixo é o histórico da decisão agora implementada em bancada.

## Preparação ESP32-S3 em 2026-10-05

O destino é a placa OEM ESP32-S3FH4R2. [HARDWARE_SPEC.md](HARDWARE_SPEC.md)
consolida SDA/SCL GPIO1/2, o SD SPI, PCA9548A mantido e alimentação
por conversor de 5 V na USB-C. ICM0/1/2 permanecem nos canais 4/5/6 e BMP0/1
nos canais 7/3, conforme a montagem mais recente confirmada pelo usuário.
Na preparação inicial, o SD usava CS=4, MOSI=5, MISO=6 e SCK=7; a pinagem
atual revisada em 2026-10-29 está descrita na seção de integração acima.

O ambiente padrão `esp32s3_supermini` compila apenas
`src/bringup/esp32s3_setup.cpp`: USB Serial/JTAG CDC, diagnóstico de flash/PSRAM
e inicialização dos barramentos. Usa SdFat 2.3.1 externo; ainda não monta SD
nem inicializa drivers dos sensores. O programa não produz pacotes v4.
Os drivers e o logger existentes da main continuam na aplicação Teensy,
selecionada explicitamente por `-e teensy40`. A fonte de preparação é excluída
desse ambiente para evitar dois pontos de entrada.

Esta separação permite compilar/validar memória e pinos antes de adaptar
`SD.sdfs` e as interfaces Teensy. O filtro do ESP32 deixa áudio fora do build.
FreeRTOS com produtor de sensores, fila limitada e consumidor SD permanece
planejado; não houve implementação de concorrência nem de filtros nesta etapa.

O snapshot em `legacy/teensy40/` não foi modificado. O registro de suspensão
de áudio em ESP32_MIGRATION descreve também mudanças feitas na antiga branch
presentation; elas não estavam integralmente na main após a troca de branch.
A documentação distingue o perfil de preparação da aplicação Teensy existente.

## Teste independente ESP32 + SD

O ambiente explícito `esp32s3_sd_test` herda plataforma, memórias, USB e
SdFat 2.3.1 do ambiente de preparação, mas compila somente
`src/diagnostics/esp32s3_sd_test.cpp`. O ambiente padrão continua sendo a
preparação sem escrita. As fontes de diagnóstico também são excluídas do
build Teensy. Não há aquisição I2C, microfone, filtros ou tarefas de sensores
no diagnóstico; a carga de 7.900 bytes/s é sintética e síncrona.

`src/config/storage_config.h` centraliza os defaults de armazenamento (buffer
8 KiB, bloco 512 bytes, SPI 12 MHz, reserva 4 MiB). `constants.h` mantém seus
nomes públicos como aliases, preservando o comportamento Teensy. A preparação
informa a frequência compartilhada; o teste compara também 4/20 MHz e
1.024/4.096 bytes. O SD usa `SdFs`/`FsFile` e `SPI` explícito com os GPIOs
confirmados. O teste usa `SHARED_SPI | USER_SPI_BEGIN`: o programa prepara
uma instância própria `SPIClass` e SdFat recebe essa mesma instância sem
chamar `begin()` novamente. Isso aplica a organização do `main` de referência
(cartão em uma instância SPI própria) aos controladores disponíveis no S3;
não copia o VSPI/CS=5 nem a aquisição SPI dos IMUs do exemplo antigo.
O modo compartilhado usa transações por operação; a matriz deve ser comparada
com outras execuções que tenham a mesma configuração.

A inicialização de referência foi baseada no `SDCard.cpp` fornecido pelo
usuário, que usa `SD.begin(cs, spi)` do Arduino-ESP32. Se SdFat falhar no boot,
o diagnóstico encerra esse driver, configura o SPI, tenta `SD.begin` a 4 MHz
(default do core 2.0.17), informa tipo/capacidade e chama `SD.end()`. Depois
reconfigura o barramento e tenta SdFat uma única vez. Essa sondagem não abre
arquivos nem habilita formatação; as bibliotecas não montam o cartão juntas.
Somente SdFat realiza os ensaios, preservando FAT/exFAT e pré-alocação.
O resultado dessa referência também envolve montagem do filesystem, portanto
uma falha em `SD.begin` não identifica sozinha uma falha de transporte SPI.

A revisão v3 compara os controladores FSPI/HSPI do S3 com os mesmos GPIOs.
Testa primeiro o controlador registrado na última execução (FSPI quando não
há registro), com montagem SdFat, referência Arduino SD e um retry SdFat.
Se falhar, encerra ambos os drivers e repete essa sequência no outro controlador.
Somente um controlador fica inicializado por vez. Caso nenhum funcione,
encerra a inicialização sem criar arquivos de ensaio. O controlador escolhido
fica nos metadados e na NVS. Se a recuperação de uma execução anterior exigir
mudar o controlador, ela é suspensa com `SD_RECOVERY_BUS_CHANGED`, mantendo
arquivos e intenção de reset; o usuário pode verificar os dados em Python ou
iniciar outra execução. Isso evita misturar configurações nas métricas da sessão.

O gerador/formato portátil fica em `sd_test_format.h`. Dados, manifesto,
checkpoints e métricas ficam em `/SDTEST/Rxxxxxx/`, com criação exclusiva e
sem sobrescrever sessões. Checkpoints são gravados somente após `data.sync()`;
o verificador confirma o prefixo durável, ignorando a cauda não confirmada em
ensaios interrompidos. NVS (`Preferences`, namespace `pbic-sdtest`) guarda a
pasta, CID do cartão e fase para recuperação após reset; arquivos no SD
continuam sendo a evidência verificável independentemente com Python.

Há dois reinícios automáticos (arquivo aberto sem sync final e arquivo fechado)
e uma etapa opcional de corte manual de energia. Reset por software não
reproduz todos os efeitos de uma queda de alimentação. A comparação de blocos
e clocks não define automaticamente a configuração final: integridade,
cadência e maiores latências precisam ser avaliadas juntas. Concorrência com
aquisição em outro núcleo permanece uma etapa futura. Roteiro em
[SD_TEST.md](SD_TEST.md), formato em [DATA_FORMAT.md](DATA_FORMAT.md).

## Distribuição planejada entre os núcleos

Registro de **2026-10-07**, baseado nos exemplos M5Stack enviados pelo usuário,
preservados e analisados em [references/m5stack/README.md](references/m5stack/README.md).
É uma proposta para a etapa de coleta, não uma mudança no firmware atual.
O funcionamento do SD será assumido para o planejamento; sua aprovação física
continua pendente no novo protótipo com outro ESP32.

| Responsabilidade | Afinidade inicial proposta | Propriedade exclusiva |
|---|---|---|
| Aquisição dos três ICM-20948 e dois BMP390, timestamps e pacotes crus | Núcleo 1 | `Wire`, seleção de canal PCA9548A e drivers dos cinco sensores |
| Gravação em blocos, pré-alocação, `sync()`, journal e manutenção | Núcleo 0 | SPI do SD, SdFat, arquivos e estado interno do logger |

Preferimos começar com coleta no núcleo 1 e armazenamento no 0, usando tarefas
explicitamente fixadas. A inversão em relação ao Capsule é uma escolha inicial
para medir, não uma exigência do hardware. Serviços do sistema continuam nos
dois núcleos; afinidade não torna um núcleo exclusivo da aplicação. Wi-Fi/BLE
continuam fora desta etapa. O `loop()` deve ceder CPU e não duplicar acessos a
sensores, SD ou filas. Prioridades e pilhas serão definidas e medidas ao portar.

A comunicação inicial recomendada é uma **fila FreeRTOS limitada de pacotes
binários completos, copiados por valor**, com armazenamento pré-alocado em
SRAM interna. O consumidor recebe a cópia e alimenta sozinho o buffer do logger.
O produtor nunca chama diretamente o `enqueue()` do logger atual a partir de
outro núcleo. Assim, ponteiros e buffers mutáveis não ficam compartilhados;
essa abordagem é mais simples de verificar do que o duplo JSON do Capsule.
Buffers por lote com transferência de posse ficam como otimização posterior,
somente se medições justificarem. PSRAM não é necessária à fila inicial.

A tarefa de coleta preservará o agendamento anti-rajada dos sensores e usará
tempo monotônico para timestamps/deadlines. NTP, formatação textual, escrita
SD e espera indefinida por espaço não entram nesse caminho. Se a fila lotar,
contabilizar a perda explicitamente e preservar a sequência de aquisição;
a política inicial proposta é rejeitar o pacote novo, mantendo os já enfileirados.
O consumidor aguarda dados com bloqueio do RTOS, sem polling contínuo.
Não executar SD/I2C em ISR nem manter mutex/seção crítica durante I/O de SD.

O núcleo extra permite que a coleta continue durante uma chamada SD longa,
até o limite da fila. Não elimina pausas do cartão ou contenção de recursos
compartilhados. Dimensionar a memória pelo pior atraso observado, pela vazão
de 7.900 bytes/s e por margem, considerando também o buffer privado do logger.
Validar jitter/deadlines perdidos, sequência/CRC, erros I2C/SD, fila máxima,
perdas, maior latência de `write`/`sync`, vazão sustentada e margem das pilhas,
inclusive com consumidor atrasado ou SD em falha. O teste SD atual é síncrono
e não valida essa concorrência. Áudio/filtros permanecem suspensos; a sessão
continua sem botão/comando de encerramento normal nesta etapa.

## Aplicação Teensy ainda não portada

As seções abaixo documentam os serviços existentes e os experimentos Teensy.
Não são o comportamento do ambiente de preparação ESP32. O mapa de canais foi
alinhado à montagem confirmada; a migração dos serviços será feita em etapa própria.

## Hardware
Microcontrolador:
Teensy 4.0

Componentes conectados:
- I2C: Um único barramento I2C conecta a Teensy ao multiplexador.

Multiplexador: PCA9548A

Sensores conectados ao PCA9548A:
- Canal 4: ICM20948 #0

- Canal 5: ICM20948 #1

- Canal 7: BMP390 #0

- Canal 3: BMP390 #1

- Canal 6: ICM20948 #2


Todos utilizam 3,3 V.

SPI: Um único módulo microSD.

I2S: Um microfone digital ICS43434

## Software
Mudança importante em relação ao projeto antigo

O firmware antigo da Nicla utilizava:

NDP.begin()
NDP.load()
Nicla_System
BMI270 interno
BMM150 interno

Nada disso deverá existir no novo firmware.

A migração seguirá o ESP32-S3; a arquitetura Teensy abaixo é referência dos serviços a portar.

- Organização do código 

PBIC/
│
├── src/
│   ├── main.cpp
│   │
│   ├── drivers/
│   │   ├── pca9548a.cpp
│   │   ├── pca9548a.h
│   │   ├── icm20948.cpp
│   │   ├── icm20948.h
│   │   ├── bmp390.cpp
│   │   ├── bmp390.h
│   │   ├── microphone.cpp
│   │   └── microphone.h
│   │
│   ├── services/
│   │   ├── imu_acquisition.cpp
│   │   ├── imu_acquisition.h
│   │   ├── sd_logger.cpp
│   │   └── sd_logger.h
│   │
│   ├── utils/
│   │   ├── crc.cpp
│   │   ├── crc.h
│   │   ├── packet.cpp
│   │   └── packet.h
│   │
│   └── config/
│       ├── pins.h
│       └── constants.h
│
├── python/
│   └── parse_data.py
│
├── docs/
│   ├── ARCHITECTURE.md
│   ├── HARDWARE_SPEC.md
│   └── FIRMWARE_GUIDELINES.md
│
└── README.md

- Estrutura do cartão SD
/session.txt

/S001/
├── imu.bin
├── meta.txt
├── journal.bin
└── status.txt

session.txt: Fica na raiz do cartão e guarda o número da última sessão
imu.bin: Contém os pacotes binários das IMUs e barômetros.
meta.txt: Descreve a sessão e a configuração.
journal.bin: Guarda checkpoints binarios append-only com CRC.
status.txt: Guarda contadores e latências do firmware.

"Exemplo:

session=1
firmware_version=0.1.0
board=Teensy 4.0

imu_sample_rate_hz=100
bmp_sample_rate_hz=25

audio_enabled=true
audio_sample_rate_hz=16000
audio_channels=1
audio_bits_per_sample=16

imu_packet_size=0
imu_magic=0xAA55
crc=CRC-8 polynomial 0x07

icm0_channel=0
icm1_channel=1
bmp0_channel=2
bmp1_channel=3
icm2_channel=4

status.txt: Registra se os componentes inicializaram corretamente e se a gravação funcionou 

## Gravação no cartão SD

O serviço `src/services/sd_logger` usa a biblioteca `SD` fornecida pelo core da
Teensy, que por sua vez utiliza SdFat. Nenhuma dependência externa foi
adicionada ao `platformio.ini`. O cartão é inicializado antes do barramento I2C
com `CS=10` e passa por um teste curto de escrita e leitura.

O firmware não espera uma conexão USB durante o boot. O stream binário USB é
controlado por `kUsbBinaryStreamEnabled` e fica desabilitado por padrão na fase
de gravação autônoma, mantendo o monitor serial legível. Ele pode ser reativado
em builds de bancada. Se o SD estiver ausente ou falhar, o diagnóstico textual
continua disponível, mas não há persistência dos pacotes enquanto o stream
binário permanecer desabilitado. Com SD válido, o contador persistente
`/session.txt` escolhe uma pasta nova `/Sxxx`, e `imu.bin` permanece aberto
durante a sessão.

Os pacotes v4 de 79 bytes entram em uma fila circular de 8192 bytes. O logger
escreve somente `imu.bin`, em blocos completos de 512 bytes, e executa no
maximo uma operacao de SD por passagem do loop. O SPI opera a 12 MHz. O
microfone esta desabilitado na configuracao normal: I2S nao e inicializado,
`audio.raw` nao e criado e a arbitragem entre IMU e audio permanece apenas no
codigo experimental.

`main` consulta a aquisicao imediatamente antes de chamar o logger. Assim, uma
rodada ja vencida e executada antes de qualquer nova operacao SD. Uma chamada
sincrona ja iniciada no SdFat nao pode ser interrompida, portanto as latencias
maximas do cartao continuam sendo medidas.

Flush nao drena a fila. `imu.bin` recebe `sync()` a cada 1000 pacotes, cerca de
dez segundos, somente quando ha no maximo 1024 bytes aguardando. O journal e
atualizado logo depois e publica apenas os bytes confirmados pelo ultimo
`sync()` bem-sucedido. `journal.bin` permanece aberto e recebe registros fixos
de 32 bytes por append, sem criar, remover ou renomear arquivos durante a
aquisicao. Um CRC-8 permite ao Python ignorar um ultimo registro incompleto ou
corrompido. `status.txt` registra as escolhas do caminho IMU e as operacoes de
manutencao.

A recuperação é medida separadamente por arquivo. Uma escrita sem nenhum byte
de progresso inicia um período de dois segundos; qualquer escrita posterior
com progresso cancela esse período. Somente dois segundos completos sem
progresso confirmam a falha. Isso evita que uma única falha após uma operação
longa seja interpretada como uma janela inteira de cartão indisponível.

Uma falha definitiva emite `SD_ERROR_CONFIRMED` uma única vez pela Serial e
desativa a gravação até o próximo reboot. Cinco segundos depois, o firmware
mantém `CS` alto, encerra o SPI e passa a usar o LED laranja integrado para dois
pulsos curtos repetidos. O LED compartilha o pino 13 com `SCK` e nunca é
controlado enquanto o SPI do SD estiver ativo. Não há tentativa de reinserção
automática do cartão.

O logger mede separadamente a maior duração de escrita, flush e atualização de
status, além de contar operações de cada tipo com duração igual ou superior a
10 ms. Esses valores permitem localizar a origem dos deadlines perdidos sem
alterar o pacote dos sensores. A remoção física ainda precisa ser validada no
hardware real.

Durante o setup, cada BMP390 fornece os 21 bytes dos registradores NVM `0x31` a
`0x45`. Eles são gravados em hexadecimal no `meta.txt`; o Python faz a
compensação Bosch, sem uso de `float` no caminho de aquisição do firmware.

## Aquisição inicial dos ICM-20948

O firmware usa `Adafruit ICM20X 2.0.7` para os breakouts ICM-20948 e um driver
próprio baseado em `Wire` para o PCA9548A. Os canais ICM atuais são `4`, `5` e `6`.

Configuração inicial de bancada:

- acelerômetro: faixa de `+/-8 g`, DLPF em bypass, taxa interna nominal 4500 Hz;
- giroscópio: faixa de `+/-2000 graus/s`, DLPF em bypass, taxa interna nominal 9000 Hz;
- filtros DLPF confirmados por readback no boot; sem filtro de software;
- AK09916 a 20 Hz, proxy consultado a 25 Hz com cache;
- rodada de leitura dos três ICMs a 100 Hz, agendada com `micros()`;
- ESP32 grava pacotes v4 de 79 bytes no SD; saída USB binária pertence ao legado.

A biblioteca Adafruit inicializa o AK09916 durante `begin_I2C()`. O driver o
mantém em modo contínuo a 20 Hz. As faixas foram ampliadas para `+/-8 g` e
`+/-2000 graus/s` depois que movimentos fortes saturaram as configurações de
bancada anteriores. A nova configuração ainda deve ser validada no uso real.

Depois de selecionar um canal, o driver aguarda 80 microssegundos antes de
acessar o sensor. Esse tempo veio da experiência com o hardware legado e ainda
precisa ser validado na montagem Teensy 4.0 + breakouts Adafruit.

O driver lê diretamente o bloco de 12 bytes de acelerômetro e giroscópio a
partir do registrador `0x2D`, em big-endian. A biblioteca Adafruit continua
responsável pela inicialização e configuração do ICM. A leitura direta permite
validar os retornos de `Wire`, transmitir os valores `int16` sem conversão no
microcontrolador e contabilizar falhas I2C por sensor. Na configuração atual,
o driver próprio também escreve e verifica os registradores de DLPF/faixa;
a leitura auxiliar rejeita NACK do SLV4 e perda de arbitragem.

O serviço `src/services/imu_acquisition` contém o agendador anti-rajada,
sequência e contadores. `src/data/imu_packet.h` define o pacote, enquanto
`src/utils/packet` e `src/utils/crc8` fazem sua montagem e validação. O contrato
com o Python está documentado em `docs/DATA_FORMAT.md`.

`src/utils/time_utils.h` centraliza a aritmetica modular de `micros()` e inclui
`static_assert` cobrindo deadlines antes e depois do retorno de `uint32` a
zero. As ferramentas Python acumulam deltas consecutivos para representar
sessoes que atravessam mais de um rollover de aproximadamente 71,6 minutos.

## Diagnóstico inicial dos AK09916

A inicialização oficial da `Adafruit ICM20X 2.0.7` configura o controlador I2C
auxiliar de cada ICM-20948, valida o AK09916 pelo registrador `WIA2=0x09` e
mantém um proxy de nove bytes de `ST1` a `ST2`. O driver confirma novamente o
`WIA2` por uma transação de um byte via `I2C_SLV4`, configura o magnetômetro a
20 Hz e lê no boot os três eixos crus little-endian, `ST1` e `ST2`.

O serviço de aquisição consulta o proxy de cada magnetômetro a 25 Hz, em fases
diferentes, mantendo os últimos eixos crus válidos









em cache. Como a leitura
automática do proxy inclui `ST2` e pode limpar `DRDY` antes da consulta da
Teensy, uma amostra é considerada nova quando `DRDY` foi capturado ou quando o
trio cru mudou em relação ao cache. Leituras com overflow são rejeitadas.

Existem contadores separados por magnetômetro para falha I2C, atualização
aceita, ausência de novidade, overrun (`DOR`) e overflow (`HOFL`). Os nove
valores crus entram no pacote v4. As consultas dos três
AK09916 são distribuídas em fases diferentes das rodadas de 100 Hz, enquanto o
último valor válido de cada sensor é repetido a partir do cache.

## Aquisição inicial dos BMP390

Os dois BMP390 permanecem definidos nos canais 2 e 3 do PCA9548A. Durante o
`setup`, o firmware testa os endereços `0x77` e `0x76` e valida o `CHIP_ID`
esperado `0x60` por meio da biblioteca `Adafruit BMP3XX 2.1.6`.

Os canais 2 e 3, ambos no endereço `0x77`, foram confirmados na montagem física.
Após o diagnóstico, o driver coloca os BMP390 em modo normal contínuo a 25 Hz.
O serviço de aquisição lê diretamente os seis bytes crus de pressão e
temperatura, em little-endian, a cada quatro rodadas das IMUs, com uma fase por
BMP, e mantém o último
valor válido de cada sensor em cache. Falhas BMP possuem contadores próprios e
não descartam a rodada das IMUs.

A API pública `Adafruit BMP3XX 2.1.6` fornece somente valores compensados e não
expõe as contagens Bosch cruas. Por isso, a biblioteca permanece responsável
pela inicialização e validação, enquanto a aquisição contínua usa os
registradores oficiais `PWR_CTRL` (`0x1B`), `OSR` (`0x1C`), `ODR` (`0x1D`) e o
bloco de dados `0x04` a `0x09`. Não há `float` nesse caminho de aquisição.

Os quatro valores crus em cache participam do pacote v4 como `uint32`, na ordem
pressão/temperatura do canal 2 e pressão/temperatura do canal 3. O pacote possui
79 bytes e o parser gera um gráfico separado com contagens cruas. A compensação
em Pa e graus Celsius depende dos coeficientes NVM individuais.
A etapa do cartão SD registrará esses coeficientes em `meta.txt`.

## Ferramentas Python para sessões longas

O parser de arquivo trabalha em blocos de 64 KiB, preservando busca de magic,
CRC e ressincronização entre blocos. O analisador calcula timing e diagnósticos
sobre toda a sessão, mas limita os pontos mantidos para gráficos. A ferramenta
`python/calibrate_magnetometer.py` produz uma estimativa inicial de hard-iron e
soft-iron diagonal somente quando a captura cobre rotação suficiente nos três
eixos.

## Diagnostico inicial do ICS43434

O diagnostico usa `AudioInputI2S` da Audio Library 1.3 fornecida pelo core
Teensyduino 1.62, sem dependencia externa no `platformio.ini`. A implementacao
oficial para Teensy 4.x fixa `BCLK=21`, `LRCLK=20` e `RX=8`; o breakout mantem
`SEL=GND`, portanto o sinal deve aparecer na porta esquerda. Fontes primarias:

- https://www.pjrc.com/teensy/gui/index.html?info=AudioInputI2S
- https://github.com/PaulStoffregen/Audio
- https://cdn-shop.adafruit.com/product-files/6049/6049_DS-000069-ICS-43434-v1.2.pdf

A biblioteca trabalha a 44100 Hz, em blocos de 128 amostras `int16`. O SAI
recebe slots I2S de 32 bits, mas o DMA oficial copia apenas os 16 bits mais
significativos de cada canal. Assim, os oito bits menos significativos da
amostra nativa de 24 bits do ICS43434 nao ficam disponiveis nesse caminho.

O modo `kMicrophoneDiagnosticEnabled=true` e isolado: nao inicializa SD, I2C ou
os demais sensores e nao cria arquivos. Um destino `AudioStream` proprio copia
os canais esquerdo e direito para uma fila circular em RAM com 16 blocos e
conta explicitamente overflow e blocos incompletos. O loop calcula taxa real,
DC, RMS AC, faixa, clipping, bits ocupados no container PCM16, atividade do LSB,
uso de memoria e CPU. A escolha entre armazenar PCM16 ou implementar um caminho
DMA de 24/32 bits sera feita depois dos resultados do hardware.

O diagnostico em hardware confirmou cerca de 44100 amostras/s, sinal somente
no canal esquerdo, DC proximo de zero e nenhuma perda da fila em 40 segundos.
Sons usuais ocuparam ate 14 bits do PCM16; por isso o formato inicial de coleta
e PCM mono `int16` little-endian. Os 24 bits nativos continuam sendo uma opcao
futura, mas nao justificam neste momento substituir o DMA oficial e estavel.

## Captura e gravacao de audio experimental

Esta implementacao esta preservada para ensaios futuros, mas nao participa do
firmware normal IMU-only. Com `kMicrophoneRecordingEnabled=false`, o I2S nao e
inicializado e nenhum arquivo de audio e aberto ou prealocado em `/Sxxx`.

`src/services/audio_capture` usa `AudioInputI2S`, cujo recebimento no Teensy
4.0 e feito por DMA, e conecta apenas a porta esquerda a um `AudioStream`
proprio. O callback copia blocos de 128 amostras para uma fila circular mono e
conta blocos recebidos, overflow, blocos incompletos e maior ocupacao. O
processamento nao usa `float` nem grava no SD dentro da interrupcao.

O loop transfere os blocos para uma segunda fila de 32768 bytes pertencente ao
`sd_logger`. A fila de captura tem 511 posicoes uteis, aproximadamente 132 KiB;
juntas oferecem aproximadamente 1,85 s de reserva a 44100 Hz. O logger mantem
`imu.bin` e `audio.raw` abertos ao mesmo tempo e escreve o audio em blocos de
ate 512 bytes. Flush, escrita e falha do audio possuem contadores e latencias
separados em `status.txt`. A janela de saude acompanha sucesso de cada arquivo
separadamente, evitando que `imu.bin` mascare uma falha de `audio.raw` ou o
inverso.

Antes de abrir a sessao, a captura mede dois segundos de sinal em RAM com o
ambiente em silencio. Media DC, RMS, pico e clipping sao registrados em
`meta.txt`. Valores acima dos limites de silencio emitem
`AUDIO_PREFLIGHT_WARNING`, mas nao bloqueiam a captura: eles podem refletir
fala ou ruido ambiental no boot e o projeto precisa preservar o sinal cru.
Somente ausencia de blocos validos emite `AUDIO_CAPTURE_REJECTED`. O firmware
nao tenta corrigir saturacao com ganho ou filtro digital.

Cada bloco de audio recebe um timestamp apenas na fila RAM. O valor nao altera
o PCM em `audio.raw`, mas permite registrar no journal o primeiro bloco exato
de cada sessao rotacionada. `meta.txt` registra formato, taxa e configuracao;
`journal.txt` e a referencia mais atual para timestamp e tamanhos validos.

No firmware normal, `FsFile::preAllocate()` reserva inicialmente 4.747.900
bytes para `imu.bin`, equivalentes a dez minutos mais um segundo de margem. A
sessao nao termina ao atingir essa reserva: o mesmo arquivo cresce e permanece
aberto ate reboot ou falha confirmada do cartao. Depois da validacao, a reserva
planejada de quatro horas sera 113.767.900 bytes; essa mudanca exige apenas
alterar `kSdPreallocationSeconds`, sem criar rotacao automatica.

Cada callback DMA atribui uma sequencia ao bloco, inclusive quando nao ha bloco
valido disponivel. Se a fila de captura transbordar ou um callback ficar
incompleto, o logger detecta o salto e insere a quantidade equivalente de
blocos zerados antes do proximo bloco real. O PCM preserva duracao e
alinhamento, mas o silencio sintetico nao recupera o sinal perdido. Os totais,
eventos e maior gap ficam em `journal.txt` e `status.txt` e sao apresentados por
`python/export_audio.py`.

No primeiro boot, a prealocacao ocorre antes de ativar o I2S. Assim, o
preflight mede o microfone depois do maior pico inicial de atividade do SD.
Durante uma rotacao, o primeiro bloco real retirado da fila permanece pendente
ate que todo silencio necessario caiba no buffer do SD. O inicio logico da nova
sessao e o instante em que a anterior atingiu seu limite, nao o final da
prealocacao. Flush, truncamento, journal e status finais avancam como etapas
separadas, uma por passagem do loop. A prealocacao SdFat ainda e uma chamada
sincrona; a fila DMA cobre essa pausa e qualquer excesso e representado por
silencio, sem encurtar a linha do tempo do audio.

O journal e escrito apos o primeiro flush, por volta de dez segundos, e depois
a cada 3000 pacotes, aproximadamente 30 segundos. Uma sessao interrompida pode
manter a cauda fisica prealocada, mas `parse_data.py`, `analyze_imu.py` e
`export_audio.py` limitam a leitura aos tamanhos confirmados.

Capturas reais mostraram dois estados distintos do sinal. S010 ficou em cerca
de `-51,7 dBFS` RMS, sem ruido aparente; S013 iniciou e permaneceu saturado,
com `-13,3 dBFS` RMS e picos em escala completa. Como a saturacao ja existia
nos primeiros segundos, ela nao foi causada pela falha posterior do SD. O
firmware nao aplica ganho digital; alimentacao, GND, DOUT e sincronismo de boot
do breakout ainda precisam ser validados antes de aceitar uma sessao de audio.

O S017 confirmou uma segunda causa independente: o volume terminou com apenas
1024 bytes livres. O logger mede os clusters no boot e reserva 4 MiB. Antes de
cada rotacao, verifica se ha espaco para a proxima prealocacao completa; falta
de espaco e reportada antes de iniciar novos fluxos.

No teste S007, o preflight anterior a prealocacao estava limpo, mas a gravacao
degradou de aproximadamente `-65 dBFS` no primeiro meio segundo para ruido com
DC e clipping durante a atividade do SD. A sessao tambem descartou 18764
pacotes IMU porque ambas as filas ficaram cheias. A versao 0.4.1 altera a ordem
de boot e a prioridade das filas; se o novo preflight ou a gravacao continuarem
ruidosos, a causa restante e eletrica entre SD e I2S e exige validacao de
alimentacao, GND e roteamento dos fios.

## Diagnostico isolado de audio no SD

O servico `src/services/audio_sd_diagnostic` existe para separar o caminho
ICS43434/DMA/SD da aquisicao I2C. Com `kAudioSdDiagnosticEnabled=true`, `main`
retorna antes de inicializar `Wire`, PCA9548A, ICM-20948 e BMP390. O teste cria
uma pasta `/Mxxx`, prealoca uma unica janela de dez minutos e nao rotaciona
arquivos. Assim, nenhuma prealocacao ocorre durante a captura.

O caminho isolado preserva PCM16 mono a 44100 Hz, sequencia DMA e zero-fill de
gaps. A mesma captura continua por duas fases de cinco minutos: 256 bytes por
escrita na primeira e 512 bytes na segunda. Nao ha flush, reinicio de I2S ou
prealocacao na transicao. Faz `sync()` a cada dez segundos e atualiza o journal
a cada trinta segundos. No limite de dez minutos, desliga a origem I2S, drena a
fila, sincroniza, trunca e grava o status final. Esse modo e temporario e
mutuamente exclusivo com o diagnostico I2S somente em RAM.

Cada fase possui contadores proprios de escritas, falhas, gaps, silencio
inserido, ocupacao do buffer e histograma de latencia nas faixas `<1`, `1-2`,
`2-5`, `5-10`, `10-20`, `20-50`, `50-100` e `>=100 ms`. O script
`python/analyze_sd_blocks.py` compara as fases, analisa RMS, picos e ocorrencias
proximas de `+/-16384` e `+/-32768`, e indica provisoriamente o menor bloco sem
falhas, gaps, escritas de pelo menos 100 ms ou assinatura PCM suspeita. O
resultado em hardware decide o bloco do logger integrado. O M002 rejeitou 1024
e 2048 bytes por corrupcao PCM, apesar de zero falhas e gaps no transporte.
O M004 validou audio inteligivel com 256 e 512 bytes. A fase de 512 bytes
reduziu pela metade as chamadas ao SD e teve menor latencia maxima; por isso o
logger integrado permanece em 512 bytes. Dois gaps isolados de um bloco nessa
fase continuam sendo tratados como contingencia.

S012 demonstrou que a continuidade de audio foi preservada por zero-fill:
300,008 s, 76 blocos perdidos em 50 eventos e maior gap de quatro blocos. No
entanto, a politica integrada priorizou audio em 51567 escritas e descartou
23551 pacotes IMU. Portanto, essa perda IMU ocorreu por starvation no
agendador integrado, nao por falta de espaco ou apenas pela prealocacao da
sessao seguinte. O teste `/Mxxx` deve determinar separadamente se ainda existem
perdas ou ruido sem qualquer transacao I2C.
