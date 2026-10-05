# Provenienza e dipendenze

Sorgenti ufficiali Frictional Games, copiati alle revisioni indicate in `upstream.json`:

- PenumbraOverture: `6a6953a9e02a6b0c152f3de78b2dc9b3a28948dc`.
- HPL1Engine: `5e441bbb247a7473e75cc0f05ca8a5b62c6ec64c`.
- OALWrapper: `668ad1f2ae88241d244a171388e929991a97de30`.

Licenze originali conservate. Le modifiche moderne comprendono comparatori const, rimozione del pragma GLaux inutilizzato, selezione del backend Windows, macro/stringhe C++17, ricompilazione di AngelScript, profili isolati e adattatore di rete.

AngelScript **2.7.1b / 20701**, copyright Andreas Jönsson, licenza zlib nelle intestazioni originali. Archivio ufficiale: https://www.angelcode.com/angelscript/sdk/files/angelscript_2.7.1b.zip ; SHA256 `FE3C8AFDFCB3D5F352DB6DED75C2F9EE8B2F8E72ED14E02466A1D992FE80D20F`. In `as_config.h` il vecchio alias `vsnprintf` è disabilitato per MSVC moderno. Nessuna libreria AngelScript precompilata del vecchio compilatore viene usata.

ENet **1.3.18**, copyright Lee Salzman, licenza MIT inclusa in `third_party/enet/LICENSE`. La copia proviene dalla mod Amnesia nello stesso workspace e contiene le modifiche Windows per il servizio UDP limitato a 64 buffer e la diagnostica dei pacchetti. Il trasporto applicativo è adattato da quella mod: magic `PO`, protocollo 2, UDP 27120. I due giochi non condividono sessioni. Il protocollo 2 aggiunge gesti affidabili del martello e rifiuta le build della mod con protocollo 1.

L'adattatore usa personaggi e mani nativi separati, equipaggiamento condiviso nell'inventario ma selezionato indipendentemente, danni e rotture decisi dall'host. HPL1 espone una correzione orizzontale di rete che conserva velocità verticale e forze; i sorgenti modificati sono inclusi nello ZIP corrispondente.

Header e import library Win32 di SDL, SDL_image, SDL_ttf, OpenAL, Ogg, Vorbis, Theora, Cg e Newton sono input di compilazione ottenuti dalla cartella `dependencies` di https://github.com/veryjos/penumbra_vr alla revisione `7d7071c74455409c2b6a58dce935b1efc0d5d51d`. Non è stato portato il codice VR. Gli input rimangono in `build/dependencies` e non vengono inclusi nel pacchetto binario o nello ZIP dei sorgenti.

Le DLL commerciali e gli asset vengono letti dall'installazione originale di Overture. Il runtime Steam è caricato facoltativamente da una DLL Valve firmata già presente sul PC; nessuna DLL Steam viene inclusa.
