# Contribuindo

Obrigado pelo interesse! O hidforge roda dentro do kernel de um console sem depurador, então um erro aparece
como luz vermelha, e não como mensagem. As regras abaixo existem por causa disso.

## Antes de abrir um PR

1. **Leia** o [`docs/desenvolvimento.md`](docs/desenvolvimento.md) e o [`docs/conhecimento.md`](docs/conhecimento.md).
2. **Meça no PC antes de mexer no driver.** Para um controle novo, rode o `tools/teste-rotas-ds4.ps1` ou o
   `tools/teste-xe2.ps1`, para saber o VID/PID, o formato do relatório e qual rota de saída ele aceita.
3. **Não edite o `hiddriver/`.** Ele é o fonte do upstream. Mudanças entram como **passos nomeados** em
   `hidforge/port_v05.py`, cada um com um comentário dizendo o porquê, ou como código no `hidforge/shim/`.
4. **Rode o build completo:** `tools/build.sh`, ou deixe o CI rodar. Ele compila as duas variantes, roda o teste
   de mesa e a auditoria do ELF. Uma função nova com `mfcr` exige revisão manual de cr2–cr4, descrita em
   `docs/desenvolvimento.md`.
5. **Teste no console com o `hidforge-diag.xex`** e anexe o `hidforge.log` ao PR.

## Estilo

- Comentários de código em inglês. A documentação em `docs/` fica em português.
- Somente aritmética inteira no driver. Nada de ponto flutuante nem código vetorial, e a auditoria confere.
- Nada de dado nosso em TRB + 0x1C e nada de reset geral do USB.

## Versões

O [`CHANGELOG.md`](CHANGELOG.md) segue o Keep a Changelog. Para publicar, mova o `[Unreleased]` para uma versão
nova e crie a tag `vX.Y.Z`. O workflow `release.yml` compila, audita e publica a release com as notas dessa seção.

## Licença

Ao contribuir, você concorda que sua contribuição fica sob a [GPL-3.0](LICENSE), a mesma do hiddriver360.
