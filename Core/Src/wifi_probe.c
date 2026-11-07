#include "wifi_probe.h"
#include "wifi.h"
#include "main.h"
#include <stdio.h>
#include <string.h>

#if defined(__has_include)
#if __has_include("wifi_settings.local.h")
#include "wifi_settings.local.h"
#define SAUNDO_WIFI_CONFIGURED 1
#endif
#endif

#ifndef SAUNDO_WIFI_CONFIGURED
#define SAUNDO_WIFI_CONFIGURED 0
#endif

#if SAUNDO_WIFI_CONFIGURED
#define PROBE_SOCKET 0U
#define PROBE_TIMEOUT_MS 5000U
#define PROBE_RESPONSE_SIZE 512U

static const uint8_t server_ip[4] = SAUNDO_SERVER_IP;
static int wifi_ready;
static WIFI_APs_t detected_networks;

static void diagnose_wifi(void)
{
    char revision[ES_WIFI_FW_REV_SIZE];
    uint8_t i;
    uint8_t found = 0U;

    if (WIFI_GetModuleFwRevision(revision, sizeof(revision)) == WIFI_STATUS_OK)
    {
        printf("[WIFI] Version du module : %s\r\n", revision);
    }
    else
    {
        printf("[WIFI] Lecture de la version du module impossible.\r\n");
    }

    memset(&detected_networks, 0, sizeof(detected_networks));
    printf("[WIFI] Recherche du point d'acces configure...\r\n");
    if (WIFI_ListAccessPoints(&detected_networks, WIFI_MAX_APS) != WIFI_STATUS_OK)
    {
        printf("[WIFI] Recherche des reseaux impossible.\r\n");
        return;
    }
    for (i = 0U; i < detected_networks.count; ++i)
    {
        if (strcmp(detected_networks.ap[i].SSID, SAUNDO_WIFI_SSID) == 0)
        {
            printf("[WIFI] Point d'acces trouve : signal %d dBm, canal %u, securite %u.\r\n",
                   (int)detected_networks.ap[i].RSSI,
                   (unsigned)detected_networks.ap[i].Channel,
                   (unsigned)detected_networks.ap[i].Ecn);
            found = 1U;
        }
    }
    if (!found)
    {
        printf("[WIFI] Point d'acces non trouve dans les %u resultats du scan.\r\n",
               (unsigned)detected_networks.count);
    }
}

static int probe_server(void)
{
    static const char request[] =
        "GET /health HTTP/1.1\r\n"
        "Host: saundo2\r\n"
        "Connection: close\r\n\r\n";
    char response[PROBE_RESPONSE_SIZE];
    uint16_t sent = 0U;
    uint16_t received;
    size_t used = 0U;
    uint32_t started;
    int ok = 0;

    if (WIFI_OpenClientConnection(PROBE_SOCKET, WIFI_TCP_PROTOCOL, "SAUNDO2",
                                  server_ip, SAUNDO_SERVER_PORT, 0) != WIFI_STATUS_OK)
    {
        printf("[WIFI] Impossible de contacter le serveur TCP.\r\n");
        return 0;
    }

    if (WIFI_SendData(PROBE_SOCKET, (const uint8_t *)request,
                      (uint16_t)(sizeof(request) - 1U), &sent, PROBE_TIMEOUT_MS) != WIFI_STATUS_OK ||
        sent != sizeof(request) - 1U)
    {
        printf("[WIFI] Echec de l'envoi de la requete HTTP.\r\n");
    }
    else
    {
        started = HAL_GetTick();
        while (used < sizeof(response) - 1U &&
               (HAL_GetTick() - started) < PROBE_TIMEOUT_MS)
        {
            received = 0U;
            if (WIFI_ReceiveData(PROBE_SOCKET, (uint8_t *)&response[used],
                                 (uint16_t)(sizeof(response) - 1U - used),
                                 &received, 200U) != WIFI_STATUS_OK)
            {
                printf("[WIFI] Erreur de reception HTTP.\r\n");
                break;
            }
            used += received;
            response[used] = '\0';
            if ((strncmp(response, "HTTP/1.1 200 ", 13U) == 0 ||
                 strncmp(response, "HTTP/1.0 200 ", 13U) == 0) &&
                strstr(response, "\r\n\r\n") != NULL &&
                strstr(strstr(response, "\r\n\r\n") + 4, "\"status\":\"ok\"") != NULL)
            {
                ok = 1;
                break;
            }
        }
        if (!ok)
        {
            printf("[WIFI] Reponse /health invalide ou delai depasse.\r\n");
        }
    }

    if (WIFI_CloseClientConnection(PROBE_SOCKET) != WIFI_STATUS_OK)
    {
        printf("[WIFI] Echec de fermeture du socket.\r\n");
    }
    return ok;
}
#endif

