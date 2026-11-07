#include "nfc_events.h"
#include "main.h"
#include <stdio.h>
#include <string.h>

/* Durée sans détection après laquelle on considère la carte retirée */
#define NFC_REMOVE_TIMEOUT_MS  2000U

/* UID à ignorer (ex : puce NFC intégrée à la carte STM32) */
static const char *blacklist[] = {
    /* "02XXXXXXXXXXXX", */
    NULL
};

static char     current_uid[NFC_UID_STR_LEN] = "";
static uint8_t  card_present = 0;
static uint32_t last_seen_ms = 0;

static void uid_to_hex(const uint8_t *uid, uint8_t len, char *out)
{
    static const char hex[] = "0123456789ABCDEF";
    if (len > NFC_UID_MAX_LEN) len = NFC_UID_MAX_LEN;
    for (uint8_t i = 0; i < len; i++) {
        out[2 * i]     = hex[uid[i] >> 4];
        out[2 * i + 1] = hex[uid[i] & 0x0F];
    }
    out[2 * len] = '\0';
}

static int is_blacklisted(const char *uid_hex)
{
    for (int i = 0; blacklist[i] != NULL; i++) {
        if (strcmp(blacklist[i], uid_hex) == 0) return 1;
    }
    return 0;
}

void nfc_events_card_seen(const uint8_t *uid, uint8_t len)
{
    char uid_hex[NFC_UID_STR_LEN];
    uid_to_hex(uid, len, uid_hex);

    if (is_blacklisted(uid_hex)) return;

    if (!card_present) {
        /* Nouvelle carte posée */
        card_present = 1;
        strcpy(current_uid, uid_hex);
        nfc_on_card_placed(current_uid);
    } else if (strcmp(current_uid, uid_hex) != 0) {
        /* Une autre carte a remplacé la précédente */
        nfc_on_card_removed(current_uid);
        strcpy(current_uid, uid_hex);
        nfc_on_card_placed(current_uid);
    }
    /* Sinon : même carte toujours présente, on ne signale rien */


    last_seen_ms = HAL_GetTick();
}

void nfc_events_poll(void)
{
    if (card_present && (HAL_GetTick() - last_seen_ms) > NFC_REMOVE_TIMEOUT_MS) {
        card_present = 0;
        nfc_on_card_removed(current_uid);
        current_uid[0] = '\0';
    }
}

/* Comportement par défaut : affichage dans le terminal.
   L'équipe pourra redéfinir ces fonctions ailleurs pour lancer la musique. */
__weak void nfc_on_card_placed(const char *uid_hex)
{
    printf(">>> CARTE POSEE : %s\r\n", uid_hex);
}

__weak void nfc_on_card_removed(const char *uid_hex)
{
    printf(">>> CARTE RETIREE : %s\r\n", uid_hex);
}
