# Validação local — 2026-09-21

Executável de desenvolvimento compilado para o Windows 10 x64 deste computador (build 19044), com filtros de câmera x64 e x86. Não é um instalador de driver de kernel.

## Passou

- Compilação C++ Release nas duas arquiteturas e painel C#/.NET Framework, sem erros.
- Enumeração real de `USB Video`, `USB3 Video` e `USB2 Video`. As duas últimas indicaram seriais USB diferentes; a primeira usa identidade por caminho/porta.
- Registro COM por usuário e descoberta da entrada virtual pelo enumerador DirectShow; remoção da entrada ao finalizar o teste.
- Fonte sintética Full HD/60: 540 quadros produzidos em 9 s. Dois clientes simultâneos receberam 301 quadros/5 s em x64/60 e 151 quadros/5 s em x86/30. Zero quadros rasgados, zero timestamps não crescentes.
- Fonte sintética 3840×2160/60: 540 quadros produzidos em 9 s. Clientes x64/60 e x86/30 receberam respectivamente 299 e 150 quadros/5 s. Zero quadros rasgados e zero timestamps não crescentes; um quadro preto de espera no cliente x64. Isso valida o transporte, não a captura física 4K60.
- `USB3 Video`, captura real Full HD/YUY2: após aquecimento, 60 quadros recebidos/s e zero descartes contados pelo worker. Clientes x64 e x86 receberam simultaneamente 301 amostras/5 s cada, sem timestamps não crescentes. Não foram gravadas imagens da câmera.
- Medição histórica de `USB2 Video` Full HD/MJPEG, antes da correção de desempenho abaixo: o modo anunciado de 50 FPS produziu aproximadamente 29–31 quadros decodificados/s. Ambos os consumidores receberam 251 amostras/5 s, com repetições. Essa medição incluía o gargalo do decodificador e não comprova uma limitação da placa ou da conexão USB.
- Diagnóstico inicial do aplicativo: detectou, abriu e publicou as três placas simultaneamente. Amostra anterior à correção de desempenho: USB Video 24, USB3 Video 60 (YUY2), USB2 Video 25 (MJPEG). O gargalo no processamento MJPEG foi posteriormente identificado; consulte a nova medição abaixo.
- Painel renderizado e inspecionado. Nomes/modos persistiram entre execuções. Ao sair do diagnóstico, os workers terminaram e as entradas virtuais foram removidas.
- O filtro rejeita dimensão não anunciada antes da conexão. Duas taxas de consumo não alteram o modo da placa.

## Pendente no uso real

- Seleção e vídeo no Discord, OBS e outros aplicativos finais. Os testes usaram processos DirectShow próprios, não esses programas.
- Desconectar/reconectar fisicamente, trocar as duas placas para USB 3 e confirmar persistência dos nomes em outra porta. O ciclo automático está implementado, mas não foi testado desconectando cabos.
- Login/reinício real do Windows com a opção de inicialização automática marcada.
- Captura física 4K60, testes longos, atraso total, uso de CPU e fidelidade de cor em padrões conhecidos.

## Ajuste da lista de modos — 2026-09-21

O painel agrupa entradas com o mesmo rótulo completo (resolução, FPS arredondado e formato). O catálogo original do driver permanece intacto e, quando uma entrada agrupada corresponde ao modo salvo, seu intervalo exato é mantido. A alteração é somente de apresentação/seleção no painel.

Compilação C# concluída. Verificação com os perfis locais: USB Video passou de 94 para 83 opções visíveis, USB3 Video de 174 para 152 e USB2 Video de 114 para 105. Nenhum rótulo duplicado restou e os três modos salvos permaneceram selecionáveis com suas chaves exatas. Full HD/60 e Full HD/59,94 continuaram separados, em YUY2 e MJPEG. Não foi necessário reiniciar a captura para essa verificação.

## Correção de desempenho MJPEG — 2026-09-21

