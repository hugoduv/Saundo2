# Saundo2 — note de présentation du projet

## 1. L’idée en quelques mots

Saundo2 est un lecteur audio piloté par des étiquettes NFC. On associe à
chaque étiquette une musique hébergée sur un ordinateur du réseau local. Quand
on approche l’étiquette du lecteur, la carte électronique lit son identifiant,
demande le morceau correspondant à l’ordinateur par Wi-Fi, puis le décode et
le joue sur une sortie audio. Retirer l’étiquette arrête la lecture.

Le projet réunit donc trois domaines :

1. **l’embarqué**, avec un microcontrôleur qui doit réagir à des événements et
   respecter le rythme de lecture audio ;
2. **les communications**, avec le NFC pour identifier l’étiquette et le Wi-Fi
   pour transporter le son ;
3. **le logiciel serveur et le traitement audio**, avec un catalogue, une
   conversion des fichiers et, à titre expérimental, des commandes vocales.

L’objectif est de rendre l’interaction simple — « poser une étiquette pour
écouter » — tout en gardant les fichiers musicaux sur un ordinateur contrôlé
par l’équipe.

## 2. Les composants

### Côté lecteur

- Une carte **B-L475E-IOT01A2**, construite autour du microcontrôleur
  **STM32L475VGTx**.
- Un module Wi-Fi **ISM43362**, intégré à la carte.
- Un lecteur NFC **X-NUCLEO-NFC08A1**, équipé d’un contrôleur ST25R3916B.
- Le microphone de la carte, utilisé par la fonction de commandes vocales.
- Le convertisseur numérique-analogique (**DAC**) du STM32 pour produire le
  signal audio. La sortie DAC utilisée est **PA4** : elle doit être reliée à
  une entrée d’amplificateur, avec une masse commune. Un haut-parleur ne doit
  pas être branché directement sur cette broche.

Le firmware est écrit en C avec la bibliothèque HAL de ST et le middleware
NFC. Il coordonne la détection des étiquettes, les échanges avec le module
Wi-Fi, le décodage MP3, le microphone et le DAC.

### Côté serveur

Le serveur est une application **Node.js** exécutée sur un ordinateur du même
réseau que la carte. Il utilise **FFmpeg** pour convertir les fichiers audio
dans un format adapté au lecteur, et expose une API HTTP.

Le catalogue des associations entre étiquettes et morceaux se trouve dans
[`Server/data/tracks.json`](Server/data/tracks.json). Les musiques originales
sont placées dans `Server/audio/`; les versions converties sont conservées
dans `Server/cache/`.

## 3. Que se passe-t-il quand on pose une étiquette ?

1. **Détection NFC.** Le lecteur obtient l’UID (*Unique Identifier*,
   identifiant unique) de l’étiquette, puis le représente sous forme
   hexadécimale. Le firmware vérifie régulièrement si l’étiquette est toujours
   présente. Si aucun passage du lecteur ne la redétecte pendant deux
   secondes, elle est considérée comme retirée.
2. **Recherche dans le catalogue.** La carte envoie une requête HTTP à
   `/api/stream-mp3/<UID>`. Le serveur relit le catalogue et cherche une
   entrée activée dont l’UID correspond.
3. **Préparation du morceau.** Si nécessaire, FFmpeg convertit le fichier
   d’origine en MP3 mono, à 32 kHz et 64 kbit/s. Cette conversion est mise en
   cache : les demandes suivantes peuvent réutiliser le fichier déjà préparé.
4. **Réception et décodage.** Le serveur envoie les octets MP3 sur le réseau.
   Le firmware reçoit des blocs, repère les trames MP3 complètes et les décode
   avec `minimp3`. Il transforme ensuite les échantillons audio en valeurs
   adaptées au DAC 12 bits.
5. **Lecture régulière.** Un temporisateur déclenche le DAC à 32 000
   échantillons par seconde. Le DMA (*Direct Memory Access*) alimente le DAC
   depuis un petit tampon en mémoire sans demander au processeur de recopier
   chaque échantillon à chaque instant.
6. **Fin ou retrait.** Quand le flux se termine, le lecteur termine le son.
   Quand l’étiquette est retirée — ou remplacée par une autre — la session
   courante est arrêtée et le nouvel UID peut lancer un autre morceau.

### Pourquoi un tampon ?

