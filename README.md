# Pet Analytics Firmware

## Preparação ESP32-S3 — etapa atual

Placa OEM ESP32-S3 Super Mini/ESP32-S3FH4R2: flash 4 MB, PSRAM Quad SPI 2 MB,
USB nativa e PCA9548A mantido. Alimentação confirmada: bateria externa com
conversor de 5 V conectado à USB-C. I2C GPIO1/2; SD SPI: CS GPIO4, MOSI GPIO5, MISO GPIO6, SCK GPIO7.
Mapa confirmado: ICM0/1/2 nos canais 4/5/6 e BMP0/1 nos canais 7/3.
Detalhes elétricos em [HARDWARE_SPEC.md](docs/HARDWARE_SPEC.md).

O ambiente padrão `esp32s3_supermini` usa `espressif32@7.1.3` e
`greiman/SdFat@2.3.1`. Ele compila somente o programa de preparação em
`src/bringup/esp32s3_setup.cpp`, com USB CDC, configuração QIO/Quad SPI e
partições para 4 MB. Inicializa os pinos I2C/SPI e informa memória detectada.
**Esse programa ainda não coleta sensores nem monta/grava o cartão SD.**
Áudio, Wi-Fi/BLE e LED RGB não são ativados nessa etapa.

Instalar dependências e compilar na raiz:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" pkg install -e esp32s3_supermini
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32s3_supermini
```

Para testar na placa, depois de conferir alimentação/módulo SD, substituir COMx
pela porta real (o primeiro upload pode exigir BOOT pressionado durante RESET):

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32s3_supermini --target upload --upload-port COMx
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" device monitor --port COMx --baud 115200
```

Esperado: `PBIC_STAGE=esp32s3_preparation`, `flash_bytes=4194304`,
`psram_bytes=2097152`, `memory_config_ok=1`, `I2C_INIT ok=1` e `mounted=0`.
Isso verifica configuração/memória e inicialização do controlador I2C, sem
comprovar presença dos sensores ou comunicação com o cartão.

O ambiente `teensy40` continua disponível explicitamente (`pio run -e teensy40`)
para o firmware Teensy da main; a cópia completa da última etapa de apresentação
está em [legacy/teensy40/SNAPSHOT.md](legacy/teensy40/SNAPSHOT.md).
Decisões e próximos passos: [ESP32_MIGRATION.md](docs/ESP32_MIGRATION.md).

## Teste independente ESP32 + PCA9548A

Ambiente `esp32s3_pca_test`: consulta `0x70`–`0x77` a 100/400 kHz e registra
retornos I2C da consulta e da escrita `0x00` nos endereços que responderem.
Inicia automaticamente e repete a cada 5 s, sem SD ou coleta de sensores.
Comandos e interpretação: [docs/PCA_TEST.md](docs/PCA_TEST.md).

## Teste independente ESP32 + SD

O ambiente `esp32s3_sd_test` usa SdFat 2.3.1 e a montagem confirmada
(módulo SD alimentado em **3,3 V**, SPI: CS GPIO4, MOSI GPIO5, MISO GPIO6, SCK GPIO7). Compara blocos e clocks,
verifica padrões/CRC e recupera checkpoints após reinício, sem áudio ou
aquisição de sensores. Começa a gravar apenas com comando serial `s`.

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32s3_sd_test
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32s3_sd_test --target upload --upload-port COMx
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" device monitor --port COMx --baud 115200
```

Roteiro, comandos, resultados e verificação Python: [docs/SD_TEST.md](docs/SD_TEST.md).
O ambiente padrão continua sendo a preparação sem escrita; dados e legado
existentes são preservados. Resultados de desempenho exigem teste físico.

## Coleta ESP32-S3 em dois núcleos — integração inicial

Ambiente explícito `esp32s3_acquisition`, adicionado em 2026-10-10.
**`pio run` sem `-e` continua compilando preparação, não coleta.** Três ICMs
e dois BMPs são lidos no núcleo 1; fila de 256 pacotes alimenta SdFat no núcleo
0. Grava contagens cruas v4/79 bytes e NVM em `/Sxxx/`, sem áudio, filtros,
Wi-Fi/BLE ou stream binário contínuo. Formato/journal compatíveis com Python.

A nova montagem passou na recuperação de prefixo, mas a suíte SD abortou
por timeout. Integração destinada à **validação de bancada**, sem aprovação
de cadência ou armazenamento longo; diagnóstico em [SD_TEST.md](docs/SD_TEST.md).

Com monitor fechado, selecionar explicitamente (ajustar COM11 se mudar):

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32s3_acquisition
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32s3_acquisition --target upload --upload-port COM11
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" device monitor --port COM11 --baud 115200
```

