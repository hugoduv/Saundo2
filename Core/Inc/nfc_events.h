#ifndef NFC_EVENTS_H
#define NFC_EVENTS_H

#include <stdint.h>

#define NFC_UID_MAX_LEN   10
#define NFC_UID_STR_LEN   (2 * NFC_UID_MAX_LEN + 1)

/* Appelée par la démo à chaque détection d'une carte */
void nfc_events_card_seen(const uint8_t *uid, uint8_t len);

/* À appeler dans la boucle principale : détecte le retrait de la carte */
void nfc_events_poll(void);

/* Événements pour le reste du projet (redéfinissables par l'équipe) */
void nfc_on_card_placed(const char *uid_hex);
void nfc_on_card_removed(const char *uid_hex);

#endif