Le débit Wi-Fi n’est pas parfaitement constant : certains blocs arrivent plus
vite que d’autres. Le firmware accumule donc les échantillons décodés dans un
tampon circulaire avant de les envoyer au DAC. Il vise environ **384 ms**
d’audio avant de démarrer ou de recharger la lecture; le tampon peut contenir
environ **512 ms**.

Cette réserve absorbe de petites variations de réseau et de temps de
traitement, mais elle n’est pas infinie. Si le réseau ou le décodage tarde
plus longtemps que ce que le tampon peut compenser, le lecteur peut produire
du silence, signaler un manque de données et attendre que le tampon se remplisse
à nouveau.

## 4. Le serveur et le catalogue

Une entrée du catalogue contient notamment :

```json
{
  "uid": "0123456789ABCDEF",
  "title": "musique",
  "file": "musique.mp3",
  "enabled": true
}
```

Le nom du fichier doit désigner un fichier situé dans `Server/audio/`. Une
entrée peut être désactivée avec `"enabled": false`. Le serveur relit le
catalogue à chaque requête : une modification de `tracks.json` ne demande pas
de redémarrage.

La route utilisée par le firmware, `/api/stream-mp3/<UID>`, fournit le MP3
préparé. Le serveur garde aussi des routes WAV, notamment pour l’API historique
et les essais. Les réponses audio acceptent les requêtes HTTP **Range** :
elles permettent de ne demander qu’une partie du fichier. Un **ETag**
(identifiant de version de la ressource) aide le lecteur à reprendre un
téléchargement interrompu sans mélanger deux versions différentes du morceau.

La première demande d’un fichier peut être plus lente, car elle attend la
conversion complète par FFmpeg. Si l’UID n’existe pas, si l’entrée est
désactivée ou si le fichier manque, le serveur répond avec une erreur plutôt
que de fournir un morceau de remplacement.

## 5. Les commandes vocales expérimentales

Quand une session d’étiquette est active, le firmware peut aussi envoyer le son
du microphone au serveur. La reconnaissance est réalisée localement sur un
serveur **Windows**, avec le moteur vocal Windows configuré en français. Les
commandes actuellement prévues sont :

- **« pause »** : suspend la lecture sans perdre sa position ;
- **« play »** : reprend à la position de pause ;
- **« recommence »** : redémarre le morceau depuis le début.

Le firmware transmet actuellement des échantillons mono à 16 kHz codés sur
8 bits non signés. Le serveur les convertit en PCM 16 bits pour le moteur de
reconnaissance. Celui-ci examine des fenêtres de 1,5 seconde qui avancent par
pas de 0,5 seconde. Le microphone reste actif pendant la session du tag, y
compris durant une pause ou après la fin du morceau. Retirer le tag arrête
l’écoute. Il n’existe pas de commande vocale « stop ».

Cette fonction est indépendante de la lecture audio : si le serveur vocal ou
son moteur ne sont pas disponibles, la lecture reste utilisable. Le firmware et
le serveur n’acceptent qu’une connexion microphone à la fois.

## 6. Mise en route (vue d’ensemble)

1. Installer **Node.js 18 ou une version plus récente** et **FFmpeg** sur le
   PC. L’encodeur `libmp3lame` de FFmpeg est requis pour produire le MP3.
2. Placer les morceaux dans `Server/audio/`, puis associer leurs UID dans
   `Server/data/tracks.json`.
3. Copier les paramètres Wi-Fi d’exemple vers une configuration locale du
   firmware et y indiquer le nom du réseau, son mot de passe, l’adresse IP du
   PC et le port du serveur. Le fichier contenant le mot de passe ne doit pas
   être partagé.
4. Démarrer le serveur depuis le dossier `Server` avec `npm.cmd start`. Par
   défaut, l’API HTTP écoute sur le port **8080**.
5. Compiler et programmer le firmware, puis vérifier sur la console série que
   le module Wi-Fi rejoint le réseau et que le test HTTP `/health` du serveur
   réussit.
6. Relier la sortie PA4 à un amplificateur, puis tester une étiquette connue.

Les instructions détaillées de configuration sont dans
[`Core/WIFI-SETUP.md`](Core/WIFI-SETUP.md) et
[`Server/README.md`](Server/README.md). Le projet principal est présenté dans
[`README.md`](README.md).

## 7. Limites et points de vigilance

### Stabilité temps réel à confirmer sur la carte

Le firmware doit partager le temps du processeur entre le décodage MP3, le
service Wi-Fi, les événements NFC et le traitement du microphone. Le DMA et
les tampons réduisent la charge immédiate, mais ne garantissent pas à eux seuls
une lecture sans interruption. Les tests de décodage exécutés sur PC vérifient
des cas logiciels; ils ne démontrent pas que le microcontrôleur tient le débit
dans toutes les conditions réelles.

