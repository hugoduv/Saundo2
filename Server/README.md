# Serveur audio Saundo2

Ce serveur associe un UID NFC à un fichier audio et expose le fichier avec
des réponses HTTP compatibles avec une lecture progressive.

## Pré-requis

- Node.js 18 ou plus récent
- Un fichier `soup.wav` et/ou `seep.wav` dans `server/audio/`

Les fichiers recommandés sont des WAV PCM mono 16 bits à 16 kHz, afin de rester
compatibles avec le lecteur DAC actuel de la STM32.

## Démarrage

Depuis le dossier `server` :

```powershell
npm start
```

Le serveur écoute par défaut sur le port `8080`. Le port peut être changé avec
la variable `PORT` :

```powershell
$env:PORT = "8080"
npm start
```

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
  "title": "soup",
  "format": "wav",
  "sampleRate": 16000,
  "channels": 1,
  "bitsPerSample": 16,
  "audioUrl": "/audio/soup.wav"
}
```

Lire le fichier :

```text
GET http://localhost:8080/audio/soup.wav
```

L'endpoint audio accepte aussi l'en-tête HTTP `Range`, utile pour le streaming
et la reprise d'un téléchargement.

## Ajouter une musique

1. Convertir le fichier en PCM mono 16 bits à 16 kHz, par exemple avec FFmpeg :

   ```powershell
   ffmpeg -i musique.mp3 -ar 16000 -ac 1 -sample_fmt s16 server/audio/musique.wav
   ```

2. Ajouter le fichier dans `server/audio/`.
3. Ajouter une entrée dans `server/data/tracks.json` :

   ```json
   {
     "uid": "0123456789ABCDEF",
     "title": "musique",
     "file": "musique.wav",
     "format": "wav",
     "sampleRate": 16000,
     "channels": 1,
     "bitsPerSample": 16,
     "enabled": true
   }
   ```

4. Redémarrer le serveur.

## Déploiement

Le serveur peut être déployé sur un VPS ou derrière un reverse proxy HTTPS.
Pour une utilisation hors réseau local, il faut obligatoirement ajouter HTTPS,
une authentification de l'appareil et une protection des fichiers audio avant
de le rendre public.
