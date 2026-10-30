# Segundo ensaio dos BMP390: movimento individual com referência fixa

Roteiro de 2026-10-20, após a análise da S004, revisado em 2026-10-27 para
**+20 cm e -20 cm**, conforme o alcance dos fios informado pelo usuário.
Duração: **16 min 30 s de aquisição**, além do tempo de inicialização do firmware.

## Objetivo

Verificar o sinal, a amplitude aproximada e a repetibilidade da resposta de cada
BMP, separando seu movimento das mudanças observadas por um segundo BMP imóvel.
Investigar a mudança lenta de pressão sem aquecer diretamente as placas, como
ocorreu na S004. Não alterar firmware, filtros, compensação ou dados originais.

## Preparação

1. Identificar as placas antes de iniciar: **BMP0 = canal de software PCA 7**;
   **BMP1 = canal 3**, conforme a montagem atual. Etiquetar o suporte ou cabo,
   deixando livre o orifício do sensor.
2. Fixar cada placa em um suporte próprio que possa ser movimentado segurando
   apenas sua base ou haste. Não segurar a placa, pressionar a cápsula nem
   cobrir o orifício. Evitar ar quente, sopro, ventilador e portas movimentadas.
3. Marcar três alturas com régua/trena: **zero, +20 cm e -20 cm**. O zero deve
   permitir os dois deslocamentos; por exemplo, cerca de 60 cm acima do chão.
   Medir pela posição do próprio sensor. São 40 cm entre os extremos.
   A folga dos fios deve permitir o percurso inteiro sem tração: não deslocar
   o ESP32 ou o BMP de referência para alcançar uma marca. Se o alcance real
   for menor, anotar a altura medida; não forçar os cabos.
4. Colocar os dois BMPs no zero e próximos entre si. O sensor de referência
   ficará nessa posição durante todo o bloco do outro. Manter orientação e
   exposição semelhantes; deixar folga nos cabos para não puxar o sensor fixo.
5. Manter ESP32, alimentação e módulo SD imóveis. Manter os ICMs imóveis também;
   não é necessário removê-los nem mudar a configuração existente.
6. Abrir o monitor serial, conservar o log completo e preparar um cronômetro
   visível. Se usar o monitor PlatformIO, o filtro `log2file` salva a saída:

   ```powershell
   & "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" device monitor --port COM11 --baud 115200 --filter log2file
   ```

   Ajustar apenas a porta se necessário. Ao terminar, localizar e guardar o
   arquivo gerado pelo filtro junto da cópia da sessão. Se perdeu o início do
   firmware, manter o monitor aberto e pressionar RESET para uma nova sessão;
   não misturar os dados de sessões diferentes.

## Marco inicial e cronograma

**Iniciar o cronômetro somente ao aparecer `ACQUISITION_START ... recording=1`.**
O tempo de montagem/varredura do SD anterior a essa mensagem não entra no roteiro.
Na análise, os tempos serão relativos ao primeiro pacote; anotar eventual atraso
entre a mensagem e o início do cronômetro.

| Cronômetro | Segundos | Ação |
|---|---:|---|
| 00:00–05:00 | 0–300 | Ambos imóveis no zero. Não tocar nas placas. |
| 05:00–07:00 | 300–420 | Ciclo 1 do BMP0; BMP1 fixo no zero. |
| 07:00–09:00 | 420–540 | Ciclo 2 do BMP0; BMP1 continua fixo. |
| 09:00–10:00 | 540–600 | Ambos imóveis no zero. |
| 10:00–12:00 | 600–720 | Ciclo 1 do BMP1; BMP0 fixo no zero. |
| 12:00–14:00 | 720–840 | Ciclo 2 do BMP1; BMP0 continua fixo. |
| 14:00–16:00 | 840–960 | Ambos imóveis no zero. |
| 16:00–16:30 | 960–990 | Continuar imóvel; margem para novos checkpoints do SD. |

Os cinco minutos iniciais são uma espera padronizada, não uma garantia de
estabilidade térmica. A estabilidade será verificada posteriormente nos dados.
Usar 240–300 s como referência inicial prevista e 900–960 s para o repouso final.
Se houver perturbação nesses trechos, registrá-la; não redefinir silenciosamente
a referência para melhorar o resultado.

## Como executar cada ciclo de dois minutos

Movimentar apenas o BMP indicado na tabela, pela haste/base do suporte. O outro
permanece imóvel no zero. Fazer transições suaves, sem impactos ou tensionar cabos.

| Tempo dentro do ciclo | Ação do sensor móvel |
|---|---|
| 00–05 s | Subir do zero para +20 cm. |
| 05–30 s | Manter imóvel em +20 cm. |
| 30–35 s | Voltar ao zero. |
| 35–60 s | Manter imóvel no zero. |
| 60–65 s | Descer do zero para -20 cm. |
| 65–90 s | Manter imóvel em -20 cm. |
| 90–95 s | Voltar ao zero. |
| 95–120 s | Manter imóvel no zero. |

Não saltar diretamente de +20 para -20 cm: o retorno intermediário ao zero
permite conferir a recuperação da linha de base. Cada patamar tem 25 s previstos;
na análise, usar preferencialmente os 15 s centrais e excluir as transições.
Para um ciclo iniciado em `t`, essas janelas são `t+10–25`, `t+40–55`,
`t+70–85` e `t+100–115` segundos. Conferir os tempos reais anotados antes de usá-las.

