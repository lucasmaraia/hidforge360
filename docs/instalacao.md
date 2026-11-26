# Instalação e uso

## Pré-requisitos

- Xbox 360 com código não assinado rodando (RGH/JTAG ou BadUpdate) e **Dashlaunch**.
- Kernel **17559** (testado) ou 17489. Em outro kernel o plugin não carrega, e a varredura de boot não age.
- O plugin roda sem o reset geral do USB, então funciona com o Aurora e o Dashlaunch num **disco USB**.

## Instalar

1. Baixe o `hidforge.xex` da [última release](https://github.com/lucasmaraia/hidforge360/releases/latest) e confira o SHA-1 com o `SHA1SUMS`.
2. Copie para a raiz do disco onde está o `launch.ini`.
3. No `launch.ini`, aponte um slot de plugin para ele, por exemplo:
   ```ini
   plugin1 = Usb:\hidforge.xex
   ```
   O `launch.ini` é **CRLF**. Use um editor que preserve as quebras de linha do Windows.
4. Ejete o disco com segurança e ligue o console.

Para desinstalar, apague a linha do plugin ou volte o `launch.ini` anterior.

## Uso

| Situação | O que fazer |
|---|---|
| Controle por cabo (DualSense, DS4, DS3) | plugar **depois** que o Aurora abrir |
| Brook Wingman XE 2 | pode ficar plugado desde o boot; ele é reconhecido sozinho uns segundos depois do carregamento do plugin |
| DualSense via XE 2 | ligar o DualSense **depois que o Aurora abrir** (veja abaixo) |
| Botão PS | um toque abre o guia do Xbox; segurar não faz nada |

**Por que ligar o DualSense só depois do Aurora:** o plugin só carrega por volta de 28 s depois de ligar o
console. Para assumir o XE 2 que já estava plugado, ele reseta a porta USB do adaptador, e o adaptador reinicia.
Se o DualSense já estava conectado nesse momento, ele fica aceso mas sem entrada até ser religado. Os detalhes
estão em [`conhecimento.md`](conhecimento.md), seção 9.

## Investigar um problema

Use o `hidforge-diag.xex` da release no lugar do `hidforge.xex`:
- ele grava `hidforge.log` na raiz do disco USB, sobrescrito a cada boot;
- mostra no anel de luz o estágio do driver.

O log traz:
- o VID/PID de cada aparelho;
- o primeiro relatório de cada slot;
- a vibração;
- a varredura de boot;
- os toques no PS.

Abra uma issue com o log e o que aconteceu.

## Limitações conhecidas

- **DualSense por cabo plugado no boot** não entra sozinho; é preciso replugar. Ele é um aparelho composto
  (áudio + HID), e a regra de segurança da varredura não reseta compostos.
- O XE 2 não repassa a cor da barra de luz ao DualSense. Isso não afeta nada.
- Não há suporte a Bluetooth direto: o 360 não tem pilha Bluetooth. Para jogar sem fio, use um adaptador que se
  apresente como controle com fio, como o XE 2.
