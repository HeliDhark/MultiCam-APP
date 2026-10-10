# MultiCam Viewer (PiP e papel de parede ao vivo) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Adicionar o `MultiCam.Viewer.exe`, que mostra a câmera gerada como papel de parede ao vivo (30 ou 60 FPS, em um monitor) e como janelas PiP sem borda (uma por câmera, com travar e sempre à frente), controlado pelo painel.

**Architecture:** Um executável nativo x64 novo (C++/Direct3D 11) lê o canal compartilhado de 3 quadros que já existe (`native/channel.hpp`), converte YUY2 para RGB na GPU e desenha em uma janela PiP ou ancorada atrás dos ícones (`WorkerW`). O painel C# inicia um processo por função, envia comandos pela entrada padrão e recebe eventos JSON pela saída padrão. A lógica sem Win32 fica em um header testável (`native/viewer_core.hpp`).

**Tech Stack:** C++17, Direct3D 11 + DXGI, HLSL compilado no build com `fxc`, C# (.NET Framework 4.8, WinForms), CMake + Ninja, PowerShell para testes.

**Spec:** `docs/superpowers/specs/2026-10-10-viewer-pip-wallpaper-design.md`

## Global Constraints

- O viewer é x64 e só lê o canal: **nunca escreve** nele e nunca bloqueia (usa `Channel::read`).
- Nenhuma dependência além de DLLs do Windows. O shader é compilado no build (bytecode embutido); **sem `d3dcompiler` em tempo de execução**.
- O canal carrega sempre YUY2 em faixa limitada (16–235/16–240), qualquer que seja o formato da câmera (MJPEG é convertido em `native/jpeg.hpp`). Matriz de cor: BT.709 se a altura ≥ 720, senão BT.601.
- Sem quadro novo: repetir o último quadro válido por até 500 ms e depois mostrar preto (mesma regra da câmera virtual).
- Papel de parede: 30 ou 60 FPS; modo `fill` (cortar) ou `contain` (barras); um único monitor escolhido; os outros monitores ficam como estão.
- PiP: arrastar o corpo move; bordas redimensionam mantendo a proporção da câmera; **a roda do mouse não redimensiona**; travado, a janela não move nem redimensiona; fora da barra de tarefas.
- Campos novos de `settings.json` são opcionais; arquivos antigos carregam com tudo desligado.
- Alvo: Windows 10 (build 19044). Em Windows onde o `WorkerW` não existe, o viewer sai com erro claro (código 5), sem tela preta.
- Textos do painel e mensagens de erro em português. Comentários existentes não relacionados são preservados.
- Git: o repositório ainda não tem commits e `android/` guarda artefatos grandes. Todo commit usa **caminhos explícitos** (`git add <arquivos>`), nunca `git add .`/`-A`. Se `git` reclamar de propriedade, use `git -c safe.directory='D:/Multi App' ...`.
- Compilar: `./build.ps1` (usa Visual Studio se houver, senão `.buildtools`). Executáveis de teste: `. tests/native.ps1; Build-NativeTarget MultiCam.<Alvo>` (criado na Task 1).

## Review Focus

Cada linha tem o teste que a fixa, na tarefa dona do código:

1. Geometria salva de um monitor que não existe mais ou fora da tela: janela volta para dentro de um monitor existente (Task 1 `clamp_*`; Task 5 `normalize_offscreen`).
2. A câmera muda de modo com o viewer aberto (16:9 para 4:3): textura e janela se adaptam, sem travar (Task 2 `size_change`; Task 3 `pip_mode_change`).
3. O viewer abre antes de o canal existir (câmera ainda iniciando): preto, sem erro, e pega os quadros quando chegam (Task 3 `pip_snapshot_color`).
4. O painel morre de repente: o viewer sai sozinho e o papel de parede volta ao normal (Task 3 `parent_exit`).
5. Linhas lixo na entrada padrão e valores inválidos em `settings.json`: ignorados ou normalizados, sem derrubar nada (Task 1 `cmd_invalid`; Task 3 `garbage_stdin`; Task 5 `normalize_values`).

---

### Task 1: Lógica pura (`viewer_core.hpp`) e infraestrutura de testes nativos

**Files:**
- Create: `native/viewer_core.hpp`
- Create: `tests/viewer_core.cpp`
- Create: `tests/native.ps1`
- Modify: `CMakeLists.txt` (alvo `MultiCam.ViewerCoreTest`, `EXCLUDE_FROM_ALL`)
- Modify: `build.ps1` (parâmetro `-Target`)