A coleta inicia automaticamente, sem `s`. Esperado: PBIC_STAGE=esp32s3_acquisition,
três ICM_INIT ok=1, dois BMP_INIT raw25hz=1 nvm_valid=1 e ACQUISITION_START
recording=1 com pasta nova. Sensor/NVM falho desabilita coleta; SD falho permite
somente diagnóstico, recording=0, sem persistir dados novos. ACQUISITION_STATUS
e I2C_FAILURES aparecem a cada 5 s mesmo abrindo monitor após o boot.

Desde 2026-10-13, cada relatório também repete `SENSOR_INIT_SUMMARY` com
`source=boot_snapshot`, `I2C_INIT`, `PCA_INIT` e os resultados `ICM_INIT`/`BMP_INIT`.
São resultados guardados no boot, não novas tentativas. Se I2C/PCA falhar,
`SENSOR_INIT_FAILED` identifica o dispositivo e `sensors_skipped=1` indica
sensores não testados. Com PCA válido, conferir ICM `ok/config_verified`, BMP
`ok/raw25hz/nvm_valid` e PCA `disable_all`. Enviar um bloco completo desse
resumo junto de `ACQUISITION_STATUS` para localizar a etapa não aprovada.

Contagem de espaço livre (correção de 2026-10-13): `SD_FREE_SCAN phase=begin/end`
identifica as consultas do boot e de preparação da sessão. Durante cada consulta,
a tarefa SD compartilha a prioridade 0 com IDLE0; ao terminar restaura sua
prioridade anterior (normalmente 2). A varredura pode levar vários segundos.
No teste, conferir conclusão das duas consultas, `SD_CAPACITY`,
`ACQUISITION_START recording=1` e status periódico sem reset por watchdog.

Começar com sensores imóveis, conferir produção próxima de 100 pacotes/s,
I2C, perdas, fila e latências; depois movimento e sessão longa. Preservar log
com reset=<motivo> se desconectar. Desligamento pode perder a cauda até o último
journal. Copiar a pasta completa (incluindo meta.txt/journal.bin) antes de analisar,
por exemplo para data/S001 (ignorada pelo Git):

```powershell
& ./.venv/Scripts/python.exe -B python/parse_data.py data/S001/imu.bin
```

Conferir CRC, lacunas e taxa efetiva com contadores seriais. Fila, pilhas,
desligamentos e crescimento após dez minutos precisam de ensaio físico;
arquivos acima de 4 GiB dependem de exFAT e não estão validados. Não apagar
sessões para repetir testes. [Arquitetura](docs/ARCHITECTURE.md).

## Avaliação dos sensores

Avaliação dos [ICM](python/avaliacao/ICM.md) implementada em
`python/avaliacao/avaliar_icm.py`: valida o prefixo confirmado da captura SD,
relata acelerômetros/giroscópios/magnetômetros e compara normas dos três ICMs.
Executa no computador, sem filtros ou alteração dos dados crus:

```powershell
& ".\.venv\Scripts\python.exe" -B python/avaliacao/avaliar_icm.py data/S002
```

Há seleção de janela e declaração explícita de repouso; veja as instruções ICM
para interpretar resultados e limites. A avaliação [BMP](python/avaliacao/BMP.md)
também está implementada: dados crus, NVM individual, pressão/temperatura,
comparação, tendências e altura relativa com referência fixa explícita:

```powershell
& ".\.venv\Scripts\python.exe" -B python/avaliacao/avaliar_bmp.py data/S001
& ".\.venv\Scripts\python.exe" -B python/avaliacao/avaliar_bmp.py data/S001 --start-s 240 --end-s 250 --reference-s 230 235
```

Sem referência, altura fica indisponível. Ambos os relatórios avaliam somente
o prefixo confirmado e preservam os arquivos originais. Passar na integridade
não comprova que o ensaio inteiro foi gravado nem certifica exatidão física.

