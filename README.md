NFC card reader, MP3 audio streamer, Cloud server, for STM32 (STM32L475VGT6)

## Configuration audio DAC

Le bouton utilisateur lance la lecture de `soup.c` sur la sortie `DAC1_OUT1` :

- **Sortie DAC** : canal 1, broche PA4 (`ARD_D7`)
- **Déclenchement DAC** : `TIM6_TRGO`
- **Format de sortie** : alignement DAC 8 bits (`DAC_ALIGN_8B_R`)
- **Source audio** : WAV PCM mono 16 bits, converti en échantillons non signés 8 bits avant l'envoi au DAC
- **Fréquence d'échantillonnage** : 16 kHz
- **TIM6** :
  - fréquence horloge : 80 MHz
  - prescaler : 39
  - période (ARR) : 124
  - fréquence de déclenchement : `80 MHz / ((39 + 1) × (124 + 1)) = 16 kHz`
- **DMA DAC** : `DMA1_Channel3`, requête 6, transfert mémoire vers périphérique, mode normal
- **DMA** : incrément mémoire activé, incrément périphérique désactivé, alignement mémoire et périphérique sur octet

Le fichier WAV doit donc être échantillonné à 16 kHz pour être lu à la bonne vitesse.