**Interfaces:**
- Produces, em `native/viewer_core.hpp`, namespace `viewer`:
  ```cpp
  struct Rect { int x = 0, y = 0, w = 0, h = 0; };
  struct Size { int w = 0, h = 0; };
  struct Point { int x = 0, y = 0; };
  enum class Fit { Fill, Contain };
  enum class Hit { Client, Caption, Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };
  enum class CommandKind { Invalid, Lock, TopMost, Resize, Move, Quit };
  struct Command { CommandKind kind = CommandKind::Invalid; int a = 0, b = 0; };
  Rect fitRect(Size source, Size target, Fit fit);
  Size keepAspect(Size requested, Size source, bool widthLeads, Size minimum, Size maximum);
  Rect clampToMonitors(Rect window, const std::vector<Rect>& monitors);
  Hit pipHit(Size window, Point point, bool locked, int border = 6);
  Command parseCommand(const std::string& line);
  std::string geometryEvent(Rect window);
  std::string statsEvent(int fps);
  std::string toggleEvent(const char* name, bool value);
  ```
- Produces: `build.ps1 -Target <alvo>` (compila só esse alvo em x64 e para antes de empacotar) e `Build-NativeTarget([string]$Target) -> string` em `tests/native.ps1` (caminho completo do `<Target>.exe`, em `build/ninja-x64` ou `build/x64/Release`).

Regras de `fitRect`/`keepAspect`: proporção calculada com inteiros e arredondamento para cima na metade (`(a*b + d/2)/d`); deslocamentos `(target - size)/2` com divisão inteira. `Fill` pode devolver `x`/`y` negativos. `keepAspect` limita a dimensão que lidera a `[minimum, maximum]`, deriva a outra; se a derivada passar do máximo dela, limita e recalcula a líder. `clampToMonitors`: escolhe o monitor com maior área de interseção (nenhum: `monitors[0]`; lista vazia: devolve a janela intacta) e move a janela para dentro dele sem mudar o tamanho (janela maior que o monitor vai para a origem do monitor). `pipHit`: bordas de `border` px; cantos têm prioridade; `locked` ou ponto fora da janela devolve `Client`. `parseCommand`: comandos `lock 0|1`, `topmost 0|1`, `resize W H` (1..16384), `move X Y` (inteiros), `quit`; tolera espaços extras e `\r`; diferencia maiúsculas; qualquer outra coisa é `Invalid`.

- [ ] **Step 1: Write the failing test** em `tests/viewer_core.cpp` (macro `CHECK(cond)` que imprime a linha e conta falhas; `main` devolve o número de falhas). Testes, com valores exatos:
  - `fit_contain`: `fitRect({1920,1080},{1000,1000},Contain)` == `{0,218,1000,563}`.
  - `fit_fill`: o mesmo com `Fill` == `{-389,0,1778,1000}`.
  - `fit_same_aspect`: `fitRect({1280,720},{640,360},Contain)` == `{0,0,640,360}`.
  - `aspect_width_leads`: `keepAspect({700,300},{1920,1080},true,{160,90},{3840,2160})` == `{700,394}`; com `false` == `{533,300}`.
  - `aspect_limits`: `keepAspect({50,10},...)` == `{160,90}`; `keepAspect({5000,5000},...,true,...)` == `{3840,2160}`.
  - `clamp_offscreen`: monitor `{0,0,1920,1080}`, janela `{5000,5000,640,360}` == `{1280,720,640,360}`.
  - `clamp_two_monitors`: monitores `{0,0,1920,1080}` e `{1920,0,1280,1024}`, janela `{1800,100,640,360}` == `{1920,100,640,360}`.
  - `clamp_no_monitors`: lista vazia devolve a janela intacta.
  - `hit_zones`: janela `{640,360}`, `locked=false`: `(0,0)` TopLeft, `(639,0)` TopRight, `(0,359)` BottomLeft, `(639,359)` BottomRight, `(320,0)` Top, `(320,359)` Bottom, `(0,180)` Left, `(639,180)` Right, `(320,180)` Caption; `locked=true` em todos esses pontos: Client; `(-5,10)` e `(700,10)`: Client.
  - `cmd_valid`: `"lock 1"`→`{Lock,1,0}`, `"lock 0"`, `"topmost 1"`, `"resize 640 360"`→`{Resize,640,360}`, `"move -100 50"`→`{Move,-100,50}`, `"quit"`, `"  lock   1  \r"`→`{Lock,1,0}`.
  - `cmd_invalid`: `""`, `"lock"`, `"lock 2"`, `"lock 1 extra"`, `"LOCK 1"`, `"resize 0 360"`, `"resize 99999 10"`, `"resize a b"`, `"%%%"`, `"move 1"` são todos `Invalid`.
  - `events`: `geometryEvent({10,20,640,360})` == `{"Event":"geometry","X":10,"Y":20,"Width":640,"Height":360}`; `statsEvent(60)` == `{"Event":"stats","Fps":60}`; `toggleEvent("lock",true)` == `{"Event":"lock","Value":1}`.