## Registro mínimo durante o teste

Anotar o tempo global do cronômetro em que cada posição foi efetivamente
atingida, mesmo que difira alguns segundos do roteiro. Uma segunda pessoa ou
um vídeo mostrando cronômetro e suporte ajuda a evitar tocar nas placas para
registrar os eventos. Não estimar novamente os horários de memória ao final.

| Sensor/ciclo | Chegou a +20 | Voltou ao zero | Chegou a -20 | Voltou ao zero | Observações |
|---|---|---|---|---|---|
| BMP0 / 1 | | | | | |
| BMP0 / 2 | | | | | |
| BMP1 / 1 | | | | | |
| BMP1 / 2 | | | | | |

Registrar também altura efetivamente medida, contato acidental, movimento do
sensor fixo, corrente de ar, cabo puxado, erro serial, reset ou desconexão.
Se algo ocorrer, preservar a sessão e marcar o instante. Não ocultar o evento
nem repetir um movimento fora do cronograma sem anotá-lo.

## O que será avaliado

- **Integridade:** CRC, sequência, timestamps, prefixo confirmado pelo journal,
  sentinelas, leituras congeladas e contadores disponíveis de I2C/SD/fila.
- **Sinal:** pressão menor em +20 cm e maior em -20 cm, comparando os patamares
  do mesmo sensor com os zeros adjacentes.
- **Amplitude e repetibilidade:** comparar os dois ciclos de cada BMP e depois
  os dois módulos. Nas condições aproximadas da S004, 20 cm correspondem a
  2,3 Pa e os 40 cm entre extremos a 4,6 Pa. São estimativas físicas, não limites
  automáticos de aprovação; serão recalculadas com as condições da nova sessão.
- **Referência imóvel:** verificar se a variação aparece só no BMP movimentado
  ou também no fixo. Variação comum indica influência adicional, mas não determina
  sua causa. Comparar sem substituir ou corrigir automaticamente os dados crus.
- **Temperatura e retorno:** verificar aquecimento durante cada movimento e
  recuperação do zero, distinguindo tendência de fundo de resposta à altura.
- **Visualização:** pressão e temperatura cruas compensadas, marcações dos
  eventos reais e comparação entre móvel/fixo. Estatísticas de patamares serão
  descritivas; não aplicar suavização nem certificar precisão só pelas médias.

O desvio observado na S004 foi de aproximadamente 4,3 Pa, maior que o sinal de
20 cm e próximo ao de 40 cm entre os extremos. A redução do percurso dificulta
a separação dos patamares nas leituras individuais cruas. Se os
patamares continuarem sem separação suficiente, o resultado será inconclusivo
para essa aplicação/configuração; não corrigir o gráfico para forçar a altura.
Não adotar a tolerância numérica do algoritmo de compensação como tolerância
experimental dos sensores.

## Encerramento e arquivos para análise

Depois de 16:30, anotar o instante de desligamento e desligar como de costume.
A margem final favorece a confirmação dos trechos anteriores; não garante um
último pacote completo nem substitui o journal. A análise usará somente seu
prefixo confirmado. Não é necessário implementar um comando de encerramento.

Guardar a pasta completa da nova sessão, sem sobrescrever S004:
`imu.bin`, `meta.txt`, `journal.bin`, `status.txt`, log serial e tabela de eventos
em um arquivo separado, por exemplo `eventos_bmp.txt`. Informar qual pasta foi
gerada; não presumir que o próximo identificador será S005.

Para uma primeira verificação textual, substituindo `Sxxx` pela sessão real:

```powershell
& ".\.venv\Scripts\python.exe" -B python/avaliacao/avaliar_bmp.py data/Sxxx --reference-s 240 300
```

Esse comando verifica o prefixo e calcula estatísticas com referência fixa;
não identifica automaticamente os ciclos nem aprova o ensaio. Enviar os arquivos
originais e os eventos para a comparação por patamar. Manter uma cópia de
segurança: `data/` é ignorado pelo Git.

## Montagem SD desta revisão

Na revisão de 2026-10-27, o usuário confirmou **HW-125 com VCC em 5 V**, com CS=GPIO4,
MOSI=GPIO5, MISO=GPIO6 e SCK=GPIO7. O log de `/SDTEST/R000003` enviado em
2026-10-27 contém os casos 0–11, a 4/12 MHz e blocos de 512/1024/4096 bytes,
todos com verificação `complete=1` e `ok=1`. Houve atrasos de agenda no teste
de carga; a integridade não certifica todos os prazos da aquisição real.
20 MHz e a recuperação após reinicialização ainda não foram concluídos nesse
log. Manter os 12 MHz atuais no ensaio BMP e preservar o log da aquisição.

Para novas gravações após a revisão de 2026-10-29, usar **CS=GPIO7, MISO=GPIO4,
SCK=GPIO6 e MOSI=GPIO5**. O log citado acima é anterior à troca e conserva a
pinagem antiga. Alimentação e cronograma do ensaio permanecem os mesmos.