Diagnóstico no Intel Core i7-3770, com as três placas abertas: cada worker antigo consumia aproximadamente 100% de um processador lógico. O cache CMake x64 tinha as opções de Release vazias. Além disso, a conversão dos planos JPEG para YUY2 fazia divisões variáveis dentro do laço de pixels.

O CMake agora exige `/O2`, `/Ob2` e `NDEBUG` em Release, inclusive na biblioteca de captura. O conversor especializa a indexação para cada subamostragem e calcula a linha de crominância uma vez por linha. A fórmula de conversão de faixa de cor foi preservada.

Medições de contadores de quadros publicados, em janelas de 10 segundos, mantendo as configurações e as três capturas simultâneas:

| Placa | Modo mantido | Antes | Após correção |
| --- | --- | --- | --- |
| Câmera 1 / USB Video | 1280×720, 60 FPS, MJPEG | 51,0 FPS | aproximadamente 59 FPS |
| Câmera 2 / USB3 Video | 1920×1080, 60 FPS, MJPEG | 23,4 FPS | aproximadamente 60 FPS |
| Câmera 3 / USB2 Video | 1280×720, 60 FPS, MJPEG | 52,1 FPS | aproximadamente 60 FPS |

Apenas habilitar a otimização de compilação levou a USB3 a 45,1 FPS; especializar a conversão permitiu atingir 60 FPS. Uma amostra posterior de CPU indicou 27,3%, 38,5% e 79,7% de um processador lógico nos três workers, total de aproximadamente 146%, contra aproximadamente 299% antes. Não foram alterados os perfis, resoluções, intervalos ou formatos de captura.

Validações finais:

- `MultiCam.JpegTest`: 21 casos sintéticos (sete subamostragens, dimensões 18×9, 1280×720 e 1920×1080), comparação de todos os pixels contra a fórmula anterior, zero diferenças. Rejeição de JPEG inválido e dimensões incompatíveis.
- Compartilhamento físico USB3 em MJPEG: dois consumidores, x64 e x86, receberam 481 amostras cada em 8 s, com zero timestamps não crescentes. Durante o teste, a captura publicada permaneceu em aproximadamente 60 FPS. A contagem `Unique` desse observador examina somente os primeiros pixels e não mede quadros únicos de vídeo real.
- O teste simultâneo revelou um timestamp não crescente na inicialização do filtro antigo. Corrigido o limite inferior dos timestamps na transição de preroll para execução e em leituras repetidas do relógio. O teste sintético agora força uma pausa de 100 ms antes de iniciar.
- Teste sintético final: 540 quadros produzidos; consumidores x64/60 e x86/30 receberam 307 e 154 amostras, incluindo preroll. Zero quadros rasgados e zero timestamps não crescentes.
- Compilação dos componentes nativos x64 e x86 concluída. Workers atualizados e reiniciados; painel permaneceu aberto. Os erros do log continuam restritos ao teste antigo das 08:41.

Esses resultados correspondem aos modos acima e ao conteúdo recebido durante o teste. A medição conta quadros após decodificação/publicação, e não pacotes USB ou imagens visualmente distintas.

## PiP e papel de parede ao vivo — 2026-10-10

Toolchain portátil (`.buildtools`, MSVC 14.51) no Windows 10 build 19044 deste computador.

- `tests/viewer.ps1`, 12 casos, todos PASS. PiP: cor da conversão YUY2→RGB conferida em snapshot (vermelho puro BT.601), teste de clique com e sem *Travar*, troca de resolução da origem com ajuste da altura, saída quando o painel morre, linhas inválidas na entrada padrão, argumentos inválidos. Papel de parede: janela filha da `WorkerW`, média de 30 FPS desenhados com a opção 30 e de 60 com a opção 60 (fonte a 60), snapshot do monitor inteiro em *Ajustar*, monitor inexistente (código 4), saída quando o painel morre.
- Revisão independente (11/10/2026): `tests/viewer.ps1` passou a ter 14 casos, todos PASS: janela do papel de parede destruída (Explorer reiniciado) sai com código 6, e janela tirada do lugar volta a cobrir o monitor. `tests/panel.ps1` passou a 11 casos, todos PASS: códigos 4 e 6 esperam e tentam de novo sem desistir, e o contador de falhas zera após um período saudável. `MultiCam.ViewerCoreTest`: comando com BOM UTF-8 no início é aceito.
- `tests/panel.ps1`, 8 casos PASS: configurações antigas carregam, valores inválidos são corrigidos, presets 16:9 e 4:3, ciclo de vida de um PiP real, limite de 3 reinícios.
- `tests/integration.ps1` continua PASS depois das mudanças (307/154 amostras, zero rasgados, zero pretos).
- Verificação visual no PC: com o papel de parede ativo, ícones e barra de tarefas aparecem por cima; ao sair, o papel de parede original volta.
- `tests/portable-check.ps1` sobre o pacote limpo: só DLLs do Windows, sem runtime extra, sem `data`.