- [ ] **Step 2: Wire the test target and runner.** `CMakeLists.txt`: `add_executable(MultiCam.ViewerCoreTest EXCLUDE_FROM_ALL tests/viewer_core.cpp)`. `build.ps1`: novo `[string]$Target`; com ele, só x64, só `--target $Target`, e `exit` logo depois do build (sem copiar para `dist`, sem compilar o painel); a configuração portátil/VS existente é reaproveitada. `tests/native.ps1` define `Build-NativeTarget` (chama `build.ps1 -Target`, resolve o caminho do `.exe`, falha se não existir).

- [ ] **Step 3: Run test to verify it fails**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -Command ". tests/native.ps1; Build-NativeTarget MultiCam.ViewerCoreTest"`
Expected: FAIL de compilação (`viewer_core.hpp` não existe ou funções indefinidas).

- [ ] **Step 4: Implement** `native/viewer_core.hpp` (header-only, só `<string>`, `<vector>`, `<algorithm>`, `<cstdio>`; **sem** `<windows.h>`, para compilar em qualquer lugar).

- [ ] **Step 5: Run test to verify it passes**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -Command ". tests/native.ps1; & (Build-NativeTarget MultiCam.ViewerCoreTest); exit $LASTEXITCODE"`
Expected: exit code 0, nenhuma linha `FAIL`.

- [ ] **Step 6: Commit**

```bash
git add native/viewer_core.hpp tests/viewer_core.cpp tests/native.ps1 CMakeLists.txt build.ps1
git commit -m "feat: add viewer core logic and native test runner"
```

---

### Task 2: Renderizador Direct3D 11 (`renderer`), shader e teste de cores

**Files:**
- Create: `native/renderer.hpp`, `native/renderer.cpp`, `native/viewer.hlsl`
- Create: `tests/renderer_test.cpp`
- Modify: `CMakeLists.txt` (função de compilação HLSL com `fxc`, alvo `MultiCam.RendererTest` `EXCLUDE_FROM_ALL`)

**Interfaces:**
- Consumes: `viewer::Fit`, `viewer::Rect`, `viewer::Size`, `viewer::fitRect` (Task 1).
- Produces, em `native/renderer.hpp`:
  ```cpp
  class Renderer {
  public:
      bool init(HWND window, std::wstring& error);           // window == nullptr: modo fora da tela
      void resize(int width, int height);                     // tamanho do alvo (back buffer ou textura fora da tela)
      bool upload(const uint8_t* yuy2, int width, int height); // width par; recria texturas se o tamanho mudar
      void draw(viewer::Fit fit);                             // sem quadro enviado: preto
      void present();                                         // só no modo com janela
      bool readback(std::vector<uint8_t>& bgra, int& width, int& height); // só fora da tela; BGRA, de cima para baixo
  };
  ```

Decisões: dispositivo `D3D_DRIVER_TYPE_HARDWARE` e, se falhar, `D3D_DRIVER_TYPE_WARP` (testes rodam sem GPU). Em janela, swap chain bitblt (`DXGI_SWAP_EFFECT_DISCARD`, `DXGI_FORMAT_B8G8R8A8_UNORM`), compatível com janela filha do `WorkerW`. Duas passadas: (1) o YUY2 sobe como textura `R8G8B8A8_UNORM` de largura `w/2` (texel = `Y0 U Y1 V`) e o pixel shader `ps_convert` escreve RGBA em uma textura de largura `w`; (2) `ps_blit` desenha essa textura com amostragem linear em um viewport igual a `fitRect`, depois de limpar o alvo para preto. Vértices: triângulo de tela cheia sem buffer (`vs_main`). `fxc` vem do `PATH` (ambiente `.buildtools`) ou do `Windows Kits/10/bin/<versão mais nova>/x64` em `ProgramFiles(x86)`; sem `fxc` o configure falha com mensagem clara. Bytecode vira header (`/Fh`, variáveis `g_vs_main`, `g_ps_convert`, `g_ps_blit`) em `build/…/shaders/`.

