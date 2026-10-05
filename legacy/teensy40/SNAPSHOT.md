# Estado preservado da Teensy 4.0

Cópia do diretório de trabalho feita em 2026-10-05 **antes da suspensão do áudio**.
Inclui firmware, `platformio.ini`, documentação, ferramentas Python e testes,
com as sete alterações locais existentes. Não é uma exportação apenas de `HEAD`.
O commit de referência, estado do Git, tamanhos e hashes individuais estão em
`snapshot-manifest.json`; `local-changes.patch` preserva o diff anterior à alteração.

Os 58 arquivos do projeto foram copiados e comparados com SHA-256.
`.git`, `.pio`, `.venv`, `.vscode` e caches Python não foram copiados.
As dependências continuam identificadas no `platformio.ini` histórico;
este snapshot não inclui os pacotes instalados nem constitui um build offline.

`experimental-data.zip` contém os **44 arquivos** de `data/`, totalizando
**236.521.430 bytes** antes da compressão. Todos foram relidos do ZIP e comparados
com os originais usando SHA-256. Os dados originais permanecem em `data/`.
O ZIP é ignorado pelo Git; o manifesto e este registro podem ser versionados.
Clonar o repositório não recupera o ZIP nem os dados originais: para um backup
independente, copiar também o ZIP e o manifesto para outro armazenamento.
A cópia atual permanece no mesmo workspace; sincronização do OneDrive não foi validada.

Verificar a preservação, na raiz do projeto (Python 3.11 ou posterior):

```powershell
& ./.venv/Scripts/python.exe -B legacy/teensy40/verify_snapshot.py
```

Para compilar o firmware histórico, usar esta pasta como raiz de projeto:

```powershell
pio run --project-dir legacy/teensy40 -e teensy40
```

O `README.md` e os documentos copiados descrevem a etapa histórica e podem conter
instruções de áudio incompatíveis com o firmware ativo após a suspensão.
Não atualizar esses arquivos históricos durante a migração.
O legado Nicla permanece separado em `legacy/ArduinoNiclaVoice/`,
`legacy/TestIMU.py`, `legacy/testIMUICM.py` e `legacy/CLAUDE.md`.
