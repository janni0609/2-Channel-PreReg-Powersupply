/**
 * netcfg.h - W5500 Ethernet bring-up, persisted LAN configuration and the SCPI
 *            TCP transport (raw socket, port 5025) for the RP2350 brain.
 *
 * Wiring (RP2350, hardware SPI0 — the OLED keeps SPI1 so the two never contend):
 *   CS  -> GPIO17   SCK -> GPIO18   MOSI -> GPIO19   MISO -> GPIO20
 *   RST -> GPIO21   INT -> GPIO22 (unused; the Ethernet library polls the W5500)
 *
 * The module owns the editable/persisted LAN settings (DHCP flag, static
 * IP/netmask/gateway/DNS, hostname). The front-panel Network submenu and the
 * SCPI SYSTem:COMMunicate:LAN commands both read and mutate these through the
 * accessors below, then call netcfg_apply() to (re)configure the interface.
 *
 * The MAC address is derived once from the RP2350 unique chip id (locally
 * administered, unicast) so every unit is unique without any stored value; it is
 * read-only.
 */
#ifndef BRAIN_NETCFG_H
#define BRAIN_NETCFG_H

#include <stdint.h>

/* Max hostname length incl. NUL (DHCP option 12 / display). */
#define NETCFG_HOSTNAME_MAX 24

/* SCPI instrument port (raw TCP, the standard SCPI-RAW "instrument" socket). */
#define NETCFG_SCPI_PORT 5025

/* Bring up the W5500 and start the SCPI TCP server. Loads the persisted config
 * (defaults applied if absent/corrupt) and derives the MAC. Non-blocking: a DHCP
 * request is started but not waited on, so a missing cable/server never stalls
 * boot. Call once from setup() AFTER EEPROM.begin(). */
void netcfg_init();

/* Service the transport every loop: accept new SCPI clients, drain their RX into
 * the SCPI parser, prune dropped connections and renew the DHCP lease. */
void netcfg_task();

/* Re-configure the interface from the current (pending) config and persist it.
 * Drops existing SCPI connections. Invoked by the menu "Apply" row and by the
 * SYST:COMM:LAN SCPI setters. */
void netcfg_apply();

/* Persist the current config to EEPROM without touching the live interface. */
void netcfg_save();

/* Request a deferred netcfg_apply(): the reconfigure runs at the top of the next
 * netcfg_task(), never mid-way through servicing a client. Used by the SCPI
 * SYST:COMM:LAN setters, which run inside client RX handling and must not tear
 * down the very socket they are being read from. */
void netcfg_request_apply();

/* --- Pending (editable, persisted) configuration ------------------------------ */
bool        netcfg_dhcp();
void        netcfg_set_dhcp(bool on);
void        netcfg_get_ip(uint8_t out[4]);     void netcfg_set_ip(const uint8_t v[4]);
void        netcfg_get_mask(uint8_t out[4]);   void netcfg_set_mask(const uint8_t v[4]);
void        netcfg_get_gw(uint8_t out[4]);     void netcfg_set_gw(const uint8_t v[4]);
void        netcfg_get_dns(uint8_t out[4]);    void netcfg_set_dns(const uint8_t v[4]);
const char *netcfg_hostname();                 void netcfg_set_hostname(const char *s);

/* --- Live interface status (what the W5500 actually has right now) ------------- */
bool netcfg_hw_present();     /* true once a W5500 responded at init          */
bool netcfg_link_up();        /* PHY link (cable) up                          */
bool netcfg_has_ip();         /* a non-zero IPv4 address is configured        */
void netcfg_live_ip(uint8_t out[4]);
void netcfg_live_mask(uint8_t out[4]);
void netcfg_live_gw(uint8_t out[4]);
void netcfg_mac(uint8_t out[6]);
int  netcfg_client_count();   /* number of connected SCPI clients             */

#endif /* BRAIN_NETCFG_H */