- [ ] **Step 1: Write the failing test** em `tests/renderer_test.cpp` (modo fora da tela, alvo 100×100, mesmo `CHECK`; tolerância ±6 por canal). Quadros sólidos YUY2 criados no teste:
  - `solid_black`: 4×2, Y=16 U=128 V=128 → pixel central (0,0,0).
  - `solid_white`: Y=235 U=V=128 → (255,255,255).
  - `red_601`: 4×2 (altura < 720), Y=82 U=90 V=240 → R≈255, G≈0, B≈0.
  - `red_709`: 1280×720, Y=63 U=102 V=240 → R≈255, G≈0, B≈0.
  - `contain_letterbox`: 4×2 vermelho, `Contain` em 100×100 → pixel `(50,50)` vermelho e `(50,5)` preto.
  - `fill_covers`: o mesmo com `Fill` → `(5,5)` e `(50,50)` vermelhos.
  - `size_change`: `upload` 4×2, depois 1280×720, depois 4×2; cada `upload` devolve `true` e o último `readback` ainda é vermelho.
  - `no_frame_is_black`: `draw` sem `upload` → `(50,50)` preto.
  - `odd_width_rejected`: `upload` com largura ímpar devolve `false`.

- [ ] **Step 2: Add the HLSL build rules and target** (`CMakeLists.txt`): função `add_hlsl_header(<entry> <profile> <variável> <saída.h>)` chamando `fxc /nologo /T <profile> /E <entry> /Vn <variável> /Fh <saída.h> native/viewer.hlsl`; alvo `MultiCam.RendererTest` (`tests/renderer_test.cpp`, `native/renderer.cpp`, ligando `d3d11`, `dxgi`) dependente dos três headers.

- [ ] **Step 3: Run test to verify it fails**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -Command ". tests/native.ps1; Build-NativeTarget MultiCam.RendererTest"`
Expected: FAIL de compilação (`renderer.hpp` ausente).

- [ ] **Step 4: Implement** `native/viewer.hlsl` (`vs_main`, `ps_convert` com BT.709/601 escolhido por uma constante de altura e faixa limitada, `ps_blit`) e `Renderer` conforme as decisões acima.

- [ ] **Step 5: Run test to verify it passes**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -Command ". tests/native.ps1; & (Build-NativeTarget MultiCam.RendererTest); exit $LASTEXITCODE"`
Expected: exit code 0, nenhuma linha `FAIL`.

- [ ] **Step 6: Commit**

```bash
git add native/renderer.hpp native/renderer.cpp native/viewer.hlsl tests/renderer_test.cpp CMakeLists.txt
git commit -m "feat: add Direct3D 11 YUY2 renderer with colour tests"
```

---

### Task 3: `MultiCam.Viewer.exe` modo PiP

**Files:**
- Create: `native/viewer.cpp`, `tests/viewer.ps1`
- Modify: `native/channel.hpp` (método `peekSize`), `native/integration.cpp` (modo `--produce-solid`), `CMakeLists.txt` (alvo `MultiCam.Viewer`, só x64), `build.ps1` (copiar `MultiCam.Viewer.exe` para `dist/x64`)

**Interfaces:**
- Consumes: `Renderer` (Task 2), `viewer::*` (Task 1), `multi::Channel::read/hasFrame/frame/reset` (já existem).
- Produces:
  - `bool Channel::peekSize(uint32_t& width, uint32_t& height)` em `native/channel.hpp`: lê o cabeçalho; `true` só se `magic` válido, `active` e dimensões entre 2 e 4096×2160 (largura par).
  - `MultiCam.Integration.exe --produce-solid <id> <w> <h> <interval> <seconds> <y> <u> <v>`: como `--produce`, mas todo quadro é o YUY2 sólido `(y,u,v)`.
  - Linha de comando: `MultiCam.Viewer.exe --pip <cameraGuid> <x> <y> <w> <h> <topmost 0|1> <locked 0|1> <parentPid> [--snapshot <arquivo.bmp>]`.
  - Eventos JSON em stdout, um por linha: `{"Event":"ready","Hwnd":<n>}`, `{"Event":"source","Width":..,"Height":..}` (ao mudar a origem), `geometry`/`lock`/`topmost` (formatos da Task 1) e `stats` a cada segundo. Comandos de stdin: os de `parseCommand`; inválidos são ignorados.
  - Códigos de saída: 0 normal, 2 argumentos inválidos, 3 falha ao iniciar o Direct3D.

Comportamento: DPI `PER_MONITOR_AWARE_V2` (`SetProcessDpiAwarenessContext`, antes de criar janelas). Janela `WS_POPUP | WS_EX_TOOLWINDOW` (+`WS_EX_TOPMOST` se pedido), posicionada com `clampToMonitors` sobre os monitores reais. `WM_NCHITTEST` usa `pipHit` (`Hit` mapeado para `HT*`); `WM_SIZING` aplica `keepAspect` com a proporção da origem; menu do botão direito com *Travar*, *Sempre à frente*, *Fechar PiP*, que emitem `lock`/`topmost` e, ao fechar, saem com 0. Depois de mover ou redimensionar, emite `geometry`. A roda do mouse é ignorada. Laço: `MsgWaitForMultipleObjects` (mensagens, handle do processo pai, evento de dados de stdin); a cada ~4 ms chama `peekSize`+`Channel::read`, faz `upload` e `draw` só quando há `Updated` ou o prazo de 500 ms estoura (preto). Sem canal ainda: preto e continua tentando abrir. Se a origem mudar de tamanho, mantém a largura da janela, recalcula a altura (`keepAspect`) e emite `source` e `geometry`. `--snapshot`: depois do primeiro `Updated` desenhado, grava o conteúdo da janela como BMP 24 bits (via `Renderer` fora da tela do mesmo tamanho da janela) e sai com 0 (timeout interno de 15 s: sai com 1). O processo sai com 0 quando o processo pai termina.

