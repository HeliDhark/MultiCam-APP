# MultiCam: captura sob demanda

Data: 2026-10-12. Estado: aguardando revisão do usuário.

## Objetivo

Hoje o painel captura toda placa conectada e ativada o tempo todo, mesmo sem nenhum aplicativo usando a câmera virtual. Cada placa em YUY2 1080p60 move cerca de 250 MB/s pelo USB e gasta kernel e CPU à toa.

Com esta mudança, uma placa só captura enquanto alguém usa a câmera virtual dela e por mais 1 minuto depois disso. Contam como uso um aplicativo (Discord, OBS…), o PiP e o papel de parede.

Critérios de sucesso:

- A câmera "(Multi)" continua sempre na lista dos aplicativos, mesmo com a placa parada.
- Quando um aplicativo começa a receber vídeo, a placa liga sozinha. A imagem aparece em cerca de 1–2 s, com preto até lá.
- Sem uso por 1 minuto, a placa para. O worker sai e o USB fica livre.
- Vários aplicativos ao mesmo tempo continuam funcionando como hoje.
- Um aplicativo que trava ou é fechado à força não mantém a placa ligada para sempre.
- Nada muda para quem já usa: reinício por falha, placa desconectada, câmera desativada, PiP e papel de parede.

## Decisões do usuário

| Tema | Decisão |
|---|---|
| Folga depois do último uso | **1 minuto** (menos esperas para quem liga e desliga a câmera com frequência). |
| Como o painel sabe do uso | **Sinal de uso numa memória compartilhada, com o painel no comando** (abordagem A). O worker não se controla sozinho. |
| O que conta como uso | Aplicativo recebendo vídeo (Pause/Run do filtro), PiP aberto, papel de parede aberto. Só listar câmeras não conta. |

Premissas minhas, aceitas: a primeira detecção da placa (ou uma troca de modo) captura por um instante para validar o modo e registrar a câmera, como hoje. Uma vaga vale como uso por 3 s depois do último sinal.

## Arquitetura

```
Aplicativo (filtro MultiCam.Camera.dll) --+
Viewer (PiP / papel de parede) -----------+--> demand: Local\MultiCam-{GUID}-demand (16 vagas)
                                          +--> evento Local\MultiCam-wake (acorda o painel)
Painel: lê as vagas -> CaptureScheduler -> inicia / mantém / encerra o worker da placa
Worker -> canal de quadros (sem mudança) -> aplicativos e viewers
```

### `native/demand.hpp` (novo)

Memória compartilhada por câmera, `Local\MultiCam-{GUID}-demand`, criada por quem chegar primeiro (`CreateFileMappingW`, leitura e escrita). Layout fixo, igual em x86 e x64:

- Cabeçalho de 16 bytes: `magic` (uint32), `version` (uint32), `nextToken` (uint32, contador para fichas únicas), reservado (uint32).
- 16 vagas de 16 bytes: `owner` (uint32, 0 = livre), `pid` (uint32), `tick` (uint64, `GetTickCount64` do último sinal).
- Tamanho total: 272 bytes.

Operações do cliente (`multi::DemandClient`):

- `begin(id)`: abre ou cria a área, pega uma ficha (`InterlockedIncrement` em `nextToken`, nunca 0) e procura uma vaga livre com `InterlockedCompareExchange(owner, ficha, 0)`. Grava `pid` e `tick` e dispara o evento `Local\MultiCam-wake`. Se não houver vaga livre, o cliente segue sem sinalizar.
- `beat()`: regrava `tick`, no máximo a cada 500 ms (chamadas mais frequentes são ignoradas).
- `end()`: libera a vaga (`owner = 0`) só se ela ainda for da ficha. Também é chamado pelo destrutor.

