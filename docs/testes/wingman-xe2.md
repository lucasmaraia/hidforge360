# Teste do Brook Wingman Converter XE 2 no Xbox 360

> Diário de testes, rodada a rodada, de como o suporte ao XE 2 e a calibração dos analógicos chegaram à
> versão 1.0.0. Os builds citados (`out/...`) ficaram na máquina de desenvolvimento e não são versionados.
> O build final equivale ao `hidforge.xex` da release v1.0.0 (sha1 `f8494f76`). Os logs estão em
> [`../logs/`](../logs/).

O XE 2 não tem modo Xbox 360. No PC ele se apresenta como **`0c12:0efa`**, e o relatório dele é igual, byte a
byte, ao de um DualShock 4 por USB. O hidforge passa a tratá-lo como DS4 (passo "Brook XE 2 detection" do
`hidforge/port_v05.py`). O build atual (`hidforge-v05port-final.xex`) **não** reconhece o XE 2: ele só aceita
o VID da Sony.

## 1. No PC (antes de ir ao console)

Já medido automaticamente em 25/11/2026 (`../logs/teste-xe2-20261125-173453-auto.txt`):

| Item | Resultado |
|---|---|
| Interface HID | só uma, a **0**; endpoints `0x84` IN (64, intervalo 1 ms) e `0x03` OUT (64, intervalo 5) |
| Entrada | ID `0x01`, 64 bytes, formato DS4: **OK**; ~1000 relatórios/s |
| Saída por `SET_REPORT` (controle) | **recusada** (erro 203), igual ao DS4 |
| Saída por endpoint OUT | aceita (`WriteFile` ok), que é a rota que o hidforge usa para o DS4 |

**Parte interativa concluída em 25/11/2026** (`../logs/teste-xe2-20261125-1749*.txt`, `-1754*` e `-1755*`):

| Item | Resultado |
|---|---|
| Analógicos | os 4 eixos de 1 a 255, centro em 128; L2/R2 de 0 a 255 |
| Botões | todos vistos; d-pad 8/8 na etapa ao vivo |
| Vibração com o relatório exato do driver (`0x03` + cor do slot) | **6/6**, incluindo meia força e repetição |
| Barra de luz | o XE 2 **não** repassa a cor ao DualSense (não afeta nada) |
| Um envio só | motor fica ~6,4 s ligado; o driver reenvia a cada 250 ms |

Para repetir: a mesma bateria, numa janela PowerShell própria, com o DualSense ligado no XE 2:

```
cd <pasta do repositório>
powershell -ExecutionPolicy Bypass -File tools\teste-xe2.ps1
```

O script pede, em ordem:
- **analógicos**: soltar tudo, depois girar os dois até o fim e apertar L2/R2 até o fundo;
- **botões**: apertar cada um quando pedir;
- **vibração e barra de luz pelas duas rotas**: responder s/n;
- **flags**;
- **quanto tempo o motor fica ligado com um envio só**.

No fim ele grava um log em `../logs/`.

O que importa: vibração e barra de luz pela rota **saída** (a do controle deve falhar), analógicos de 0 a 255 e
todos os botões vistos. Uma etapa só: `-Etapa analog`, `botoes`, `vibra`, `flags` ou `expira`.

## 2. No console: primeiro teste, com o build de diagnóstico

1. Conectar o HD no PC e **conferir a letra** (era `G:`).
2. Copiar `out/hidforge-xe2-diag.xex` sobre `G:\hidforge.xex`. O `launch.ini` não muda.
3. Fechar o Docker Desktop e rodar `wsl --shutdown`, depois ejetar o HD.
4. Ligar o console e esperar o Aurora. **Só então** plugar o XE 2, com o DualSense já pareado nele.
5. Checklist:
   - [ ] o anel de luz mostra o estágio do driver ao plugar (build de diagnóstico)
   - [ ] o controle ganha um slot de jogador
   - [ ] analógicos esquerdo e direito, nos quatro sentidos
   - [ ] gatilhos L2/R2 analógicos (aceleração gradual num jogo de corrida)
   - [ ] d-pad, ✕○□△, L1/R1, L3/R3, Options (Start), Create (Back)
   - [ ] PS tocado abre o guia
   - [ ] PS segurado 2 s desliga o console
   - [ ] vibração num jogo, nos dois motores
   - [ ] regressão: DualSense direto no console continua funcionando (e DS4/DS3, se estiverem à mão)
