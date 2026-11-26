# Changelog

Todas as mudanças relevantes do hidforge. O formato segue o [Keep a Changelog](https://keepachangelog.com/pt-BR/1.1.0/),
e as versões seguem o [Versionamento Semântico](https://semver.org/lang/pt-BR/).

## [Unreleased]

## [1.0.0] - 2026-11-26

Primeira versão publicada. É um port do hiddriver360 v0.5 compilado com o toolchain aberto xenon-forge e testado
num Xbox 360 modelo E, kernel 17559, com Aurora e Dashlaunch rodando de um disco USB.

### Adicionado
- **Roda sem o reset geral do USB.** A v0.3+ do upstream reinicia o USB ao carregar, o que derruba o disco USB de
  onde o sistema roda e trava o console. Sem o reset, o plugin funciona com Aurora e Dashlaunch em disco USB.
- **Vibração** no DualSense, no DualShock 4 e no DualShock 3, pelo endpoint de saída, com o endpoint de controle
  como reserva. O DS4 recebe a cor do slot do jogador na barra de luz.
- **DualShock 3** (`054c:0268`): comando de ativação, layout do relatório e LED do jogador.
- **Brook Wingman Converter XE 2** (`0c12:0efa`), tratado como DualShock 4. Com ele, um DualSense ou outro
  controle Bluetooth joga sem fio, com vibração.
- **Tratamento dos analógicos** (`shim/sticks.inc`):
  - zona morta interna radial de 4 contagens, contra a deriva;
  - antizona morta de 5800 no direito e 5200 no esquerdo, cerca de 66 % da zona morta do XInput, para tirar a
    folga dupla com o jogo;
  - zona morta externa, com saída máxima a 95 % do curso;
  - resposta linear, como no controle do 360;
  - Y centrado.
- **Curva dos gatilhos:** de 0 a 3 dá 0, a partir de 245 dá 255, e entre os dois é linear.
- **Controle plugado no boot** (`shim/boot.inc`): a varredura das portas USB reseta **só o nó sem driver**, e o
  kernel reenumera aquele aparelho para o driver. O disco USB nunca é tocado, e um kernel diferente faz o módulo
  não agir.
- **Build de diagnóstico** (`HIDFORGE_DIAG=1`): `hidforge.log` no disco USB e o estágio do driver no anel de luz.
- Ferramentas de teste no PC (`tools/`), teste de mesa dos analógicos, build reproduzível (`tools/build.sh`),
  auditoria do ELF como portão (`tools/audit-elf.sh`), CI e releases pelo GitHub Actions.

### Alterado
- **Botão PS:** um toque abre o guia uma única vez por pressionada. O upstream repetia o guia a cada segundo
  enquanto o botão ficava apertado. Um PS que já está apertado quando o controle conecta só arma o botão, e não há
  guia nos primeiros 20 s do boot.
- Os buffers do driver passaram a ser estáticos por slot, sem pool dentro dos ganchos, com pilha de 64 KiB e
  flush de cache depois de instalar os Detours. Sem isso, as versões oficiais travavam este console.

### Corrigido
- O relatório de saída ficava em TRB + 0x1C, onde a pilha USB escreve os bytes transferidos. Agora há um campo
  reservado, protegido por `static_assert`.
- `SET_REPORT` com o `wValue`/`wIndex` errados no caminho do controle.
- Leitura de `connectedControllers[-1]` quando chegava um relatório de um slot não registrado.
- Leitura misturada do estado entre o `interruptHandler` e o `XamInputGetStateHook`, agora resolvida com um
  contador de sequência.
- O Y invertido saía descentrado (-256), e a conversão linear de 8 bits somava a zona morta do jogo.

### Removido
- O desligamento do console ao segurar o PS por 2 s, que existiu numa versão interna.

### Limitações conhecidas
- Um DualSense ligado **antes** de o Aurora abrir, via XE 2 plugado no boot, fica sem entrada até ser religado,
  porque o reset da porta acontece depois que o plugin carrega, por volta de 28 s.
- O DualSense **por cabo** plugado no boot ainda precisa ser replugado, porque é um aparelho composto.

[Unreleased]: https://github.com/lucasmaraia/hidforge360/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/lucasmaraia/hidforge360/releases/tag/v1.0.0
