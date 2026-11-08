# Serveur audio Saundo2

Ce serveur associe un UID NFC à un fichier audio et expose le fichier avec
des réponses HTTP compatibles avec une lecture progressive.

## Pré-requis

- Node.js 18 ou plus récent
- FFmpeg accessible dans le `PATH` du serveur, ou configuré avec `FFMPEG_PATH`
- Les fichiers audio indiqués dans `data/tracks.json`, placés dans `audio/`

Le serveur convertit automatiquement les fichiers en WAV PCM mono 16 bits à
16 kHz lors de la première requête audio. La première lecture attend la fin
de la conversion ; les suivantes utilisent le fichier converti en cache.
Le fichier original n'est pas modifié.

## Commandes vocales expérimentales

Sur Windows, le serveur ouvre aussi un port TCP dédié à la reconnaissance :
le port HTTP plus un (par défaut **8081**). Le moteur de reconnaissance vocal
français Windows doit être installé. La carte envoie le signal de son microphone
tant que la session du tag est active, y compris pendant une pause et après la
fin du morceau. Le serveur recherche localement « pause », « play » et
« recommence » ; il ne sauvegarde pas les échantillons.

- **« pause »** suspend la lecture sans perdre la position.
- **« play »** reprend à la position de la pause.
- **« recommence »** relance le morceau au début, même après sa fin.
- Retirer la carte arrête la lecture et le microphone. « stop » n'est plus
  une commande vocale.

Le microphone et la reconnaissance restent à tester sur la carte réelle. Le son
du haut-parleur peut masquer la voix ou déclencher la commande par erreur.
L'audio circule en clair sur le réseau local et le port vocal n'est pas
authentifié : utiliser uniquement un réseau de confiance, ne pas exposer ces
ports sur Internet. Si nécessaire, autoriser le port TCP 8081 dans le pare-feu
uniquement sur le réseau approprié.

La lecture audio reste utilisable si la reconnaissance est indisponible. Les
erreurs apparaissent dans le terminal du serveur et sur le port série avec le
préfixe `[VOICE]`. Si le port HTTP est modifié, ouvrir le port suivant.
Une seule connexion vocale est acceptée à la fois. Après une coupure ou un
redémarrage de la carte, le serveur libère une connexion inactive au bout de
15 secondes. Si la carte affiche `ERROR busy`, attendre ce délai puis retirer
et reposer le tag pour réessayer.

## Démarrage

Depuis le dossier `server` :

```powershell
cd C:\Users\hugdu\Dev\STM32\Saundo2\Server
$env:FFMPEG_PATH = "C:\ffmpeg\bin\ffmpeg.exe"
npm.cmd start
```

Le serveur écoute par défaut sur le port `8080`. Le port peut être changé avec
la variable `PORT` :

```powershell
$env:PORT = "8080"
npm.cmd start
```

Si FFmpeg n'est pas dans le `PATH`, indiquer le chemin complet avant le lancement :

```powershell
$env:FFMPEG_PATH = "C:\ffmpeg\bin\ffmpeg.exe"
npm.cmd start
```

Il faut redémarrer le serveur après modification de son code ou de cette variable.

## API

Vérifier que le serveur fonctionne :

```text
GET http://localhost:8080/health
```

Rechercher une musique avec un UID :

```text
GET http://localhost:8080/api/tracks/04301DCA936680
```

Réponse :

```json
{
  "uid": "04301DCA936680",
  "title": "rick",
  "format": "wav",
  "sampleRate": 16000,
  "channels": 1,
  "bitsPerSample": 16,
  "audioUrl": "http://localhost:8080/audio/rick.wav"
}
```

Lire le fichier :

```text
GET http://localhost:8080/audio/rick.wav
```

L'endpoint audio accepte aussi l'en-tête HTTP `Range`, utile pour le streaming
et la reprise d'un téléchargement. Les métadonnées de l'API décrivent toujours
le fichier **converti**, et non le format du fichier original.

Pour un appareil embarqué, il est aussi possible de récupérer directement le
WAV converti associé à un UID, sans analyser la réponse JSON :

```text
GET http://localhost:8080/api/stream/04301DCA936680
```

Cette route renvoie 404 si l'UID est inconnu, désactivé ou si son fichier
audio manque. Elle accepte également `Range`. Un en-tête WAV ne fait pas
nécessairement 44 octets : le lecteur doit rechercher le bloc `data`.

## Ajouter une musique

1. Ajouter le fichier dans `audio/` (par exemple WAV ou MP3 lisible par FFmpeg).
2. Ajouter une entrée dans `data/tracks.json` :

   ```json
   {
     "uid": "0123456789ABCDEF",
     "title": "musique",
     "file": "musique.mp3",
     "enabled": true
   }
   ```

Le catalogue est relu à chaque requête : pas besoin de redémarrer pour le modifier.
Si le fichier original change, une nouvelle conversion est effectuée à la
prochaine demande. Les fichiers convertis sont stockés dans `cache/` ; les
anciennes versions peuvent être supprimées manuellement lorsque le serveur
est arrêté. Ce dossier n'est pas destiné à être versionné.

## Déploiement

Le serveur peut être déployé sur un VPS ou derrière un reverse proxy HTTPS.
Pour une utilisation hors réseau local, il faut obligatoirement ajouter HTTPS,
une authentification de l'appareil et une protection des fichiers audio avant
de le rendre public.