- [ ] **Step 1: Write the failing test** `tests/viewer.ps1` (build de `MultiCam.Viewer` e `MultiCam.Integration`; P/Invoke de `SendMessage`, `GetParent`, `GetClassName`; helper que inicia o viewer lendo stdout/stderr e devolve eventos). Seção PiP:
  - `pip_snapshot_color`: inicia o viewer **antes** do produtor; depois inicia `--produce-solid` 640×360, intervalo `166667`, por 6 s, com `y=82 u=90 v=240` (vermelho em BT.601, porque 640×360 tem altura < 720); espera o viewer sair com 0 em ≤ 10 s; o BMP tem 320×180 (largura e altura iniciais passadas ao viewer) e o pixel central tem R ≥ 247, G ≤ 8, B ≤ 8.
  - `pip_lock_hittest`: com `ready.Hwnd`, `SendMessage(hwnd, 0x84, 0, MAKELPARAM(cx,cy))` no centro devolve `2` (HTCAPTION); depois de enviar `lock 1`, devolve `1` (HTCLIENT); depois de `lock 0`, `2` de novo.
  - `pip_mode_change`: produtor 640×360 por 3 s, depois 640×480 por 3 s; os eventos incluem `source` 640×360 e depois `source` 640×480, e um `geometry` posterior com `Width=320` e `Height=240`.
  - `parent_exit`: viewer com `parentPid` de um `cmd /c ping -n 4 127.0.0.1` descartável; sai com 0 em ≤ 3 s depois que esse processo termina.
  - `garbage_stdin`: envia `%%%`, `resize abc`, `lock 9` e `quit`; sai com 0 em ≤ 3 s, sem eventos de erro.
  - `bad_args`: `--pip` sem argumentos sai com 2.

- [ ] **Step 2: Run test to verify it fails**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File tests/viewer.ps1`
Expected: FAIL (alvo `MultiCam.Viewer` não existe).

- [ ] **Step 3: Implement** `Channel::peekSize`, `--produce-solid` em `integration.cpp`, o alvo `MultiCam.Viewer` (`native/viewer.cpp`, `native/renderer.cpp`; ligando `d3d11 dxgi user32 gdi32 shcore dwmapi`; `if(CMAKE_SIZEOF_VOID_P EQUAL 8)`), a cópia em `build.ps1` e `native/viewer.cpp` (modo PiP) conforme o comportamento acima.

- [ ] **Step 4: Run test to verify it passes**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File tests/viewer.ps1`
Expected: `PASS` para todos os casos da seção PiP; saída final com exit code 0.

- [ ] **Step 5: Commit**

```bash
git add native/viewer.cpp native/channel.hpp native/integration.cpp tests/viewer.ps1 CMakeLists.txt build.ps1
git commit -m "feat: add Viewer executable with PiP mode"
```

---

### Task 4: `MultiCam.Viewer.exe` modo papel de parede

**Files:**
- Modify: `native/viewer.cpp`, `tests/viewer.ps1`

**Interfaces:**
- Consumes: tudo da Task 3; `viewer::fitRect`.
- Produces: linha de comando `MultiCam.Viewer.exe --wallpaper <cameraGuid> <monitor> <fps 30|60> <fit fill|contain> <parentPid> [--snapshot <arquivo.bmp>]`, onde `<monitor>` é o `DeviceName` do Windows (por exemplo `\\.\DISPLAY1`). Novos códigos de saída: 4 (monitor não encontrado; mensagem em stderr com a palavra "monitor"), 5 (falha ao ancorar no `WorkerW`; mensagem clara). Mesmos eventos `ready`/`source`/`stats` e mesmos comandos (`quit`).

