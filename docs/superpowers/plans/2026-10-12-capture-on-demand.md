# MultiCam: captura sob demanda Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A placa só captura enquanto um aplicativo, o PiP ou o papel de parede usa a câmera virtual dela, e por mais 1 minuto depois disso. A câmera "(Multi)" continua sempre visível.

**Architecture:** Cada cliente (filtro dentro do app, viewer) mantém uma vaga numa memória compartilhada por câmera (`Local\MultiCam-{GUID}-demand`) e renova o horário a cada 500 ms. O painel lê as vagas, decide com um `CaptureScheduler` puro (relógio injetado) e liga ou desliga o worker pelo ciclo de sessões que já existe. Um evento nomeado acorda o painel quando um cliente começa.

**Tech Stack:** C++17 (header-only, Win32), C# 5 (.NET Framework 4.8, `System.IO.MemoryMappedFiles`), CMake + Ninja, PowerShell.

**Spec:** `docs/superpowers/specs/2026-10-12-capture-on-demand-design.md`

## Global Constraints

- Folga depois do último uso: **60 s**. Vaga vale como uso por **3000 ms** depois do último sinal. Renovação: no máximo a cada **500 ms**.
- Área: `Local\MultiCam-{GUID}-demand`, com `{GUID}` em maiúsculas e chaves, como o canal (`new Guid(id).ToString("B").ToUpperInvariant()`). Evento: `Local\MultiCam-wake` (auto-reset).
- Layout, igual em x86 e x64, 272 bytes: cabeçalho de 16 bytes `magic`=`0x444D434D`, `version`=1, `nextToken`, `reserved`; depois 16 vagas de 16 bytes `owner` (uint32, 0 = livre), `pid` (uint32), `tick` (uint64, `GetTickCount64`). Vaga `i` no offset `16 + i*16`.
- Só listar câmeras não conta como uso. Conta: thread de entrega do filtro rodando (Pause/Run), viewer aberto.
- Painel: **só C# 5** (sem `$""`, `?.`, `out var`, membros com `=>`).
- Parar por falta de uso usa `EndSession` (mantém o registro). `Stop(c)` (retira o registro) continua só para falhas, placa sumida há 10 s e câmera desativada.
- Nunca apague nem sobrescreva `MultiCam-Final/data` nem `dist/data`.
- Scripts que registram câmeras de teste nunca podem ter a saída cortada por `| Select-Object -First N`.
- Commits com lista explícita de arquivos, identidade `MultiCam Dev <dev@localhost>` por `GIT_AUTHOR_*`/`GIT_COMMITTER_*`, e `git -c safe.directory='D:/Multi App'`.

## Review Focus

Cada linha tem o teste que a fixa, na tarefa dona do código:

1. **Aplicativo morto segurando a vaga:** a vaga deixa de contar em ~3 s. Teste `demand_cross_killed` (Task 2).
2. **Cliente x86 (app 32 bits):** o painel x64 lê a vaga escrita pelo x86. Teste `demand_cross_x86` (Task 2).
3. **Evento de despertar chega enquanto o painel está ocupado num `RefreshCameras`:** o pedido não se perde e a atualização roda logo depois. Bandeira `wakePending` (Task 4), conferida no roteiro manual da Task 5.
4. **Painel aberto depois que um app já usa a câmera** (a área foi criada pelo cliente): o painel vê o uso. Coberto porque, em todos os testes cruzados, a área é criada pelo cliente e não pelo painel (Task 2).
5. **Câmera ociosa ao abrir o programa:** ela valida, captura por 1 min e para, ficando "Em espera". Teste `scheduler_validation_then_idle` (Task 2) e roteiro manual (Task 5).

---

### Task 1: Vagas de uso em `native/demand.hpp`

**Files:**
- Create: `native/demand.hpp`
- Create: `tests/demand_test.cpp`
- Modify: `CMakeLists.txt` (alvo `MultiCam.DemandTest EXCLUDE_FROM_ALL`, ao lado de `MultiCam.PipelineTest`)

**Interfaces:**
- Produces:
  - `namespace multi { constexpr uint32_t DemandMagic = 0x444D434D, DemandVersion = 1, DemandSlots = 16; constexpr ULONGLONG DemandFreshMs = 3000, DemandBeatMs = 500; }`
  - `struct DemandSlot { volatile LONG owner; uint32_t pid; volatile LONG64 tick; };` (16 bytes) e `struct DemandArea { uint32_t magic, version; volatile LONG nextToken; uint32_t reserved; DemandSlot slots[16]; };` (`static_assert(sizeof(DemandArea) == 272)`).
  - `class DemandClient { public: ~DemandClient(); bool begin(const std::wstring& id, ULONGLONG now = GetTickCount64()); void beat(ULONGLONG now = GetTickCount64()); void end(); bool holding() const; };`. `id` é o GUID em texto `{...}`, como em `Channel::open`. Não copiável.
  - `bool demandActive(const std::wstring& id, ULONGLONG now = GetTickCount64());`
  - `inline std::wstring demandName(const std::wstring& id)` = `objectName(id, L"-demand")` (de `channel.hpp`). `constexpr wchar_t DemandWakeName[] = L"Local\\MultiCam-wake";`

