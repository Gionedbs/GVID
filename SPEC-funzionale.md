# Specifica funzionale: video server 4 IN / 2 OUT

Documento per sviluppare un prodotto nuovo. Descrive comportamenti, non contiene codice del software di riferimento (SMSexpress 64 bit). Lo pseudocodice decompilato non va copiato nel progetto.

Legenda: **[O]** osservato nel programma, **[D]** dedotto dall'osservazione, **[N]** decisione per il nuovo progetto.

## 1. Requisiti della macchina [N]

| Voce | Valore |
|---|---|
| Ingressi / uscite | 4 SDI IN, 2 SDI OUT |
| Sincronismo | Ingresso reference (black burst o tri-level), uscite agganciate |
| Rete | Controllo e trasferimento file su porte separate |
| Codec | XDCAM HD422: MPEG-2 4:2:2 HL, 50 Mb/s, GOP 12 (M=3), MXF OP1a |
| Formato | 1080i50 |
| Audio | 4 tracce PCM 24 bit 48 kHz |
| Timecode | LTC, commutabile tra generatore interno ed esterno |
| Scheda | AJA Corvid 88 (8 SDI bidirezionali, reference, LTC) |
| Telecomando | Pannello proprietario da sviluppare in seguito, non compatibile RUS |
| Occupazione | Circa 25 GB/ora per canale, 98 GB/ora con 4 ingressi attivi |
| Prototipo | Server Dell PowerEdge R530 recuperato da una macchina YouPlay (Axel Technology) |

### Prototipo: Dell PowerEdge R530 (dati di catalogo, da confrontare con la macchina reale)

| Voce | Valore |
|---|---|
| Formato | Rack 2U |
| CPU | 1 o 2 Intel Xeon E5-2600 v3/v4, chipset C610 |
| RAM | 12 slot DDR4, fino a 384 GB |
| Slot | 5 PCIe a mezza altezza (3 PCIe 3.0, 2 PCIe 2.0); riser opzionale per slot a piena altezza |
| Dischi | Fino a 8 da 3,5" SAS/SATA/SSD |
| RAID | PERC S130 (software), H330, H730, H730P |
| Gestione | iDRAC8 |
| Sistemi supportati da Dell | Windows Server 2008/2012 (R2), SUSE, Red Hat |

- La Corvid 88 esiste in versione a basso profilo (CRV88-9-S), che entra negli slot a mezza altezza senza riser. È una scheda PCIe 2.0 x8, va bene in uno slot PCIe 3.0. Con la versione a basso profilo i connettori SDI sono su cavi di breakout.
- YouPlay supporta schede Blackmagic, AJA Corvid e Bluefish: la scheda già montata potrebbe essere una di queste.

### Prototipo: configurazione reale (inventario del 18/11/2026)

| Voce | Valore |
|---|---|
| BIOS | 1.5.4 del 10/05/2015 |
| Sistema | Windows 10 Pro 1607 (build 14393), installato il 22/09/2016, 64 bit |
| CPU | 2 × Xeon E5-2620 v3, 2,4 GHz: 12 core, 24 thread |
| RAM | 32 GB, 4 × 8 GB DDR4-2133 RDIMM (A1, A2, B1, B2), 8 slot liberi |
| Disco di sistema | SSD Kingston SV300S3 da 120 GB sul SATA della scheda madre, senza RAID. C: 105 GB (33 liberi), S: "Setup" 2 GB |
| Dischi media | PERC H330 Mini, volume RAID 1 da 5588 GB. W: "HD-1 (MEDIA-RAID1)", 2086 GB liberi |
| Scheda video I/O | Blackmagic DeckLink: 4 canali SDI (ID PCI BDBD:A11B, 4 istanze), probabilmente una sola scheda a 4 canali da identificare a vista |
| Timecode | Adrienne Electronics (AEC) PCIe-TC, driver 5.29.12 (lettore/generatore LTC). Tre funzioni secondarie (2392, 2393, 2394) senza driver |
| Grafica | NVIDIA NVS 300, 512 MB |
| Rete | 4 × Broadcom NetXtreme Gigabit, una collegata a 1 Gb/s e tre disattivate |
| Slot PCIe | 1, 3 e 4 occupati (DeckLink, AEC, NVS 300); 2 e 5 liberi |