## Aplicação Teensy da main — referência durante a migração

As instruções abaixo descrevem a aplicação Teensy ainda não portada. Para
compilá-la/carregá-la, selecionar explicitamente `-e teensy40`; `pio run`
sem ambiente agora seleciona a preparação ESP32. As ferramentas de aquisição
Python não recebem pacotes do programa de preparação.

## Visão Geral

Este repositório contém o firmware do wearable **Pet Analytics**
desenvolvido para pesquisa PBIC. O foco desta etapa é coleta robusta de
dados, gravação em cartão SD e posterior processamento em Python/IA. Não
há inferência embarcada nesta fase.
Objetivo

Este projeto é um wearable para cães destinado à coleta de dados para pesquisa.
Nesta etapa não existe classificação em tempo real. O objetivo é coletar dados
sincronizados dos sensores em Arduino/C++ na Teensy 4.0 e interpretá-los em
Python. O firmware grava de forma autônoma no cartão SD. O stream binário USB
é opcional para diagnóstico de bancada e fica desabilitado por padrão durante
a etapa de gravação no SD.
## Hardware

-   Teensy 4.0
-   PCA9548A
-   3×ICM-20948
-   2×BMP390
-   ICS43434
-   microSD
-   LiPo 3.7V

## Estado atual

Os três ICM-20948 nos canais 4, 5 e 6 do PCA9548A são lidos a 100 Hz. O firmware
transmite pela USB pacotes binários de 79 bytes com timestamp, sequência, 27
valores crus `int16` dos ICMs, quatro valores `uint32` crus dos BMP390 e CRC-8.
O mesmo fluxo é gravado em `/Sxxx/imu.bin`; `meta.txt` preserva a configuração
e os 21 bytes NVM individuais de cada BMP390, enquanto `status.txt` registra
contadores da aquisição e do SD.
O formato completo está em
`docs/DATA_FORMAT.md`.

O microfone esta desativado na configuracao normal: I2S nao e inicializado e
nenhum `audio.raw` e criado. O codigo de diagnostico do ICS43434 permanece no
repositorio para retomada futura, sem participar da aquisicao atual.

## Sessões no cartão SD

Com um cartão FAT ou exFAT conectado nos pinos documentados, o firmware cria
uma pasta `/Sxxx` no boot e mantem o mesmo `imu.bin` aberto continuamente ate
o reboot ou uma falha confirmada do SD. Nao existe encerramento por tempo nem
rotacao para outra pasta. O firmware nao espera a USB e funciona sem
computador.

Se um arquivo do SD ficar dois segundos completos sem qualquer progresso de
escrita, o firmware emite
`SD_ERROR_CONFIRMED`, desativa a gravação até o reboot e, após cinco segundos,
pisca duas vezes o LED laranja integrado. O LED compartilha o pino do clock SPI
e só é controlado depois que o SPI foi encerrado com segurança.

Para reduzir a carga e melhorar a margem elétrica, o SD opera a 12 MHz.
`imu.bin` usa escritas completas de 512 bytes. A aquisicao dos sensores e
consultada antes de qualquer operacao do SD. O arquivo recebe no boot uma
prealocacao de dez minutos e cresce normalmente ao ultrapassar essa reserva.
Flush e journal ocorrem a cada 1000 pacotes, aproximadamente dez segundos;
`journal.bin` recebe um registro binario append-only com CRC e guarda o prefixo
confirmado por `sync()`. Em queda de energia,
o Python ignora a cauda prealocada e os dados posteriores ao ultimo journal.

Depois de desligar a Teensy e remover o cartão, analise a sessão diretamente:
ajuste a letra da unidade caso o Windows monte o cartão em outro caminho.

```powershell
python python/parse_data.py E:/S001/imu.bin
python python/analyze_imu.py E:/S001/imu.bin --no-show
```

Quando `meta.txt` acompanha `imu.bin`, o analisador aplica automaticamente a
compensação Bosch e gera o gráfico BMP em Pa e graus Celsius.

Para uma captura USB de bancada, altere temporariamente
`kUsbBinaryStreamEnabled` para `true` em `src/config/constants.h`, recompile e
use `python/capture_serial.py`. Com o valor padrão `false`, o monitor serial
permanece textual e a aquisição continua sendo gravada somente no SD.

