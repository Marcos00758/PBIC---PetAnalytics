# Pet Analytics Firmware

## Visão Geral

Este repositório contém o firmware do wearable **Pet Analytics**
desenvolvido para pesquisa PBIC. O foco desta etapa é coleta robusta de
dados, gravação em cartão SD e posterior processamento em Python/IA. Não
há inferência embarcada nesta fase.
Objetivo

Este projeto é um wearable para cães destinado à coleta de dados para pesquisa.
Nesta etapa não existe classificação em tempo real. O objetivo é coletar dados
sincronizados dos sensores em Arduino/C++ na Teensy 4.0 e interpretá-los em
Python. A gravação no cartão SD será adicionada em uma etapa posterior.
## Hardware

-   Teensy 4.0
-   PCA9548A
-   3×ICM-20948
-   2×BMP390
-   ICS43434
-   microSD
-   LiPo 3.7V

## Estado atual

Os três ICM-20948 nos canais 0, 1 e 4 do PCA9548A são lidos a 100 Hz. O firmware
transmite pela USB pacotes binários de 45 bytes com timestamp, sequência, 18
valores crus `int16` e CRC-8. O formato completo está em
`docs/DATA_FORMAT.md`.

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
