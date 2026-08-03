/**
 * netcfg.cpp - W5500 Ethernet bring-up + persisted LAN config + SCPI TCP server.
 * See netcfg.h for the pin map and the design overview.
 */
#include <Arduino.h>
#include <SPI.h>
#include <Ethernet.h>
#include <EEPROM.h>
#include <string.h>
#include <pico/unique_id.h>

#include "netcfg.h"
#include "scpi.h"
#include "channel_link.h"   // keep the channel watchdogs fed across a DHCP attempt

// ── W5500 pin map (RP2350 hardware SPI0) ────────────────────────────────────────
#define NET_PIN_CS   17
#define NET_PIN_SCK  18
#define NET_PIN_MOSI 19
#define NET_PIN_MISO 20
#define NET_PIN_RST  21
// GPIO22 (INT) is intentionally unused: the Arduino Ethernet library polls the
// W5500 over SPI rather than taking an interrupt.

// ── SCPI transport limits ───────────────────────────────────────────────────────
#define NET_MAX_CLIENTS 4      // concurrent SCPI sockets (W5500 has 8 hw sockets)
#define NET_LINE_MAX    256    // SCPI line length cap (spec §2)

// ── DHCP timing ─────────────────────────────────────────────────────────────────
// Every DHCP acquisition in this library is *blocking* (Dhcp.cpp spins until it
// has a lease or the timeout expires), so every millisecond spent here is a
// millisecond loop() is not polling the front panel or feeding the channels.
//
// The initial lease attempt at boot is therefore skipped entirely unless the PHY
// already reports link-up: with no cable there is nobody to answer, and spending
// the full timeout would leave the front panel dead for seconds after power-on for
// no gain. It is also bounded, so a cable into a network without a DHCP server can
// only stall startup briefly (the OLED is already showing the readout by then, and
// the channel outputs are still off). If it is skipped or fails, netcfg_task()
// retries while the cable is up — promptly on the link-up edge, then on a slow
// cadence — so the supply self-heals once the network appears without any panel
// interaction.
//
// The retry budget is deliberately shorter than the channel-side comms watchdog
// (COMMS_TIMEOUT_MS = 1000 ms): a retry that outran it would make both channels
// drop their outputs every retry period. A LAN DHCP server answers in tens of
// milliseconds, so 800 ms is ample; if none does, we simply try again later.
#define NET_DHCP_INIT_TIMEOUT_MS  6000
#define NET_DHCP_INIT_RESP_MS     2000
#define NET_DHCP_RETRY_TIMEOUT_MS 800
#define NET_DHCP_RETRY_RESP_MS    250
#define NET_DHCP_RETRY_PERIOD_MS  20000u
// Settle time granted to the switch port after a link-up edge before the first
// retry fires (autoneg/STB learning would swallow a DISCOVER sent immediately).
#define NET_DHCP_LINKUP_DELAY_MS  1000u

// ── Persisted configuration ─────────────────────────────────────────────────────
// Own EEPROM slot (magic/version/CRC), separate from the thermal/runtime/setpoint
// stores in main.cpp. Base address 96 leaves room below for those blobs.
#define NET_STORE_ADDR    96
#define NET_STORE_MAGIC   0x4E455431u   /* 'NET1' */
#define NET_STORE_VERSION 1

struct NetStore {
    uint32_t magic;
    uint16_t version;
    uint8_t  dhcp;                        // 1 = DHCP, 0 = static
    uint8_t  reserved;
    uint8_t  ip[4];
    uint8_t  mask[4];
    uint8_t  gw[4];
    uint8_t  dns[4];
    char     hostname[NETCFG_HOSTNAME_MAX];
    uint16_t crc;
};

// Live (pending) config, seeded with defaults; overwritten by cfgLoad().
static bool    s_dhcp        = true;
static uint8_t s_ip[4]       = { 192, 168, 1, 50 };
static uint8_t s_mask[4]     = { 255, 255, 255, 0 };
static uint8_t s_gw[4]       = { 192, 168, 1, 1 };
static uint8_t s_dns[4]      = { 192, 168, 1, 1 };
static char    s_hostname[NETCFG_HOSTNAME_MAX] = "psu-2ch";