6. Desligar, trazer o HD e copiar `G:\hidforge.log` para `../logs/`. As linhas que interessam:
   - `EINTIM: HID device vendor id: c12, product id: efa`: é o que o XE 2 se apresentou como diante do 360;
   - `HIDFORGE: slot N type 0 first report 01 ...`: o primeiro relatório, que deve começar com `01 80 80 80 80 08`.

### Se não funcionar

| Sintoma | O que significa | Próximo passo |
|---|---|---|
| Nenhuma linha `HID add device` no log ao plugar | o XE 2 não enumerou como HID no 360 (caso parecido com o do 8BitDo) ou escolheu um modo que não é HID | anotar o que a luz do XE 2 faz; testar com hub USB 2.0 no meio |
| `vendor id` / `product id` diferentes de `c12`/`efa` | diante do 360 o XE 2 escolheu outro modo | trazer o log; adicionar o PID novo e conferir o formato pelo `first report` |
| Detectado, botões ok, sem vibração | relatório de saída recusado no 360 | trazer o log (linhas `HIDFORGE: slot ... rumble`) |
| Detectado, botões trocados | formato diferente do DS4 nesse modo | o `first report` mostra o layout |

## 3. Se tudo funcionar

Copiar `out/hidforge-xe2.xex` (o build limpo, sem log e sem piscada) sobre `G:\hidforge.xex`.

## Voltar atrás

Copiar `out/hidforge-v05port-final.xex` sobre `G:\hidforge.xex`: é o build de antes, sem o XE 2.

## 4. Analógicos mais precisos (25/11/2026)

O primeiro teste no console deu certo, até o PS segurado para desligar. O log está em
`../logs/hidforge-xe2-console1.log`: `c12:efa` no OHCI, primeiro relatório `01 80 80 80 80 08`, vibração com
status 0. A queixa foi a mira do analógico direito: degraus ao mirar devagar, zona morta grande e deriva.

A causa no código da v0.5 é que o byte do analógico era convertido linearmente, `(v-128)*256`. A zona morta
do próprio jogo (~26 %) somava-se a isso, e o Y saía descentrado (-256).

`shim/sticks.inc` (`HfStick`) trata os dois eixos juntos, de forma radial:
- **zona morta interna** de 4 contagens, contra o ruído e a parada fora do centro;
- **antizona morta**: o primeiro degrau fora da zona morta já cai na borda da zona morta do jogo, 7849 no
  esquerdo e 8689 no direito;
- **Y centrado**.

O estado passa do `interruptHandler` para o hook sob um contador de sequência. O teste de mesa
(`hidforge/tests/sticks_test.c`) passou. Ajustes rápidos ficam no topo de `sticks.inc`: `HF_INNER_DZ_256`,
`HF_ANTI_DZ_L` e `HF_ANTI_DZ_R`.

Medido no PC em 25/11/2026 (`../logs/teste-xe2-precisao-xe2-20261125-230525.txt`):
- **atualização**: mediana de 1 ms e p90 de 4 ms, então o XE 2 não acrescenta atraso;
- **zona morta própria**: o XE 2 não tem nenhuma, e o valor sai do centro de 1 em 1;
- **degraus**: nenhum pulado no movimento lento;
- **repouso**: o analógico para até 2 contagens fora do 128 (130 num teste, 127 no outro). Por isso a zona morta interna passou de 2 para 4 contagens: com a antizona morta, um resto de 3 contagens viraria deriva no jogo.