Pendente, só manual: mover, redimensionar, travar e destravar o PiP pelo painel com uma placa real; conferir que posição e tamanho voltam ao reiniciar; pausa do papel de parede com um programa em tela cheia; uso de CPU/GPU do papel de parede a 60 FPS; Windows 11.

## Reproduzir

`tests/integration.ps1` registra uma câmera temporária, produz quadros sintéticos, abre dois consumidores, verifica integridade/temporização e remove a entrada em `finally`. `-Width 3840 -Height 2160` exercita 4K60.

`tests/physical.ps1 -DeviceName 'USB3 Video'` abre a placa durante alguns segundos, registra saída temporária, verifica dois consumidores e limpa tudo. Não grava vídeo.

`dist/MultiCam.exe --diagnose` exercita o aplicativo completo durante aproximadamente 12 segundos, salva o relatório e uma imagem somente do painel em `artifacts/ui` e encerra. Perfis ficam salvos; a inicialização automática não é ativada pelo teste.

`python tests/live_status.py --seconds 10` lê apenas os contadores das capturas já abertas, sem acessar pixels nem abrir as placas novamente. `--output artifacts/status.json` salva a medição.

`tests/viewer.ps1` compila e testa o visualizador (PiP e papel de parede; a área de trabalho muda por alguns segundos). `tests/panel.ps1` testa a lógica do painel depois de `./build.ps1`. `tests/portable-check.ps1 [-Root pasta]` falha se algum binário depender de algo fora do Windows ou se houver a pasta `data`.

O alvo CMake `MultiCam.JpegTest` é opcional: compile-o com `--config Release --target MultiCam.JpegTest`, coloque `turbojpeg.dll` ao lado do executável em `build/x64/Release` e execute-o para comparar a conversão com a fórmula de referência.

Artefatos locais de teste estão em `artifacts/`, fora do controle de versão. Logs de antigas tentativas com conversores Windows permanecem no diagnóstico; a versão final usa libjpeg-turbo para MJPEG.
- Ritmo da câmera virtual (11/10/2026), `tests/pacing.ps1`: fonte parecida com uma placa real (relógio 0,1% mais lento e cada quadro chegando de 0 a 4 ms atrasado). Antes da correção, um app a 60 FPS recebeu 30 quadros repetidos e 29 pulados em 10 s; depois, 0 e 0 nos clientes x64 60 FPS, x64 30 FPS e x86 60 FPS. Com a fonte parada, o app continua recebendo cerca de 60 FPS (último quadro e depois preto). CPU com dois apps em 1080p60: 5% de um núcleo por app, contra 9,6% antes (cópia direta da memória compartilhada para o buffer do app).
- Decodificação MJPEG em 2 threads (11/10/2026), `MultiCam.PipelineTest`: ordem de captura preservada, captura nunca bloqueada e falha de decodificação sem travar a fila. Com quadros de 12 ms chegando a cada 8 ms, 1 thread publica 38 de 60 e 2 threads publicam 60 de 60. JPEG 1080p real com muito detalhe a 60 FPS: 33 FPS com 1 thread e 57 FPS com 2. Na FreeSky real (MJPEG 1080p60, imagem simples): 60 FPS e 61% de um núcleo.