Conseguenze:
- Il sistema operativo soddisfa i requisiti del kit (Windows 10 1607). Non riceve più aggiornamenti: tenere la macchina fuori da Internet.
- L'SSD di sistema è un modello consumer di circa 10 anni, senza ridondanza: è il punto più fragile della macchina e va salvato per primo.
- Le prove vanno fatte sul volume media: `esegui_prove.ps1 -Cartella W:\prove`.
- La DeckLink presente offre 4 canali SDI in totale, ognuno usato come ingresso o come uscita. Non bastano per 4 IN + 2 OUT, ma permettono le prime prove su hardware vero (per esempio 2 IN + 2 OUT) prima di acquistare la Corvid 88. SDK Blackmagic gratuito.
- YouPlay ricava l'LTC dalla scheda AEC, non dalla scheda video. Una combinazione DeckLink + AEC è un'alternativa alla Corvid 88, da valutare. Va verificato come viene gestito il reference, perché non risulta un ingresso dedicato.
- Con 2 slot liberi la Corvid 88 a basso profilo si può montare senza togliere le schede attuali, se uno dei due slot è x8.

Prove con scheda simulata sull'R530 (18/11/2026, `prototipo/rapporto-prove-R530-2026-11-18.txt`):
- Tutte le prove funzionali superate: registrazione, riproduzione e rallenty senza fotogrammi errati, catalogo e playlist.
- Registrazione 4 canali in tempo reale per 120 s: nessun fotogramma perso, coda massima 8 su 24.
- Alle velocita' 2x e 3x il sistema arriva a circa 1,05x il tempo reale. Il limite qui e' probabilmente il disco (SSD esterno a 31 MB/s, velocita' da USB 2.0), non la CPU: va rimisurato con USB 3.0 attivo.
- Tempo di cue in riproduzione: 150-380 ms.
- Con le DLL FFmpeg di build MinGW i programmi non partono su Windows 10 1607 (0xC0000139): FFmpeg va compilato con MSVC (`vserver/tools/build-ffmpeg.sh`).

Dischi per le prove [N]: si usano SSD esterni USB, collegati alle porte USB 3.0 posteriori. I dischi originali YouPlay (SSD di sistema e volume media W:) restano intatti.
- Fasi 1–2: Windows YouPlay esistente, cartella delle prove sull'SSD esterno.
- Dalla fase 4: un Windows separato avviato da un SSD esterno, perché i driver Blackmagic e gli SDK aggiornati modificherebbero il sistema YouPlay.
- Le misure di prestazioni disco e la prova di mancanza di corrente sugli SSD esterni non valgono per la macchina finale: vanno ripetute sui dischi definitivi.

Fasi di prova sulla macchina:
1. Immagine dei dischi originali, poi inventario (`kit-R530/inventario.ps1`).
2. Prove con scheda simulata (`kit-R530/esegui_prove.ps1`): disco, registrazione 4 canali in tempo reale e a velocità 2× e 3×, riproduzione, catalogo e playlist. Richiede Windows 10 / Server 2016 o successivo.
3. Scelta della scheda: quella già presente, se adatta, oppure Corvid 88 a basso profilo.
4. Integrazione della scheda reale: ingressi, uscite, reference, LTC.
5. Prove lunghe (ore) e prova di mancanza di corrente.

## 2. Modello generale

- Il programma è organizzato per canale, con strutture dati separate e fino a 6 canali. [O]
- Le clip sono organizzate in banchi e pagine da 18 posizioni, adatte a un pannello di comando. [O]
- Gli identificativi clip sono inferiori a 20.000. [O]
- Ogni clip ha un descrittore di dimensione fissa (512 byte). [O] Nel nuovo progetto diventa un record di database. [N]
- Gli errori operativi vanno sia nel registro a video sia nel file di log, con il canale indicato. [O]

## 3. Velocità e slow motion

- La velocità è un intero con segno: **16 = 1× avanti**, **-16 = 1× indietro**, **0 = stop**. [O]
- I valori intermedi indicano la velocità in sedicesimi, quindi 8 sarebbe 0,5×. [D]
- La velocità richiesta passa da una funzione di correzione prima di essere applicata. [O] Le sue regole non sono state ancora ricostruite.
- A velocità 1× avanti e indietro esistono percorsi dedicati, separati dal rallenty. [O]
- La posizione avanza per campi, con incrementi e decrementi in base al segno. [O]
- Anche una riduzione a stop passa dalla stessa logica: velocità 0 ferma il canale. [O]

Nuovo progetto [N]:
- Mantenere la scala a sedicesimi nell'API interna, perché è semplice e già collaudata sul campo.
- XDCAM è long-GOP: servono decodifica anticipata e cache di fotogrammi in memoria per canale, invece del salto diretto possibile con JPEG2000.
- La cache deve coprire almeno un GOP aperto più il GOP precedente, sia avanti sia indietro.

## 4. Playlist

- Una playlist per canale, fino a 1000 elementi da 20 byte. [O]
- Ogni elemento ha clip, tipo, punto iniziale e punto finale. [D]
- Prima del caricamento il sistema attende che il canale sia pronto, con timeout e messaggio di errore che indica banco e pagina. [O]
- Un puntatore oltre la lista è un errore. Un puntatore pari alla fine viene riportato sull'ultimo elemento con un indicatore di fine. [O]
- È possibile partire da un offset, espresso in fotogrammi e convertito in campi. [O]
- Un elemento può riferirsi a una clip su un altro server, caricata via rete. [O]
- Con un'opzione per canale, il canale successivo carica la stessa clip in parallelo (funzionamento accoppiato). [O]
- Operazioni disponibili: inserimento, spostamento, taglio, modifica punti IN/OUT, conversione in nuova clip, loop e transizioni. [O]

Nuovo progetto [N]: conservare queste regole. Il funzionamento accoppiato coincide bene con le 2 uscite (programma e backup, oppure fill e key).

## 5. Timecode

- Generatore con sorgente interna o LTC esterno. [O]
- Uscita LTC configurabile, con modalità loop. [O]
- Il pannello di stato mostra lettore, generatore, offset e l'avviso "no signal". [O]
- Esiste una tabella di ricerca del timecode su disco: 2880 voci, una ogni 1500 unità di posizione, salvata come file e ricostruibile su richiesta. [O]

Nuovo progetto [N]:
- Timecode di ogni fotogramma salvato nel file MXF e nel database.
- Ricerca per timecode tramite indice del database, senza tabella separata.
- Segnalazione esplicita di assenza o discontinuità del timecode.

## 6. Genlock

- Il genlock è impostato per canale, con una funzione che applica la modifica a tutti i canali. [O]
- Il comportamento dipende dal chip della scheda (LMH1982 o GS4911). [O]
- Esiste una tabella di offset per formato e frequenza, più una regolazione utente limitata a ±3999. [O]
- Lo stato di aggancio è mostrato nell'interfaccia. [O]

Nuovo progetto [N]:
- Il controllo dei chip viene sostituito dall'SDK della scheda.
- Vanno mantenuti: stato di aggancio per canale, regolazione di fase delle uscite (con i limiti della scheda scelta), allarme alla perdita del reference.

## 7. Parametri di codifica verificati [N]

Prova eseguita con FFmpeg 9.0 LGPL, su segnale sintetico e 4 canali in parallelo:

`mpeg2video`, `yuv422p`, profilo 4:2:2, livello HL, 50 Mb/s con min e max uguali, buffer 17825792, GOP 12, 2 B-frame, GOP aperto, `+ildct+ilme`, scansione alternata, DC a 10 bit, quantizzazione non lineare con `qmax 28`, tabella VLC intra, BT.709, contenitore MXF.

Il GOP chiuso produce GOP da 10 fotogrammi, quindi non è utilizzabile con questi parametri.

## 8. Aperti

Da verificare:
- Con AJA, prima dell'acquisto: reference e LTC vanno usati insieme, ma il connettore posteriore è commutabile tra i due. Il secondo ingresso LTC è su un header interno e richiede un cablaggio dedicato.
- Apertura dei file MXF nei sistemi di montaggio, usando un file XDCAM di riferimento per il confronto.
- Prestazioni e sincronismo su scheda reale.
- Prototipo R530: inventario fatto il 18/11/2026 (vedi sezione 1). Restano da verificare a vista il modello esatto della DeckLink e della scheda AEC, i dischi fisici dietro il PERC, la larghezza (x4/x8) e la generazione degli slot liberi 2 e 5, e il collegamento del reference. Prima di qualsiasi modifica va fatta un'immagine completa dei dischi originali YouPlay. Da verificare che la Corvid 88 entri fisicamente in uno slot PCIe 3.0 x8 e che la scheda già presente non possa sostituirla.
- Fotogrammi persi: ora ogni buco è coperto ripetendo l'immagine precedente con audio muto e viene conteggiato. Le ripetizioni passano dall'encoder, quindi un sovraccarico prolungato ritarda la chiusura del file: va misurato sulla macchina finale.
- Costanza del bitrate su contenuti reali: con un pattern sintetico anomalo oscilla per GOP, anche usando FFmpeg da riga di comando.
- Riproduzione: i campi di parità opposta sono interpolati dalle righe vicine; la qualità del rallenty va valutata a occhio su video reale. Se la decodifica non è pronta, l'uscita ripete il fotogramma precedente e la posizione resta ferma, quindi la clip dura un po' di più.
- Catalogo: integrità provata con l'arresto brusco del processo; una vera mancanza di corrente va provata sulla macchina e sui dischi definitivi. I file sono indicati con il percorso completo, quindi spostarli richiederà una procedura dedicata.
- Regole di correzione della velocità e curve del rallenty.
- Transizioni delle playlist.