void wifi_probe_start(void)
{
#if SAUNDO_WIFI_CONFIGURED
    uint8_t address[4];

    if (strlen(SAUNDO_WIFI_SSID) == 0U || strlen(SAUNDO_WIFI_SSID) > 32U ||
        strlen(SAUNDO_WIFI_PASSWORD) < 8U || strlen(SAUNDO_WIFI_PASSWORD) > 63U ||
        strchr(SAUNDO_WIFI_SSID, '\r') != NULL || strchr(SAUNDO_WIFI_SSID, '\n') != NULL ||
        strchr(SAUNDO_WIFI_PASSWORD, '\r') != NULL || strchr(SAUNDO_WIFI_PASSWORD, '\n') != NULL)
    {
        printf("[WIFI] Configuration WPA2 invalide.\r\n");
        return;
    }

    printf("[WIFI] Initialisation du module...\r\n");
    if (WIFI_Init() != WIFI_STATUS_OK)
    {
        printf("[WIFI] Initialisation du module impossible.\r\n");
        return;
    }
    printf("[WIFI] Connexion au point d'acces...\r\n");
    if (WIFI_Connect(SAUNDO_WIFI_SSID, SAUNDO_WIFI_PASSWORD, WIFI_ECN_WPA2_PSK) != WIFI_STATUS_OK)
    {
        printf("[WIFI] Connexion ou recuperation des parametres impossible.\r\n");
        diagnose_wifi();
        return;
    }
    if (WIFI_GetIP_Address(address, sizeof(address)) != WIFI_STATUS_OK)
    {
        printf("[WIFI] Impossible d'obtenir l'adresse IP.\r\n");
        return;
    }
    printf("[WIFI] Connecte : %u.%u.%u.%u\r\n",
           (unsigned)address[0], (unsigned)address[1],
           (unsigned)address[2], (unsigned)address[3]);
    printf("[WIFI] Test du serveur %u.%u.%u.%u:%u...\r\n",
           (unsigned)server_ip[0], (unsigned)server_ip[1],
           (unsigned)server_ip[2], (unsigned)server_ip[3],
           (unsigned)SAUNDO_SERVER_PORT);
    if (probe_server())
    {
        if (WIFI_SetCommandTimeout(3000U) == WIFI_STATUS_OK)
        {
            printf("[WIFI] Serveur /health OK.\r\n");
            wifi_ready = 1;
        }
        else
        {
            printf("[WIFI] Configuration du delai du module impossible.\r\n");
        }
    }

#else
    printf("[WIFI] Configuration absente : voir wifi_settings.example.h.\r\n");
#endif
}

int wifi_probe_is_ready(void)
{
#if SAUNDO_WIFI_CONFIGURED
    return wifi_ready;
#else
    return 0;
#endif
}

const uint8_t *wifi_probe_server_ip(void)
{
#if SAUNDO_WIFI_CONFIGURED
    return server_ip;
#else
    return NULL;
#endif
}

uint16_t wifi_probe_server_port(void)
{
#if SAUNDO_WIFI_CONFIGURED
    return SAUNDO_SERVER_PORT;
#else
    return 0U;
#endif
}
