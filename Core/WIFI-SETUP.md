# Test Wi-Fi et serveur

Cette étape teste la connexion Wi-Fi au démarrage puis l'endpoint `/health`.
La lecture audio reste pour l'instant celle intégrée dans le firmware : le
streaming n'est pas encore implémenté.

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

En l'absence du fichier local, le firmware saute le test et conserve le
comportement NFC/audio existant. Le pilote initialise le module via SPI3
(16 bits, 10 MHz), indépendamment des paramètres SPI3 générés par CubeMX.
Le test est bloquant et ne s'exécute qu'au démarrage, avant la boucle NFC.
Les échecs sont signalés sur le port série ; redémarrer la carte pour réessayer.

La réussite est indiquée par `[WIFI] Serveur /health OK.`. Si la connexion
Wi-Fi fonctionne mais que le test TCP échoue, vérifier l'IPv4 du PC, le
serveur, le pare-feu et l'isolation des clients du téléphone.

Si la connexion échoue, le firmware indique si l'échec concerne la connexion
au point d'accès ou la lecture des paramètres réseau, puis recherche le SSID
configuré sans afficher le nom ni le mot de passe. Le scan est limité à
10 réseaux : l'absence du SSID dans les résultats ne prouve pas qu'il est
invisible. Le code de sécurité affiché vaut 0 pour ouvert, 1 pour WEP, 2 pour
WPA, 3 pour WPA2 et 4 pour WPA/WPA2 ; toute autre valeur est non reconnue.

## Pilote ST

Les fichiers `es_wifi.c`, `es_wifi_io.c`, `wifi.c` et leurs en-têtes
proviennent de l'exemple Wi-Fi B-L475E-IOT01A de STM32CubeL4 v1.18.2.
`es_wifi_io.c` a été adapté pour signaler les expirations SPI. Les conditions
d'utilisation se trouvent dans `STM32CubeL4-Package-license.md`.
