# Firmware

- Restrições do firmware
Não usar delay() no loop principal.
Usar micros() ou millis() para temporização.
Manter a aquisição em frequência fixa e previsível.
Não bloquear o loop durante gravações no SD.
Acessar os sensores externos somente pelo PCA9548A.
Trabalhar exclusivamente com lógica e alimentação de 3,3 V.
Não usar pinos sem documentá-los em pins.h.
Não alterar o formato do pacote sem atualizar o parser Python.
Não gravar logs de texto a cada amostra.
Usar buffers e flush periódico para SD e áudio.
Validar retorno de inicialização de SD, PCA, sensores e microfone.
Registrar falhas e contadores em status.txt.
Manter áudio e IMU em arquivos separados.
Evitar números mágicos; usar constexpr.
Separar drivers, serviços, dados, configuração e utilidades.

- Compatibilidade com Python
O firmware e o parser Python fazem parte do mesmo sistema.
Toda vez que modificar:

tamanho de pacote;
ordem dos campos;
tipo de dados;
endianness;
CRC;
frequência;
estrutura da sessão;

verifique também python/parse_data.py.

Nunca considere uma alteração de formato concluída se o parser Python não tiver sido atualizado e validado.

- Responsabilidades por pasta
src/main.cpp: coordena a aplicação. Deve permanecer simples.
src/drivers/: comunicação direta com hardware.
src/utils/: CRC, pacote e funções auxiliares.
src/config/: pinos e constantes globais.
python/: leitura, validação e análise dos arquivos produzidos pelo firmware.
docs/: documentação da arquitetura, hardware e formato de dados.

Não coloque lógica de driver diretamente em main.cpp.