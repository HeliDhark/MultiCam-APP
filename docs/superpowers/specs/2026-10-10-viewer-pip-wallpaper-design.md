# MultiCam Viewer: PiP e papel de parede ao vivo

Data: 2026-10-10. Estado: aguardando revisão do usuário.

## Objetivo

Duas funções novas, independentes entre si, que mostram a câmera gerada pelo MultiCam sem passar por outro aplicativo:

1. **Papel de parede ao vivo:** a câmera de uma placa vira o papel de parede de **um** monitor escolhido, a 30 ou 60 FPS (configurável, para o usuário comparar).
2. **PiP por câmera:** uma janela sem borda para cada câmera gerada, com tamanho e posição escolhidos, que pode ficar sempre à frente e pode ser **travada**.

Critérios de sucesso:

- Cada função liga e desliga pelo painel e volta sozinha quando o programa abre (e com "Iniciar com o Windows").
- Fluidez comparável à da câmera virtual, sem consumo alto de CPU.
- Nenhuma dependência além de DLLs do Windows. O pacote continua portátil.
- Capturas e aplicativos que já usam a câmera virtual não são afetados.

## Decisões do usuário

| Tema | Decisão |
|---|---|
| Papel de parede | Ao vivo (não imagem atualizada de tempos em tempos). |
| FPS do papel de parede | Configurável: 30 ou 60. |
| Vários monitores | A câmera ocupa **um** monitor escolhido; os outros mantêm o papel de parede normal. |
| PiP: mover e redimensionar | Arrastar a janela move; arrastar as bordas redimensiona mantendo a proporção; presets no painel. **A roda do mouse não redimensiona.** |
| PiP: travar | Opção **Travar**. Travado, a janela não move nem redimensiona. |
| PiP: sempre à frente | Opcional por câmera. |
| Persistência | Tamanho e posição ficam salvos. |
| Abordagem | Executável nativo novo com Direct3D 11. |

Premissa minha, aceita: modo *Preencher* (corta) ou *Ajustar* (barras) para o papel de parede.

## Arquitetura

Um executável novo, `MultiCam.Viewer.exe` (x64, C++, Direct3D 11), com dois modos. O painel (`MultiCam.exe`) inicia um processo por PiP ativo e um para o papel de parede, como já faz com os workers de captura.

```
Worker (captura) -> canal compartilhado (3 quadros, sem mutex) -> câmera virtual (aplicativos)
                                                              -> Viewer --pip (um por câmera)
                                                              -> Viewer --wallpaper (um, em um monitor)
Painel -> inicia, para e configura os viewers; viewers -> painel: posição e tamanho do PiP
```

### Fluxo dos quadros

- O viewer usa o mesmo `native/channel.hpp` da câmera virtual: lê sem bloquear, repete o último quadro válido por até 500 ms e depois mostra preto. **Nunca escreve no canal.**
- O canal carrega sempre YUY2, qualquer que seja o formato da câmera: o worker decodifica MJPEG com o turbojpeg antes de gravar (`native/worker.cpp`). O viewer só precisa de YUY2.
- O quadro sobe como textura `R8G8B8A8_UNORM` de largura `w/2` (cada texel guarda `Y0 U Y1 V`). Um pixel shader converte para RGB. O shader é compilado **no build** (bytecode embutido), sem `d3dcompiler` em tempo de execução.

### Ciclo de vida

- O painel inicia o viewer quando a função está ativa e a câmera existe. O viewer observa o processo do painel e um evento de parada e encerra se o painel morrer, como o worker já faz.
- O painel envia comandos pela entrada padrão (por exemplo, `lock 1`, `topmost 0`, `size 640 360`, `quit`). O viewer responde pela saída padrão com linhas JSON: FPS desenhados, e posição e tamanho finais do PiP depois de mover ou redimensionar.
- Se o worker reiniciar, os viewers continuam abertos mostrando o último quadro e depois preto.

## Componentes

### `native/viewer.cpp` (novo, `MultiCam.Viewer.exe`)

Argumentos: `--pip <id> ...` e `--wallpaper <id> ...`. Cada modo recebe o id (GUID) da câmera e a configuração inicial. Auxiliares: `--snapshot arquivo.bmp` (salva o quadro desenhado, para testes).

**Modo `--pip`:**
- Janela `WS_POPUP` sem borda, com `WS_EX_TOOLWINDOW` (fora da barra de tarefas) e, opcionalmente, `WS_EX_TOPMOST`.
- Mover: arrastar o corpo da janela (`WM_NCHITTEST` retorna `HTCAPTION`). Redimensionar: bordas de 6 px retornam `HTLEFT/HTRIGHT/...`, com a proporção da câmera mantida em `WM_SIZING`.
- Travado: `WM_NCHITTEST` retorna `HTCLIENT` em tudo, então a janela não move nem redimensiona.
- Menu do botão direito: *Travar*, *Sempre à frente*, *Fechar PiP*. Alterações são devolvidas ao painel.
- Tamanho mínimo e máximo razoáveis; a janela nunca é criada fora de um monitor existente.

