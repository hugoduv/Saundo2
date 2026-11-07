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

## Démarrage

Depuis le dossier `server` :

```powershell
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
