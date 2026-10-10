# Dependências e modificações

`third_party/libdshowcapture`: OBS Project, LGPL-2.1-or-later.
Origem: https://github.com/obsproject/libdshowcapture
Commit base: `c13d4b7b0c66979396ba0a9060c9aafc15bb7b22`.
Licença completa: `third_party/libdshowcapture/COPYING`.

Modificações locais em 2026-09-21:

- `source/dshow-base.cpp`: seleção estrita por DevicePath e ativação somente do moniker escolhido, para nunca cair em outra placa com nome igual.
- `source/output-filter.cpp`: validação de tipos de mídia contra os modos anunciados; rejeição de alteração de formato durante conexão; retirada de anúncio indevido de IMemInputPin; descommit do alocador para liberar a thread de saída no encerramento.
- O CMake raiz compila os fontes genéricos e substitui controles HDR específicos de fabricante por funções sem efeitos em `native/vendor-sdr.cpp`. Não envia comandos HID/tonemapping a outras placas e não requer ATL.

O diretório `external/capture-device-support` está preservado na cópia de desenvolvimento, mas não é compilado neste perfil SDR. Origem: https://github.com/elgatosf/capture-device-support, commit `fe9630974d47f51bf54826e72fb8b654e620aa93`, licença MIT, arquivo LICENSE no próprio diretório.

O código próprio deste protótipo é disponibilizado sob LGPL-2.1-or-later, com o texto em `LICENSE`. A distribuição local inclui código-fonte e instruções de compilação, permitindo modificar e recompilar a biblioteca vinculada estaticamente. Não redistribua apenas os binários sem cumprir as licenças correspondentes.

`third_party/turbojpeg`: libjpeg-turbo 3.2.0. Cabeçalho e licenças oficiais de https://github.com/libjpeg-turbo/libjpeg-turbo/tree/3.2.0. Biblioteca x64 proveniente do runtime local fornecido com o ambiente (pacote Poppler); SHA-256 `1236c17093564d6a830daa214bde1c5de45853ae951a8f37c419af893d4fdcac`. A DLL depende apenas de KERNEL32.dll. Licenças BSD/IJG completas em `third_party/turbojpeg/LICENSE.md` e `README.ijg`, também copiadas à distribuição. Pode ser substituída por build oficial compatível da API TurboJPEG. O suporte usa API dinâmica e não precisa de biblioteca de importação.