Comportamento: envia `0x052C` (`wParam 0xD`, `lParam 1`, `SMTO_NORMAL`, 1000 ms) ao `Progman`; procura a janela de nível superior que tem `SHELLDLL_DefView` e usa a `WorkerW` irmã seguinte; se não existir, sai com 5. A janela vira filha da `WorkerW` (`WS_CHILD`), com posição relativa à origem da área virtual e tamanho do retângulo do monitor (`EnumDisplayMonitors`/`GetMonitorInfo`). Desenha só quando há quadro novo, limitado a `fps` (temporizador de espera de alta resolução); ao passar de 500 ms sem quadro, preto. Pausa o desenho quando a sessão está bloqueada (`WTSRegisterSessionNotification`) ou quando a janela em primeiro plano cobre o monitor inteiro (`GetForegroundWindow` + `GetWindowRect`, ignorando `Progman`/`WorkerW`/o próprio processo). Ao sair (qualquer motivo, inclusive o pai morrer), destrói a janela e chama `SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, nullptr, 0)` para o Windows redesenhar o papel de parede normal.

- [ ] **Step 1: Write the failing test** (seção papel de parede em `tests/viewer.ps1`; usa `[System.Windows.Forms.Screen]::PrimaryScreen.DeviceName`; o teste avisa no início que a tela do usuário muda por alguns segundos):
  - `wallpaper_attaches`: produtor sólido a 60 FPS (`166667`); viewer `--wallpaper`; o evento `ready` traz `Hwnd` e `GetParent(hwnd)` tem classe `WorkerW`; `quit` sai com 0.
  - `wallpaper_fps_30` e `wallpaper_fps_60`: com o produtor a 60 FPS por 6 s, a média dos eventos `stats` (descartando o primeiro) fica em `[26,32]` para `30` e em `[52,62]` para `60`.
  - `wallpaper_snapshot`: com `--snapshot` e `fit contain` o BMP tem o tamanho do monitor e o pixel central é a cor sólida enviada (±8).
  - `wallpaper_monitor_missing`: monitor `\\.\DISPLAY99` sai com 4 em ≤ 3 s e o stderr contém "monitor".
  - `wallpaper_parent_exit`: igual a `parent_exit` da Task 3, mais conferir que `GetParent` de um `Hwnd` guardado já não é válido (`IsWindow` falso) depois da saída.

- [ ] **Step 2: Run test to verify it fails**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File tests/viewer.ps1`
Expected: FAIL nos casos `wallpaper_*` (modo desconhecido, saída 2).

- [ ] **Step 3: Implement** o modo `--wallpaper` em `native/viewer.cpp` conforme o comportamento acima, reaproveitando o laço, os eventos e o `--snapshot` do PiP.

- [ ] **Step 4: Run test to verify it passes**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File tests/viewer.ps1`
Expected: `PASS` em todos os casos (PiP e papel de parede), exit code 0.

- [ ] **Step 5: Manual check no PC do usuário** (não automatizável): ícones e barra de tarefas continuam visíveis por cima; abrir um programa em tela cheia pausa o desenho (`stats` com `Fps` 0); fechar o viewer devolve o papel de parede normal.

- [ ] **Step 6: Commit**

```bash
git add native/viewer.cpp tests/viewer.ps1
git commit -m "feat: add Viewer wallpaper mode"
```

---

### Task 5: Painel, parte lógica: configurações, presets e gerência dos viewers

**Files:**
- Create: `app/Viewers.cs`, `tests/panel.ps1`
- Modify: `app/MultiCam.cs` (campos novos em `Camera` e `Config`), `build.ps1` (compilar os dois `.cs` em `dist/MultiCam.exe`)

**Interfaces:**
- Consumes: linha de comando e eventos do viewer (Tasks 3 e 4); `Camera`, `Mode`, `Config`, `Storage`, `Native.Quote` (`app/MultiCam.cs`).
- Produces, em `namespace MultiCam` (`app/Viewers.cs`, tudo `public`):
  ```csharp
  public class PipSettings { public bool Enabled, TopMost, Locked; public int X, Y, Width, Height; public bool HasPosition; }
  public class WallpaperSettings { public bool Enabled; public string CameraId, Monitor, Fit = "fill"; public int Fps = 30; }
  public static class ViewerSettings {
      public static void Normalize(Config config);                       // preenche/corrige tudo; nunca lança
      public static List<System.Drawing.Size> Presets(Mode mode);        // 4 tamanhos, larguras 320/480/640/960
  }
  public sealed class ViewerProcess : IDisposable {
      public static ViewerProcess StartPip(string viewerPath, Camera camera, int parentPid);
      public static ViewerProcess StartWallpaper(string viewerPath, WallpaperSettings settings, int parentPid);
      public bool Ready { get; } public long Hwnd { get; } public int Fps { get; }
      public bool Exited { get; } public int ExitCode { get; } public string Error { get; }
      public event Action<int,int,int,int> GeometryChanged;              // x, y, width, height
      public event Action<bool> LockChanged; public event Action<bool> TopMostChanged;
      public void Send(string command);                                  // uma linha de comando do viewer
      public void Dispose();                                             // envia quit, espera 2 s, depois Kill
  }
  public sealed class ViewerManager : IDisposable {
      public ViewerManager(string viewerPath, int parentPid);
      public bool Reconcile(Config config, Func<Camera,bool> cameraReady); // true se algo de configuração mudou e precisa salvar
      public string Status(Camera camera);                                // "", "Iniciando…", ou "Falhou: <motivo>"
      public string WallpaperStatus();
      public void StopAll(); public void Dispose();
  }
  ```
