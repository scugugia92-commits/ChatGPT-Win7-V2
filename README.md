# ChatGPT Win7 x86 — V2

Client OpenAI nativo Win32 pensato per **Windows 7 SP1 32 bit**, senza Electron, WebView2 o .NET moderno.

## Funzioni V2

- Interfaccia scura con barra laterale e conversazioni locali.
- Target **x86 / 32 bit**.
- Rilevamento SSSE3 a runtime con percorso SIMD dedicato e fallback x86.
- OpenAI **Responses API** con streaming del testo.
- Allegati multipli tramite pulsante `+`.
- Drag & drop dei file nella finestra.
- Incolla screenshot/immagine dagli appunti con `Ctrl+V` nel campo messaggio.
- Immagini: PNG, JPG/JPEG, WEBP e GIF non animata.
- File: PDF, TXT/MD/JSON/XML/HTML, DOC/DOCX/RTF/ODT, PPT/PPTX, CSV/XLS/XLSX e altri formati accettati dall'API.
- Cronologia locale in `%APPDATA%\ChatGPT-Win7\chats`.
- API key non incorporata nell'eseguibile.
- `Ctrl+Invio` invia il messaggio.
- Pulsante `Stop` interrompe la richiesta in corso.

## Come ottenere l'EXE senza compilare sul PC

1. Crea un repository gratuito su GitHub.
2. Carica **tutto il contenuto di questa cartella**, compresa `.github`.
3. Apri la scheda **Actions** del repository.
4. Seleziona **Build Windows 7 x86 EXE**.
5. Premi **Run workflow**.
6. A compilazione conclusa, nella pagina del workflow scarica l'artifact **ChatGPT-Win7-x86-V2**.
7. Dentro troverai `ChatGPT-Win7-x86-V2.zip`, che contiene `ChatGPT-Win7.exe`.

La compilazione avviene online con MinGW-w64 in modalità i686 e collegamento statico del runtime C++.

## API key

L'API di OpenAI è distinta dall'abbonamento ChatGPT. Serve una chiave API valida con fatturazione API attiva.

Sul PC Windows 7 esegui:

    set_api_key.bat

Incolla la chiave. Poi **chiudi e riapri** il client.

In alternativa, dal Prompt dei comandi:

    setx OPENAI_API_KEY "sk-..."

## Windows 7 e TLS

Il programma usa WinHTTP e forza TLS 1.2. Windows 7 SP1 deve avere gli aggiornamenti Schannel/TLS 1.2 e i certificati radice sufficientemente aggiornati per collegarsi a `api.openai.com`.

La verifica HTTPS dei certificati NON viene disabilitata.

## Allegati

Il client carica prima gli allegati su `/v1/files`:

- immagini con `purpose=vision`, poi le passa come `input_image`;
- documenti con `purpose=user_data`, poi li passa come `input_file`.

I PDF vengono elaborati dall'API includendo testo e immagini delle pagine sui modelli vision. Per i documenti non-PDF vengono normalmente estratti i contenuti testuali. Per i fogli di calcolo l'API applica il proprio flusso di elaborazione spreadsheet.

Il client blocca file singoli oltre 50 MB per restare entro i limiti pratici degli input file.

## Sicurezza

Questa build è pensata per uso personale. Una API key inserita in un'app desktop rimane accessibile all'utente del computer. Non distribuire una tua API key ad altre persone insieme al programma.

## Nota sulla grafica Markdown

La V2 mostra correttamente il testo e conserva la sintassi Markdown prodotta dal modello. Per mantenere dipendenze minime e massima compatibilità con Windows 7 non integra Chromium/WebView. Una futura V3 può aggiungere un renderer RichEdit per titoli, grassetto e blocchi di codice senza browser embedded.
