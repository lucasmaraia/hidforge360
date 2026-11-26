# Desenvolvimento

Como compilar, auditar, testar e investigar o hidforge. O **porquê** de cada regra está em
[`conhecimento.md`](conhecimento.md), que reúne a pilha USB do 360, as rotas de saída HID, os relatórios de cada
controle, as armadilhas do toolchain e os erros já cometidos. Leia os dois antes de mexer no código.

## Como o projeto é montado

O hidforge **não é um fork editado à mão**. O código original do hiddriver360 v0.5 fica intocado em
[`hiddriver/`](../hiddriver/), e o [`hidforge/port_v05.py`](../hidforge/port_v05.py) gera o `main.cpp` final
aplicando **passos nomeados**: substituições de texto exatas que **param a compilação** se o texto do upstream
não bater. Assim, subir de versão do upstream mostra exatamente o passo que precisa ser revisto, em vez de
produzir um binário silenciosamente errado.

| Caminho | O que é |
|---|---|
| `hiddriver/` | fonte do hiddriver360 v0.5 (EinTim23, GPL-3.0), sem alterações; é a entrada do port |
| `hidforge/port_v05.py` | os passos do port, cada um com nome e comentário dizendo por que existe |
| `hidforge/shim/shim.h`, `shim.cpp` | runtime mínimo no lugar do `<xtl.h>` e da libc: memória, log opcional, blocos de saída, desligamento |
| `hidforge/shim/rumble.inc` | vibração do DualSense, do DS4 e do DS3 (endpoint de saída, com o controle como reserva) |
| `hidforge/shim/ds3.inc` | DualShock 3: comando de ativação e layout do relatório |
| `hidforge/shim/sticks.inc` | tratamento dos analógicos e dos gatilhos, estado sob contador de sequência, estado do botão PS |
| `hidforge/shim/boot.inc` | controles plugados no boot: varredura das portas e reset só do nó sem driver |
| `hidforge/shim/BUCK`, `xex.toml`, `tramp.S` | flags de compilação, cabeçalho do XEX (base `0x90B00000`, como a v0.2), trampolim |
| `hidforge/tests/sticks_test.c` | teste de mesa dos analógicos e gatilhos (gcc do host) |
| `hidforge/legacy/port.py` | port antigo, sobre o PR #129 do upstream; só histórico |
| `tools/` | build, auditoria, testes no PC e leitor de cabeçalho XEX |
| `hidprobe/`, `patches/` | sonda usada no diagnóstico inicial e o patch de manter o USB ligado; só histórico |

## Compilar

### Pelo GitHub Actions

Todo push na branch `hidforge` roda o [`ci.yml`](../.github/workflows/ci.yml): compila as duas variantes, audita
os ELFs, roda o teste de mesa e publica os `.xex` como artifact. Uma tag `v*` roda o
[`release.yml`](../.github/workflows/release.yml), que faz o mesmo e cria a release com as notas da versão no
[`CHANGELOG.md`](../CHANGELOG.md) e os SHA-1 dos arquivos.

### Localmente

Precisa de Linux. No Windows, um contêiner Docker da imagem `rust:1-bookworm` resolve:

```sh
docker run --rm -it -v "$PWD:/repo" -w /repo rust:1-bookworm bash tools/build.sh dist
```