- `Camera` ganha `public PipSettings Pip = new PipSettings();`; `Config` ganha `public WallpaperSettings Wallpaper = new WallpaperSettings();`.

Regras: `Normalize` troca `null` por padrão, `Fps` diferente de 30/60 vira 30, `Fit` diferente de `fill`/`contain` vira `fill`, `Width`/`Height` ≤ 0 viram o preset de 480 de largura com a proporção do modo (16:9 se não houver modo), e `HasPosition` fica falso se `X`/`Y` forem absurdos (|valor| > 100000). Presets: altura = `round(largura * Mode.Height / Mode.Width)` (16:9 sem modo). `Reconcile`: PiP roda se `Camera.Enabled && Pip.Enabled && cameraReady(camera)`; o papel de parede roda se `Wallpaper.Enabled` e a câmera dele existe e está pronta. Mudar `TopMost`/`Locked`/tamanho do PiP vira comando `topmost`/`lock`/`resize` ao processo vivo; mudar monitor, FPS ou ajuste do papel de parede reinicia o viewer. Viewer que sai sozinho com código ≠ 0 e ≠ 4/5 é reiniciado depois de 2 s; após 3 falhas seguidas para e `Status` devolve `Falhou: <motivo>`; códigos 4/5 não reiniciam. Eventos `GeometryChanged`/`LockChanged`/`TopMostChanged` atualizam `Camera.Pip`, e `Reconcile` devolve `true` quando isso acontece.

- [ ] **Step 1: Write the failing test** `tests/panel.ps1` (carrega `dist/MultiCam.exe` por reflexão, como `tests/integration.ps1`; usa `[MultiCam.Storage]::Json`):
  - `old_settings_load`: JSON antigo `{"AutoStart":false,"Cameras":[{"Identity":"x","Id":"<guid>","Alias":"A","Enabled":true}]}` desserializa em `Config`; depois de `Normalize`, `Cameras[0].Pip` não é nulo e está desativado, `Wallpaper` não é nulo, desativado, `Fps` 30, `Fit` `fill`.
  - `normalize_values`: `Pip` com `Width=0`, `Height=-5`, `X=9999999`; `Wallpaper` com `Fps=45`, `Fit="x"`, e `"Pip":null` em outra câmera → depois de `Normalize`: 480×270, `HasPosition` falso, `Fps` 30, `Fit` `fill`, `Pip` não nulo.
  - `normalize_offscreen`: uma geometria válida salva (`X=-3000`, `Y=50`, 640×360, `HasPosition` verdadeiro) é **mantida** por `Normalize` (a correção para a tela é do viewer, via `clampToMonitors`).
  - `presets_16_9`: `Presets` de 1920×1080 devolve 320×180, 480×270, 640×360, 960×540; `presets_4_3`: 640×480 devolve 320×240, 480×360, 640×480, 960×720; sem modo: os de 16:9.
  - `viewer_process_lifecycle`: com o produtor `--produce-solid` rodando, `StartPip` fica `Ready` em ≤ 5 s com `Hwnd` ≠ 0; `Send("lock 1")` não lança; `Dispose` encerra o processo em ≤ 3 s (o processo some).
  - `manager_restart_limit`: `ViewerManager` apontado para um executável falso que sai com 1 (um `cmd.exe /c exit 1` em `.cmd`) mostra `Falhou:` em `Status` depois de 3 tentativas e não inicia uma quarta (conferir o contador de processos criados).