Leitura (`multi::demandActive(id, now)` em C++ e a mesma regra em C#): há uso se alguma vaga tem `owner != 0` e `now - tick < 3000` ms.

### Clientes

- **Filtro (`native/camera.cpp`):** `begin` quando a thread de entrega começa (Pause/Run) e `end` no Stop. O laço de entrega chama `beat()` a cada volta. Como ele acorda pelo menos uma vez por período, o sinal fica sempre em dia.
- **Viewer (`native/viewer.cpp`):** `begin` ao ficar pronto, `beat()` no laço principal e `end` na saída.

### Painel

- **`app/Demand.cs` (novo):** `Demand.Active(cameraId)`, que lê a área com `MemoryMappedFile.OpenExisting`; se a área não existe, não há uso. Também tem a thread que espera o evento `Local\MultiCam-wake` e pede uma atualização imediata ao formulário (`BeginInvoke`, mesmo mecanismo do aviso de USB).
- **`CaptureScheduler` (em `app/Demand.cs`):** lógica pura com relógio injetado. Entrada: há uso agora, a sessão está rodando, a câmera já foi validada nesta conexão. Mantém o horário do último uso por câmera. Saída: `Start`, `Keep` ou `Stop`.
  - Não validada: `Start` (validação). Ao validar (`Published`), conta como uso naquele instante.
  - Há uso: `Start` se a sessão não roda; senão `Keep`.
  - Sem uso há menos de 60 s: `Keep`.
  - Sem uso há 60 s ou mais: `Stop` se a sessão roda; senão nada.
- **`RefreshCameras`:** antes de iniciar uma sessão, consulta o scheduler. Uma sessão só começa se a decisão for `Start` e os atrasos de nova tentativa (`retry`) permitirem. `Stop` chama `EndSession`, que mantém o registro. Nada mais muda: falhas, placa sumida por 10 s, câmera desativada, troca de modo.
- **Estado validado:** a câmera conta como validada enquanto estiver em `registered` (que já existe). `Stop(c)` a tira de lá, e a próxima conexão valida de novo.
- **Lista e bandeja:** nova legenda **Em espera** (registrada, placa parada). O texto da barra de status passa a contar as câmeras registradas: "N compartilhada(s)".
- **Viewers:** `cameraReady` continua sendo "câmera registrada". O viewer aberto sinaliza uso, e isso mantém a placa ligada.

## Erros e casos de borda

- **Worker cai durante o uso:** segue a regra de falhas de hoje (nova tentativa em 2 s, 3 falhas retiram o registro). O aplicativo vê o último quadro e depois preto.
- **Uso volta durante o minuto de folga:** a placa continua ligada, sem espera.
- **Aplicativo travado ou morto:** a vaga envelhece e deixa de contar em 3 s. Depois de mais 60 s, a placa para.
- **Tabela cheia (16 clientes da mesma câmera):** o 17º funciona, mas não sinaliza. Os outros já mantêm a placa ligada.
- **Painel fechado:** os clientes continuam criando a área e sinalizando; ninguém lê. Ao reabrir, o painel valida e segue pelas vagas.
- **Relógio:** `GetTickCount64` é do sistema todo, então os horários de processos diferentes são comparáveis.

## Testes

- **`MultiCam.DemandTest` (nativo):** pegar vaga e ter uso; `beat` mantém; `end` libera; vaga vencida (horário antigo) não conta; tabela cheia; dois clientes no mesmo processo; ficha nunca é 0.
- **`tests/panel.ps1`:**
  - `CaptureScheduler` com relógio falso: valida na primeira vez; uso inicia; 59 s sem uso mantém; 60 s para; uso volta e inicia.
  - Teste cruzado: `MultiCam.Integration --demand <guid> <segundos>` mantém uma vaga viva; `Demand.Active` em C# vê uso durante e não vê depois que o cliente sai (vaga liberada) nem depois que ele é morto (vaga vencida após 3 s).
- **`tests/pacing.ps1` e `tests/integration.ps1`:** continuam passando. O filtro agora também sinaliza uso.
- **Manual, com a placa e o Discord:**
  - abrir a câmera com a placa parada e medir o tempo até a imagem;
  - fechar e ver a placa parar depois de ~1 min, com o worker sumindo no Gerenciador de Tarefas;
  - dois Discords ao mesmo tempo;
  - PiP ligado mantém a placa ligada.

## Fora do escopo

- Liga e desliga por câmera nas configurações (o 1 minuto vale para todas).
- Manter o worker aberto com a placa pausada para religar mais rápido (abordagem B, recusada).
- Log do formato negociado por cada aplicativo (pendência separada).
