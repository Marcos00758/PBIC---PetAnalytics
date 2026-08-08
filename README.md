# Pet Analytics Firmware

## Visão Geral

Este repositório contém o firmware do wearable **Pet Analytics**
desenvolvido para pesquisa PBIC. O foco desta etapa é coleta robusta de
dados, gravação em cartão SD e posterior processamento em Python/IA. Não
há inferência embarcada nesta fase.
Objetivo

Este projeto é um wearable para cães destinado à coleta de dados para pesquisa.
Nesta etapa não existe classificação em tempo real. O objetivo é apenas coletar dados sincronizados dos sensores e gravá-los no cartão SD.
O firmware deverá ser escrito em Arduino/C++ para Teensy 4.0 e o processamento inicial dos dados para gravação no cartão sd em python.
## Hardware

-   Teensy 4.0
-   PCA9548A
-   3×ICM-20948
-   2×BMP390
-   ICS43434
-   microSD
-   LiPo 3.7V