- [ ] **Step 1: Escrever `tests/demand_test.cpp`** (mesmo estilo de `CHECK` de `tests/pipeline_test.cpp`, GUID novo por caso via `CoCreateGuid` + `guidText`):
  - `slot_counts`: `begin(id)` → `true`; `holding()`; `demandActive(id)`.
  - `end_releases`: depois de `end()`, `!demandActive(id)` e `!holding()`.
  - `stale_does_not_count`: `begin(id, GetTickCount64() - 10000)` → `!demandActive(id)`; depois `beat()` (agora) → `demandActive(id)`.
  - `beat_throttled`: `begin(id, t)`; `beat(t + 100)` não muda o `tick` (lido pela área mapeada); `beat(t + 600)` muda.
  - `table_full`: 16 clientes com `begin` → `true`; o 17º → `false` e `!holding()`; `demandActive` continua `true`.
  - `two_clients_same_process`: dois clientes, `end()` de um → ainda há uso; `end()` do outro → não há.
  - `destructor_releases`: cliente num bloco; ao sair do bloco, `!demandActive(id)`.
  - `no_area_no_demand`: `demandActive` de um GUID nunca usado → `false`.
  - `wake_event`: abre `DemandWakeName` (`CreateEventW`, auto-reset) antes; depois de `begin`, `WaitForSingleObject(evt, 0) == WAIT_OBJECT_0`.
- [ ] **Step 2:** `. tests/native.ps1; Build-NativeTarget MultiCam.DemandTest` → falha de compilação: `demand.hpp` não existe.
- [ ] **Step 3: Implementar `native/demand.hpp`.**
  - A área é aberta ou criada com `CreateFileMappingW(INVALID_HANDLE_VALUE, …, PAGE_READWRITE, 0, sizeof(DemandArea), name)`.
  - `magic` e `version` são gravados com `InterlockedCompareExchange` quando a área está zerada.
  - A ficha vem de `InterlockedIncrement(&nextToken)`; se der 0, incrementa de novo.
  - A vaga é tomada com `InterlockedCompareExchange(&slot.owner, token, 0) == 0`.
  - `tick` é gravado com `InterlockedExchange64` e lido com `InterlockedCompareExchange64(&tick, 0, 0)` (x86 seguro).
  - `end()` libera com `InterlockedCompareExchange(&owner, 0, token)`.
  - `demandActive` usa `OpenFileMappingW(FILE_MAP_READ)`; sem área, `false`; `magic` errado, `false`.
  - `begin` dispara o evento com `CreateEventW(nullptr, FALSE, FALSE, DemandWakeName)` + `SetEvent` + `CloseHandle`.
- [ ] **Step 4:** rodar `MultiCam.DemandTest` → `PASS`, três vezes seguidas.
- [ ] **Step 5: Commit** `native/demand.hpp tests/demand_test.cpp CMakeLists.txt` — `feat: shared demand slots for capture on demand`.

### Task 2: Leitura no painel e `CaptureScheduler`

**Files:**
- Create: `app/Demand.cs`
- Modify: `build.ps1` (adicionar `app/Demand.cs` às fontes do `csc`, como `$viewersSource`)
- Modify: `native/integration.cpp` (modo `--demand <guid> <segundos>`)
- Test: `tests/panel.ps1`