O [`tools/build.sh`](../tools/build.sh):
1. clona o [xenon-forge](https://github.com/landaire/xenon-forge) no commit fixado
   (`f369053666c0d56176c8fdefd7d87f3dcc168c63`) em `.xenon-forge/`. Dá para apontar para um clone existente com
   `XENON_FORGE_DIR`;
2. roda o teste de mesa;
3. porta e compila com `buck2` as variantes `HIDFORGE_DIAG=0` e `HIDFORGE_DIAG=1`;
4. audita os dois ELFs e para em qualquer achado;
5. grava `dist/hidforge.xex`, `dist/hidforge-diag.xex` e `dist/SHA1SUMS`.

O build é **reproduzível**: o `hidforge.xex` da v1.0.0 gerado por esse script é idêntico, byte a byte, ao que foi
testado no console (sha1 `f8494f76`). O de diagnóstico muda a cada build, porque leva `__DATE__`/`__TIME__` no
log.

### As duas variantes

- **`HIDFORGE_DIAG=0`**, o `hidforge.xex`: o build normal, sem log, sem piscar o anel de luz e sem thread extra
  de log.
- **`HIDFORGE_DIAG=1`**, o `hidforge-diag.xex`:
  - grava `hidforge.log` na raiz do disco USB, sobrescrito a cada boot;
  - mantém uma thread de batimento;
  - mostra no anel de luz o estágio do driver (`Stage()`);
  - registra o primeiro relatório de cada slot, a varredura de boot, o guia e o roteamento de entrada do XAM.

  Ele serve para investigar no console sem reescrever nada.

### Flags que não podem sair do `BUCK`

`-mno-altivec -mno-vmx128 -mno-vsx -fno-vectorize -fno-slp-vectorize`: código vetorial dentro do contexto de USB
travava o console. A auditoria confere que nenhuma instrução vetorial ou de ponto flutuante escapou.

## Auditoria do ELF (obrigatória)

O [`tools/audit-elf.sh`](../tools/audit-elf.sh) roda em todo build e falha se:

1. existir **qualquer instrução vetorial ou de ponto flutuante** (`v*`, `lv*`, `stv*`, `lf*`, `stf*`, `f*`,
   `mtvscr`, `dcbz128`);
2. uma função **fora da lista revisada** tiver `mfcr`. O `mfcr` no prólogo indica que a função guarda condições
   em cr2–cr4, e cr2–cr4 não são confiáveis depois de chamadas ao kernel ou ao XAM neste console.

Para pôr uma função nova nessa lista, revise à mão cada leitura de cr2–cr4 contra as chamadas anteriores. Entre a
escrita e a leitura só podem existir chamadas a funções nossas que também não tenham `mfcr`. Isso já pegou dois
casos no `boot.inc`, contados no [`conhecimento.md`](conhecimento.md), seção 8b. A solução foi isolar a chamada
ao kernel numa função não embutida e combinar os resultados como inteiros.

No desmonte do `llvm-objdump`, o endereço fica na **coluna 0**. Uma regex com `^\s+` não encontra nada e dá um
falso "ok".

## Testar

1. **No PC, antes do console.** Controle novo: rodar o [`tools/teste-rotas-ds4.ps1`](../tools/teste-rotas-ds4.ps1)
   ou o [`tools/teste-xe2.ps1`](../tools/teste-xe2.ps1), com a biblioteca [`tools/hidlib.ps1`](../tools/hidlib.ps1),
   e saber qual rota de saída o aparelho aceita e o tamanho do relatório **antes** de escrever código.
   `teste-xe2.ps1 -Etapa precisao` mede o ruído, a zona morta e a taxa real de atualização dos analógicos.
2. **Teste de mesa:** `gcc -std=c99 -Wall -Wextra -O2 hidforge/tests/sticks_test.c && ./a.out`.
3. **No console**, com o `hidforge-diag.xex` primeiro. Os procedimentos estão em
   [`instalacao.md`](instalacao.md), e o diário completo do XE 2 em
   [`testes/wingman-xe2.md`](testes/wingman-xe2.md).

## Regras que não podem ser quebradas

- **Nunca** pôr dado nosso em TRB + 0x1C, onde a pilha USB grava os bytes transferidos. Os `static_assert` em
  `shim/rumble.inc` quebram a compilação se alguém tentar.
- **Sem reset geral do USB.** Ele derruba o disco USB de onde o Aurora e o Dashlaunch rodam e trava o console. Só
  o reset de um nó sem driver é permitido (`shim/boot.inc`).
- `launch.ini` é **CRLF**. O `sed -i` do Git Bash converte o arquivo para LF; edite pelo PowerShell com
  `[System.IO.File]::ReadAllText`/`WriteAllText` e `UTF8Encoding($false)`.
- Antes de ejetar o disco do console do PC, feche o Docker Desktop e rode `wsl --shutdown`. O WSL segura o disco.
- Não fazer patch binário no `.xex` do autor original.

## Ambiente em que tudo foi testado

- Xbox 360 **modelo E**, kernel **2.0.17559.0**, ABadAvatar/BadUpdate com XeUnshackle BETA v1.03.
- Aurora e Dashlaunch rodando de um disco USB (HD e, depois, pen drive FAT32 de 16 GB).
- `launch.ini`: `plugin1 = Usb:\hidforge.xex`, junto com Xbdm e JRPC2.

## Referências

- hiddriver360: https://github.com/EinTim23/hiddriver360 (tag v0.5; PR #129 com a vibração do DualSense).
- xenon-forge: https://github.com/landaire/xenon-forge (toolchain; `src/sdk/include/xbox360/`,
  `docs/llvm-xbox360-abi-evidence.md`).
- tritondriver360: https://github.com/AustinJMann/tritondriver360 (vibração funcionando no 360).
- hiddriver360 do kadirdogan: https://github.com/kadirdogan/hiddriver360 (reset por nó no boot, kernel 17511).
- Ordinais retail do XAM e do kernel: https://github.com/emoose/xbox-reversing (`x360_imports.py`) e
  https://github.com/exjam/xbox360-emu (`docs/kernel/ordinals/`).
- Formatos de relatório: Linux `drivers/hid/hid-playstation.c` e `hid-sony.c`, SDL `SDL_hidapi_ps4.c`, USB Host
  Shield `PS4USB.h`.