static uint8_t s_mac[6];               // derived from the chip id at init
static bool    s_hwPresent   = false;
static bool    s_inited      = false;
static bool    s_applyPending = false;
static uint32_t s_lastRetryMs = 0;
static bool    s_prevLink    = false;   // link-up edge detector for the retry clock
// True only while we believe we hold a DHCP lease. This gates Ethernet.maintain()
// — see netcfg_task() for why calling it without a lease is fatal to the UI.
static bool    s_leased      = false;

// ── SCPI TCP server + client pool ───────────────────────────────────────────────
static EthernetServer s_server(NETCFG_SCPI_PORT);
static EthernetClient s_clients[NET_MAX_CLIENTS];
static char           s_line[NET_MAX_CLIENTS][NET_LINE_MAX];
static uint16_t       s_lineLen[NET_MAX_CLIENTS];
static bool           s_lineOverflow[NET_MAX_CLIENTS];

// CRC16-CCITT (poly 0x1021, init 0xFFFF) — same routine as the main.cpp stores.
static uint16_t netCrc16(const uint8_t *data, uint16_t len) {
    uint16_t crc = 0xFFFFu;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

static void deriveMac() {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    // Locally-administered, unicast MAC from the low chip-id bytes.
    memcpy(s_mac, &id.id[2], 6);
    s_mac[0] &= (uint8_t)~0x01;   // unicast
    s_mac[0] |= 0x02;             // locally administered
}

static void cfgLoad() {
    NetStore s;
    EEPROM.get(NET_STORE_ADDR, s);
    if (s.magic != NET_STORE_MAGIC || s.version != NET_STORE_VERSION) return;
    if (s.crc != netCrc16((const uint8_t *)&s, offsetof(NetStore, crc)))  return;
    s_dhcp = s.dhcp != 0;
    memcpy(s_ip,   s.ip,   4);
    memcpy(s_mask, s.mask, 4);
    memcpy(s_gw,   s.gw,   4);
    memcpy(s_dns,  s.dns,  4);
    s.hostname[NETCFG_HOSTNAME_MAX - 1] = '\0';
    strncpy(s_hostname, s.hostname, NETCFG_HOSTNAME_MAX);
    s_hostname[NETCFG_HOSTNAME_MAX - 1] = '\0';
}

void netcfg_save() {
    NetStore s;
    memset(&s, 0, sizeof(s));
    s.magic   = NET_STORE_MAGIC;
    s.version = NET_STORE_VERSION;
    s.dhcp    = s_dhcp ? 1 : 0;
    memcpy(s.ip,   s_ip,   4);
    memcpy(s.mask, s_mask, 4);
    memcpy(s.gw,   s_gw,   4);
    memcpy(s.dns,  s_dns,  4);
    strncpy(s.hostname, s_hostname, NETCFG_HOSTNAME_MAX);
    s.hostname[NETCFG_HOSTNAME_MAX - 1] = '\0';
    s.crc = netCrc16((const uint8_t *)&s, offsetof(NetStore, crc));
    EEPROM.put(NET_STORE_ADDR, s);
    EEPROM.commit();   // RP2350 EEPROM emulation is a RAM shadow; commit flushes it
}

// Hardware-reset the W5500 and re-configure the interface from the pending config.
static void bringUp() {
    // Hardware reset pulse (W5500 needs >=500 us low, then ~50 ms to settle).
    pinMode(NET_PIN_RST, OUTPUT);
    digitalWrite(NET_PIN_RST, LOW);
    delay(2);
    digitalWrite(NET_PIN_RST, HIGH);
    delay(60);

    // Route SPI0 to the W5500 pins before the library opens the bus.
    SPI.setRX(NET_PIN_MISO);
    SPI.setCS(NET_PIN_CS);
    SPI.setSCK(NET_PIN_SCK);
    SPI.setTX(NET_PIN_MOSI);
    SPI.begin();

    Ethernet.init(NET_PIN_CS);

    if (s_dhcp) {
        // linkStatus() probes/initialises the chip itself, so it is safe (and cheap)
        // before any begin(). With no cable there is no point paying the DHCP
        // timeout: configure the interface unaddressed and leave acquisition to the
        // link-gated retry in netcfg_task().
        if (Ethernet.linkStatus() == LinkON) {
            // Bounded, so a network without a DHCP server can't hang boot for long.
            s_leased = (Ethernet.begin(s_mac, NET_DHCP_INIT_TIMEOUT_MS,
                                       NET_DHCP_INIT_RESP_MS) == 1);
        } else {
            s_leased = false;
            IPAddress zero(0, 0, 0, 0);
            Ethernet.begin(s_mac, zero, zero, zero, zero);   // non-blocking
        }
    } else {
        s_leased = false;                           // static: nothing to renew
        IPAddress ip(s_ip[0], s_ip[1], s_ip[2], s_ip[3]);
        IPAddress dns(s_dns[0], s_dns[1], s_dns[2], s_dns[3]);
        IPAddress gw(s_gw[0], s_gw[1], s_gw[2], s_gw[3]);
        IPAddress mask(s_mask[0], s_mask[1], s_mask[2], s_mask[3]);
        Ethernet.begin(s_mac, ip, dns, gw, mask);   // non-blocking
    }

    s_hwPresent = (Ethernet.hardwareStatus() != EthernetNoHardware);
    s_lastRetryMs = millis();
    s_server.begin();
}

void netcfg_init() {
    deriveMac();
    cfgLoad();

    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        s_lineLen[i]      = 0;
        s_lineOverflow[i] = false;
    }

    bringUp();
    s_inited = true;
}

