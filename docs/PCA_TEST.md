# Teste isolado ESP32-S3 + PCA9548A

Ambiente `esp32s3_pca_test`, diagnóstico v1 (2026-10-13). Usa SDA GPIO1,
SCL GPIO2 e timeout I2C de 10 ms, iguais à aquisição. Não depende do driver
PCA nem das bibliotecas dos sensores. Mantém o CS do SD alto; não inicializa
SPI, SD, áudio ou aquisição.

Inicia automaticamente e repete a cada 5 s, para permitir abrir o serial
depois do boot. Em cada rodada configura 100 kHz e depois 400 kHz, consultando
somente endereços de 7 bits `0x70` a `0x77`. Onde houver resposta, escreve
`0x00` (desativar todos os canais PCA), usando transações separadas com STOP.
Registra o retorno bruto de `Wire.endTransmission()` nas duas operações.

## Executar

Na raiz do projeto, fechar o monitor anterior com Ctrl+C. Ajustar COM11:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32s3_pca_test --target upload --upload-port COM11
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" device monitor --port COM11 --baud 115200
```

Não há comando serial para iniciar. Copiar uma rodada completa desde
`PBIC_STAGE=esp32s3_pca_test` até o segundo `PCA_SCAN_END`.

## Interpretar

Exemplo de sucesso em um endereço (repetir nas duas frequências):

```text
PCA_RESULT hz=100000 address=0x70 op=probe code=0
PCA_RESULT hz=100000 address=0x70 op=write_zero code=0
```

| Código | Significado nesta versão do Arduino-ESP32 |
|---|---|
| 0 | Transação concluída com sucesso |
| 2 | `ESP_FAIL`, normalmente NACK; não distingue endereço de dados |
| 4 | Outro erro |
| 5 | Timeout |

Mapeamento conferido no `Wire.cpp` instalado (core 2.0.17), disponível também
no [código Espressif](https://github.com/espressif/arduino-esp32/blob/2.0.17/libraries/Wire/src/Wire.cpp).
`PCA_WRITE_SKIPPED` significa que não houve escrita porque a consulta falhou.
`responses/writes_ok` resumem a rodada. Endereços sem dispositivo normalmente
retornam código 2. `i2c_ready=0` ou `clock_ok=0` impede executar a etapa respectiva.

Resposta em outro endereço da faixa pode indicar configuração diferente de
A0/A1/A2; não alterar automaticamente o endereço do firmware principal.
ACK e escrita aceita não identificam o modelo do chip nem validam os canais
ou sensores. Os clocks são os solicitados, não medições elétricas.
Endereçamento e controle conforme o [datasheet TI](https://www.ti.com/lit/ds/symlink/pca9548a.pdf).

Compilação `run -e esp32s3_pca_test` aprovada: 19.268 bytes de RAM estática,
279.953 bytes de flash, somente Wire como dependência. Sem upload pelo agente;
conexão e diferenças entre 100/400 kHz dependem do teste na placa.