**Modo `--wallpaper`:**
- Envia `0x052C` ao `Progman` para criar o `WorkerW`, encontra o `WorkerW` correto, anexa a janela com `SetParent` e a dimensiona ao retângulo do monitor escolhido.
- Renderiza a 30 ou 60 FPS com um temporizador de alta precisão. Pausa o desenho se o monitor estiver coberto por uma janela em tela cheia ou a sessão estiver bloqueada.
- *Preencher* (corta, mantém proporção) ou *Ajustar* (barras pretas), calculado no shader.
- Ao encerrar, o Windows volta ao papel de parede normal. O viewer força a atualização do papel de parede ao sair.

### `native/channel.hpp`

Sem mudanças de formato. Só leitura pelo viewer.

### `app/MultiCam.cs` (painel)

- **Editor por câmera:** *PiP ativo*, *Sempre à frente*, *Travar*, presets de tamanho (calculados com a proporção do modo da câmera) e *Restaurar posição*.
- **Seção "Papel de parede":** ativar, câmera de origem, monitor, 30 ou 60 FPS, *Preencher/Ajustar*.
- **Lista de câmeras:** coluna *Exibição* com o resumo (por exemplo, "PiP · papel de parede").
- **Gerência de viewers:** classe nova, no estilo de `Session`, que inicia, vigia e reinicia os viewers, com o mesmo limite de 3 falhas seguidas já usado para os workers.

### `settings.json`

Todos os campos novos são opcionais, e arquivos antigos carregam com tudo desligado.

- `Camera.Pip`: `Enabled`, `TopMost`, `Locked`, `X`, `Y`, `Width`, `Height`.
- `Config.Wallpaper`: `Enabled`, `CameraId`, `Monitor`, `Fps` (30 ou 60), `Fit` (`fill` ou `fit`).
- Posição e tamanho em pixels do monitor. Se o monitor sumir, a janela vai para o monitor principal.

### Build

- `CMakeLists.txt`: alvo `MultiCam.Viewer` (x64), ligando `d3d11`, `dxgi`, `user32`, `gdi32`, `shcore` e o que for necessário. O shader é compilado pelo `fxc` do Windows SDK durante o build.
- `build.ps1` copia o viewer para `dist/x64`. A toolchain portátil em `.buildtools` precisa ter o `fxc.exe`; se não tiver, o plano de implementação trata disso.

## Erros

- Viewer que morre: reiniciado até 3 falhas seguidas; a mensagem aparece no painel.
- Monitor escolhido ausente: o papel de parede pausa e avisa, sem trocar de monitor sozinho.
- A técnica do `WorkerW` falhando: o viewer sai com um erro claro, que o painel mostra. Sem tela preta.
- Janela PiP fora de qualquer monitor ao abrir: reposicionada no monitor principal.
- Canal ausente ou câmera sem quadros: o viewer mostra o último quadro por 500 ms e depois preto, igual à câmera virtual.

## Testes

- O teste de integração atual (`tests/integration.ps1`) continua valendo.
- Teste novo: o produtor simulado (`MultiCam.Integration --produce`) alimenta o canal, o viewer roda com `--snapshot`, e o teste confere as cores (conversão YUY2 para RGB), a proporção e o modo *Preencher/Ajustar*.
- Teste de travar: com `lock 1` os comandos de mover e redimensionar não mudam a geometria.
- Compatibilidade de configuração: um `settings.json` antigo (sem campos novos) carrega e tudo fica desligado.
- Verificação de portabilidade: o viewer importa só DLLs do Windows.
- Manual, no PC do usuário: medir os FPS a 30 e a 60 (o viewer imprime os FPS desenhados), e confirmar o comportamento do papel de parede com ícones e barra de tarefas.

## Fora do escopo

- Tela de bloqueio como papel de parede.
- Mais de um monitor com câmeras diferentes ao mesmo tempo.
- Efeitos, filtros, espelhamento, áudio.
- Roda do mouse para redimensionar o PiP (removida a pedido).
- Windows 11 24H2: a técnica do `WorkerW` mudou nessa versão. O alvo é o Windows 10 do usuário (build 19044); em versões novas o viewer deve falhar com mensagem clara, e uma adaptação fica para depois.

## Riscos

- **Papel de parede (`WorkerW`):** comportamento depende da versão do Windows. Mitigação: validar no PC do usuário e falhar com mensagem clara.
- **60 FPS no papel de parede:** depende de a placa de vídeo manter a conversão no shader sem custo. Mitigação: o usuário compara 30 e 60 FPS, e o viewer imprime os FPS reais.
- **`fxc` na toolchain portátil:** precisa estar no Windows SDK baixado. A verificar no plano.