void netcfg_apply() {
    netcfg_save();
    if (!s_inited) return;
    // Drop any live SCPI sessions; the interface is about to be reconfigured.
    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (s_clients[i]) s_clients[i].stop();
        s_lineLen[i]      = 0;
        s_lineOverflow[i] = false;
    }
    bringUp();
}

void netcfg_request_apply() { s_applyPending = true; }

// Response sink handed to the SCPI parser: write straight to the originating
// TCP client. `ctx` is the EthernetClient* for that session.
static void netWrite(void *ctx, const char *data, size_t len) {
    EthernetClient *c = (EthernetClient *)ctx;
    if (c && c->connected() && len > 0)
        c->write((const uint8_t *)data, len);
}

// Feed received bytes for one client through its line assembler, dispatching a
// complete line (LF-terminated, CR ignored) to the SCPI parser. Over-long lines
// are dropped and flagged so the terminating LF reports a single framing error.
static void serviceClient(int slot) {
    EthernetClient &c = s_clients[slot];
    int avail = c.available();
    while (avail-- > 0) {
        int ci = c.read();
        if (ci < 0) break;
        char ch = (char)ci;
        if (ch == '\n') {
            if (s_lineOverflow[slot]) {
                scpi_report_line_overflow(netWrite, &c);
                s_lineOverflow[slot] = false;
            } else {
                s_line[slot][s_lineLen[slot]] = '\0';
                scpi_process_line(s_line[slot], netWrite, &c);
            }
            s_lineLen[slot] = 0;
        } else if (ch != '\r') {
            if (s_lineLen[slot] < NET_LINE_MAX - 1) {
                s_line[slot][s_lineLen[slot]++] = ch;
            } else {
                s_lineOverflow[slot] = true;   // keep consuming until the LF
            }
        }
    }
}

