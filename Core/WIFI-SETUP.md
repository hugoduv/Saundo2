# Test Wi-Fi et serveur

Au démarrage, le firmware teste la connexion Wi-Fi et l'endpoint `/health`.
Lorsqu'un tag est détecté, il demande son WAV au serveur et le lit pendant
la réception. Le serveur doit rester lancé pendant la lecture.

## Préparation

1. Activer le partage de connexion du téléphone en 2,4 GHz et WPA2 personnel.
   Connecter le PC au même partage. Certains téléphones isolent leurs clients :
   dans ce cas, il faudra utiliser un autre point d'accès.
2. Sur le PC, exécuter `ipconfig` et relever l'adresse IPv4 de l'interface Wi-Fi
   connectée au téléphone. Ce n'est pas nécessairement l'adresse de la passerelle.
3. Copier `Inc/wifi_settings.example.h` vers `Inc/wifi_settings.local.h`.
   Modifier localement le SSID, le mot de passe, les quatre octets de l'IPv4 du
   PC et, si nécessaire, le port du serveur. Saisir des chaînes C valides
   (échapper les guillemets et antislashs).
4. Le fichier local est ignoré par Git. Ne pas le partager : les identifiants
   sont également inclus dans le firmware compilé et dans les fichiers de build.
5. Redémarrer le serveur depuis `Server` avec `npm.cmd start`. Vérifier
   `http://localhost:8080/health` sur le PC. Si nécessaire, autoriser le port
   TCP 8080 dans le pare-feu Windows uniquement sur le réseau approprié.
6. Dans STM32CubeIDE, rafraîchir le projet, compiler et flasher la carte.
   Ouvrir le port série ST-LINK à 115200 bauds, 8N1, puis redémarrer la carte.

En l'absence du fichier local, le firmware saute le test et signale que le
streaming n'est pas disponible. Le pilote initialise le module via SPI3
(16 bits, 10 MHz), indépendamment des paramètres SPI3 générés par CubeMX.
Le test est bloquant et ne s'exécute qu'au démarrage, avant la boucle NFC.
Les échecs sont signalés sur le port série ; redémarrer la carte pour réessayer.

La réussite est indiquée par `[WIFI] Serveur /health OK.`. Si la connexion
Wi-Fi fonctionne mais que le test TCP échoue, vérifier l'IPv4 du PC, le
serveur, le pare-feu et l'isolation des clients du téléphone.

## Lecture en streaming

1. Redémarrer le serveur pour disposer de la route `/api/stream/<UID>`.
   FFmpeg doit être installé et configuré comme indiqué dans `Server/README.md`.
2. Après `[WIFI] Serveur /health OK.`, poser un tag associé à un fichier dans
   `Server/data/tracks.json`. Attendre `[AUDIO] Lecture en cours.`. La première
   demande peut prendre plus de temps car le serveur convertit le fichier.
3. Retirer le tag : la lecture s'arrête après le délai de détection de retrait
   (environ deux secondes lorsque la boucle NFC est disponible). Un tag inconnu,
   une erreur réseau, un WAV non pris en charge ou un buffer vide arrête la
   lecture et produit un message `[AUDIO]` sur le port série.

La carte lit le WAV PCM mono 16 bits à 16 kHz, convertit les échantillons pour
le DAC 8 bits et utilise un tampon de 32768 échantillons (environ deux secondes)
avec préchargement d'une seconde. Les en-têtes HTTP et les blocs WAV sont lus
progressivement ; il ne faut pas présumer un en-tête WAV de 44 octets. Le
pilote Wi-Fi utilise des opérations bloquantes avec délais : la réactivité et
le débit doivent être vérifiés sur la carte. En cas de perte réseau, retirer
le tag et redémarrer la carte après rétablissement du Wi-Fi. HTTP est prévu
pour ce prototype local, pas pour un serveur exposé sur Internet.

La commande vocale expérimentale utilise le microphone intégré et le serveur
Windows local. Autoriser aussi le port TCP suivant le port HTTP (8081 par
défaut), puis vérifier `[VOICE] Ecoute active.` avant de dire « pause »,
« play » ou « recommence ». « play » reprend à la position de pause ;
« recommence » relance au début. Retirer le tag arrête la lecture et l'écoute.
L'audio du microphone est transmis en clair sur le réseau local pendant la
session, y compris en pause et après la fin. Il n'est pas sauvegardé par le serveur.
Le fonctionnement et la fiabilité en présence du haut-parleur doivent être
vérifiés sur la carte ; voir `Server/README.md`.
Le microphone utilise DFSDM1 canal 1 et filtre 0 avec DMA1 canal 4 ; le
diviseur DFSDM est fixé à 50 dans la section utilisateur de
`HAL_DFSDM_ChannelMspInit()`, avant activation de l'horloge par la HAL. Après
régénération CubeMX, contrôler les affectations DMA avant de recompiler.

Le fichier `X-CUBE-NFC6/Target/demo_polling.c` transmet les UID détectés à
`nfc_events_card_seen()` pour déclencher la lecture. Après une régénération
du code NFC, vérifier que cet appel est toujours présent avant de recompiler.

Si la connexion échoue, le firmware indique si l'échec concerne la connexion
au point d'accès ou la lecture des paramètres réseau, puis recherche le SSID
configuré sans afficher le nom ni le mot de passe. Le scan est limité à
10 réseaux : l'absence du SSID dans les résultats ne prouve pas qu'il est
invisible. Le code de sécurité affiché vaut 0 pour ouvert, 1 pour WEP, 2 pour
WPA, 3 pour WPA2 et 4 pour WPA/WPA2 ; toute autre valeur est non reconnue.

Si l'envoi microphone échoue, le pilote affiche l'étape et un code d'erreur
SPI ou un statut de socket, sans afficher le contenu audio. Si les connexions
suivantes échouent aussi, redémarrer la carte après avoir vérifié que le serveur
est toujours actif ; conserver les messages série complets pour le diagnostic.
Si le serveur vocal répond `ERROR busy` après une coupure ou un redémarrage,
attendre 15 secondes que l'ancienne connexion inactive soit libérée, puis
retirer et reposer le tag.

## Pilote ST

Les fichiers `es_wifi.c`, `es_wifi_io.c`, `wifi.c` et leurs en-têtes
proviennent de l'exemple Wi-Fi B-L475E-IOT01A de STM32CubeL4 v1.18.2.
`es_wifi_io.c` a été adapté pour signaler les expirations SPI. Les conditions
d'utilisation se trouvent dans `STM32CubeL4-Package-license.md`.