## Captura de bancada

Instale a única dependência Python desta etapa:

```powershell
python -m pip install -r python/requirements.txt
```

Com a Teensy programada e conectada, a porta é detectada automaticamente e uma
janela exata de 10 segundos do relógio do sensor é salva em `data/`:

```powershell
python python/capture_serial.py
```

Também é possível informar a porta e o arquivo:

```powershell
python python/capture_serial.py --port COM3 --output data/teste_icm.bin
python python/parse_data.py data/teste_icm.bin
```

Para validar CRC, sequencia, frequencia, periodo, jitter e perdas, e gerar o
graficos separados para acelerometro/giroscopio, magnetometros e BMP390 crus:

```powershell
python python/analyze_imu.py data/teste_icm.bin
```

O grafico e salvo ao lado da captura como `.png` e tambem e aberto na tela. Em
ambientes sem interface grafica, use `--no-show`; `--output` permite escolher o
caminho da imagem.

Para estimar uma calibracao magnetica inicial depois de uma rotacao 3D ampla:

```powershell
python python/calibrate_magnetometer.py data/rotacao_3d.bin
```

## Diagnostico do microfone

### Teste isolado microfone + SD

`kAudioSdDiagnosticEnabled=true` ativa temporariamente um teste de dez
minutos que inicializa somente o ICS43434 e o cartao SD. Nesse modo, o firmware
nao inicializa `Wire`, PCA9548A, ICM-20948 ou BMP390 e nao cria pastas `/Sxxx`.
Ele cria uma unica pasta `/Mxxx`, prealoca `audio.raw` antes de iniciar o I2S e
nao faz rotacao automatica. Os primeiros cinco minutos usam escritas de 256
bytes e os cinco seguintes usam 512 bytes, sem interrupcao ou nova
prealocacao entre as fases. Ao fim, desliga a captura, drena o buffer, trunca
o arquivo e emite `MIC_SD_TEST_COMPLETED`.

Carregue e monitore:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run --target upload --target monitor --upload-port COM3
```

Depois de `MIC_SD_TEST_COMPLETED`, desligue a Teensy, remova o cartao e use a
letra atribuida pelo Windows:

```powershell
Get-Content E:/M001/status.txt
Get-Content E:/M001/journal.txt
python python/analyze_sd_blocks.py E:/M001
python python/export_audio.py E:/M001
ffplay data/M001_audio.wav
```

O resultado esperado e aproximadamente 600 segundos, zero falhas de escrita e,
idealmente, zero blocos perdidos ou preenchidos com silencio. O analisador
mostra histogramas de latencia separados, inspeciona cada metade do PCM em
busca do padrao de bits altos observado no M002 e apresenta uma recomendacao
provisoria. Para voltar ao firmware completo depois do teste, altere somente
`kAudioSdDiagnosticEnabled=false`.

`kMicrophoneDiagnosticEnabled=true` inicia somente o
ICS43434 em RAM. SD, PCA9548A e sensores nao sao inicializados nesse modo. O
monitor serial informa, a cada dois segundos, taxa efetiva, perdas da fila,
uso de memoria, DC, RMS, clipping e atividade dos dois canais.

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run --target upload --target monitor --upload-port COM3
```

O teste deve incluir alguns segundos em silencio, fala em nivel normal e sons
fortes sem encostar no microfone. O canal esperado e o esquerdo porque `SEL`
esta ligado ao GND.

Na configuracao normal, `kMicrophoneDiagnosticEnabled=false`,
`kAudioSdDiagnosticEnabled=false` e `kMicrophoneRecordingEnabled=false`.
Os modos de audio devem ser habilitados apenas para diagnosticos dedicados.

O arquivo cru tambem pode ser ouvido diretamente com FFplay 8 usando
`-ch_layout mono`:

```powershell
ffplay -f s16le -ar 44100 -ch_layout mono "E:/M001/audio.raw"
```

Para audio de baixo nivel, prefira `python/export_audio.py --gain-db 18`.
O ganho afeta somente o WAV de reproducao; `audio.raw` permanece inalterado.
O exportador informa `peak_safe_gain_db` e avisa quando o ganho escolhido
produz clipping; nao se deve amplificar uma captura que ja esteja proxima da
escala completa.