- [ ] **Step 2: Run test to verify it fails**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File tests/panel.ps1`
Expected: FAIL (`MultiCam.ViewerSettings` não existe).

- [ ] **Step 3: Implement** `app/Viewers.cs`, os campos novos em `app/MultiCam.cs` e a lista de fontes do `csc` em `build.ps1` (passando os dois `.cs` no arquivo de resposta). Chamar `ViewerSettings.Normalize(config)` no fim de `Storage.Load`.

- [ ] **Step 4: Run test to verify it passes**

Run: `./build.ps1; powershell -NoProfile -ExecutionPolicy Bypass -File tests/panel.ps1`
Expected: `PASS` em todos os casos, exit code 0. Em seguida, `powershell -NoProfile -ExecutionPolicy Bypass -File tests/integration.ps1` continua com `PASS`.

- [ ] **Step 5: Commit**

```bash
git add app/Viewers.cs app/MultiCam.cs tests/panel.ps1 build.ps1
git commit -m "feat: add panel viewer settings, presets and process manager"
```

---

### Task 6: Painel, interface, documentação e pacote final

**Files:**
- Modify: `app/MultiCam.cs` (controles e integração com `ViewerManager`), `README.md`, `VALIDATION.md`
- Create: `tests/portable-check.ps1`

**Interfaces:**
- Consumes: `ViewerManager`, `ViewerSettings`, `PipSettings`, `WallpaperSettings` (Task 5).
- Produces: `tests/portable-check.ps1 [-Root <pasta>]` (padrão `dist`): falha com exit code 1 se algum `.exe`/`.dll` em `<Root>` importar DLL fora do Windows (aceita nomes que existam em `System32`/`SysWOW64` e `api-ms-win-*`) ou se existir `<Root>/data`.

Interface do painel (textos em português): no editor por câmera: caixas *PiP ativo*, *Sempre à frente*, *Travar*, lista de tamanhos com os presets de `ViewerSettings.Presets` (por exemplo "480 × 270") e botão *Restaurar posição* (zera `HasPosition` e reenvia `move`/`resize`); seção *Papel de parede* com *Ativar*, lista de câmeras (nomes `Alias`), lista de monitores (`Screen.AllScreens`, mostrando `DeviceName` e a resolução), botões de opção *30 FPS* / *60 FPS* e *Preencher* / *Ajustar*; coluna *Exibição* na lista ("PiP", "Papel de parede", "PiP · Papel de parede" ou vazio) e o `Status` do `ViewerManager` no detalhe quando houver falha. `RefreshCameras` chama `viewers.Reconcile(config, c => sessions[c.Id].Published)` e salva se devolver `true`; `Cleanup` chama `viewers.StopAll()`; o painel usa `Path.Combine(Storage.Root,"x64","MultiCam.Viewer.exe")` e o próprio PID como `parentPid`. A lógica de tela já está coberta pelos testes da Task 5; a interface em si é verificada à mão (Step 6), porque WinForms não é testável por reflexão.

- [ ] **Step 1: Write the failing test** `tests/portable-check.ps1` (importações dos binários lidas por `Add-Type` com um leitor de tabela de importação PE; confere `dist/x64/MultiCam.Viewer.exe` e todos os outros binários, e a ausência de `data`).

- [ ] **Step 2: Run test to verify it fails**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File tests/portable-check.ps1`
Expected: FAIL com a mensagem "dist/data deve ser removida antes de distribuir" (a pasta existe hoje com os perfis do usuário).

- [ ] **Step 3: Implement** a interface e a integração em `app/MultiCam.cs` e o script `tests/portable-check.ps1`. Atualizar `README.md` (novas funções, comandos, limitações do `WorkerW` no Windows 11 24H2, uso de 30/60 FPS) e `VALIDATION.md` (testes novos e o que só é manual).

- [ ] **Step 4: Run the whole suite**

Run: `./build.ps1; powershell -NoProfile -ExecutionPolicy Bypass -File tests/integration.ps1; powershell -NoProfile -ExecutionPolicy Bypass -File tests/panel.ps1; powershell -NoProfile -ExecutionPolicy Bypass -File tests/viewer.ps1`
Expected: `PASS` em todos; exit code 0.

- [ ] **Step 5: Regenerate the final package and verify portability**

Run: recriar `MultiCam-Final` com `robocopy dist MultiCam-Final /E /XD data`, copiar `README.md` para `MultiCam-Final\LEIA-ME.md` e `VALIDATION.md` para `MultiCam-Final\VALIDACAO.md`; depois `powershell -NoProfile -ExecutionPolicy Bypass -File tests/portable-check.ps1 -Root MultiCam-Final`.
Expected: `portable-check.ps1 -Root MultiCam-Final` termina com `PASS` e exit code 0; `MultiCam-Final\x64\MultiCam.Viewer.exe` existe.

- [ ] **Step 6: Manual check no PC do usuário** (não automatizável): ativar o PiP e o papel de parede pelo painel; mover, redimensionar, travar e destravar; confirmar que tamanho e posição voltam depois de reiniciar o programa; comparar o papel de parede a 30 e a 60 FPS olhando a coluna de FPS.

- [ ] **Step 7: Commit**

```bash
git add app/MultiCam.cs README.md VALIDATION.md tests/portable-check.ps1 tests/panel.ps1
git commit -m "feat: add panel controls for PiP and wallpaper, docs and portability check"
```