La lecture continue, les retraits d’étiquette, les commandes vocales et leur
coexistence doivent donc être éprouvés sur la carte. Les diagnostics série
permettent notamment d’observer les durées de réception, de décodage et de
traitement NFC/voix ainsi que les recharges du tampon. Une reconnexion Wi-Fi
peut entraîner une pause audible. La reprise HTTP limite les données à
retélécharger, mais ne supprime pas ce temps d’attente.

### Dépendance au réseau local

La carte et le PC doivent pouvoir communiquer sur le même réseau. Le firmware
est configuré avec une adresse IP de serveur : si cette adresse change, il faut
mettre à jour la configuration. La qualité du signal, la congestion du réseau,
le point d’accès et les délais du module Wi-Fi influencent l’expérience.
Le débit MP3 est nettement inférieur à celui du flux PCM historique, mais
cela ne corrige pas automatiquement les problèmes de communication SPI ou de
signal DRDY déjà observés avec le module Wi-Fi.

### Sécurité et confidentialité

Le serveur utilise **HTTP**, sans chiffrement ni authentification de l’appareil.
Le flux audio et, si la fonction vocale est activée, les échantillons du
microphone circulent en clair sur le réseau. Le logiciel indique ne pas
enregistrer ces échantillons, mais ils sont transmis au PC pour reconnaissance.
Il faut donc réserver l’installation à un réseau local de confiance et ne pas
exposer le serveur ou ses ports vocaux à Internet.

### Reconnaissance vocale non garantie

La reconnaissance dépend de Windows, de la présence du moteur français, du
microphone, du bruit ambiant et du son joué par le haut-parleur. La musique peut
masquer une commande ou contribuer à une reconnaissance erronée. Si le tampon
microphone déborde, des échantillons anciens sont écartés; une commande dite
pendant cette perte peut ne pas être reconnue. Cette fonction est donc
expérimentale, pas une interface de contrôle garantie.

### Contraintes matérielles et de contenu

- Un amplificateur est nécessaire pour attaquer un haut-parleur depuis PA4.
- Le serveur dépend de FFmpeg; un fichier source absent, illisible ou sans piste
  audio utilisable ne peut pas être servi correctement.
- La conversion initiale prend du temps et demande de l’espace disque pour le
  cache.
- Le catalogue fait le lien entre un UID et un fichier : il faut maintenir ces
  associations et tester les étiquettes réellement utilisées.

## 8. Comment évaluer le projet

Une démonstration complète devrait vérifier successivement :

1. que chaque étiquette reconnue donne le bon UID et le bon morceau ;
2. que la lecture démarre, continue jusqu’au bout et s’arrête au retrait ;
3. que le remplacement rapide d’une étiquette ne laisse pas l’ancienne session
   active ;
4. que pause, reprise et redémarrage vocaux fonctionnent avec le son réel ;
5. que les essais restent fiables quand le signal Wi-Fi est moins bon ;
6. que les journaux ne montrent pas de sous-alimentation répétée du tampon,
   d’erreur DMA ou de déconnexion.

Ces essais doivent être répétés sur le matériel cible : une réussite sur PC ou
une compilation correcte ne suffit pas à valider le comportement temps réel.

## 9. Petit glossaire

- **UID** : identifiant numérique lu sur une étiquette NFC.
- **NFC** : communication radio à très courte portée, utilisée ici pour lire
  l’étiquette présentée au lecteur.
- **MP3** : format de compression audio qui réduit le nombre d’octets à
  transmettre, au prix d’une perte d’information par rapport à certains
  formats non compressés.
- **PCM** : représentation numérique directe des échantillons sonores.
- **Échantillonnage à 32 kHz** : production de 32 000 valeurs de son par
  seconde et par canal.
- **DAC** : circuit qui transforme des nombres calculés par le microcontrôleur
  en une tension analogique.
- **DMA** : mécanisme matériel qui transfère des données entre mémoire et
  périphérique avec peu d’intervention du processeur.
- **Tampon circulaire** : zone mémoire réutilisée en boucle, où un composant
  peut déposer des données pendant qu’un autre les consomme.
- **HTTP Range / ETag** : mécanismes HTTP pour demander une portion d’un
  fichier et vérifier qu’une reprise concerne toujours la même version.