void netcfg_task() {
    if (!s_inited) return;

    // Deferred reconfigure requested by a SCPI LAN setter (done here, outside the
    // per-client RX loop, so we never tear down a socket mid-read).
    if (s_applyPending) { s_applyPending = false; netcfg_apply(); return; }

    // Accept a newly-connected client into a free (or dead) slot; refuse if full.
    EthernetClient incoming = s_server.accept();
    if (incoming) {
        int slot = -1;
        for (int i = 0; i < NET_MAX_CLIENTS; i++) {
            if (!s_clients[i] || !s_clients[i].connected()) { slot = i; break; }
        }
        if (slot < 0) {
            incoming.stop();                 // pool full: reject
        } else {
            if (s_clients[slot]) s_clients[slot].stop();
            s_clients[slot]      = incoming;
            s_lineLen[slot]      = 0;
            s_lineOverflow[slot] = false;
        }
    }

    // Drain each live client; reap the ones that dropped.
    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (!s_clients[i]) continue;
        if (s_clients[i].connected()) serviceClient(i);
        else                          s_clients[i].stop();
    }

    // DHCP lease renewal. Only ever call maintain() while we actually hold a lease:
    // with none, the library's checkLease() matches its "_rebindInSec == 0 &&
    // _dhcp_state == STATE_DHCP_START" arm — precisely the state a *failed* attempt
    // leaves behind — and re-runs the blocking request_DHCP_lease() on every single
    // call, at the timeout of the last begin(). Unguarded, a booted-with-no-DHCP-
    // server unit therefore stalls loop() for 6 s per pass, forever: the front panel
    // stops responding and both channel links flap on their 500 ms telemetry
    // timeout. Re-acquisition belongs to the bounded, rate-limited retry below.
    if (s_dhcp && s_leased) {
        int rc = Ethernet.maintain();
        // 1 = renew failed, 3 = rebind failed: the lease is gone and the library is
        // back in STATE_DHCP_START, so stop calling maintain() and let the retry
        // path (with its short budget) do the re-acquisition.
        if (rc == 1 || rc == 3) s_leased = false;
    }

    // Slow DHCP retry: reacquire a lease if we booted before the network came up,
    // or lost it later. Gated on link-up, so an unplugged cable costs nothing.
    bool linkUp = netcfg_link_up();
    if (linkUp && !s_prevLink) {
        // Cable just appeared: don't make the user wait out a whole retry period.
        // Backdate the retry clock so the next attempt fires after the settle time.
        s_lastRetryMs = millis() - (NET_DHCP_RETRY_PERIOD_MS - NET_DHCP_LINKUP_DELAY_MS);
    }
    s_prevLink = linkUp;

    if (s_dhcp && !s_leased && linkUp) {
        uint32_t now = millis();
        if ((uint32_t)(now - s_lastRetryMs) >= NET_DHCP_RETRY_PERIOD_MS) {
            s_lastRetryMs = now;
            // The attempt blocks for up to NET_DHCP_RETRY_TIMEOUT_MS with the UART
            // links unserviced. Ping both channels first so their comms watchdogs
            // start the window fresh, and again after so a link marked down on the
            // brain side recovers on the next telemetry push instead of waiting.
            channel_ping(0); channel_ping(1);
            s_leased = (Ethernet.begin(s_mac, NET_DHCP_RETRY_TIMEOUT_MS,
                                       NET_DHCP_RETRY_RESP_MS) == 1);
            channel_ping(0); channel_ping(1);
            channel_link_task();
        }
    }
}

// ── Pending-config accessors ────────────────────────────────────────────────────
bool netcfg_dhcp()              { return s_dhcp; }
void netcfg_set_dhcp(bool on)   { s_dhcp = on; }

void netcfg_get_ip(uint8_t o[4])    { memcpy(o, s_ip, 4); }
void netcfg_set_ip(const uint8_t v[4])   { memcpy(s_ip, v, 4); }
void netcfg_get_mask(uint8_t o[4])  { memcpy(o, s_mask, 4); }
void netcfg_set_mask(const uint8_t v[4]) { memcpy(s_mask, v, 4); }
void netcfg_get_gw(uint8_t o[4])    { memcpy(o, s_gw, 4); }
void netcfg_set_gw(const uint8_t v[4])   { memcpy(s_gw, v, 4); }
void netcfg_get_dns(uint8_t o[4])   { memcpy(o, s_dns, 4); }
void netcfg_set_dns(const uint8_t v[4])  { memcpy(s_dns, v, 4); }

const char *netcfg_hostname()   { return s_hostname; }
void netcfg_set_hostname(const char *s) {
    if (!s) return;
    strncpy(s_hostname, s, NETCFG_HOSTNAME_MAX);
    s_hostname[NETCFG_HOSTNAME_MAX - 1] = '\0';
}

// ── Live status ─────────────────────────────────────────────────────────────────
bool netcfg_hw_present() { return s_hwPresent; }
bool netcfg_link_up()    { return s_inited && Ethernet.linkStatus() == LinkON; }

bool netcfg_has_ip() {
    IPAddress ip = Ethernet.localIP();
    return !(ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0);
}

static void ipToBytes(IPAddress ip, uint8_t o[4]) {
    o[0] = ip[0]; o[1] = ip[1]; o[2] = ip[2]; o[3] = ip[3];
}
void netcfg_live_ip(uint8_t o[4])   { ipToBytes(Ethernet.localIP(),   o); }
void netcfg_live_mask(uint8_t o[4]) { ipToBytes(Ethernet.subnetMask(),o); }
void netcfg_live_gw(uint8_t o[4])   { ipToBytes(Ethernet.gatewayIP(), o); }
void netcfg_mac(uint8_t o[6])       { memcpy(o, s_mac, 6); }

int netcfg_client_count() {
    int n = 0;
    for (int i = 0; i < NET_MAX_CLIENTS; i++)
        if (s_clients[i] && s_clients[i].connected()) n++;
    return n;
}
