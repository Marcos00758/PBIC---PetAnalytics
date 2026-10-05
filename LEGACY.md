# Legados preservados

`legacy/teensy40/` preserva a última etapa Teensy de apresentação antes da
migração, com código, configuração, documentação, ferramentas, alterações
locais e manifesto de integridade. Consulte [SNAPSHOT.md](legacy/teensy40/SNAPSHOT.md).
Não editar o snapshot. Seu ZIP de dados experimentais permanece local e ignorado
pelo Git; o clone recupera o manifesto, mas não esse ZIP.

O legado Nicla permanece separado em `legacy/ArduinoNiclaVoice/`,
`legacy/TestIMU.py`, `legacy/testIMUICM.py` e `legacy/CLAUDE.md`.
Nenhum arquivo dessas etapas foi alterado na preparação ESP32.

O destino atual é ESP32-S3 Super Mini OEM/ESP32-S3FH4R2, documentado em
[HARDWARE_SPEC.md](docs/HARDWARE_SPEC.md). O ambiente padrão `esp32s3_supermini`
é de preparação (USB/memória/barramentos); o firmware Teensy da main permanece
selecionável com `-e teensy40` enquanto seus serviços são portados.
