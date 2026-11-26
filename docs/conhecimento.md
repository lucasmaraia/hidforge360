# hidforge — conhecimento técnico

Tudo o que foi aprendido construindo um driver de controles PlayStation para o Xbox 360 modelo E
(kernel 2.0.17559.0, exploit ABadAvatar/BadUpdate com XeUnshackle v1.03), compilado com toolchain aberto.
O estado operacional e o "como continuar" ficam no `desenvolvimento.md`; aqui fica o que se aprendeu, para não
redescobrir nada.

Índice:
[1. O projeto](#1-o-projeto) ·
[2. O console](#2-o-console-e-por-que-ele-é-diferente) ·
[3. Toolchain](#3-o-toolchain-aberto-xenon-forge) ·
[4. Auditoria do ELF](#4-auditoria-do-elf-obrigatória-a-cada-build) ·
[5. Arquitetura](#5-arquitetura-do-hidforge) ·
[6. Pilha USB do 360](#6-pilha-usb-do-360) ·
[7. Rotas de saída HID](#7-rotas-de-saída-hid-a-lição-mais-cara-do-projeto) ·
[8. Relatórios dos controles](#8-relatórios-dos-controles) ·
[9. Botão PS](#9-botão-ps) ·
[10. Método de diagnóstico](#10-método-de-diagnóstico) ·
[11. Ambiente](#11-ambiente-e-armadilhas-de-ferramenta) ·
[12. Erros cometidos](#12-erros-cometidos-e-o-que-eles-ensinam) ·
[13. Referências](#13-referências)

---

## 1. O projeto

`hidforge` é um port do **hiddriver360 v0.5** (EinTim23, GPL-3.0) compilado com **xenon-forge**
(landaire, Apache-2.0), rodando como plugin do Dashlaunch, **sem o reset de USB** que trava este console.

Resultado final: DualSense, DualShock 3 e DualShock 4 v2 jogam e vibram; o botão PS tocado abre o guia e
segurado por 2 s desliga o console; o plugin não escreve log nenhum e não toca no anel de luz.

Três peças:

| Peça | Papel |
|---|---|
| `hidforge/port_v05.py` | reescreve o `main.cpp` do upstream em passos nomeados; cada passo **para a compilação** se o texto não bater |
| `hidforge/shim/` | runtime mínimo que substitui `<xtl.h>` e a libc: memória, log opcional, vibração, DS3, desligamento |
| `tools/` | leitor de cabeçalho XEX, teste de rotas HID no PC, teste de vibração no navegador |

O porte por script, em vez de um fork editado à mão, é o que permite subir de versão do upstream: se o texto
mudar, o passo correspondente falha com o nome dele, em vez de produzir um binário silenciosamente errado.

## 2. O console e por que ele é diferente

- **O Aurora e o Dashlaunch rodam de um HD USB.** Isso muda tudo: qualquer coisa que reinicie a pilha de USB
  derruba o disco de onde o sistema roda, e o console trava (luz vermelha ou tela congelada).
- **Da v0.3 em diante o hiddriver360 reinicia o USB ao carregar**
  (`UsbdPowerDownNotification` + `UsbdDriverEntry`). É por isso que só a v0.1 e a v0.2 oficiais funcionam aqui.
  O port tira esse reset e, com ele, os patches de kernel que tornariam um segundo `UsbdDriverEntry` seguro.
  Consequência de projeto: **o controle tem de ser plugado depois que o plugin carrega.**
- A **ordem dos plugins não era a causa** dos travamentos: a v0.5 travou em `plugin1`, inclusive sozinha.
- **Cabeçalhos XEX** (ler com `tools/xexhdr.py`): v0.2 carrega em `0x90B00000` com páginas de 4 KB; v0.3+ em
  `0x81F00000` com páginas de 64 KB; Xbdm em `0x91000000`; JRPC2 em `0x91900000`. Todos com
  `module_flags 0xA`, comprimidos e cifrados retail. **O hidforge usa `0x90B00000`, como a v0.2.** O aviso de
  "conflito de endereço base" com Xbdm e JRPC2 nunca se manifestou.
- O **XeUnshackle v1.03 já aplica o UsbdSecPatch**; instalar esse plugin à parte é redundante.
- `Dumpfile` do Dashlaunch **não** gravou `crashlog.txt` em nenhum dos travamentos — não serve de instrumento
  aqui; o `Dumpfile` fica vazio.

### O que destravou o console (não desfazer)

Os cinco primeiros builds travavam ao plugar o controle. Funcionou quando três coisas valeram juntas:

1. **buffers estáticos** no lugar de alocação de pool **dentro do gancho** `HidAddDevice`;
2. pilha de **64 KiB** na thread de log;
3. `KeFlushCacheRange` + `KeSweepIcacheRange` **depois** de instalar os Detours.

Alocar do pool **na carga do plugin** (`ShimInit`) é seguro — é o que o driver faz com os quatro blocos de
saída. Alocar dentro do contexto de USB não é.

## 3. O toolchain aberto (xenon-forge)

Gera XEX de módulo de sistema (`module_flags 0xA`, com `DllMain`), com imports por ordinal de kernel e XAM.
Compilar com `-m release -m ppc64-ilp32`.

Armadilhas, todas pagas em travamento de console ou em bug silencioso:

- **O clang vetoriza loops.** O parser HID virou VMX, e código vetorial dentro do contexto de USB trava o
  console. As flags `-mno-altivec -mno-vmx128 -mno-vsx -fno-vectorize -fno-slp-vectorize` **não podem sair**
  do `shim/BUCK`.
- **Função sem `return` no fim:** o compilador da Microsoft devolve o que estiver em r3; o clang pode tratar o
  caminho como inalcançável e não emitir retorno. O BUCK usa `-Werror=return-type` e o porte corrige duas
  funções do upstream que caíam do fim.
- **O clang guarda condições em cr2–cr4 através de chamadas** (comprovado no `interruptHandler` e no
  `XamInputGetStateHook`). cr2–cr4 são não voláteis pela ABI, mas não há garantia de que o kernel e o XAM da
  Microsoft os preservem. Daí a auditoria da seção 4.
- **Não há biblioteca C++**: sem `std::string`, `fstream`, `deque`. Por isso o mapeamento por JSON e o
  mapeamento interativo do upstream ficaram fora do porte.
- O **log não pode esperar disco** dentro do contexto de USB — o log vai justamente para o HD USB. Por isso
  `HIDFORGE_SYNC_LOG` é 0 e a escrita acontece numa thread separada.
- Uma thread criada com `ExCreateThread(..., EX_CREATE_FLAG_SYSTEM | 0x18000424)` **sobrevive à troca de
  título** (é o que o upstream faz).

### A flag `HIDFORGE_DIAG`

Todo o diagnóstico vive atrás dela, em `shim/shim.h` e `shim/shim.cpp`:

| | `HIDFORGE_DIAG=0` (padrão) | `HIDFORGE_DIAG=1` |
|---|---|---|
| `DbgPrint` | `((void)0)` — nada no binário | grava em `hidforge.log` no HD |
| `Stage()` / `ShimPollLog()` | inline vazias | estágio no anel de luz, log na thread do título |
| Threads | nenhuma extra | thread de log com batimento de 5 s |
| Anel de luz | nunca tocado | pisca o estágio, via `HalSendSMCMessage` |
| Tamanho do `.xex` | 10 KB | 14 KB |

Isso é o que permite entregar um build limpo sem perder a capacidade de investigar: o diagnóstico é um
`-DHIDFORGE_DIAG=1`, não uma reescrita.

## 4. Auditoria do ELF (obrigatória a cada build)

O `llvm-objdump` fica em
`/work/xenon-forge/buck-out/v2/art/toolchains/*/__ppc64-prebuilt-linux-x86_64__/*/bin/llvm-objdump`
e o ELF ligado é o alvo `image` em `buck-out/.../__hidforge__/`.

**No desmonte o endereço fica na coluna 0.** Um regex com `^\s+` não acha nada e dá um falso "ok" — isso
aconteceu de verdade neste projeto. Extrair os mnemônicos com `awk -F'\t' '/^[0-9a-f]+:/ {print $2}'`.

Duas verificações:

1. **Zero instruções vetoriais e de ponto flutuante**: nada que comece com `v`, `lv`, `stv`, `lf`, `stf`,
   `f[acdimnrs]`, nem `mtvscr`/`mfvscr`/`dcbz128`.
2. **cr2–cr4 nunca lido depois de chamada ao kernel ou ao XAM.** `mfcr` no prólogo marca as funções que usam
   esses campos; para cada condição guardada lá, conferir que entre a escrita e a leitura só há chamadas a
   funções **nossas**, e que essas funções não têm `mfcr` (se não têm, não tocam cr2–cr4).

Estado auditado do build final:

- zero vetorial/ponto flutuante;
- `mfcr` em `interruptHandler`, `XamInputGetStateHook`, `initFunctionPointers` e `Detour::Install`;
- em `interruptHandler`, o `crset 10` (cr2) é lido só depois de `DS3Fill`, `memcpy` e `RumblePump` — todas
  nossas e sem `mfcr` — e antes do `bctrl` do kernel;
- em `XamInputGetStateHook`, o `crmove 8, 1` (cr2, o estado do botão PS) é lido só depois de
  `bl GetTickCount`, que não toca CR nenhum e não chama ninguém; o `bctrl` do `XamInputGetState` original
  acontece antes de qualquer escrita em cr2–cr4.

## 5. Arquitetura do hidforge

### `port_v05.py`

Lê o `main.cpp` da tag v0.5 e aplica passos nomeados (`sub`, `cut`, `mark`). Cada passo exige um número exato
de casamentos; se o upstream mudar, a mensagem diz qual passo quebrou. Os grupos de passos:

- **compatibilidade**: `#include "shim.h"`, `__declspec(align)` → `__attribute__((aligned))`,
  `unsigned __int8` → `uint8_t`, tipos do `HalSendSMCMessage`, renomear ponteiros que colidem com os headers;
- **retornos faltando**: as duas funções que caíam do fim;
- **sem reset de USB**: remove os patches de kernel e a chamada de reinicialização;
- **sem pool no gancho**: extensão HID e buffer de relatório estáticos, por slot;
- **vibração e DS3**: inclui `rumble.inc` e `ds3.inc` e enxerta as chamadas de attach/detach/pump/request;
- **botão PS**: troca o bloco `if (b.ps)` pela versão com duração;
- **estágios** (`Stage(n)`) e `ShimPollLog()`: viram nada com `HIDFORGE_DIAG=0`.

### `shim/`

| Arquivo | Conteúdo |
|---|---|
| `shim.h` | substitui `<xtl.h>`; `DbgPrint`, `Stage`, `ShimPollLog`, `ShimPowerOff`, interlocked, blocos de saída |
| `shim.cpp` | `memset/memcpy/memcmp/malloc/calloc/free` sobre `ExAllocatePoolWithTag`, log opcional, anel de luz opcional, `ShimFlushCode`, `ShimInit`, `ShimPowerOff` |
| `rumble.inc` | vibração dos três controles, escolha de rota, bloco de saída do pool |
| `ds3.inc` | comando de ativação e parser de entrada do DualShock 3 |
| `tramp.S`, `xex.toml`, `BUCK` | trampolim dos Detours, base `0x90B00000`, flags de compilação |

Regra de concorrência da vibração: **a thread do título só registra** os valores pedidos
(`RumbleRequest`); o envio acontece a partir das conclusões de transferência de USB (`RumbleComplete` →
`RumblePump`) e do `interruptHandler`. Assim nunca há mais de uma transferência de saída em voo e a thread do
título nunca mexe na fila de USB. Um efeito inalterado é reenviado a cada 250 ms (`RUMBLE_REFRESH_MS`),
porque alguns controles expiram o efeito.

## 6. Pilha USB do 360

As estruturas são as mesmas nos três códigos de referência (v0.5, PR #129 do hiddriver360, tritondriver360).

### TRB de transferência (0x1C bytes)

```
+0x00  endpoint          (ponteiro que UsbdOpenEndpoint devolve)
+0x04  callback          (conclusão; recebe o endereço do TRB e o status)
+0x08  savedEndpoint     (copiar de endpoint; a fila pode reusar o campo endpoint)
+0x0C  padding[4]
+0x10  flags             (1)
+0x11  controllerIndex
+0x12  pad
+0x13  endpointIndex
+0x14  buffer            (ponteiro para os dados)
+0x18  length
+0x1C  >>> a pilha escreve aqui quantos bytes transferiu <<<
```

> ### ⚠️ TRB + 0x1C não é seu
>
> **Foi o bug que custou mais tempo no projeto.** O bloco de saída era
> `struct { RumbleTrb trb; BYTE report[80]; }`, e como o TRB tem exatamente 0x1C bytes, `report[0]` caía em
> cima do campo onde a pilha escreve a contagem de bytes. O ID do relatório e os bytes de flags de **todo**
> relatório moravam num campo da pilha de USB. O DualSense sobrevivia a isso; o DualShock 4 não, e o
> sintoma era "transferência completa com status 0 e o aparelho ignora", que parece problema de barramento ou
> de conteúdo e não de layout.
>
> O `tritondriver360` reserva esses 4 bytes e protege o offset com um `static_assert` cuja mensagem é
> "Output report overlaps USB transfer bookkeeping". O `rumble.inc` agora faz o mesmo, com três asserts
> (0x1C, 0x20 e o tamanho do bloco do pool). **Nunca ponha dado nosso em TRB + 0x1C.**

### Extensão HID do controle

```
+0x00  deviceHandle
+0x04  TRB de entrada
+0x20  bytes transferidos na entrada
+0x24  TRB de controle          (na v0.5: defaultEndpoint)
+0x40  bytes transferidos no controle
+0x44  setup packet:  bmRequestType, bRequest, wValue, wIndex, wLength
```

Nos nomes que a v0.5 usa: `defaultEndpoint`(+0x24), `keyboardCompletionHandler`(callback),
`defaultEndpoint2`(saved), `byte34`(flags), `dword38`(buffer), `dword3C`(length), e no setup packet
`byte44`/`byte45`/`word46`/`interfaceNumber`/`word4A`.

**Os campos de 16 bits do setup packet são invertidos** — gravar com `swap_endianness_16`.

### Detalhes que economizam horas

- `UsbdQueueAsyncTransfer` devolve um **token**, não um status. O status chega só na conclusão, e `0` é
  sucesso. Testar `NT_ERROR` no retorno da fila é enganoso.
- A conclusão pode rodar **antes** de `UsbdQueueAsyncTransfer` voltar. Marcar "ocupado" **antes** de enfileirar.
- `UsbdGetInterfaceDescriptor(handle)` devolve a interface **HID** do aparelho, e o `index` de
  `UsbdGetEndpointDescriptor(handle, index, tipo, direção)` é o índice do endpoint **dentro** dessa interface,
  não o número da interface.
- O driver preenche o setup packet, na adição do aparelho, com o `SET_IDLE` do HID do kernel
  (`main.cpp:443`), e **wIndex já fica com o número da interface HID, invertido**. Reaproveitar esse valor;
  zerá-lo quebra qualquer controle cuja interface HID não seja a 0.
- O TRB tem de continuar **no mesmo endereço** que o Usbd viu quando o endpoint foi aberto — por isso os
  blocos de saída são alocados uma vez, em `ShimInit`, e nunca movidos.
- **DualSense entra pelo controlador rápido (EHCI); DualShock 3 e 4 pelo de velocidade plena (OHCI)**
  (`UsbdGetDeviceSpeed` devolve 0 no OHCI; o log do upstream chama isso de `IS USB1.0: 1`). Um **hub USB 2.0**
  move um controle para o controlador rápido — serve de experimento sem recompilar.
- Transferência interrupt OUT **funciona nos dois barramentos**, inclusive no OHCI (comprovado com o
  DualShock 3 vibrando por essa rota).

## 7. Rotas de saída HID: a lição mais cara do projeto

Um relatório de saída HID pode ir por dois caminhos, e **qual deles o aparelho aceita é característica do
aparelho**:

| Rota | Como | Dados |
|---|---|---|
| **Endpoint de saída** (interrupt OUT) | `UsbdOpenEndpoint(..., tipo 3, bEndpointAddress, ...)` e enfileirar no TRB próprio | **com** o byte de ID na frente |
| **Endpoint de controle** (`SET_REPORT`) | setup packet `bmRequestType 0x21`, `bRequest 0x09`, `wValue = (tipo << 8) \| ID` (tipo 2 = output, 3 = feature), `wIndex` = interface HID | **sem** o byte de ID; ele viaja no `wValue` |

Pela especificação HID, quando o aparelho declara um endpoint de saída, é por ele que os relatórios vão; o
`SET_REPORT` no controle é o alternativo. Na prática:

- **DualShock 4: só o endpoint de saída.** Recusa o `SET_REPORT` no controle (medido no PC: `WriteFile`
  acende e vibra, `HidD_SetOutputReport` devolve falso). No console a recusa aparecia como `c0051004`.
- **DualShock 3: as duas rotas funcionam.** O comando de ativação dele, porém, é um `SET_REPORT` de
  **feature** `0x03F4` e tem de ir pelo controle.
- **DualSense: endpoint de saída.**

Todos os projetos abertos usam o endpoint de saída para esses controles: Linux `hid-playstation.c`
(`hid_hw_output_report`), SDL (`SDL_hid_write`), USB Host Shield (`outTransfer` no pipe de saída), ds4drv.
Nenhum usa o endpoint de controle para o DS4.

## 8. Relatórios dos controles

### DualSense (054c:0ce6)
Saída `0x02`, **63 bytes**, pelo endpoint: `[1] = 0x01|0x02` (vibração compatível + seleção de haptics),
`[3]` motor direito, `[4]` motor esquerdo.

### DualShock 3 (054c:0268)
**Não manda entrada nenhuma antes do comando de ativação**: `SET_REPORT` de feature `0x03F4` com
`42 0C 00 00`, pelo endpoint de controle (é o que o PR #129 e o PS3USB do USB Host Shield fazem).

Entrada `0x01`: `[2]` select/L3/R3/start/↑→↓← (bits 0 a 7), `[3]` L2/R2/L1/R1/△○✕□, `[4]` bit 0 = PS,
`[6..9]` analógicos, `[18]`/`[19]` pressão de L2/R2.

Saída `0x01`, 48 bytes de dados (49 com o ID, pelo endpoint): `[1]`/`[2]` motor pequeno (duração/ligado),
`[3]`/`[4]` motor grande (duração/força), `[9]` LED do jogador (`0x02 << índice`), `[10..]` quatro blocos
`FF 27 10 00 32` de temporização dos LEDs.

### DualShock 4 v2 (054c:09cc)
**Quatro interfaces**: 0 Audio Control (sem endpoints), 1 e 2 Audio Streaming, **3 HID** — endpoints
`0x84` IN e `0x03` OUT, pacote 64, intervalo 5. No Windows o caminho do device termina em `mi_03`.
O áudio não trafega por USB, mas as interfaces existem no descritor e deslocam o número da interface HID.

Saída `0x05`, **32 bytes com o ID** (31 sem): `[1]` flags (`0x01` motor, `0x02` barra de luz, `0x04` piscar),
`[2]`/`[3]` zero, `[4]` motor direito, `[5]` motor esquerdo, `[6..8]` RGB, `[9]`/`[10]` tempos de piscada.
O Linux manda `0x01` e `0x02` **juntos** mesmo quando só um mudou, porque alguns controles de terceiros
cancelam um ao receber o outro — vale copiar esse comportamento.

Em USB não precisa de leitura prévia nenhuma (ao contrário do Bluetooth, que exige ler a feature de
calibração e assinar os relatórios com CRC-32).

### Brook Wingman Converter XE 2 (0c12:0efa), medido no PC em 25/11/2026
Conversor (aqui com um DualSense nele). **Não tem modo Xbox 360**: só PS4/PS3/Switch/PC. No PC ele se apresenta
como um DS4: **uma interface HID só (a 0)**, endpoints `0x84` IN (64, intervalo 1 ms, ~1000 relatórios/s) e
`0x03` OUT (64, intervalo 5). A entrada `0x01` de 64 bytes é igual à do DS4, e há uma segunda coleção vendor
`fff0`, só com feature de 64 bytes (autenticação do PS4, irrelevante no 360). A saída `0x05` de 32 bytes
**só passa pelo endpoint OUT**: o `HidD_SetOutputReport` falha com erro 203, como no DS4. O hidforge o trata
como `SONY_DUALSHOCK4`. **No 360 ele se apresenta igual** (`c12:efa`, OHCI, primeiro relatório `01 80 80 80 80 08`)
e funciona inteiro: botões, analógicos, vibração, PS tocado e PS segurado.

### Analógicos (`shim/sticks.inc`)

A conversão do upstream, `(v-128)*256`, somada à zona morta do jogo (7849 no esquerdo, 8689 no direito), dava
uma zona morta grande, degraus ao mirar devagar e o Y descentrado (-256).

O que foi medido no XE 2 (`tools/teste-xe2.ps1 -Etapa precisao`):
- atualização a cada 1 ms (mediana), p90 de 4 ms;
- nenhuma zona morta própria;
- em repouso, o analógico para até 2 contagens fora do 128.

O que o usuário aprovou no console, depois de 4 rodadas:
- resposta **linear**, porque a curva meia-quadrática deixou a mira "presa";
- **antizona morta de cerca de 66 % da zona morta do jogo** (5800 no direito, 5200 no esquerdo). Com 100 % (8689)
  a mira saía brusca do centro;
- zona morta interna radial de 4 contagens;
- zona morta externa com saída máxima em 121 de 127 contagens;
- gatilhos com 0 até 3 e 255 a partir de 245.

O estado do slot passa do `interruptHandler` para o hook sob um contador de sequência.

## 8b. Controle já plugado no boot (`shim/boot.inc`)

Sem o reset geral do USB, o que já está plugado no boot nunca chega ao `HidAddDeviceHook`. A saída é resetar
**só o nó da porta** de um aparelho sem driver, pelos exports do kernel. A técnica vem do fork kadirdogan
(17511), e a varredura só de leitura confirmou o layout no 17559 (`logs/hidforge-xe2-console5.log`):

| Ordinal | Função | 17559 | Primeiros bytes |
|---|---|---|---|
| 886 | `KeCallAndWaitForDpcRoutine(dpc, NULL, 2, NULL, NULL)` | `80071D00` | `7d 88 02 a6` |
| 880 | `UsbdGetRootHubDeviceNode(controlador)` | `800D8730` | `3d 60 80 1b 54 6a 10 3a` (tabela em `0x801A8230`, `host+0x48`) |
| 881 | `UsbdGetPortDeviceNode(root, porta)` | `800D8598` | `89 23 00 08 3d 40 80 1b` |
| 758 | `UsbdResetDevice(node)` | `800D8B58` | `38 80 00 01 4b ff ff 34` (igual ao 17511) |

O layout do nó:
- cada nó ocupa 16 bytes;
- o dono fica em +4, as flags em +8 e o tipo em +11;
- `root[8]&3` é igual ao índice do controlador;
- o ponteiro do nó é o mesmo `deviceHandle` que o `HidAddDevice` recebe.

Regra de elegibilidade: `!owner && !(flags&0xE0) && !(type&0xE0) && (type&7)∈{2,4}`. Com o XE 2 plugado no boot,
ela escolheu só o nó dele (`c3 p4`, flags `13`, tipo `1c`). Os nós com dono (um driver do kernel e um módulo em
`0x910…`) ficam de fora, e o pen drive nunca é tocado. Se o código dos exports não bater (outro kernel), o módulo
não faz nada.

**Armadilha encontrada na auditoria:** com o `MmIsAddressValid` embutido em várias checagens, o clang guardou o
resultado combinado em cr2 através da chamada seguinte. A solução foi isolar a chamada numa função não embutida e
combinar os resultados como inteiros. Depois disso, nenhuma função nova tem `mfcr`.

**Resultado no console (rodada 5):** o reset do `c3 p4` reenumerou o XE 2 (`HID add device E1EBFA50`), e ele
ganhou o slot sozinho, sem tocar no pen drive. Mas **resetar a porta reinicia o XE 2, e ele derruba o
Bluetooth** com um DualSense que já estava conectado; o controle teve de ser religado. Por isso, da rodada 6
em diante, as varreduras começam **1 s** depois do carregamento e se repetem a cada 500 ms até ~12 s. Cada
porta é resetada no máximo uma vez, e um nó em transição é visto de novo na varredura seguinte. O uso
recomendado é ligar o console e só então ligar o DualSense.

**DualSense por cabo plugado no boot:** não entrou na rodada 5. A hipótese é que ele é composto (áudio + HID),
e a regra exclui compostos. O nó dele ainda precisa ser lido no log antes de mudar a regra.

Cores da barra por slot de jogador, iguais às do PS4 e às do `hid-sony`: azul, vermelho, verde, rosa. Isso é
útil como **instrumento**: se a barra muda de cor, o relatório de saída foi aceito — diagnóstico visível sem
log nenhum.

## 9. Botão PS

**Não existe API no XAM que receba um guia segurado.** Conferido nos ordinais retail
(`emoose/xbox-reversing/x360_imports.py`) e nos headers do xenon-forge:

- `XamInputSendXenonButtonPress` (ordinal 1286) e `XAutomationpInputXenonButton` (0x3D7) recebem **só** o
  índice do usuário: é um toque, sem press/release, então o XAM nunca mede duração;
- não há nenhum export de menu de energia na lista de `XamShow*` do XAM retail;
- nenhum projeto aberto implementa o segurar (a issue #120 do hiddriver360 pede algo parecido).

**Removido na rodada 6 (26/11/2026), a pedido do usuário:** o PS segurado não desliga mais o console. Agora
é **um toque no guia por pressionada**, na descida do botão, com 1 s de carência; segurar não repete. O
upstream repetia o guia a cada segundo enquanto o botão estava apertado. `ShimPowerOff` continua no `shim.cpp`,
sem nenhuma chamada. O texto abaixo descreve a versão antiga.

**O PS que liga o controle não pode virar guia no boot (rodada 7, hipótese a confirmar pelo log).** Na rodada 6,
um DualSense ligado na logo do Xbox ficou conectado, aceso fixo no XE 2, mas morto no Aurora até ser religado.
Ligado já no Aurora, funcionava. O DualSense liga apertando o PS. Um `XamInputSendXenonButtonPress` enviado tão
cedo deve deixar `XampInputRoutedToSysapp[user]` marcado sem guia nenhum na tela, e o gancho devolve estado
vazio ao título enquanto essa marca existir.

As guardas:
- `g_psWasDown` começa em `true` e é armado de novo no `HidAddDeviceHook`, então um PS que já está embaixo no
  começo só arma o botão;
- nenhum guia nos primeiros 20 s depois do boot (`HF_PS_BOOT_QUIET_MS`).

O build de diagnóstico loga o guia enviado ou ignorado e cada mudança da marca, com o tempo desde o boot.

**Hipótese descartada pelo log da rodada 7** (`logs/hidforge-xe2-console8.log`): nenhum guia saiu no boot, e a
marca só mudou depois de toques reais no Aurora. A causa real:
- **o plugin só carrega por volta de 28 s depois de ligar o console** (`boot reset ... t=28915`);
- um DualSense ligado na logo já está conectado ao XE 2 quando o reset da porta acontece;
- depois do reset, ele fica aceso, mas sem entrada, até ser religado.

Como o plugin não carrega mais cedo, a **regra de uso é ligar o DualSense depois que o Aurora abrir**. As
guardas do PS ficaram, porque não atrapalham. A alternativa não tentada, a pedido do usuário, é adotar o nó
sem reset, chamando o `HidAddDeviceHook` direto pela DPC. O log mostra que o nó adotado difere só no dono
(flags `13` e tipo `1c` iguais). Essa alternativa pode travar o boot.

**Como foi feito (versão antiga):** a duração é medida no próprio gancho `XamInputGetStateHook`. Soltar antes de 2 s dá o
toque de sempre (`XamInputSendXenonButtonPress`, com 1 s de carência); passar de 2 s chama `ShimPowerOff()`
→ `HalReturnToFirmware(HalPowerDownRoutine)` (o enum `FIRMWARE_REENTRY` está em
`xbox360/xboxkrnl/hal.h`; a rotina 5 é o desligamento). Uma pressionada que **deixa de ser vista** — o guia
aberto pode engolir as consultas de certos `flags` — é descartada, nunca vira desligamento nem um segundo
toque no guia. O gancho roda em thread de título, nível passivo, que é lugar seguro para essa chamada.

**Ideia não explorada**, se um dia o menu nativo for desejado: `XAutomationpBindController` +
`XAutomationpInputSetState(user, XINPUT_GAMEPAD*)` alimentando o bit `XINPUT_GAMEPAD_GUIDE` (0x0400)
enquanto o PS está segurado, para o XAM medir a duração ele mesmo. Mexe com slots de usuário e pode não
funcionar.

### Outras respostas sobre entrada

- **Dongle Bluetooth:** o 360 não tem pilha Bluetooth; o plugin teria de implementar tudo. Sem fio que
  funciona hoje: adaptador que se apresenta como controle com fio, como o Sony CUH-ZWA1 (`054c:0ba0`, já
  reconhecido pelo driver, só para DS4).
- **8BitDo Ultimate C 2.4G (81HC)**, o pedido original: **não enumera** no 360, nem por cabo nem pelo dongle
  (issue #122 do hiddriver360). O firmware se desconecta no `SET_CONFIGURATION` porque o 360 não pede os
  string descriptors antes, e depois ainda exige `SET_IDLE` + report descriptor. Isso acontece **antes** de
  qualquer driver, então nem UsbdSec nem hiddriver resolvem. A correção existe só no fork
  `Lucas-Tito/hiddriver-kernel-17150-port`, com endereços do **kernel 17150**; deslocamentos 17150→17559
  levantados: −0xF18 (`UsbdGetInterfaceDescriptor`), −0xF30 (faixa `0x800D9C38`–`0x800E1514`), −0xF68
  (`HidAddDevice`/`HidRemoveDevice`). Não há port para 17559.

## 10. Método de diagnóstico

O que funcionou, em ordem de custo:

1. **Testar o aparelho no PC primeiro.** `tools/teste-rotas-ds4.ps1` (com `tools/hidlib.ps1`) enumera os HID
   por SetupAPI, mostra VID/PID, strings, tamanhos de relatório e o caminho do device, e manda o mesmo
   relatório que o driver manda pelas duas rotas: `WriteFile` (endpoint de saída) e `HidD_SetOutputReport`
   (`SET_REPORT` no controle). Em dois minutos isso respondeu o que três ciclos de console não responderam:
   os bytes estavam certos, os motores funcionavam, e o controle recusa a rota de controle.
   `tools/teste-vibracao.html` faz o equivalente pela Gamepad API, para comparar controles — tem de ser
   **arquivo local no Chrome**, porque a Gamepad API é bloqueada dentro de um artifact.
2. **Instrumento visível vale mais que log.** A barra de luz do DS4 e o LED do DS3 dizem na hora se o
   relatório foi aceito, sem ejetar HD nem ler arquivo. O anel de luz do console serve para o mesmo quando o
   assunto é travamento (`Stage()` com `HIDFORGE_DIAG=1`).
3. **Um teste que separa hipóteses vale mais que um teste que confirma a preferida.** A pergunta "interrupt
   OUT chega a algum aparelho do barramento lento?" foi respondida mandando a vibração do **DS3** — que
   funcionava pela rota de controle — pelo endpoint. Um controle que já funciona é o melhor instrumento para
   testar um caminho.
4. **Comparar com código que funciona em hardware.** O `tritondriver360` roda num 360 real; foi lendo o
   `OutputTransfer` dele que o campo de TRB+0x1C apareceu.

## 11. Ambiente e armadilhas de ferramenta

- Contêiner Docker **`xforge`** (imagem `rust:1-bookworm`, volume `xforge-work`): `/work/xenon-forge`,
  `/work/hid-v05`, `/work/hidforge-src/` (cópia das fontes) e `/work/hf05.sh`, que porta e compila.
- No Git Bash, `export MSYS_NO_PATHCONV=1` antes de comandos `docker` com caminhos `/work/...` — **mas o
  `docker cp` precisa do caminho local em formato Windows** (`C:/...`), porque a variável desliga a conversão.
- **Heredoc e `python -c` no Bash comem barras invertidas.** Para texto com `\` (fonte C, caminhos do
  Windows), escrever um arquivo e rodá-lo, nunca inline.
- `launch.ini` é **CRLF**. Editar por PowerShell com `[System.IO.File]::ReadAllText`/`WriteAllText` e
  `UTF8Encoding($false)`. `sed -i` do Git Bash converte o arquivo todo para LF.
- **A letra do HD muda** entre sessões (foi `E:`, virou `G:`). Conferir antes de gravar.
- **Antes de o usuário ejetar o HD:** fechar o Docker Desktop e rodar `wsl --shutdown` — o WSL segura o disco.
- `Remove-Item` em arquivo na raiz de um drive pode ser bloqueado por ferramentas de automação; `rm` do Bash funciona.
- Um pacote `xbox-sdks` baixado à parte era o SDK do **Xbox original** (22 GB, inútil para o 360). O SDK do 360
  instalado em `C:\Program Files (x86)\Microsoft Xbox 360 SDK` veio só com ferramentas (imagexex, xbWatson,
  xbcp), sem compilador/headers/libs; a instalação "Full" exige o Visual Studio 2010.

## 12. Erros cometidos e o que eles ensinam

| O que pareceu ser | O que era | Lição |
|---|---|---|
| Problema de barramento: "OHCI não entrega interrupt OUT" | o relatório estava em cima de um campo da pilha de USB (TRB+0x1C) | "status 0 e o aparelho ignora" é sintoma de **dado corrompido**, não só de transporte |
| `c0051004` por `wValue`/`wIndex` errados | os dois estavam de fato errados, **mas** o DS4 recusa a rota de controle de qualquer forma | corrigir o que está errado não prova que era a causa; testar o aparelho antes |
| Conteúdo do relatório errado (flags, tamanho) | o conteúdo era byte a byte igual ao do Linux | comparar com a referência **antes** de ciclar variantes |
| Auditoria "limpa" do desmonte | o regex exigia espaço no início da linha e não casava nada | um filtro que nunca acha nada e um código que nunca erra são indistinguíveis — testar o filtro com um caso positivo |
| Auditoria "limpa" do build do XE 2 | o `find` pegou o primeiro `image` de `buck-out`, um ELF de dias antes | auditar o link `image` **ao lado do `hidforge.xex` do build** (`buck-out/v2/art/xbox360/<hash>/src/examples/hidforge/__hidforge__/image`) e conferir o tamanho e a hora do ELF; `/work/audit-elf.sh <ELF>` faz isso |

A ordem certa, aprendida à força: **medir o aparelho no PC → comparar com código que funciona em hardware →
só então mexer no driver.** Três ciclos de ejetar HD, bootar console e testar em jogo foram gastos antes
disso.

## 13. Referências

- **hiddriver360** — https://github.com/EinTim23/hiddriver360 (tag v0.5 é a base; PR #129 traz a vibração do
  DualSense e o comando do DS3; issues #120 guia por combinação, #122 o 8BitDo).
- **xenon-forge** — https://github.com/landaire/xenon-forge (toolchain; `src/sdk/include/xbox360/` tem os
  headers de kernel e XAM; `docs/llvm-xbox360-abi-evidence.md`).
- **tritondriver360** — https://github.com/AustinJMann/tritondriver360 (vibração funcionando num 360;
  `hiddriver/controller_usb.cpp`, `hiddriver/driver_types.h`, `hiddriver/rumble_output.h`, `ARCHITECTURE.md`).
- **Ordinais retail de kernel e XAM** — https://github.com/emoose/xbox-reversing (`x360_imports.py`);
  assinaturas em `xamp.h` do XBLS Research; lista de ordinais em
  https://github.com/exjam/xbox360-emu (`docs/kernel/ordinals/xam.ord`).
- **Formato dos relatórios** — Linux `drivers/hid/hid-playstation.c` (DS4 e DualSense) e
  `drivers/hid/hid-sony.c` (DS3); SDL `src/joystick/hidapi/SDL_hidapi_ps4.c`; USB Host Shield
  `PS4USB.h`/`PS4Parser.h`; https://www.psdevwiki.com/ps4/DS4-USB; https://gimx.fr/wiki (DualShock 4).
- **HID: endpoint de saída como rota padrão** — https://lkml.iu.edu/hypermail/linux/kernel/1005.2/00088.html
- **openxbdm** — https://github.com/landaire/openxbdm (`docs/re-notes/retail-dashlaunch.md`).