**Interfaces:**
- Consumes: layout e nomes da Task 1 (Global Constraints).
- Produces (C#, namespace `MultiCam`):
  - `public static class Demand { public static bool Active(string cameraId); public static bool Active(string cameraId, ulong nowTick); public static ulong Now(); }`. `Now()` = `GetTickCount64` (P/Invoke). Lê com `MemoryMappedFile.OpenExisting(name, MemoryMappedFileRights.Read)`; qualquer exceção → `false`.
  - `public sealed class DemandWaker : IDisposable { public DemandWaker(Action wake); }`: thread de fundo que espera `Local\MultiCam-wake` (`EventWaitHandle` auto-reset, criado se não existir) com timeout de 1 s até `Dispose`, e chama `wake()` a cada sinal.
  - `public enum CaptureAction { Keep, Start, Stop }`.
  - `public sealed class CaptureScheduler { public static readonly TimeSpan Grace /* 60 s */; public CaptureAction Decide(string id, bool validated, bool demand, bool running, DateTime now); public void Touch(string id, DateTime now); public void Forget(string id); }`. Regras do spec: não validada → `running ? Keep : Start`; com uso → registra `now` e `running ? Keep : Start`; sem uso há menos de `Grace` → `Keep`; sem uso há `Grace` ou mais → `running ? Stop : Keep`. Câmera validada sem registro de uso conta como uso antigo (não liga).
  - Integration: `MultiCam.Integration.exe --demand <guid> <segundos>` segura uma vaga (`beat` a cada 200 ms), libera e imprime `{"Demand":1}`.

- [ ] **Step 1: Testes em `tests/panel.ps1`** (`$t0 = [DateTime]::UtcNow`, `$s = New-Object MultiCam.CaptureScheduler`):
  - `scheduler_validation_then_idle`:
    - `Decide(id,$false,$false,$false,$t0)` = `Start`.
    - `Touch(id,$t0)`.
    - `Decide(id,$true,$false,$true,$t0+59s)` = `Keep`.
    - `Decide(id,$true,$false,$true,$t0+60s)` = `Stop`.
    - `Decide(id,$true,$false,$false,$t0+61s)` = `Keep`.
  - `scheduler_demand_restarts`:
    - Depois do caso acima, `Decide(id,$true,$true,$false,$t0+70s)` = `Start`.
    - `Decide(id,$true,$false,$true,$t0+129s)` = `Keep`.
    - `Decide(id,$true,$false,$true,$t0+130s)` = `Stop`.
  - `scheduler_forget`: `Forget(id)`; `Decide(id,$true,$false,$true,$t0+131s)` = `Stop` (sem uso registrado).
  - `demand_cross_x64`:
    - `Start-Process build/x64/Release/MultiCam.Integration.exe --demand <guid> 3`.
    - Depois de 700 ms, `[MultiCam.Demand]::Active($id)` = `$true`.
    - Depois que o processo sai, `Active` = `$false` (vaga liberada, sem esperar 3 s).
  - `demand_cross_x86`: igual, com `build/Win32/Release/MultiCam.Integration.exe`.
  - `demand_cross_killed`:
    - `--demand <guid> 30`; depois de 700 ms, `Active` = `$true`.
    - `Kill()`; imediatamente `Active` = `$true` (vaga ainda fresca).
    - Depois de 3,5 s, `Active` = `$false`.
  - `demand_waker`:
    - `New-Object MultiCam.DemandWaker ([Action]{ $script:woke = $true })`.
    - Roda `--demand <guid> 1`; em até 1 s, `$woke`.
    - `Dispose()`.
- [ ] **Step 2:** `./build.ps1` falha (tipos inexistentes) ou `tests/panel.ps1` mostra FAIL nesses casos.
- [ ] **Step 3: Implementar** `app/Demand.cs`, o modo `--demand` em `native/integration.cpp` e a fonte nova no `build.ps1`.
- [ ] **Step 4:** `./build.ps1` e depois `tests/panel.ps1` → todos PASS (11 antigos + 7 novos).
- [ ] **Step 5: Commit** `app/Demand.cs build.ps1 native/integration.cpp tests/panel.ps1` — `feat: panel reads demand slots; capture scheduler`.

### Task 3: Clientes sinalizam uso (filtro e viewer)

**Files:**
- Modify: `native/camera.cpp` (`stream()`: `DemandClient` local, `begin(id_)` no começo, `beat()` a cada volta do laço, `end()` na saída)
- Modify: `native/viewer.cpp` (`DemandClient` em `wWinMain`: `begin(app.opt.camera)` depois do evento `ready`, `beat()` a cada volta do laço principal, `end()` antes de sair)
- Test: `tests/pacing.ps1`, `tests/viewer.ps1`

**Interfaces:**
- Consumes: `multi::DemandClient` (Task 1), `MultiCam.Demand.Active` (Task 2).

- [ ] **Step 1: Testes.**
  - `tests/pacing.ps1`, caso `consumer_signals_demand`:
    - Durante os consumidores, `[MultiCam.Demand]::Active($camera.Id)` = `$true`.
    - Depois que todos terminam (`Stop` do grafo libera), `Active` = `$false` em até 1 s.
  - `tests/viewer.ps1`, caso `viewer_signals_demand`:
    - Carregar `dist/MultiCam.exe`.
    - PiP pronto: `Active` = `$true` para o GUID.
    - Depois de `quit`: `$false`.
- [ ] **Step 2:** `./build.ps1`, depois `tests/pacing.ps1` e `tests/viewer.ps1` → os dois casos novos falham (nenhum cliente sinaliza).
- [ ] **Step 3: Implementar** nos dois arquivos, como descrito em **Files**.
- [ ] **Step 4:** `./build.ps1`, depois `tests/pacing.ps1` (5 PASS), `tests/viewer.ps1` (15 PASS) e `tests/integration.ps1` (PASS). Se `dist\x64\MultiCam.Camera.dll` estiver presa, renomeie para `build\presas\` (mesmo volume) antes do build.
- [ ] **Step 5: Commit** `native/camera.cpp native/viewer.cpp tests/pacing.ps1 tests/viewer.ps1` — `feat: virtual camera and viewers signal demand`.

### Task 4: Painel captura sob demanda

**Files:**
- Modify: `app/MultiCam.cs`:
  - campos `CaptureScheduler scheduler`, `DemandWaker waker` e `bool wakePending`;
  - `RefreshCameras`, `Stop`, `RenderList`, `Cleanup` e o construtor do formulário.

**Interfaces:**
- Consumes: `Demand.Active`, `DemandWaker`, `CaptureScheduler`, `CaptureAction` (Task 2).

- [ ] **Step 1: Ligar o scheduler em `RefreshCameras`**, no laço de placas, depois de `if(!c.Enabled) {Stop(c);continue;}`:
  - `validated = registered.Contains(c.Id)`, `demand = Demand.Active(c.Id)`, `running = sessions.ContainsKey(c.Id)`.
  - `action = scheduler.Decide(c.Id, validated, demand, running, DateTime.UtcNow)`.
  - Com sessão: a lógica de saída e publicação continua igual. No ponto em que publica, chamar também `scheduler.Touch(c.Id, DateTime.UtcNow)`. Depois, se `action == CaptureAction.Stop`, chamar `EndSession(c)`, que não conta falha.
  - Sem sessão: se `action != CaptureAction.Start`, `continue`. Senão, segue o caminho atual (`retry`, `ReadModes`, `new Session`).
  - `Stop(c)` também chama `scheduler.Forget(c.Id)`.
- [ ] **Step 2: Despertar.**
  - No construtor: `waker = new DemandWaker(() => BeginInvoke((Action)Wake))`.
  - `Wake()`: se `busy`, `wakePending = true`; senão `timer.Interval = 50`.
  - No `finally` de `RefreshCameras`: se `wakePending`, zera a bandeira e põe `timer.Interval = 50`.
  - `Cleanup()` chama `waker.Dispose()` antes de parar as câmeras.
- [ ] **Step 3: Textos.**
  - Em `RenderList`, quando `s == null` e `registered.Contains(c.Id)`, o estado é `"Em espera"`. Isso vem antes de `errors`/`"Detectada"` e depois de `"Desconectada"`/`"Desativada"`.
  - A barra de status passa a usar `registered.Count + " compartilhada(s)"`.
- [ ] **Step 4:** `./build.ps1` (compila sem erro, só C# 5), depois `tests/panel.ps1` e `tests/integration.ps1` → PASS.
- [ ] **Step 5: Commit** `app/MultiCam.cs` — `feat: capture each card only while its camera is in use`.

### Task 5: Documentação, pacote e validação com a placa real

**Files:**
- Modify: `README.md` (comportamento sob demanda, legenda "Em espera", 1 min de folga, primeira imagem em ~1–2 s)
- Modify: `VALIDATION.md` (resultados dos testes e do roteiro manual)
- Modify: `CLAUDE.md` (fora do git: pendência concluída, `tests/demand` na lista de alvos)

- [ ] **Step 1:** `./build.ps1` e depois todos os testes: `panel.ps1`, `integration.ps1`, `pacing.ps1`, `viewer.ps1` e os alvos `DemandTest`, `PipelineTest`, `ViewerCoreTest`, `RendererTest`, `JpegTest` → todos PASS.
- [ ] **Step 2:** com o MultiCam fechado pelo usuário, rodar `package.ps1` → `PASS`, com `data` preservado.
- [ ] **Step 3: Roteiro manual com o usuário** (MultiCam aberto a partir do `MultiCam-Final`):
  - Ao abrir: as duas placas validam, aparecem "Compartilhando" e, ~1 min depois, "Em espera". Os workers somem do Gerenciador de Tarefas.
  - Abrir a câmera FreeSky no Discord: a imagem aparece em ~1–2 s (medir). Só a FreeSky liga.
  - Abrir a mesma câmera no Discord PTB ao mesmo tempo: as duas imagens fluem.
  - Fechar a câmera nos dois: a placa continua ligada ~1 min e depois volta para "Em espera".
  - Ligar o PiP da FreeSky: a placa liga e continua ligada enquanto o PiP estiver aberto.
- [ ] **Step 4: Commit** `README.md VALIDATION.md` — `docs: capture on demand`.
