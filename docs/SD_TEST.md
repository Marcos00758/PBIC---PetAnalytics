# Teste independente ESP32-S3 + microSD

Implementação SDT1 para a placa OEM FH4R2 e módulo alimentado em **3,3 V**,
com conexões já concluídas pelo usuário: SCK GPIO7, MISO GPIO6, MOSI GPIO5,
CS GPIO4. Ambiente `esp32s3_sd_test`, SdFat **2.3.1** explícito, `SdFs`/`FsFile`
para FAT16/FAT32/exFAT. Compartilha defaults em `src/config/storage_config.h`
com a aplicação existente, sem portar o logger Teensy neste passo.

## Executar na bancada

Substituir COMx pela porta da placa. O primeiro upload pode exigir BOOT/RESET.

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32s3_sd_test
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32s3_sd_test --target upload --upload-port COM11

```

No monitor, usar os caracteres abaixo (não é necessário Enter):

| Comando | Ação |
|---|---|
| `s` | Nova execução em pasta inédita; matriz e dois resets automáticos |
| `v` | Verificar novamente a execução mais recente, identificada na NVS |
| `p` | Após a matriz/recuperação, criar caso de corte manual de energia; um por execução |

No boot aparece `SD_CARD` com CID, fabricante/produto/serial, capacidade em
setores, tipo de filesystem, clock e versão SdFat. Antes de `s` não há criação
de nova execução. Se houver execução anterior na NVS, ela é verificada
automaticamente no boot; as métricas dessa leitura são acrescentadas ao CSV.
Ao trocar de cartão, o CID evita aplicar a recuperação pendente ao cartão novo.

Uma execução completa retém aproximadamente **83 MiB**, mais metadados,
checkpoints e possível caso de corte de energia. Há reserva de 4 MiB e margem
adicional por caso. Recomenda-se pelo menos 100 MiB livres. O programa nunca
formata o cartão nem apaga sessões existentes; cada arquivo é criado com
exclusividade. A falta de espaço ou erro aborta a matriz e mantém as evidências.

## Matriz e interrupções

SPI solicitado: **4, 12 e 20 MHz**; blocos: **512, 1.024 e 4.096 bytes**.
Cada combinação produz um caso de throughput máximo (8 MiB, pré-alocação
2 MiB), seguido de carga sintética de 79 bytes a cada 10 ms (7.900 bytes/s,
237.056 bytes, aproximadamente 30 s, pré-alocação 64 KiB). Todos crescem além
da pré-alocação. IDs 000–017 seguem ordem clock → bloco → máximo/cadenciado.
Os nove ensaios cadenciados somam pelo menos 4,5 minutos; escrita, verificação
e resets acrescentam tempo. Firmware/CPU limitam o throughput observado;
clocks são configurações solicitadas, não medição elétrica da frequência.

Os registros determinísticos de 256 bytes têm seed, sequência e CRC32. A
verificação fecha/reabre os arquivos e compara **todos os bytes confirmados**.
Casos concluídos são truncados ao tamanho real. A cada 64 KiB ou um segundo
com novos dados, sincroniza dados e depois grava/sincroniza um checkpoint;
checkpoints confirmam apenas registros completos. Não há impressão por bloco
durante a medição. Métricas são gravadas depois de cada fase.

Após a matriz, na configuração compartilhada de 12 MHz:

1. **Caso 018:** pré-aloca 512 KiB, escreve/sincroniza 64 KiB, confirma o
   checkpoint e escreve mais 17 KiB + 17 bytes sem sync/close final.
   Executa `ESP.restart()` com os arquivos abertos. No boot confere toda a
   execução, incluindo os 64 KiB do prefixo; cauda não confirmada não é aprovada.
2. **Caso 019:** escreve um novo arquivo completo de 8 MiB, fecha/reabre/verifica
   e reinicia novamente. No boot verifica todos os casos e emite
   `SD_RECOVERY phase=2 integrity_ok=1 suite_ok=1` quando não há falhas.
3. **Caso 020, opcional:** comando `p` prepara prefixo/cauda como no caso 018 e
   imprime **`SD_POWER_CUT_READY`**. Depois não faz novas operações SD. Cortar
   fisicamente a alimentação da placa/cartão e religar. Se a USB do computador
   continuar alimentando a placa, desligar apenas a bateria não produz o corte.
   No boot deve aparecer `SD_RECOVERY phase=3 integrity_ok=1 suite_ok=1`.
   `p` repetido não sobrescreve esse caso; usar `s` para outra execução.

A intenção de recuperação é armazenada em NVS antes da interrupção. Um erro
durante preparação/recuperação deixa evidências e não provoca loop de resets.
A USB pode desconectar nos reinícios; reabrir o monitor e usar `v` se necessário.
Reset por software mantém o cartão energizado e **não valida queda de energia**.
O corte manual após `READY` testa arquivos abertos sem sync final; não equivale
a cortar energia no instante exato de uma transação SPI. Testes repetidos em
outros momentos exigem procedimento adicional e devem ser registrados.

## Interpretar resultados

Arquivos: `meta.txt`, `manifest.txt`, `results.csv`, `errors.txt` (se erro),
`cNNN.bin` e `cNNN.chk`, em `/SDTEST/Rxxxxxx/`. Protocolo completo em
[DATA_FORMAT.md](DATA_FORMAT.md). CID bruto e número de setores identificam
o cartão; anotar também marca/modelo comercial e fiação na ficha de bancada.
Erros incluem caso, clock e bloco; caso 999 identifica preparação/metadados.
Código SdFat zero não elimina falhas lógicas: conferir também `operation`,
especialmente `integrity_or_size`, `checkpoint_order` e falta de espaço.

`results.csv` registra pré-alocação, escrita, sync de dados, sync de checkpoint
e leitura/verificação. Tem número de chamadas, menor/média/maior latência,
histograma, bytes e tempo. Velocidade efetiva B/s = `bytes*1000000/elapsed_us`.
Escrita inclui geração e manutenção no loop; leitura inclui conferência do
padrão. Latências por chamada isolam SdFat. Não interpretar esse throughput
efetivo como benchmark exclusivo do controlador SPI. Tamanho final e tempo
do loop são diferentes do tempo de encerramento; o sync final aparece nas
métricas de manutenção.

Na carga cadenciada, `late_slots` conta oportunidades de 10 ms inteiramente
perdidas; `max_lateness_us` mede o pior atraso na geração. Ao perder uma
oportunidade, o teste continua o fluxo sequencial sem fabricar uma rajada
retroativa. Assim, integridade pode passar com taxa abaixo de 7.900 bytes/s;
um `ok=1` não aprova automaticamente a cadência. Comparar também throughput,
atrasos e latências máximas, especialmente escritas/sync acima de 10 ms.

Não há sensores, microfone, filtros, Wi-Fi/BLE ou uso obrigatório de PSRAM
nesse build. A carga é produzida e consumida no mesmo fluxo; não valida I2C
nem aquisição em outro núcleo. Dois núcleos permitirão separar aquisição e
SD futuramente, mas as pausas do cartão continuarão exigindo buffer/fila e
medição de perdas. O buffer compartilhado tem 8 KiB; blocos são agregados sem
descartar bytes do padrão.

FAT/exFAT são aceitos pela biblioteca e o formato usa offsets/contadores uint64.
A matriz de 8 MiB **não valida arquivos acima de 4 GiB** nem todo tipo de
filesystem/cartão. Repetir com cartões e formatos de interesse, sem formatar
automaticamente dados de pesquisa. Integridade elétrica, brownouts e tolerância
a interrupções só podem ser aceitos após testes físicos.

## Verificação independente no computador

Copiar a pasta inteira da execução para `data/sd-test/` (ignorada pelo Git),
preservando arquivos originais antes de qualquer análise:

```powershell
& ./.venv/Scripts/python.exe -B python/verify_sd_test.py data/sd-test/R000001
```

O script usa somente a biblioteca padrão, emite JSON e retorna 0 se os prefixos
declarados forem íntegros, 1 em erro. Confere também arquivos ausentes/extras,
manifesto e conteúdo com CRC recalculado em Python. Arquivos interrompidos
podem ter `complete=false`, `checkpoint_tail=true` e bytes não confirmados;
isso é recuperação de prefixo, não aprovação da cauda. Uma execução abortada
pode conter prefixos íntegros e ainda não ter realizado toda a matriz:
o campo `matrix_complete` só é verdadeiro com os casos 000–019, completos
000–017/019 e prefixo íntegro do 018. `power_cut_prefix_verified` informa a
integridade do 020, que só existe após `p`; confirmar também o registro de
corte físico e a recuperação de fase 3 para validar esse procedimento.

Testes automatizados do formato/verificador:

```powershell
& ./.venv/Scripts/python.exe -B -m unittest discover -s python/tests -p test_verify_sd_test.py -v
```

Exigem `g++` no PATH para gerar fixtures com o header C++ real; se ausente,
esse grupo é marcado como skipped. Há testes de CRC/padrão, sequência trocada,
truncamento, manifesto, arquivos ausentes, checkpoint inválido/regressivo e
cauda interrompida. Compilação/testes de host não substituem o ensaio na placa.

## Validação desta implementação — 2026-10-05

- Builds aprovados: `esp32s3_sd_test`, `esp32s3_supermini` e `teensy40`.
- Diagnóstico: 32.284 bytes de RAM estática e 311.125 bytes de flash; core
  Arduino-ESP32 2.0.17 e SdFat 2.3.1 fixados pelo PlatformIO.
- 39 testes Python aprovados, incluindo 15 novos testes do diagnóstico com
  fixtures produzidas pelo C++ real e conferidas em Python.
- Snapshot Teensy confirmado: 58 arquivos de projeto, patch local e 44 arquivos
  experimentais (236.521.430 bytes). Nenhum desses arquivos foi alterado.
- `git diff --check` aprovado. Nenhum upload, commit ou push realizado.
- Pendentes: ensaio físico da matriz, resets/corte de energia, alimentação,
  clocks efetivos, cartões/filesystems e carga com sensores nos dois núcleos.