Para medir no PC:
```
powershell -ExecutionPolicy Bypass -File tools\teste-xe2.ps1 -Etapa precisao
powershell -ExecutionPolicy Bypass -File tools\teste-xe2.ps1 -Etapa precisao -Pad dualsense   # DualSense por cabo
```

No console, com `hidforge-xe2-sticks-diag.xex`:
- [ ] a mira lenta está suave?
- [ ] a mira começa a mexer logo?
- [ ] a deriva parou?
- [ ] a mira ficou sensível demais? Se ficou, baixar `HF_ANTI_DZ_R`.
- [ ] o DualSense direto continua bom?

### Rodada 2 (25/11/2026)

No console, a rodada 1 (`10d771bc`) deixou a mira **melhor, mas sensível**: ela saía do centro rápido demais. O log está em `../logs/hidforge-xe2-console2.log`, limpo. A rodada 2 traz três mudanças:
- **curva de resposta** no direito: `HF_CURVE_R = 128`, meia quadrática, com metade da inclinação perto do centro e o fim do curso ainda em 32767. O esquerdo continua linear (`HF_CURVE_L = 0`);
- **antizona morta do direito** de 8689 para **7000**;
- **proteção no `interruptHandler`**: um relatório sem slot registrado é descartado, em vez de acessar `connectedControllers[-1]`.

No teste de mesa, de 5 a 22 contagens o direito vai de 7105 a 9160, contra 8051 a 11495 no esquerdo linear. A etapa `precisao` agora mede os dois analógicos.

Se a mira ainda estiver sensível, subir `HF_CURVE_R` para 192. Se ficar lenta, baixar para 64.

### Rodada 3 (25/11/2026)

No console, a rodada 2 deixou o direito **preso**: era preciso empurrar mais. O usuário quer uma sensação próxima da nativa do 360, nos dois analógicos.

O analógico do 360 é linear e o jogo aplica a própria zona morta, então:
- **os dois analógicos ficam lineares**, sem curva (`HF_CURVE_L = HF_CURVE_R = 0`);
- a antizona morta fica em cerca de 58 % da zona morta padrão: **5000** no direito e **4500** no esquerdo. É o meio do caminho entre o original (0, zona morta grande) e a rodada 1 (8689, saída brusca);
- a zona morta interna de 4 contagens e a proteção da rodada 2 continuam.

No teste de mesa, de 5 a 22 contagens o direito vai de 5225 a 9063 e o esquerdo de 4729 a 8636.

Para ajustar no console, mexer só na antizona morta:
- ainda com zona morta: subir para ~6500;
- ainda brusco: baixar para ~3500.

### Rodada 4 (26/11/2026): "360 modernizado" e controle plugado no boot

No console, a rodada 3 ficou **boa**. O pedido agora é um 360 original, um pouco mais solto e assertivo, e não precisar replugar o XE 2.

**Consenso da pesquisa:**
- O analógico do 360 original não tem zona morta interna de hardware e tem uma zona morta externa de ~0,5 mm. O centro varia de 7 a 10 %, e é por isso que os jogos usam ~25 % de zona morta ([gamepadla](https://gamepadla.com/xbox-360-controller.html)).
- O ajuste moderno (DS4Windows, Steam Input) usa:
  - antizona morta de 0,10 a 0,20;
  - zona morta interna pequena;
  - zona morta externa de 2 a 5 %;
  - resposta linear.

**O que mudou:**
- **Antizona morta:** 5800 no direito e 5200 no esquerdo, cerca de 66 % da zona morta do jogo. É um pouco mais solto que a rodada 3.
- **Zona morta externa:** a saída chega a 100 % com 121 de 127 contagens (~95 %).
- **Gatilhos (`HfTrigger`):** de 0 a 3 dá 0, a partir de 245 dá 255, e entre os dois é linear.
- **Resposta:** continua linear, com a zona morta interna de 4 contagens.

**Controle plugado no boot, etapa 1:** só leitura, só no build de diagnóstico (`shim/boot.inc`).
- Uma thread espera 15 s e grava no log os endereços e os 64 primeiros bytes do código dos exports 758, 880, 881 e 886.
- Depois, por `KeCallAndWaitForDpcRoutine` no processador 2, grava cada nó de porta: dono em +4, flags em +8, tipo em +11 e os 32 primeiros bytes. **Nada é resetado.**
- A etapa 2 (`UsbdResetDevice` só no nó sem driver) depende desse log.
- A técnica vem do fork [kadirdogan/hiddriver360](https://github.com/kadirdogan/hiddriver360) (kernel 17511, não confirmado). O fork [PrinceGhostFace](https://github.com/PrinceGhostFace/hiddriver360) pula o reset quando há disco USB.

**No console:**
1. Plugar o XE 2 **antes** de ligar.
2. Ligar e esperar ~20 s no Aurora.
3. Replugar o XE 2 e testar a calibração.
4. Trazer o pen drive.

Se o console travar, desligar e voltar o `hidforge-xe2-sticks3-diag.xex`.

### Rodada 5 (26/11/2026): controle plugado no boot, etapa 2

No console, a rodada 4 deixou a **calibração boa**, e a varredura só de leitura não travou nada (`../logs/hidforge-xe2-console5.log`).

O XE 2 plugado no boot ficou no nó `c3 p4` (`E1EBF990`): sem dono, flags `13`, tipo `1c`. Quando o XE 2 foi replugado, o `HID add device` veio com esse mesmo endereço. Os nós com dono, entre eles o do pen drive, não passam na regra. Detalhes no `../conhecimento.md`, seção 8b.

O que o build faz agora:
- 10 s depois do carregamento, confere o código dos exports do kernel;
- varre as portas;
- chama `UsbdResetDevice` **só** nos nós sem driver, um por vez;
- o kernel reenumera o aparelho, e o hook o pega.

Isso vale para os dois builds; só o de diagnóstico grava log.

**No console:**
1. Plugar o XE 2 **antes** de ligar.
2. Ligar e **não replugar**.
3. Em uns 10–15 s ele deve ganhar um slot sozinho.
4. Conferir que o pen drive e o Aurora seguem normais e que plugar e desplugar depois continua funcionando.

No log devem aparecer `boot reset c3 p4 node E1EBF990 ... reset sent` e depois `HID add device`.

Se travar: desligar e voltar o `hidforge-xe2-r4-diag.xex`.

### Rodada 6 (26/11/2026)

No console, a rodada 5 reconheceu o XE 2 plugado no boot (`../logs/hidforge-xe2-console6.log`: `reset sent`, depois `HID add device` e slot 0). Ficaram três pendências.

1. **O DualSense precisava ser religado.** O reset da porta reinicia o XE 2, e ele derruba o Bluetooth. Mudança: as varreduras agora começam 1 s depois do carregamento e se repetem a cada 500 ms até ~12 s, com cada porta resetada no máximo uma vez. O reset acontece logo no começo do boot, então basta ligar o DualSense **depois** de ligar o console.
2. **PS segurado desligava o console.** Removido: agora é um toque no guia por pressionada, e segurar não faz nada.
3. **DualSense por cabo no boot não entrou.** Provavelmente porque ele é composto (áudio + HID), e a regra exclui compostos. Nesta rodada o build de diagnóstico só registra o nó dele; a regra muda na rodada 7.

**No console:**
1. Com o XE 2 plugado e o console desligado, ligar o console e só depois ligar o DualSense. Ele deve jogar sem replug.
2. Testar o PS: um toque abre o guia, e segurar não desliga.
3. O **último** boot antes de trazer o pen drive deve ser só com o DualSense no cabo, sem o XE 2. Anotar se ele entrou.

### Rodada 7 (26/11/2026)

Na rodada 6 (`../logs/hidforge-xe2-console7.log`), o reset do boot saiu 1 s depois do carregamento e o XE 2 ganhou o slot. Mas um DualSense ligado na logo do Xbox ficou **aceso fixo e morto** no Aurora até ser religado. Ligado já no Aurora, funcionou.

A hipótese: o PS que liga o DualSense vira um toque no guia antes de o sistema estar pronto. O XAM fica com a entrada "desviada para o guia" sem guia nenhum na tela.

As correções:
- um PS que já está apertado quando o controle começa só arma o botão;
- nenhum guia nos primeiros 20 s depois do boot.

O build de diagnóstico loga cada guia enviado ou ignorado e cada mudança de `XampInputRoutedToSysapp`.

**No console:**
1. Ligar o DualSense na logo do Xbox. Ele deve comandar no Aurora sem religar.
2. Testar o PS no Aurora: um toque abre o guia.
3. No último boot antes de trazer o pen drive, usar só o DualSense no cabo.

### Conclusão (26/11/2026)

O log da rodada 7 descartou a hipótese do guia. O plugin só carrega cerca de 28 s depois de ligar o console, então um DualSense ligado na logo já está conectado quando a porta do XE 2 é resetada, e fica sem entrada. A regra de uso é **ligar o DualSense depois que o Aurora abrir**.

No pen drive ficou o build limpo `out/hidforge-xe2-r7.xex` (`f8494f76`).

Pendente: o DualSense por cabo plugado no boot. Se for preciso, fazer um boot com o build de diagnóstico e trazer o log.

## Builds

| Arquivo | sha1 | O que é |
|---|---|---|
| `out/hidforge-xe2-diag.xex` | `ff509fd3` | build final + XE 2, com `hidforge.log`, anel de luz e o primeiro relatório no log |
| `out/hidforge-xe2.xex` | `fc40c721` | build final + XE 2, limpo |
| `out/hidforge-xe2-sticks-diag.xex` | `10d771bc` | XE 2 + analógicos tratados, com log |
| `out/hidforge-xe2-sticks.xex` | `d49d963f` | XE 2 + analógicos tratados, limpo |
| `out/hidforge-xe2-sticks2-diag.xex` | `3562163a` | rodada 2 (curva, antizona morta 7000, proteção), com log |
| `out/hidforge-xe2-sticks2.xex` | `176be137` | rodada 2, limpo |
| `out/hidforge-xe2-sticks3-diag.xex` | `e5a7bdcc` | rodada 3 (linear nos dois, antizona morta 5000/4500), com log |
| `out/hidforge-xe2-sticks3.xex` | `edb13653` | rodada 3, limpo |
| `out/hidforge-xe2-r4-diag.xex` | `25fcd57b` | rodada 4 (calibração moderna e gatilhos) + varredura de boot só leitura, com log |
| `out/hidforge-xe2-r4.xex` | `3af8c82e` | rodada 4, calibração, sem varredura e sem log |
| `out/hidforge-xe2-r5-diag.xex` | `a87f30ae` | rodada 5: calibração da rodada 4 + reset do nó sem driver no boot, com log |
| `out/hidforge-xe2-r5.xex` | `d64549aa` | rodada 5, limpo (também reseta no boot) |
| `out/hidforge-xe2-r6-diag.xex` | `19905c5d` | rodada 6: reset cedo (1–12 s), PS sem desligar, log das mudanças nos nós |
| `out/hidforge-xe2-r6.xex` | `0ca65338` | rodada 6, limpo |
| `out/hidforge-xe2-r7-diag.xex` | `c3718f73` | rodada 7: PS de ligar o controle não vira guia (armado na conexão, 20 s de silêncio no boot), com log do guia e do roteamento |
| `out/hidforge-xe2-r7.xex` | `f8494f76` | rodada 7, limpo |

Os dois passaram na auditoria do ELF (zero vetorial/ponto flutuante; cr2 em `interruptHandler` lido só depois de
funções nossas sem `mfcr`).
